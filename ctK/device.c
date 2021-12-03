// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2016-2018 Intel Corporation. All rights reserved. */
#include <linux/memremap.h>
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/pfn_t.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/libnvdimm.h>
#include "dax-private.h"
#include "bus.h"

#ifdef CTFS_PSWAP
#include <asm/tlbflush.h>
#include <asm/pgalloc.h>
#include <asm/tlb.h>
#include <asm/set_memory.h>
#include <asm/pkeys.h>

#define DAX_PGPROT_MPK(pgprot, mpk) __pg(pgprot_val(PAGE_SHARED) + arch_calc_vm_prot_bits(0, mpk))

static struct dax_runtime dax_rt_instance = {.master = NULL, .pswap_temp = NULL};
static struct dax_runtime *rt = &dax_rt_instance;
static const struct file_operations dax_fops;
static const char dax_master_magic_word[64] = DAX_MASTER_MAGIC_WORD;
static dax_master_page_t *current_master = NULL;
static struct vm_area_struct *current_vma = NULL;
static unsigned long current_start;
static unsigned long *pswap_fast_first = NULL, 
					 *pswap_fast_second,
					 **pswap_fast_ptep1,
					 **pswap_fast_ptep2,
					 *pswap_fast_first_p,
					 *pswap_fast_second_p;

DEFINE_MUTEX(dax_lock);
// Declarations of the pcow() funcs
// static unsigned long find_cow_page(dax_runtime_t *rt, unsigned long user_va);
// static int pfn_callback_cow(pte_t *pte, unsigned long addr, void *data);
// static vm_fault_t dev_dax_cow_fault(struct vm_fault *vmf);
static relptr_t find_dax_ptep(dax_runtime_t * rt, relptr_t addr, relptr_t ** pmdp, relptr_t ** pudp);

phys_addr_t dax_pgoff_to_phys(struct dev_dax *dev_dax, pgoff_t pgoff,
		unsigned long size);

long dax_ioctl(struct file *, unsigned int, unsigned long);

/* allocate one page in bitmap
 * @return 	relptr to the beginning
 */
inline static relptr_t alloc_dax_pg(dax_runtime_t * rt){
	relptr_t pos;
	pos = bitmap_find_free_region((relptr_t *)rt->bitmap, rt->num_pages,0);
	clwb(rt->bitmap + (pos/64));
#if !defined(PSWAP_DEBUG)
	printk("Bitmap allocated: \t%lu\n", pos);
#endif
	return (pos << PAGE_SHIFT);
}

inline static void free_dax_pg(dax_runtime_t * rt, relptr_t page){
	unsigned long pos = page>>PAGE_SHIFT;
	bitmap_release_region((relptr_t *)rt->bitmap, pos, 0);
	clwb(rt->bitmap + (pos/64));
#if !defined(PSWAP_DEBUG)
	printk("Bitmap Freed:     \t%lu\n", pos);
#endif
}

/* allocate 512 pages in bitmap
 * @return 	relptr to the beginning
 */
inline static relptr_t alloc_dax_512pg(dax_runtime_t * rt){
	relptr_t pos;
#if PSWAP_DEBUG > 2
	printk("\t\t\t\t Bitmap allocation: rt->bitmap: %#lx, rt->num_pages: %#lx",rt->bitmap, rt->num_pages);
#endif
	pos = bitmap_find_free_region((unsigned long *)rt->bitmap, rt->num_pages,9);
	
#if PSWAP_DEBUG > 2
	printk("\t\t\t\t Bitmap allocated: \t%lu - %lu\n", pos, pos + 512);
#endif
	clwb(rt->bitmap + (pos/64));
	return (pos << PAGE_SHIFT);
}

inline static void free_dax_512pg(dax_runtime_t * rt, relptr_t page){
	relptr_t pos = page>>PAGE_SHIFT;
	bitmap_release_region((relptr_t *)rt->bitmap, pos, 9);
	clwb(rt->bitmap + (pos/64));
#if !defined(PSWAP_DEBUG)
	printk("Bitmap Freed:     \t%lu - %lu\n", pos, pos + 512);
#endif
}

inline static unsigned dax_get_ptep(struct mm_struct *mm, unsigned long addr, pte_t **ptep, pmd_t **pmdp)
{
	pgd_t	*pgd;
	p4d_t	*p4d;
	pud_t	*pud;
	pmd_t	*pmd;
	pte_t	*pte;
	int     retval = 0;

	pgd = pgd_offset(mm, addr & PAGE_MASK);
	if (pgd) {
		p4d = p4d_alloc(mm, pgd, addr & PAGE_MASK);
		pud = pud_alloc(mm, p4d, addr & PAGE_MASK);
		pmd = pmd_alloc(mm, pud, addr & PAGE_MASK);
		*pmdp = pmd;
		if (!pmd_none(*pmd)) {
			pte = pte_offset_kernel(pmd, addr & PAGE_MASK);
			if (pte) {
				retval = 1;
				*ptep = pte;
			}
		}
		else {
			*ptep = NULL;
		}
	}
	return retval;
}

inline static unsigned dax_get_ptep_noalloc(struct mm_struct *mm, unsigned long addr, pte_t **ptep)
{
	pgd_t	*pgd;
	p4d_t	*p4d;
	pud_t	*pud;
	pmd_t	*pmd;
	pte_t	*pte;

	pgd = pgd_offset(mm, addr & PAGE_MASK);
	if (pgd) {
		p4d = p4d_offset(pgd, addr & PAGE_MASK);
		if(p4d_none(*p4d) || unlikely(p4d_bad(*p4d))){
			return 0;
		}
		pud = pud_offset(p4d, addr & PAGE_MASK);
		if(pud_none(*pud) || unlikely(pud_bad(*pud))){
			return 0;
		}
		pmd = pmd_offset(pud, addr & PAGE_MASK);
		if(pmd_none(*pmd) || unlikely(pmd_bad(*pmd))){
			return 0;
		}
		pte = pte_offset_kernel(pmd, addr & PAGE_MASK);
		*ptep = pte;
		return 1;
	}
	return 0;
}

inline static unsigned dax_get_pmdp(struct mm_struct *mm, unsigned long addr, pmd_t **pmdp)
{
	pgd_t	*pgd;
	p4d_t	*p4d;
	pud_t	*pud;
	pmd_t	*pmd;

	pgd = pgd_offset(mm, addr & PAGE_MASK);
	if (pgd) {
		p4d = p4d_alloc(mm, pgd, addr & PAGE_MASK);
		pud = pud_alloc(mm, p4d, addr & PAGE_MASK);
		pmd = pmd_alloc(mm, pud, addr & PAGE_MASK);
		*pmdp = pmd;
		return 1;
	}
	return 0;
}

inline static unsigned dax_get_pudp(struct mm_struct *mm, unsigned long addr, pud_t **pudp)
{
	pgd_t	*pgd;
	p4d_t	*p4d;
	pud_t	*pud;

	pgd = pgd_offset(mm, addr & PAGE_MASK);
	if (pgd) {
		p4d = p4d_alloc(mm, pgd, addr & PAGE_MASK);
		pud = pud_alloc(mm, p4d, addr & PAGE_MASK);
		*pudp = pud;
		return 1;
	}
	return 0;
}

static int dax_free_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t	*pgd;
	p4d_t	*p4d;
	pud_t	*pud;
	pmd_t	*pmd;
	pte_t	*pte;
	int     retval = 0;

	pgd = pgd_offset(mm, addr & PAGE_MASK);
	if (pgd) {
		p4d = p4d_offset(pgd, addr & PAGE_MASK);
		if(!p4d_present(*p4d)){
			return 0;
		}
		pud = pud_offset(p4d, addr & PAGE_MASK);
		if(!pud_present(*pud)){
			return 0;
		}
		pmd = pmd_offset(pud, addr & PAGE_MASK);
		if (pmd_present(*pmd)) {
			pte = pte_offset_map(pmd, 0);
#if PSWAP_DEBUG > 1
			printk("\tDAX free_pmd, pte: %#lx \n", pte);
#endif
			pte_free(mm, pfn_to_page(((unsigned long)pte - PAGE_OFFSET)>> PAGE_SHIFT));
			pmd->pmd = 0;
			// mm_dec_nr_ptes(mm);
			
		}
	}
	return retval;
}

static dax_master_page_t * get_master_page(struct vm_area_struct *vma){
	struct file *filp;
	struct dev_dax *dev_dax;
	phys_addr_t dax_start_addr;
	if(likely(vma == current_vma && vma->vm_start == current_start)){
		return current_master;
	}
	if(unlikely(!vma)){
		return NULL;
	}
	if(unlikely(!vma_is_dax(vma))){
		return NULL;
	}
	filp = vma->vm_file;
	if(unlikely(!filp)){
		return NULL;
	}
	dev_dax = filp->private_data;
	dax_start_addr = dax_pgoff_to_phys(dev_dax, 0, PMD_SIZE) + DAX_PSWAP_SHIFT;
	current_master = (dax_master_page_t *) ((void *)dax_start_addr + __PAGE_OFFSET);
	if(unlikely(MASTER_NOT_INIT(current_master))){
		current_master = NULL;
		return NULL;
	}
	current_start = vma->vm_start;
	current_vma = vma;
	return current_master;
}

/* we handle pswap crush here */
static dax_runtime_t * get_dax_runtime(dax_master_page_t * master_page){
	if(unlikely(master_page != rt->master)){
		rt->master = master_page;
		rt->start_paddr = (phys_addr_t) ((void*)master_page - __PAGE_OFFSET);
		rt->start = (void*)master_page;
		rt->bitmap = rt->start + master_page-> bm_start_offset;
		rt->pgd = DAX_REL2ABS(master_page-> pgd_offset);
		rt->mpk[0] = mm_pkey_alloc(current->mm);
		rt->mpk[1] = mm_pkey_alloc(current->mm);
		rt->mpk[2] = mm_pkey_alloc(current->mm);
		if(rt->pswap_temp == NULL){
			rt->pswap_temp = kmalloc(2 * PAGE_SIZE, GFP_KERNEL);
		}
	}
	if(unlikely(master_page->pswap_state != DAX_PSWAP_NORMAL)){
		/* we have serious trouble here */
		if(!master_page->pswap_short){
			unsigned long i, j, k, l;
			unsigned long *ptep;
			volatile dax_swap_frame_t *swap_frame = NULL;
			switch (master_page->pswap_state)
			{
			case DAX_PSWAP_STEP2:
				//restore the pages according to the journal
				printk("CRASHED: Trying to recover from slow pswap Step 2, %lx affected\n", master_page->pswap_npgs);
				for(i=0; i<master_page->pswap_npgs; i++){
					j = i / DAX_PSWAP_PER_MASTER;		//first 16
					l = i % DAX_PSWAP_PER_PAGE;	//last 128
					if(unlikely(l == 0)){
						k = (i - (j * DAX_PSWAP_PER_MASTER))  / DAX_PSWAP_PER_PAGE;		//middle 512
						swap_frame = (dax_swap_frame_t *) (DAX_REL2ABS(master_page->pswap_frame[j] + (k << PAGE_SHIFT)));
					}
					// ptep = find_dax_ptep(rt, swap_frame[l].first);
					// *DAX_PTEP_OFFSET(ptep, swap_frame[l].first) = swap_frame[l].first_p;
					// ptep = find_dax_ptep(rt, swap_frame[l].second);
					// *DAX_PTEP_OFFSET(ptep, swap_frame[l].second) = swap_frame[l].second_p;
				}

			case DAX_PSWAP_STEP3:
			case DAX_PSWAP_STEP1:
				printk("CRASHED: Trying to recover from slow pswap Step 1 or 3, %lx affected\n", master_page->pswap_npgs);

				//free the allocated journal
				for(i=0; i<(master_page->pswap_npgs)/DAX_PSWAP_PER_MASTER + 1; i++){
					if(master_page->pswap_frame[i] != 0){
						free_dax_512pg(rt, master_page->pswap_frame[i]);
					}
				}
				//finished
				master_page->pswap_npgs = 0;
				master_page->pswap_state = DAX_PSWAP_NORMAL;
				memset(master_page->pswap_frame, 0, DAX_PSWAP_MASTER_PGS * 8);
				break;
			
			default:
				printk("CRASHED: UNKNOWN slow pswap status. The storage is broken!\n");
				break;
			}
		}
		else{
			if(master_page->pswap_state == DAX_PSWAP_STEP2){
				unsigned long i;
				unsigned long *ptep;
				volatile dax_swap_frame_t *swap_frame = rt->pswap_fast_frame;
				printk("CRASHED: Trying to recover from fast pswap Step 2, %lx affected\n", master_page->pswap_npgs);
				// for(i=0; i<master_page->pswap_npgs; i++){
				// 	ptep = find_dax_ptep(rt, swap_frame[i].first);
				// 	*DAX_PTEP_OFFSET(ptep, swap_frame[i].first) = swap_frame[i].first_p;
				// 	ptep = find_dax_ptep(rt, swap_frame[i].second);
				// 	*DAX_PTEP_OFFSET(ptep, swap_frame[i].second) = swap_frame[i].second_p;
				// }
				master_page->pswap_npgs = 0;
				master_page->pswap_state = DAX_PSWAP_NORMAL;
				master_page->pswap_short = 0;
			}
			else{
				printk("CRASHED: UNKNOWN pswap status. The storage is broken!\n");
			}
		}
		
	}
	return rt;
}

/* Downgrade a huge page to normal pages
 *
 * @param[in]	pmdpp	where need to downgrade
 */
static void dax_downgrade_huge(dax_runtime_t * rt, relptr_t *pmdp){
	relptr_t pmd_pfn;
	relptr_t * pte_pt;
	unsigned long i;
#if PSWAP_DEBUG > 1
	printk("\tDAX pswap downgrade begin: pmdp: %#lx\n",(unsigned long)pmdp);
#endif
	pmd_pfn = DAX_HUGE2REL(*pmdp);
	pte_pt = DAX_REL2ABS(alloc_dax_pg(rt));
#if PSWAP_DEBUG > 1
	printk("\t    pmd_pfn: %#lx\n",(unsigned long)pmd_pfn);
#endif
	for (i = 0; i < 512; i++){
		pte_pt[i] = pmd_pfn + (i << PAGE_SHIFT);
	}
	arch_wb_cache_pmem(pte_pt, PAGE_SIZE);
	*pmdp = DAX_ABS2REL(pte_pt);
	arch_wb_cache_pmem(pmdp, 8);
#if PSWAP_DEBUG > 1
	printk("\t    Finished downgrade\n");
#endif
}

/* find the pte in the dax
 * if the pte haven't been allocated, allocate one
 * if it's HUGE, return the starting relptr_t
 * of the pmd. ptr will set to the pmdp.
 * else, return 0.
 * pmdp will be returned in parameter.
 * @param[in]	relative address of target 
 * @param[out]	pmdp to the the corresponding pmd entry. 
 * @param[out]	pudp
 * @return		starting rel ptr if it's HUGE page, 0 otherwise
 */ 
static relptr_t find_dax_ptep(dax_runtime_t * rt, relptr_t addr, relptr_t ** pmdp, relptr_t ** pudp){
	unsigned long pgd_offset, pud_offset, pmd_offset;
	relptr_t *dax_pudp, *dax_pmdp;
	relptr_t ret;
#if PSWAP_DEBUG > 2
	printk("\tFind_dax_ptep: addr: %#lx, base: %#lx\n", 
	addr, rt->vaddr_base);
#endif
	/* PGD */
	pgd_offset = (addr & PGDIR_MASK) >> PGDIR_SHIFT;
	dax_pudp = DAX_REL2ABS(rt->pgd[pgd_offset]);
	if(unlikely(dax_pudp == rt->start)){
		// need to allocate
		rt->pgd[pgd_offset] = alloc_dax_pg(rt);
		dax_pudp = DAX_REL2ABS(rt->pgd[pgd_offset]);
		memset(dax_pudp, 0, PAGE_SIZE);
		arch_wb_cache_pmem(dax_pudp, PAGE_SIZE);
		arch_wb_cache_pmem(&rt->pgd[pgd_offset], 8);
	}

	/* PUD */
	pud_offset = (addr & PUD_MASK) >> PUD_SHIFT;
	pud_offset &= 0x01ff;
	dax_pmdp = DAX_REL2ABS(dax_pudp[pud_offset]);
	if(pudp != NULL){
		*pudp = &dax_pudp[pud_offset];
		if(pmdp == NULL){
			return 0;
		}
	}
	if(unlikely(dax_pmdp == rt->start)){
		// need to allocate
		dax_pudp[pud_offset] = alloc_dax_pg(rt);
		dax_pmdp = DAX_REL2ABS(dax_pudp[pud_offset]);
		memset(dax_pmdp, 0, PAGE_SIZE);
		arch_wb_cache_pmem(dax_pudp, PAGE_SIZE);
		arch_wb_cache_pmem(&dax_pudp[pud_offset], 8);
	}
#if PSWAP_DEBUG > 2
	printk("\t\t  pud_offset: %#lx, pudp: %#lx start: %#lx, pmdp: %#lx\n", 
	pud_offset, (unsigned long)(dax_pudp), (unsigned long)rt->start, (unsigned long)dax_pmdp);
#endif

	/* PMD */
	pmd_offset = (addr & PMD_MASK) >> PMD_SHIFT;
	pmd_offset &= 0x01ff;
	if(unlikely(dax_pmdp[pmd_offset] == 0)){
		// need to allocate
		// allocate HUGE first
#ifdef PSWAP_HUGE
		dax_pmdp[pmd_offset] = DAX_SET_HUGE(alloc_dax_512pg(rt));
		arch_wb_cache_pmem(&dax_pmdp[pmd_offset], 8);
		ret = DAX_HUGE2REL(dax_pmdp[pmd_offset]);
#else
		unsigned long i = 0;
		relptr_t * dax_ptep, pte, pmd;
		pmd = alloc_dax_pg(rt);
		dax_ptep = DAX_REL2ABS(pmd);
		pte = alloc_dax_512pg(rt);
		while(i < 512){
			dax_ptep[i] = pte + (i << PAGE_SHIFT);
			i++;
		}
		arch_wb_cache_pmem(dax_ptep, PAGE_SIZE);
		dax_pmdp[pmd_offset] = pmd;
		arch_wb_cache_pmem(&dax_pmdp[pmd_offset], 8);
		ret = 0;
#endif
	}
	else{
		if(DAX_IF_HUGE(dax_pmdp[pmd_offset])){
			// printk("\t\t\t HUGE!");
			ret = DAX_HUGE2REL(dax_pmdp[pmd_offset]);
		}
		else{
			ret = 0;
		}
	}
	*pmdp = &dax_pmdp[pmd_offset];
#if PSWAP_DEBUG > 2
	printk("\t\t  DAX PTE: pmdp: %#lx, pmd_offset: %#lx, return: %#lx\n", 
	(unsigned long)dax_pmdp, pmd_offset, ret);
#endif
	return ret;
}

static inline pmd_t calculate_pmd(unsigned long paddr, unsigned char mpk){
	pmd_t ret = {.pmd = paddr & PMD_MASK};
	ret.pmd |= 0x0a7 | ((unsigned long)0x01 << 63);
	ret.pmd |= (unsigned long)mpk << 59;
	ret.pmd |= _PAGE_DEVMAP | _PAGE_SPECIAL;
	return ret;
}

static inline pte_t calculate_pte(unsigned long paddr, unsigned char mpk){
	pte_t ret = {.pte = paddr & PAGE_MASK};
	ret.pte |= 0x027 | ((unsigned long)0x01 << 63);
	ret.pte |= (unsigned long)mpk << 59;
	ret.pte |= _PAGE_DEVMAP | _PAGE_SPECIAL;
	return ret;
}

static inline void install_pmd(struct vm_fault *vmf, unsigned long addr){
	relptr_t *dax_pmdp, *dax_ptep, paddr_rel;
	pte_t * ptep, pte;
	pmd_t * pmdp, pmd;
	pud_t *vpud;
	unsigned long i;
	unsigned char mpk, mpk_type;
	struct mm_struct *mm = current->mm;
	relptr_t addr_offset = addr - rt->vaddr_base;
	paddr_rel = find_dax_ptep(rt, addr_offset, &dax_pmdp, NULL);
	if(vmf == NULL){
		vpud = NULL;
		pmdp = NULL;
	}
	else{
		vpud = vmf->pud;
		pmdp = vmf->pmd;
#if PSWAP_DEBUG >1
	printk("\t\tDAX INTALL_PMD addr: %#lx, find returned: %#lx, dax_pmdp: %#lx\n", 
	addr_offset, paddr_rel, *dax_pmdp);
	printk("\t\t\tvmf: pud: %#lx, pmd: %#lx, pte: %#lx, *pmdp: %#lx\n", 
	vmf->pud, vmf->pmd, vmf->pte, pmdp? pmdp->pmd: 0);
#endif
	}
	if(pmdp == NULL){
		if(vpud == NULL){
			p4d_t	*p4d;
			pgd_t	*pgd;
			pgd = pgd_offset(mm, addr & PAGE_MASK);
			p4d = p4d_alloc(mm, pgd, addr & PAGE_MASK);
			vpud = pud_alloc(mm, p4d, addr & PAGE_MASK);
		}
		pmdp = pmd_alloc(mm, vpud, addr & PAGE_MASK);
	}
	if((pmdp->pmd & (_PAGE_PRESENT | _PAGE_PSE)) == (_PAGE_PRESENT) ){
		// it's not huge
		// pte is present
		ptep = pte_offset_kernel(pmdp, addr & PAGE_MASK);
		if(paddr_rel != 0){
			// it's HUGE
			mpk_type = (*dax_pmdp >> 1 ) & 0b011;
			mpk = rt->mpk[mpk_type]; 
			free_page((unsigned long)ptep);
			mm_dec_nr_ptes(mm);
			pmd = calculate_pmd(DAX_REL2PHY(paddr_rel), mpk);
			set_pmd(pmdp, pmd);
		}
		else{
			// page by page
			for(i = 0; i < PTRS_PER_PMD; i++){
				dax_ptep = DAX_REL2ABS(*dax_pmdp);
				mpk_type = (dax_ptep[i] >> 1 ) & 0b011;
				mpk = rt->mpk[mpk_type]; 
				pte = calculate_pte(DAX_REL2PHY(dax_ptep[i]), mpk);
				set_pte(ptep, pte);
			}
		}
	}
	else{
		// pte is none
		if(paddr_rel != 0){
			// it's HUGE
			mpk_type = (*dax_pmdp >> 1 ) & 0b011;
			mpk = rt->mpk[mpk_type]; 
			pmd = calculate_pmd(DAX_REL2PHY(paddr_rel), mpk);
#if PSWAP_DEBUG >2
			printk("\t\t\t installed %#lx @ %#lx, paddr_base: %#lx, mpk: %d\n", 
			pmd.pmd, (unsigned long)pmdp, rt->start_paddr, mpk);
#endif
			set_pmd(pmdp, pmd);
		}
		else{
			ptep = pte_alloc_map(mm, pmdp, addr);	// it includes incr mm ptes
			// page by page
#if PSWAP_DEBUG >1
				printk("\t\t\t install pmd: %#lx @ %#lx for %#lx\n",
				pmdp->pmd, pmdp, addr);
#endif
			for(i = 0; i < PTRS_PER_PMD; i++){
				dax_ptep = DAX_REL2ABS(*dax_pmdp);
				mpk_type = (dax_ptep[i] >> 1 ) & 0b011;
				mpk = rt->mpk[mpk_type]; 
				pte = calculate_pte(DAX_REL2PHY(dax_ptep[i]), mpk);
#if PSWAP_DEBUG >1
				printk("\t\t\t installed pte: %#lx @ %#lx for %#lx\n",
				pte.pte, ptep + i, addr);
#endif
				set_pte(ptep + i, pte);
			}
		}
	}
}

static void init_dax(dax_master_page_t * mast_page, unsigned long dax_size){
	unsigned pgs_bitmap, i;
	relptr_t pgd_offset;
	unsigned long * pgdp;

	printk("Initializing Dax for %lu @ %#lx\n", dax_size, (unsigned long)mast_page);
	/* Reset the master page */
	memset((void*)mast_page,0,PAGE_SIZE);
	/* Set the magic code */
	strcpy(&(mast_page->magic_word[0]), dax_master_magic_word);
	mast_page->num_pages = dax_size >> PAGE_SHIFT;
	
	/* init runtime */
	rt->master = mast_page;
	rt->num_pages = mast_page->num_pages;
	rt->start_paddr = (phys_addr_t) ((void*)mast_page - __PAGE_OFFSET);
	rt->start = (void*)mast_page;
	rt->mpk[0] = mm_pkey_alloc(current->mm);
	rt->mpk[1] = mm_pkey_alloc(current->mm);
	rt->mpk[2] = mm_pkey_alloc(current->mm);
	
	/* init bitmap */
	mast_page->bm_start_offset = PAGE_SIZE;
	rt->bitmap = rt->start + mast_page-> bm_start_offset;
	bitmap_zero((unsigned long *)rt->bitmap, rt->num_pages);
	bitmap_set((unsigned long *)rt->bitmap, 0, 1);
	pgs_bitmap = ((rt->num_pages >> PAGE_SHIFT) >>3) + 1;
	for (i = 0; i < pgs_bitmap; i++){
		bitmap_set((unsigned long *)rt->bitmap, i + 1, 1);
	}

	/* pswap fast allocation */
	// mast_page->pswap_fast_pg = mast_page->bm_start_offset + (pgs_bitmap << PAGE_SHIFT);
	// bitmap_set((unsigned long *)rt->bitmap, pgs_bitmap + 1, 1);
	// rt->pswap_fast_frame = DAX_REL2ABS(mast_page->pswap_fast_pg);
	// memset((void*)rt->pswap_fast_frame, 0, PAGE_SIZE);

	/* alloc pgd */
	pgd_offset = alloc_dax_pg(rt);
	mast_page-> pgd_offset = pgd_offset;
	pgdp = DAX_REL2ABS(pgd_offset);
	rt->pgd = pgdp;
	memset(pgdp, 0, PAGE_SIZE);

	/* fill first pgd */
	pgdp[0] = alloc_dax_pg(rt);
	pgdp[1] = alloc_dax_pg(rt);
	memset(DAX_REL2ABS(pgdp[0]), 0, PAGE_SIZE);
	memset(DAX_REL2ABS(pgdp[1]), 0, PAGE_SIZE);
	wbinvd();
	rt->pswap_temp = kmalloc(2 * PAGE_SIZE, GFP_KERNEL);
	printk("Initialized Dax for %lu @ %#lx\n", dax_size, (unsigned long)mast_page);
}


#endif

static int check_vma(struct dev_dax *dev_dax, struct vm_area_struct *vma,
		const char *func)
{
	struct dax_region *dax_region = dev_dax->region;
	struct device *dev = &dev_dax->dev;
	unsigned long mask;

	if (!dax_alive(dev_dax->dax_dev))
		return -ENXIO;

	/* prevent private mappings from being established */
	if ((vma->vm_flags & VM_MAYSHARE) != VM_MAYSHARE) {
		dev_info_ratelimited(dev,
				"%s: %s: fail, attempted private mapping\n",
				current->comm, func);
		return -EINVAL;
	}

	mask = dax_region->align - 1;
	if (vma->vm_start & mask || vma->vm_end & mask) {
		dev_info_ratelimited(dev,
				"%s: %s: fail, unaligned vma (%#lx - %#lx, %#lx)\n",
				current->comm, func, vma->vm_start, vma->vm_end,
				mask);
		return -EINVAL;
	}

	if ((dax_region->pfn_flags & (PFN_DEV|PFN_MAP)) == PFN_DEV
			&& (vma->vm_flags & VM_DONTCOPY) == 0) {
		dev_info_ratelimited(dev,
				"%s: %s: fail, dax range requires MADV_DONTFORK\n",
				current->comm, func);
		return -EINVAL;
	}

	if (!vma_is_dax(vma)) {
		dev_info_ratelimited(dev,
				"%s: %s: fail, vma is not DAX capable\n",
				current->comm, func);
		return -EINVAL;
	}

	return 0;
}

/* see "strong" declaration in tools/testing/nvdimm/dax-dev.c */
__weak phys_addr_t dax_pgoff_to_phys(struct dev_dax *dev_dax, pgoff_t pgoff,
		unsigned long size)
{
	struct resource *res = &dev_dax->region->res;
	phys_addr_t phys;

	phys = pgoff * PAGE_SIZE + res->start;
	if (phys >= res->start && phys <= res->end) {
		if (phys + size - 1 <= res->end)
			return phys;
	}

	return -1;
}

static vm_fault_t __dev_dax_pte_fault(struct dev_dax *dev_dax,
				struct vm_fault *vmf, pfn_t *pfn)
{
	struct device *dev = &dev_dax->dev;
	struct dax_region *dax_region;
	phys_addr_t phys;
	unsigned int fault_size = PAGE_SIZE;

	if (check_vma(dev_dax, vmf->vma, __func__))
		return VM_FAULT_SIGBUS;

	dax_region = dev_dax->region;
	if (dax_region->align > PAGE_SIZE) {
		dev_dbg(dev, "alignment (%#x) > fault size (%#x)\n",
			dax_region->align, fault_size);
		return VM_FAULT_SIGBUS;
	}

	if (fault_size != dax_region->align)
		return VM_FAULT_SIGBUS;

	phys = dax_pgoff_to_phys(dev_dax, vmf->pgoff, PAGE_SIZE);
	if (phys == -1) {
		dev_dbg(dev, "pgoff_to_phys(%#lx) failed\n", vmf->pgoff);
		return VM_FAULT_SIGBUS;
	}

	*pfn = phys_to_pfn_t(phys, dax_region->pfn_flags);

	return vmf_insert_mixed(vmf->vma, vmf->address, *pfn);
}

static vm_fault_t __dev_dax_pmd_fault(struct dev_dax *dev_dax,
				struct vm_fault *vmf, pfn_t *pfn)
{
	unsigned long pmd_addr = vmf->address & PMD_MASK;
	struct device *dev = &dev_dax->dev;
	struct dax_region *dax_region;
	phys_addr_t phys;
	pgoff_t pgoff;
	unsigned int fault_size = PMD_SIZE;

	if (check_vma(dev_dax, vmf->vma, __func__))
		return VM_FAULT_SIGBUS;

	dax_region = dev_dax->region;
	if (dax_region->align > PMD_SIZE) {
		dev_dbg(dev, "alignment (%#x) > fault size (%#x)\n",
			dax_region->align, fault_size);
		return VM_FAULT_SIGBUS;
	}

	/* dax pmd mappings require pfn_t_devmap() */
	if ((dax_region->pfn_flags & (PFN_DEV|PFN_MAP)) != (PFN_DEV|PFN_MAP)) {
		dev_dbg(dev, "region lacks devmap flags\n");
		return VM_FAULT_SIGBUS;
	}

	if (fault_size < dax_region->align)
		return VM_FAULT_SIGBUS;
	else if (fault_size > dax_region->align)
		return VM_FAULT_FALLBACK;

	/* if we are outside of the VMA */
	if (pmd_addr < vmf->vma->vm_start ||
			(pmd_addr + PMD_SIZE) > vmf->vma->vm_end)
		return VM_FAULT_SIGBUS;

	pgoff = linear_page_index(vmf->vma, pmd_addr);
	phys = dax_pgoff_to_phys(dev_dax, pgoff, PMD_SIZE);
	if (phys == -1) {
		dev_dbg(dev, "pgoff_to_phys(%#lx) failed\n", pgoff);
		return VM_FAULT_SIGBUS;
	}

	*pfn = phys_to_pfn_t(phys, dax_region->pfn_flags);

	return vmf_insert_pfn_pmd(vmf, *pfn, vmf->flags & FAULT_FLAG_WRITE);
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
static vm_fault_t __dev_dax_pud_fault(struct dev_dax *dev_dax,
				struct vm_fault *vmf, pfn_t *pfn)
{
	unsigned long pud_addr = vmf->address & PUD_MASK;
	struct device *dev = &dev_dax->dev;
	struct dax_region *dax_region;
	phys_addr_t phys;
	pgoff_t pgoff;
	unsigned int fault_size = PUD_SIZE;


	if (check_vma(dev_dax, vmf->vma, __func__))
		return VM_FAULT_SIGBUS;

	dax_region = dev_dax->region;
	if (dax_region->align > PUD_SIZE) {
		dev_dbg(dev, "alignment (%#x) > fault size (%#x)\n",
			dax_region->align, fault_size);
		return VM_FAULT_SIGBUS;
	}

	/* dax pud mappings require pfn_t_devmap() */
	if ((dax_region->pfn_flags & (PFN_DEV|PFN_MAP)) != (PFN_DEV|PFN_MAP)) {
		dev_dbg(dev, "region lacks devmap flags\n");
		return VM_FAULT_SIGBUS;
	}

	if (fault_size < dax_region->align)
		return VM_FAULT_SIGBUS;
	else if (fault_size > dax_region->align)
		return VM_FAULT_FALLBACK;

	/* if we are outside of the VMA */
	if (pud_addr < vmf->vma->vm_start ||
			(pud_addr + PUD_SIZE) > vmf->vma->vm_end)
		return VM_FAULT_SIGBUS;

	pgoff = linear_page_index(vmf->vma, pud_addr);
	phys = dax_pgoff_to_phys(dev_dax, pgoff, PUD_SIZE);
	if (phys == -1) {
		dev_dbg(dev, "pgoff_to_phys(%#lx) failed\n", pgoff);
		return VM_FAULT_SIGBUS;
	}

	*pfn = phys_to_pfn_t(phys, dax_region->pfn_flags);

	return vmf_insert_pfn_pud(vmf, *pfn, vmf->flags & FAULT_FLAG_WRITE);
}
#else
static vm_fault_t __dev_dax_pud_fault(struct dev_dax *dev_dax,
				struct vm_fault *vmf, pfn_t *pfn)
{
	return VM_FAULT_FALLBACK;
}
#endif /* !CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */

static vm_fault_t dev_dax_huge_fault(struct vm_fault *vmf,
		enum page_entry_size pe_size)
{
#ifdef CTFS_PSWAP
	unsigned long addr = vmf->address;
	// unsigned long dax_start_va = vmf->vma->vm_start;
	unsigned long pmd_addr = addr & PMD_MASK;
	struct vm_area_struct *vma;
	vma = vmf->vma;
#if PSWAP_DEBUG > 1
		printk("DAX PAGE FAULT: PID: %d @%#lx \n",
			current->pid , addr);
#endif
	mutex_lock(&dax_lock);
	if(unlikely(rt->vaddr_base != vma->vm_start)){
		dax_master_page_t * master_page;
#ifdef PSWAP_DEBUG
		printk("DAX INITIAL PAGE FAULT: PID: %d @%#lx \n",
			current->pid , addr);
#endif
		master_page = get_master_page(vma);
		if(unlikely(!master_page)){
			printk("DAX FAULT: Error: User addr invalid, not in DAX\n");
			mutex_unlock(&dax_lock);
			goto out_error;
		}
		if(unlikely(MASTER_NOT_INIT(master_page))){
			printk("DAX FAULT: Error: User addr invalid, Dax not inited\n");
			mutex_unlock(&dax_lock);
			goto out_error;
		}
		get_dax_runtime(master_page);
		rt->vaddr_base = vma->vm_start;
	}

	install_pmd(vmf, pmd_addr);
#if PSWAP_DEBUG > 0
	// printk("PID: %d, DAX fault %d @%s: flag: %d\n\tpg_prot: %#lx, pfn flag: %#llx (%#lx - %#lx) @%#lx \n",
	// 		current->pid , pe_size, current->comm,
	// 		vmf->flags , vma->vm_page_prot.pgprot, dax_region->pfn_flags, 
	// 		vmf->vma->vm_start, vmf->vma->vm_end, addr);
#endif
#if PSWAP_DEBUG > 2
	printk("DAX fault: Master page finished initializing. Magic: %s; DAX_REL2ABS_offset: %lx\n", m_page_p->magic_word, DAX_REL2ABS_offset);
	printk("           rt->pud %#lx, *rt->pud: %#lx, pgs_bitmap: %ld, bits available: %ld\n",
	(unsigned long)rt->pud, *(rt->pud), ((rt->num_pages >> PAGE_SHIFT) >>3) + 1, rt->num_pages - bitmap_weight(rt->bitmap, rt->num_pages));
#endif

	mutex_unlock(&dax_lock);
	return VM_FAULT_NOPAGE;

out_error:
	printk("DAX fault error: \n");
	return VM_FAULT_SIGBUS;

#else


	struct file *filp = vmf->vma->vm_file;
	unsigned long fault_size;
	vm_fault_t rc = VM_FAULT_SIGBUS;
	int id;
	pfn_t pfn;
	struct dev_dax *dev_dax = filp->private_data;

#ifdef CTFS_PSWAP
	printk("DAX fault %s: %s (%#lx - %#lx) @%#lx size = %d\n", current->comm,
			(vmf->flags & FAULT_FLAG_WRITE) ? "write" : "read",
			vmf->vma->vm_start, vmf->vma->vm_end, vmf->address, pe_size);
	phys_addr_t dax_start_vaddr;
	dax_start_vaddr = dax_pgoff_to_phys(dev_dax, 0, PMD_SIZE) + __PAGE_OFFSET;
	char * dax_start_str = (char *)dax_start_vaddr;
	printk("Dax Start reads: %c%c%c%c\n",*dax_start_str, *(dax_start_str+1), 
	*(dax_start_str+2), *(dax_start_str+3));
#endif
	dev_dbg(&dev_dax->dev, "%s: %s (%#lx - %#lx) size = %d\n", current->comm,
			(vmf->flags & FAULT_FLAG_WRITE) ? "write" : "read",
			vmf->vma->vm_start, vmf->vma->vm_end, pe_size);

	id = dax_read_lock();
	switch (pe_size) {
	case PE_SIZE_PTE:
		fault_size = PAGE_SIZE;
		rc = __dev_dax_pte_fault(dev_dax, vmf, &pfn);
		break;
	case PE_SIZE_PMD:
		fault_size = PMD_SIZE;
		rc = __dev_dax_pmd_fault(dev_dax, vmf, &pfn);
		break;
	case PE_SIZE_PUD:
		fault_size = PUD_SIZE;
		rc = __dev_dax_pud_fault(dev_dax, vmf, &pfn);
		break;
	default:
		rc = VM_FAULT_SIGBUS;
	}

	if (rc == VM_FAULT_NOPAGE) {
		unsigned long i;
		pgoff_t pgoff;

		/*
		 * In the device-dax case the only possibility for a
		 * VM_FAULT_NOPAGE result is when device-dax capacity is
		 * mapped. No need to consider the zero page, or racing
		 * conflicting mappings.
		 */
#ifdef CTFS_PSWAP
		printk("DAX Fault VM_FAULT_NOPAGE, fault size: %ld\n",fault_size);
#endif
		pgoff = linear_page_index(vmf->vma, vmf->address
				& ~(fault_size - 1));
		for (i = 0; i < fault_size / PAGE_SIZE; i++) {
			struct page *page;

			page = pfn_to_page(pfn_t_to_pfn(pfn) + i);
			if (page->mapping)
				continue;
			page->mapping = filp->f_mapping;
			page->index = pgoff + i;
		}
	}
	dax_read_unlock(id);

	return rc;

#endif
}

static int dax_close_callback(pte_t *pte, unsigned long addr, void *data){
	(void)pte;
	(void)data;
	arch_wb_cache_pmem((void*)addr, PAGE_SIZE);
	return 0;
}

static void dev_dax_close(struct vm_area_struct * vma){
	wbinvd();
	// apply_to_existing_page_range(vma->vm_mm, vma->vm_start, 
	// vma->vm_end - vma->vm_start, dax_close_callback , NULL);
}

static vm_fault_t dev_dax_fault(struct vm_fault *vmf)
{
	
	return dev_dax_huge_fault(vmf, PE_SIZE_PTE);
}

static int dev_dax_split(struct vm_area_struct *vma, unsigned long addr)
{
	struct file *filp = vma->vm_file;
	struct dev_dax *dev_dax = filp->private_data;
	struct dax_region *dax_region = dev_dax->region;

	if (!IS_ALIGNED(addr, dax_region->align))
		return -EINVAL;
	return 0;
}

static unsigned long dev_dax_pagesize(struct vm_area_struct *vma)
{
	struct file *filp = vma->vm_file;
	struct dev_dax *dev_dax = filp->private_data;
	struct dax_region *dax_region = dev_dax->region;

	return dax_region->align;
}

static const struct vm_operations_struct dax_vm_ops = {
	.fault = dev_dax_fault,
	.huge_fault = dev_dax_huge_fault,
    .pfn_mkwrite = dev_dax_fault,
	.close = dev_dax_close,
	.split = dev_dax_split,
	.pagesize = dev_dax_pagesize,
};

static int dax_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct dev_dax *dev_dax = filp->private_data;
	int rc, id;

	dev_dbg(&dev_dax->dev, "trace\n");

	/*
	 * We lock to check dax_dev liveness and will re-check at
	 * fault time.
	 */
	id = dax_read_lock();
	rc = check_vma(dev_dax, vma, __func__);
	dax_read_unlock(id);
	if (rc)
		return rc;

	vma->vm_ops = &dax_vm_ops;
	// vma->vm_flags |= VM_HUGEPAGE;
	vma->vm_flags |= VM_PFNMAP;
	return 0;
}

/* return an unmapped area aligned to the dax region specified alignment */
static unsigned long dax_get_unmapped_area(struct file *filp,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long flags)
{
	unsigned long off, off_end, off_align, len_align, addr_align, align;
	struct dev_dax *dev_dax = filp ? filp->private_data : NULL;
	struct dax_region *dax_region;

	if (!dev_dax || addr)
		goto out;

	dax_region = dev_dax->region;
	align = dax_region->align;
	off = pgoff << PAGE_SHIFT;
	off_end = off + len;
	off_align = round_up(off, align);

	if ((off_end <= off_align) || ((off_end - off_align) < align))
		goto out;

	len_align = len + align;
	if ((off + len_align) < off)
		goto out;

	addr_align = current->mm->get_unmapped_area(filp, addr, len_align,
			pgoff, flags);
	if (!IS_ERR_VALUE(addr_align)) {
		addr_align += (off - addr_align) & (align - 1);
		return addr_align;
	}
 out:
	return current->mm->get_unmapped_area(filp, addr, len, pgoff, flags);
}

static void dax_invalidate_page (struct page* page, unsigned int offset, unsigned int size){
	(void)offset;
	(void)size;
	page->mapping = NULL;
	atomic_set(&page->_refcount, 0);
}


static const struct address_space_operations dev_dax_aops = {
	.set_page_dirty		= noop_set_page_dirty,
	.invalidatepage		= dax_invalidate_page,
};

static int dax_open(struct inode *inode, struct file *filp)
{
	struct dax_device *dax_dev = inode_dax(inode);
	struct inode *__dax_inode = dax_inode(dax_dev);
	struct dev_dax *dev_dax = dax_get_private(dax_dev);

	dev_dbg(&dev_dax->dev, "trace\n");
	inode->i_mapping = __dax_inode->i_mapping;
	inode->i_mapping->host = __dax_inode;
	inode->i_mapping->a_ops = &dev_dax_aops;
	filp->f_mapping = inode->i_mapping;
	filp->f_wb_err = filemap_sample_wb_err(filp->f_mapping);
	filp->private_data = dev_dax;
	inode->i_flags = S_DAX;

#ifdef CTFS_PSWAP
	mutex_init(&dax_lock);
	if(pswap_fast_first == NULL){
		pswap_fast_first = kmalloc(DAX_PSWAP_FAST_PGS * sizeof(unsigned long), GFP_KERNEL);
		pswap_fast_second = kmalloc(DAX_PSWAP_FAST_PGS * sizeof(unsigned long), GFP_KERNEL);
		pswap_fast_ptep1 = kmalloc(DAX_PSWAP_FAST_PGS * sizeof(unsigned long), GFP_KERNEL);
		pswap_fast_ptep2 = kmalloc(DAX_PSWAP_FAST_PGS * sizeof(unsigned long), GFP_KERNEL);
		pswap_fast_first_p = kmalloc(DAX_PSWAP_FAST_PGS * sizeof(unsigned long), GFP_KERNEL);
		pswap_fast_second_p = kmalloc(DAX_PSWAP_FAST_PGS * sizeof(unsigned long), GFP_KERNEL);
	}
#endif

	return 0;
}

static int dax_release(struct inode *inode, struct file *filp)
{
	struct dev_dax *dev_dax = filp->private_data;

	dev_dbg(&dev_dax->dev, "trace\n");
	return 0;
}

static const struct file_operations dax_fops = {
	.llseek = noop_llseek,
	.owner = THIS_MODULE,
	.open = dax_open,
	.release = dax_release,
	.get_unmapped_area = dax_get_unmapped_area,
	.mmap = dax_mmap,
	.mmap_supported_flags = MAP_SYNC,
#ifdef CTFS_PSWAP
	.unlocked_ioctl = dax_ioctl
#endif
};

static void dev_dax_cdev_del(void *cdev)
{
	cdev_del(cdev);
}

static void dev_dax_kill(void *dev_dax)
{
	kill_dev_dax(dev_dax);
}

int dev_dax_probe(struct device *dev)
{
	struct dev_dax *dev_dax = to_dev_dax(dev);
	struct dax_device *dax_dev = dev_dax->dax_dev;
	struct resource *res = &dev_dax->region->res;
	struct inode *inode;
	struct cdev *cdev;
	void *addr;
	int rc;

	/* 1:1 map region resource range to device-dax instance range */
	if (!devm_request_mem_region(dev, res->start, resource_size(res),
				dev_name(dev))) {
		dev_warn(dev, "could not reserve region %pR\n", res);
		return -EBUSY;
	}

	dev_dax->pgmap.type = MEMORY_DEVICE_DEVDAX;
	addr = devm_memremap_pages(dev, &dev_dax->pgmap);
	if (IS_ERR(addr))
		return PTR_ERR(addr);

	inode = dax_inode(dax_dev);
	cdev = inode->i_cdev;
	cdev_init(cdev, &dax_fops);
	if (dev->class) {
		/* for the CONFIG_DEV_DAX_PMEM_COMPAT case */
		cdev->owner = dev->parent->driver->owner;
	} else
		cdev->owner = dev->driver->owner;
	cdev_set_parent(cdev, &dev->kobj);
	rc = cdev_add(cdev, dev->devt, 1);
	if (rc)
		return rc;

	rc = devm_add_action_or_reset(dev, dev_dax_cdev_del, cdev);
	if (rc)
		return rc;

	run_dax(dax_dev);
	return devm_add_action_or_reset(dev, dev_dax_kill, dev_dax);
}
EXPORT_SYMBOL_GPL(dev_dax_probe);

static int dev_dax_remove(struct device *dev)
{
	/* all probe actions are unwound by devm */
	return 0;
}

static struct dax_device_driver device_dax_driver = {
	.drv = {
		.probe = dev_dax_probe,
		.remove = dev_dax_remove,
	},
	.match_always = 1,
};

static int __init dax_init(void)
{
	return dax_driver_register(&device_dax_driver);
}

static void __exit dax_exit(void)
{
	dax_driver_unregister(&device_dax_driver);
}

#ifdef CTFS_PSWAP

/* implementation of pswap
 * Swapping the page mapping of two given
 * address and their following consecutive npgs pages
 * Consistency guarantee: only the first group of pages
 * are guaranteed to be consistent (all or nothing done)
 * The second group may be flushed or leaked in the case
 * of a critical failure. 
 * @param[in] ufirst the starting address of the first group of pages
 * @param[in] usecond the starting address of the second group of pages
 * @param[in] npgs number of consectuive pages
 * @return error number
 */
static int dax_pswap(unsigned long ufirst, unsigned long usecond, unsigned long npgs){
	/* variables */
	struct mm_struct *mm;
	dax_master_page_t * master_page;
	
	char aligned;	// indicate if the pair is pmd alligned
	unsigned long first, second;
	unsigned long cur1, cur2;		// current processing page. Dealing with second first
	unsigned long rem;		// number of pages remaining
	unsigned long i, temp_i;
	unsigned long ret;
	struct vm_area_struct *vma;
	unsigned flush_tlb = 0;
	unsigned shoot_pmd = 0;
	/* assingment */

	mm = current->mm;
	
	vma = find_vma(current->mm, ufirst);
#if PSWAP_DEBUG > 1
		printk("PSWAP: PID: %d  %#lx <-> %#lx  npgs: %lu\n",
			current->pid , ufirst, usecond, npgs);
#endif
	mutex_lock(&dax_lock);
	if(rt->vaddr_base != vma->vm_start){
	
		if(!vma){
			mutex_unlock(&dax_lock);
			return -1;
		}
		master_page = get_master_page(vma);
		if(unlikely(!master_page)){
			printk("DAX pswap: Error: User addr invalid, not in DAX\n");
			goto err_out;
		}
		if(unlikely(MASTER_NOT_INIT(master_page))){
			printk("DAX pswap: Error: User addr invalid, Dax not inited\n");
			goto err_out;
		}
		get_dax_runtime(master_page);
		rt->vaddr_base = vma->vm_start;
	}
	else{
		master_page = rt->master;
	}

	if((ufirst & (PMD_SIZE - 1)) == (usecond & (PMD_SIZE - 1)) ){
		aligned = 1;
	}
	else{
		aligned = 0;
	}

	if(unlikely(!aligned && npgs > 64)){
		printk("DAX pswap: Error: unsupported param. ufrist: %#lx, usecond: %#lx, npgs: %ld\n", 
		ufirst, usecond, npgs);
		goto err_out;
	}
	
#if PSWAP_DEBUG > 1
	printk("PID: %d,DAX pswap begins: %ld pages \n",current->pid , npgs);
#endif
	/* Now let's rock! */
#if PSWAP_DEBUG > 1
	printk("DAX pswap: start step 1\n");
#endif
	first = ufirst - rt->vaddr_base;
	second = usecond - rt->vaddr_base;
	
#if PSWAP_DEBUG > 1
	if(aligned){
		relptr_t * pmdp;
		unsigned long pmd_off, pte_off;
		pmd_off =  (first & PMD_MASK) >> PMD_SHIFT;
		pmd_off &= 0x01ff;
		pte_off = (first & PAGE_MASK) >> PAGE_SHIFT;
		pte_off &= 0x01ff;
		find_dax_ptep(rt, first, &pmdp, NULL);
		printk("\t\tbefore swap: first: %#lx, pmd_off: %#lx, pte_off: %#lx \n\t\t\tpmdp: %#lx, *pmdp: %#lx, pte: %#lx",
		first, pmd_off, pte_off,
		(unsigned long)pmdp, *pmdp, 
		*pmdp ? ((relptr_t*) DAX_REL2ABS(*pmdp))[pte_off]: 0);

		pmd_off =  (second & PMD_MASK) >> PMD_SHIFT;
		pmd_off &= 0x01ff;
		pte_off = (second & PAGE_MASK) >> PAGE_SHIFT;
		pte_off &= 0x01ff;
		find_dax_ptep(rt, second, &pmdp, NULL);
		printk("\t\tbefore swap: second: %#lx, pmd_off: %#lx, pte_off: %#lx \n\t\t\tpmdp: %#lx, *pmdp: %#lx, pte: %#lx",
		second, pmd_off, pte_off,
		(unsigned long)pmdp, *pmdp, 
		*pmdp ? ((relptr_t*) DAX_REL2ABS(*pmdp))[pte_off]: 0);;
	}
	
#endif
	if(aligned){
		// it's pmd and above pswap
		unsigned long cur1_vpn, cur2_vpn;
		relptr_t * beg_pgr_pmdp[2] = {NULL, NULL};
		relptr_t * beg_pmdr_pmdp[2] = {NULL, NULL};
		relptr_t * end_pmdr_pmdp[2] = {NULL, NULL};
		relptr_t * end_pgr_pmdp[2] = {NULL, NULL};


		/* step 0 start. Copy first to temp.
		 * if error, nothing happens 
		 */
		cur1 = first;
		cur2 = second;
		rem = npgs;
		i = 0;
		temp_i = 0;
		cur1_vpn = cur1 >> PAGE_SHIFT;
#if PSWAP_DEBUG > 1
		printk("\tDAX aligned pswap start step 0. first: %#lx, second: %#lx, npgs: %lu\n",
		(unsigned long)first, (unsigned long)second, rem);
#endif
		// page residue at beginning till the pmd boundary
		if(cur1 & (PMD_SIZE - 1)){
			unsigned long next_pmd_vpn = ((cur1 & PMD_MASK) + PMD_SIZE) >> PAGE_SHIFT;
			relptr_t * ptep;
			relptr_t pte_index;
#if PSWAP_DEBUG > 1
		printk("\t\t DAX aligned pswap pte residue 1\n");
#endif
			ret = find_dax_ptep(rt, cur1, &beg_pgr_pmdp[0], NULL);
#ifdef PSWAP_HUGE
			if(ret){
				// it's huge page
				// need to down grade
				dax_downgrade_huge(rt, beg_pgr_pmdp[0]);
				shoot_pmd = 1;
			}
#endif
			ret = find_dax_ptep(rt, cur2, &beg_pgr_pmdp[1], NULL);
#ifdef PSWAP_HUGE
			if(ret){
				// it's huge page
				// need to down grade
				dax_downgrade_huge(rt, beg_pgr_pmdp[1]);
				shoot_pmd = 1;
			}
#endif
			ptep = DAX_REL2ABS(*beg_pgr_pmdp[0]);
			pte_index = (cur1 & (PMD_SIZE - 1)) >> PAGE_SHIFT;
			// Now copy to the temp
			for(i = 0; i + cur1_vpn < next_pmd_vpn; i++){
				rt->pswap_temp[temp_i] = ptep[pte_index + i];
				rem --;
				temp_i++;
				cur1 += PAGE_SIZE;
				cur2 += PAGE_SIZE;
				if(rem == 0){
					break;
				}
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
		}
		// pmd residue at beginning till the pud boundary
		if(rem >= 512 && (cur1 & (PUD_SIZE - 1))){
			unsigned long next_pud_vpn = ((cur1 & PUD_MASK) + PUD_SIZE) >> PAGE_SHIFT;
			find_dax_ptep(rt, cur1, &beg_pmdr_pmdp[0], NULL);
			find_dax_ptep(rt, cur2, &beg_pmdr_pmdp[1], NULL);
#if PSWAP_DEBUG > 1
		printk("\tDAX aligned pswap pmd residue 1: %#lx, rem: %ld, tempi: %lu\n",
		(unsigned long)cur1, rem, temp_i);
#endif
			// Now copy to the temp
			for(i = 0; (i << 9) + cur1_vpn < next_pud_vpn; i ++){
				rt->pswap_temp[temp_i] = beg_pmdr_pmdp[0][i];
				rem -= 512;
				temp_i++;
				cur1 += PMD_SIZE;
				cur2 += PMD_SIZE;
				if(rem < 512){
					break;
				}
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
		}
		// puds
		if(rem >= 512 * 512){
			// now cur should be pud alligned
			while(rem >= 512 * 512){
				relptr_t *pudp;
#if PSWAP_DEBUG > 1
				printk("\t\tPSWAP0: swaping pud: cur1: %#lx, cur2: %#lx, rem: %lu, tempi: %lu\n", 
				cur1, cur2, rem, temp_i);
#endif
				find_dax_ptep(rt, cur1, NULL, &pudp);
				rt->pswap_temp[temp_i] = *pudp;
				rem -= 512 * 512;
				temp_i++;
				cur1 += PUD_SIZE;
				cur2 += PUD_SIZE;
#if PSWAP_DEBUG > 1
			printk("\t\t\t pudp0: %#lx, rem: %lu, tempi: %lu\n", 
			(unsigned long)pudp, rem, temp_i);
#endif
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
		}
		// pmd residue at the end
		if(rem >= 512){
#if PSWAP_DEBUG > 1
		printk("\tDAX aligned pswap pmd residue 2: %#lx, rem: %ld, tempi: %lu\n",
		(unsigned long)cur1, rem, temp_i);
#endif
			find_dax_ptep(rt, cur1, &end_pmdr_pmdp[0], NULL);
			find_dax_ptep(rt, cur2, &end_pmdr_pmdp[1], NULL);
			i = 0;
			while(rem >= 512){
				rt->pswap_temp[temp_i] = end_pmdr_pmdp[0][i];
				rem -= 512;
				temp_i++;
				cur1 += PMD_SIZE;
				cur2 += PMD_SIZE;
				i++;
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
		}
		// page residue at the end
		if(rem){
			relptr_t * ptep;
			ret = find_dax_ptep(rt, cur1, &end_pgr_pmdp[0], NULL);
#if PSWAP_DEBUG > 1
		printk("\tDAX aligned pswap pte residue 2: %#lx, rem: %ld, tempi: %lu\n",
		(unsigned long)cur1, rem, temp_i);
#endif
#ifdef PSWAP_HUGE
			if(ret){
				// it's huge page
				// need to down grade

				dax_downgrade_huge(rt, end_pgr_pmdp[0]);
				shoot_pmd = 1;
			}
#endif
			ret = find_dax_ptep(rt, cur2, &end_pgr_pmdp[1], NULL);
#ifdef PSWAP_HUGE
			if(ret){
				// it's huge page
				// need to down grade

				dax_downgrade_huge(rt, end_pgr_pmdp[1]);
				shoot_pmd = 1;
			}
#endif
			ptep = DAX_REL2ABS(*end_pgr_pmdp[0]);
			i = 0;
			while(rem > 0){
				rt->pswap_temp[temp_i] = ptep[i];
				rem -= 1;
				temp_i++;
				cur1 += PAGE_SIZE;
				cur2 += PAGE_SIZE;
				i++;
			}
		}

		/* Starting step 1
		 * Copy second to first
		 * From now on, the recovery strategy will be redo
		 * if error, the original first will be erased
		 * page leak occurs
		 * 
		 */
		master_page->pswap_state = DAX_PSWAP_STEP1;
		master_page->pswap_npgs = npgs;
		master_page->pswap_frame.first = ufirst;
		master_page->pswap_frame.second = usecond;
		arch_wb_cache_pmem(&master_page->pswap_state, 64);
#if PSWAP_DEBUG > 1
		printk("\tDAX pswap start step 1. \n");
#endif
		// copy second -> first
		cur1 = first;
		cur2 = second;
		rem = npgs;
		i = 0;
		temp_i = 0;
		cur1_vpn = cur1 >> PAGE_SHIFT;
		cur2_vpn = cur2 >> PAGE_SHIFT;
		// page residue at beginning till the pmd boundary
		if(cur1 & (PMD_SIZE - 1)){
			unsigned long next_pmd_vpn = ((cur1 & PMD_MASK) + PMD_SIZE) >> PAGE_SHIFT;
			relptr_t * ptep0, * ptep1;
			relptr_t pte_index;
#if PSWAP_DEBUG > 1
			printk("\t\t  PSWAP:1 page residue: cur1: %#lx, rem: %lu, tempi: %lu\n",
			cur1, rem, temp_i);
#endif
			ptep1 = DAX_REL2ABS(*beg_pgr_pmdp[1]);
			ptep0 = DAX_REL2ABS(*beg_pgr_pmdp[0]);
			pte_index = (cur1 & (PMD_SIZE - 1)) >> PAGE_SHIFT;
			// Now copy from second to first
			for(i = 0; i + cur1_vpn < next_pmd_vpn; i++){
				ptep0[pte_index + i] = ptep1[pte_index + i];
				rem --;
				temp_i++;
				cur2 += PAGE_SIZE;
				cur1 += PAGE_SIZE;
				if(rem == 0){
					break;
				}
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
			cur2_vpn = cur2 >> PAGE_SHIFT;
			arch_wb_cache_pmem(ptep0, PAGE_SIZE);
		}
		// pmd residue at beginning till the pud boundary
		if(rem >= 512 && (cur1 & (PUD_SIZE - 1))){
			unsigned long next_pud_vpn = ((cur1 & PUD_MASK) + PUD_SIZE) >> PAGE_SHIFT;
			// Now copy from second to first
#if PSWAP_DEBUG > 1
		printk("\tDAX aligned pswap pmd residue 1: %#lx, rem: %ld, tempi: %lu\n",
		(unsigned long)cur1, rem, temp_i);
#endif
			for(i = 0; (i << 9) + cur1_vpn < next_pud_vpn; i ++){
				beg_pmdr_pmdp[0][i] = beg_pmdr_pmdp[1][i];
				rem -= 512;
				temp_i++;
				cur2 += PMD_SIZE;
				cur1 += PMD_SIZE;
				if(rem < 512){
					break;
				}
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
			cur2_vpn = cur2 >> PAGE_SHIFT;
			arch_wb_cache_pmem(beg_pmdr_pmdp[0], i << 3);
		}
		// puds
		if(rem >= 512 * 512){
			// now cur should be pud alligned
			while(rem >= 512 * 512){
				relptr_t *pudp0, *pudp1;
#if PSWAP_DEBUG > 1
				printk("\t\tPSWAP1: swaping pud: cur1: %#lx, cur1: %#lx, rem: %lu, tempi: %lu\n", 
				cur1, cur1, rem, temp_i);
#endif
				find_dax_ptep(rt, cur1, NULL, &pudp0);
				find_dax_ptep(rt, cur2, NULL, &pudp1);
				*pudp0 = *pudp1;
				rem -= 512 * 512;
				temp_i++;
				cur2 += PUD_SIZE;
				cur1 += PUD_SIZE;
#if PSWAP_DEBUG > 1
			printk("\t\t\t pudp0: %#lx, pudp1: %#lx rem: %lu, tempi: %lu\n", 
			(unsigned long)pudp0, (unsigned long)pudp1, rem, temp_i);
#endif
				arch_wb_cache_pmem(pudp0, 8);
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
			cur2_vpn = cur2 >> PAGE_SHIFT;
		}
		// pmd residue at the end

		if(rem >= 512){
			i = 0;
			while(rem >= 512){
				end_pmdr_pmdp[0][i] = end_pmdr_pmdp[1][i];
				rem -= 512;
				temp_i++;
				cur1 += PMD_SIZE;
				cur2 += PMD_SIZE;
				i++;
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
			cur2_vpn = cur2 >> PAGE_SHIFT;
			arch_wb_cache_pmem(end_pmdr_pmdp[0], i << 3);
		}
		// page residue at the end
		if(rem){
			relptr_t * ptep0, *ptep1;
			ptep0 = DAX_REL2ABS(*end_pgr_pmdp[0]);
			ptep1 = DAX_REL2ABS(*end_pgr_pmdp[1]);
			i = 0;
			while(rem > 0){
				ptep0[i] = ptep1[i];
				rem -= 1;
				temp_i++;
				cur1 += PAGE_SIZE;
				cur2 += PAGE_SIZE;
				i++;
			}
			arch_wb_cache_pmem(ptep0, PAGE_SIZE);
		}

		/* starting step 2 
		 * Copy the temp to second
		 * if error, the original first will be erased
		 * part of the erased pages will be leacked. 
		 */
#if PSWAP_DEBUG > 1
		printk("\tDAX pswap start step 2. \n");
#endif
		master_page->pswap_state = DAX_PSWAP_STEP2;
		arch_wb_cache_pmem(&master_page->pswap_state, 64);

		cur1 = first;
		cur2 = second;
		rem = npgs;
		i = 0;
		temp_i = 0;
		cur1_vpn = cur1 >> PAGE_SHIFT;
		cur2_vpn = cur2 >> PAGE_SHIFT;
		// page residue at beginning till the pmd boundary
		if(cur1 & (PMD_SIZE - 1)){
			unsigned long next_pmd_vpn = ((cur1 & PMD_MASK) + PMD_SIZE) >> PAGE_SHIFT;
			relptr_t * ptep1;
			relptr_t pte_index;
			ptep1 = DAX_REL2ABS(*beg_pgr_pmdp[1]);
			pte_index = (cur1 & (PMD_SIZE - 1)) >> PAGE_SHIFT;
			// Now copy from temp to second
			for(i = 0; i + cur1_vpn < next_pmd_vpn; i++){
				ptep1[pte_index + i] = rt->pswap_temp[temp_i];
				rem --;
				temp_i++;
				cur2 += PAGE_SIZE;
				cur1 += PAGE_SIZE;
				if(rem == 0){
					break;
				}
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
			cur2_vpn = cur2 >> PAGE_SHIFT;
			arch_wb_cache_pmem(ptep1, PAGE_SIZE);
		}
		// pmd residue at beginning till the pud boundary
		if(rem >= 512 && (cur1 & (PUD_SIZE - 1))){
			unsigned long next_pud_vpn = ((cur1 & PUD_MASK) + PUD_SIZE) >> PAGE_SHIFT;
			// Now copy from temp to second
			for(i = 0; (i << 9) + cur1_vpn < next_pud_vpn; i ++){
				beg_pmdr_pmdp[1][i] = rt->pswap_temp[temp_i];
				rem -= 512;
				temp_i++;
				cur2 += PMD_SIZE;
				cur1 += PMD_SIZE;
				if(rem < 512){
					break;
				}
			}
			cur2_vpn = cur2 >> PAGE_SHIFT;
			cur1_vpn = cur1 >> PAGE_SHIFT;
			arch_wb_cache_pmem(beg_pmdr_pmdp[1], i << 3);
		}
		// puds
		if(rem >= 512 * 512){
			// now cur should be pud alligned
			while(rem >= 512 * 512){
				relptr_t *pudp1;
#if PSWAP_DEBUG > 1
			printk("\t\tPSWAP2: swaping pud: cur1: %#lx, cur2: %#lx, rem: %lu, tempi: %lu\n", 
			cur1, cur2, rem, temp_i);
#endif
				find_dax_ptep(rt, cur2, NULL, &pudp1);
				*pudp1 = rt->pswap_temp[temp_i];
				rem -= 512 * 512;
				temp_i++;
				cur2 += PUD_SIZE;
				cur1 += PUD_SIZE;
#if PSWAP_DEBUG > 1
			printk("\t\t\t pudp1: %#lx, rem: %lu, tempi: %lu\n", 
			(unsigned long)pudp1, rem, temp_i);
#endif
				arch_wb_cache_pmem(pudp1, 8);
			}
			cur2_vpn = cur2 >> PAGE_SHIFT;
			cur1_vpn = cur1 >> PAGE_SHIFT;
		}
		// pmd residue at the end
		if(rem >= 512){
			i = 0;
			while(rem >= 512){
				end_pmdr_pmdp[1][i] = rt->pswap_temp[temp_i];
				rem -= 512;
				temp_i++;
				cur2 += PMD_SIZE;
				i++;
			}
			cur2_vpn = cur2 >> PAGE_SHIFT;
			arch_wb_cache_pmem(end_pmdr_pmdp[1], i << 3);
		}
		// page residue at the end
		if(rem){
			relptr_t *ptep1;
			ptep1 = DAX_REL2ABS(*end_pgr_pmdp[1]);
			i = 0;
			while(rem > 0){
				ptep1[i] = rt->pswap_temp[temp_i];
				rem -= 1;
				temp_i++;
				cur2 += PAGE_SIZE;
				i++;
			}
			arch_wb_cache_pmem(ptep1, PAGE_SIZE);
		}

		/* finished pswap */
		master_page->pswap_state = DAX_PSWAP_NORMAL;

		// real page table
		cur1 = ufirst;
		cur2 = usecond;
		rem = npgs;
		i = 0;
		cur1_vpn = cur1 >> PAGE_SHIFT;
		cur2_vpn = cur2 >> PAGE_SHIFT;
#if PSWAP_DEBUG > 1
		printk("\tDAX pswap starting real page table. %lu\n"
		, rem);
#endif
		// page residue at beginning till the pmd boundary
		if(cur1 & (PMD_SIZE - 1)){
			unsigned long next_pmd_vpn = ((cur1 & PMD_MASK) + PMD_SIZE) >> PAGE_SHIFT;
			pte_t * ptep1 = NULL, *ptep2 = NULL, pte;
			unsigned allocated = 0;

			allocated += dax_get_ptep_noalloc(mm, cur1, &ptep1);
			allocated += dax_get_ptep_noalloc(mm, cur2, &ptep2);

			if(shoot_pmd == 0 && allocated == 2){
				// swap
				for(i = 0; i + cur1_vpn < next_pmd_vpn; i++){
#if PSWAP_DEBUG > 1
		printk("\t\t Swap pte 1: ptep1: %#lx <-> ptep2: %#lx\n"
		, 
		ptep1[i],
		ptep2[i]);
#endif
					pte = ptep2[i];
					set_pte(ptep2 + i, ptep1[i]);
					set_pte(ptep1 + i, pte);
					rem --;
					cur1 += PAGE_SIZE;
					cur2 += PAGE_SIZE;
					pv_ops.mmu.flush_tlb_one_user(cur1);
					pv_ops.mmu.flush_tlb_one_user(cur2);
					if(rem == 0){
						break;
					}
				}
				cur1_vpn = cur1 >> PAGE_SHIFT;
				cur2_vpn = cur2 >> PAGE_SHIFT;
			}
			else{
				if(allocated == 1 || shoot_pmd == 1){
#if PSWAP_DEBUG > 1
		printk("\t\t Shoot pmd 1\n"
		);
#endif
					// shoot down
					flush_tlb = 1;
					dax_free_pmd(mm, cur1);
					dax_free_pmd(mm, cur2);
				}
				if((next_pmd_vpn - cur1_vpn) < rem){
					rem -= (next_pmd_vpn - cur1_vpn);
				}
				else{
					rem = 0;
				}
				cur1 += (next_pmd_vpn - cur1_vpn) << PAGE_SHIFT;
				cur2 += (next_pmd_vpn - cur1_vpn) << PAGE_SHIFT;
				cur1_vpn = cur1 >> PAGE_SHIFT;
				cur2_vpn = cur2 >> PAGE_SHIFT;

			}
				
		}

		// pmd residue at beginning till the pud boundary
		if(rem >= 512 && (cur1 & (PUD_SIZE - 1))){
			unsigned long next_pud_vpn = ((cur1 & PUD_MASK) + PUD_SIZE) >> PAGE_SHIFT;
			pmd_t *pmdp1 = NULL, *pmdp2 = NULL, pmd;

			dax_get_pmdp(mm, cur1, &pmdp1);
			dax_get_pmdp(mm, cur2, &pmdp2);
			flush_tlb = 1;
#if PSWAP_DEBUG > 1
		printk("\t\t Swap pmd 1: pmdp1: %#lx <-> pmdp2: %#lx\n"
		, 
		pmdp1,
		pmdp2);
#endif
			// swap
			for(i = 0; (i << 9) + cur1_vpn < next_pud_vpn; i++){
#if PSWAP_DEBUG > 1
		printk("\t\t\t Swap pmd 1: pmd1: %#lx <-> pmd2: %#lx\n"
		, 
		pmdp1[i],
		pmdp2[i]);
#endif
				pmd = pmdp2[i];
				set_pmd(pmdp2 + i, pmdp1[i]);
				set_pmd(pmdp1 + i, pmd);
				rem -= 512;
				cur1 += PMD_SIZE;
				cur2 += PMD_SIZE;
				if(rem < 512){
					break;
				}
			}
			cur1_vpn = cur1 >> PAGE_SHIFT;
			cur2_vpn = cur2 >> PAGE_SHIFT;
		}
		// puds
		if(rem >= 512 * 512){
			pud_t *pudp1 = NULL, *pudp2 = NULL, pud;
			flush_tlb = 1;
			while(rem >= 512 * 512){
				dax_get_pudp(mm, cur1, &pudp1);
				dax_get_pudp(mm, cur2, &pudp2);
#if PSWAP_DEBUG > 1
		printk("\t\t Swap pud: pudp1: %#lx <-> pudp2: %#lx\n"
		, 
		*pudp1,
		*pudp2);
#endif
				pud = *pudp2;
				set_pud(pudp2, *pudp1);
				set_pud(pudp1, pud);
				rem -= 512 * 512;
				cur1 += PUD_SIZE;
				cur2 += PUD_SIZE;
			}
		}
		// pmd residue at the end
		if(rem >= 512){
			pmd_t *pmdp1 = NULL, *pmdp2 = NULL, pmd;
			dax_get_pmdp(mm, cur1, &pmdp1);
			dax_get_pmdp(mm, cur2, &pmdp2);
			flush_tlb = 1;
			i = 0;
			while(rem >= 512){
#if PSWAP_DEBUG > 1
		printk("\t\t Swap pmd 2: pmdp1: %#lx <-> pmdp2: %#lx\n"
		, 
		pmdp1[i],
		pmdp2[i]);
#endif
				pmd = pmdp2[i];
				set_pmd(pmdp2 + i, pmdp1[i]);
				set_pmd(pmdp1 + i, pmd);
				rem -= 512;
				cur1 += PMD_SIZE;
				cur2 += PMD_SIZE;

			}
		}
		// page residue at the end
		if(rem){
			pte_t * ptep1 = NULL, *ptep2 = NULL, pte;
			unsigned allocated = 0;

			allocated += dax_get_ptep_noalloc(mm, cur1, &ptep1);
			allocated += dax_get_ptep_noalloc(mm, cur2, &ptep2);

			if(shoot_pmd == 0 && allocated == 2){
				// swap
				i = 0;
				while(rem > 0){
#if PSWAP_DEBUG > 1
		printk("\t\t Swap pte 2: ptep1: %#lx <-> ptep2: %#lx\n"
		, 
		ptep1[i],
		ptep2[i]);
#endif
					pte = ptep2[i];
					set_pte(ptep2 + i, ptep1[i]);
					set_pte(ptep1 + i, pte);
					pv_ops.mmu.flush_tlb_one_user(cur1);
					pv_ops.mmu.flush_tlb_one_user(cur2);
					rem --;
					cur1 += PAGE_SIZE;
					cur2 += PAGE_SIZE;
					i ++;
				}
			}
			else{
				
				if(allocated == 1 || shoot_pmd == 1){
					// shoot down
					flush_tlb = 1;
					dax_free_pmd(mm, cur1);
					dax_free_pmd(mm, cur2);
				}
			}
		}
#if PSWAP_DEBUG > 1
		printk("\tDAX pswap All done \n");
#endif
	}
	else{
		/* case of unalligned pswap in
		 * the range of single pmd
		 */
		relptr_t * pmdp[2] = {NULL, NULL};
		relptr_t * ptep[2] = {NULL, NULL};
		relptr_t pte_index[2];
		relptr_t ret;
		pte_t * ptep1 = NULL, *ptep2 = NULL, pte;
		unsigned allocated = 0;

		/* step 0 start. Copy first to temp.
		 * if error, nothing happens 
		 */
#if PSWAP_DEBUG > 1
		printk("\tDAX pswap start step 0. first: %#lx, second: %#lx, npgs; %ld\n",
		(unsigned long)first, (unsigned long)second, npgs);
#endif
		cur1 = first;
		cur2 = second;
		i = 0;
		ret = find_dax_ptep(rt, cur1, &pmdp[0], NULL);
#ifdef PSWAP_HUGE
		if(ret){
			// it's huge page
			// need to down grade
			dax_downgrade_huge(rt, pmdp[0]);
			shoot_pmd = 1;
		}
#endif
		pte_index[0] = (cur1 & (PMD_SIZE - 1)) >> PAGE_SHIFT;
		ptep[0] = DAX_REL2ABS(*pmdp[0]);

		ret = find_dax_ptep(rt, cur2, &pmdp[1], NULL);
#ifdef PSWAP_HUGE
		if(ret){
			// it's huge page
			// need to down grade
			dax_downgrade_huge(rt, pmdp[1]);
			shoot_pmd = 1;
		}
#endif
		pte_index[1] = (cur2 & (PMD_SIZE - 1)) >> PAGE_SHIFT;
		ptep[1] = DAX_REL2ABS(*pmdp[1]);
#if PSWAP_DEBUG > 1
		printk("\t\tpswap_temp: %#lx, ptep[0]: %#lx, ptep[1]: %#lx; pte_index[0]: %ld, pte_index[1]: %ld\n",
		(unsigned long)rt->pswap_temp, (unsigned long)ptep[0], (unsigned long)ptep[1],
		pte_index[0], pte_index[1]);
#endif
		for(i = 0; i < npgs; i++){
			rt->pswap_temp[i] = ptep[0][pte_index[0] + i];
		}

		/* Starting step 1
		 * Copy second to first
		 * From now on, the recovery strategy will be redo
		 * if error, the original first will be erased
		 * page leak occurs
		 * 
		 */
		master_page->pswap_state = DAX_PSWAP_STEP1;
		master_page->pswap_npgs = npgs;
		master_page->pswap_frame.first = ufirst;
		master_page->pswap_frame.second = usecond;
		arch_wb_cache_pmem(&master_page->pswap_state, 64);
#if PSWAP_DEBUG > 1
		printk("\tDAX pswap start step 1. \n");
#endif
		for(i = 0; i < npgs; i++){
			ptep[0][pte_index[0] + i] = ptep[1][pte_index[1] + i];
		}
		arch_wb_cache_pmem(&ptep[0][pte_index[0]], npgs << 3);

		/* starting step 2 
		 * Copy the temp to second
		 * if error, the original first will be erased
		 * part of the erased pages will be leacked. 
		 */
		master_page->pswap_state = DAX_PSWAP_STEP2;
		arch_wb_cache_pmem(&master_page->pswap_state, 64);
#if PSWAP_DEBUG > 1
		printk("\tDAX pswap start step 2. \n");
#endif
		for(i = 0; i < npgs; i++){
			ptep[1][pte_index[1] + i] = rt->pswap_temp[i];
		}
		arch_wb_cache_pmem(&ptep[1][pte_index[1]], npgs << 3);

		/* finished pswap */
		master_page->pswap_state = DAX_PSWAP_NORMAL;

		// real page table
		allocated += dax_get_ptep_noalloc(mm, ufirst, &ptep1);
		allocated += dax_get_ptep_noalloc(mm, usecond, &ptep2);

		if(shoot_pmd == 0 && allocated == 2){
			// swap
			for(i = 0; i < npgs; i++){
				pte = ptep2[i];
				set_pte(ptep2 + i, ptep1[i]);
				set_pte(ptep1 + i, pte);
				pv_ops.mmu.flush_tlb_one_user(ufirst + (i << PAGE_SHIFT));
				pv_ops.mmu.flush_tlb_one_user(usecond + (i << PAGE_SHIFT));
			}
		}
		else if (allocated == 1 || shoot_pmd == 1){
			// shoot down
			pte_t* ptep = ptep1 ? ptep1 : ptep2;
			pte.pte = 0;
			for(i = 0; i < npgs; i++){
				set_pte(ptep + i, pte);
				pv_ops.mmu.flush_tlb_one_user(ufirst + (i << PAGE_SHIFT));
				pv_ops.mmu.flush_tlb_one_user(usecond + (i << PAGE_SHIFT));
			}
		}
		else{
			// do nothing
		}
	}
	// for(i = 0; i < npgs; i++){
		// pte_t *ptep1, *ptep2;
		// pte_t temp;
		// relptr_t f, s;
		// f = ufirst + (i << PAGE_SHIFT);
		// s = usecond + (i << PAGE_SHIFT);
		// dax_get_ptep(mm, f, &ptep1);
		// dax_get_ptep(mm, s, &ptep2);
		// temp = *ptep1;
		// *ptep1 = *ptep2;
		// *ptep2 = temp;
		// pv_ops.mmu.flush_tlb_one_user(f);
		// pv_ops.mmu.flush_tlb_one_user(s);
	// }
	// for(i = 0; i < npgs; i += 512){
	// 	dax_free_pmd(mm, ufirst + (i << PAGE_SHIFT));
	// 	dax_free_pmd(mm, usecond+ (i << PAGE_SHIFT));
	// }
	// if(((ufirst + (npgs << PAGE_SHIFT)) & (PMD_SIZE - 1)) != 0 ||
	// 	((usecond + (npgs << PAGE_SHIFT)) & (PMD_SIZE - 1)) != 0)
	// {
	// 	dax_free_pmd(mm, ufirst + (npgs << PAGE_SHIFT));
	// 	dax_free_pmd(mm, usecond + (npgs << PAGE_SHIFT));
	// }
	if(flush_tlb){
#if PSWAP_DEBUG > 1
		printk("\tDAX pswap Flushed TLB \n");
#endif
		pv_ops.mmu.flush_tlb_user();
	}
#if PSWAP_DEBUG > 1
	if(aligned){
		relptr_t * pmdp;
		unsigned long pmd_off, pte_off;
		pmd_off =  (first & PMD_MASK) >> PMD_SHIFT;
		pmd_off &= 0x01ff;
		pte_off = (first & PAGE_MASK) >> PAGE_SHIFT;
		pte_off &= 0x01ff;
		find_dax_ptep(rt, first, &pmdp, NULL);
		printk("\t\t  After swap: first: %#lx, pmd_off: %#lx, pte_off: %#lx \n\t\t\t  pmdp: %#lx, *pmdp: %#lx, pte: %#lx",
		first, pmd_off, pte_off,
		(unsigned long)pmdp, *pmdp, 
		*pmdp ? ((relptr_t*) DAX_REL2ABS(*pmdp))[pte_off]: 0);

		pmd_off =  (second & PMD_MASK) >> PMD_SHIFT;
		pmd_off &= 0x01ff;
		pte_off = (second & PAGE_MASK) >> PAGE_SHIFT;
		pte_off &= 0x01ff;
		find_dax_ptep(rt, second, &pmdp, NULL);
		printk("\t\t  After swap: second: %#lx, pmd_off: %#lx, pte_off: %#lx \n\t\t\t  pmdp: %#lx, *pmdp: %#lx, pte: %#lx",
		second, pmd_off, pte_off,
		(unsigned long)pmdp, *pmdp, 
		*pmdp ? ((relptr_t*) DAX_REL2ABS(*pmdp))[pte_off]: 0);;
	}
	
#endif


	mutex_unlock(&dax_lock);
	return 0;
err_out: 
	printk("ERROR: DAX PSWAP: Invalid parameter!");
	mutex_unlock(&dax_lock);
	return EINVAL;
}



inline long dax_handle_pswap(unsigned long ptr){
	dax_ioctl_pswap_t ctl;
	long ret = 0;
	if(copy_from_user((void*) &ctl, (void*)ptr, sizeof(dax_ioctl_pswap_t))){
		return EFAULT;
	}
	ret = dax_pswap(ctl.ufirst, ctl.usecond, ctl.npgs);
	// if(ctl->npgs > DAX_PSWAP_FAST_PGS){
	// 	ret = dax_pswap(ctl->ufirst, ctl->usecond, ctl->npgs, ctl->flag);
	// }
	// else{
	// 	ret = dax_pswap_fast(ctl->ufirst, ctl->usecond, ctl->npgs, ctl->flag);
	// }
	return ret;
}

long dax_handle_reset(struct file * filp){

	struct dev_dax *dev_dax;
	phys_addr_t dax_start_addr;
	dax_master_page_t * m_page_p;
	printk("DAX RESET: Start!");
	dev_dax = filp->private_data;
	dax_start_addr = dax_pgoff_to_phys(dev_dax, 0, PMD_SIZE)+ DAX_PSWAP_SHIFT;
	m_page_p = (void *)dax_start_addr + __PAGE_OFFSET;
	if(MASTER_NOT_INIT(m_page_p)){
		return EINVAL;
	}
	else{
		printk("DAX RESET: kernel dax reset finished!");
		strcpy(m_page_p->magic_word, "badbeef");
		clwb(m_page_p);
		return 0;
	}
}

long dax_handle_pcow(unsigned long ptr) {
    dax_ioctl_pcow_t * ctl = kmalloc(sizeof(dax_ioctl_pcow_t), GFP_KERNEL);
    long ret;
    if(copy_from_user((void*) ctl, (void*)ptr, sizeof(dax_ioctl_pcow_t))) {
        kfree(ctl);
        return EFAULT;
    }
    // ret = dax_pcow(ctl->src, ctl->dest, ctl->size, ctl->flag);
    kfree(ctl);
    return ret;
}

long dax_handle_init(struct file * filp, unsigned long ptr){
	struct dev_dax *dev_dax;
	phys_addr_t dax_start_addr;
	dax_master_page_t * m_page_p;
	struct dax_region *dax_region;
	dax_ioctl_init_t frame;

	if(copy_from_user(&frame, (void*)ptr, sizeof(dax_ioctl_init_t))){
		printk("DAX INIT: Error: User addr invalid: %#lx\n", ptr);
		return -1;
	}
	dev_dax = filp->private_data;
	dax_region = dev_dax->region;
	dax_start_addr = dax_pgoff_to_phys(dev_dax, 0, PMD_SIZE)+ DAX_PSWAP_SHIFT;
	m_page_p = (void *)dax_start_addr + __PAGE_OFFSET;
	if(MASTER_NOT_INIT(m_page_p)){
		init_dax(m_page_p, dax_region->res.end - dax_region->res.start);
	}
	frame.space_total = m_page_p->num_pages * PAGE_SIZE;
	frame.mpk_meta = rt->mpk[DAX_MPK_META];
	frame.mpk_file = rt->mpk[DAX_MPK_FILE];
	frame.mpk_default = rt->mpk[DAX_MPK_DEFAULT];
	copy_to_user((void*)ptr, &frame, sizeof(frame));
	return 0;
}

long dax_handle_prefault(unsigned long ptr){
	dax_ioctl_prefault_t frame;
	unsigned long i;
	struct vm_area_struct *vma;
	// relptr_t *dax_pmdp, *dax_ptep;

	if(copy_from_user(&frame, (void *)ptr, sizeof(dax_ioctl_prefault_t))){
		printk("DAX Prefault: Error: User addr invalid\n");
		return -1;
	}
#ifdef PSWAP_DEBUG
	printk("DAX Prefault: start: %#lx, npmd: %lx", (unsigned long)frame.addr, frame.n_pmd);
#endif
	vma = find_vma(current->mm, (unsigned long)frame.addr);

	mutex_lock(&dax_lock);
	if(rt->vaddr_base != vma->vm_start){
		dax_master_page_t * master_page;
#ifdef PSWAP_DEBUG
		printk("DAX Prefault: PID: %d\n",
			current->pid );
#endif
		if(!vma){
			mutex_unlock(&dax_lock);
			return -1;
		}
		master_page = get_master_page(vma);
		if(unlikely(!master_page)){
			printk("DAX Prefault: Error: User addr invalid, not in DAX\n");
			return -1;
		}
		if(unlikely(MASTER_NOT_INIT(master_page))){
			printk("DAX Prefault: Error: User addr invalid, Dax not inited\n");
			return -1;
		}
		get_dax_runtime(master_page);
		rt->vaddr_base = vma->vm_start;
	}
	for(i = 0; i < frame.n_pmd; i++){
		install_pmd(NULL, (unsigned long)frame.addr + (i << PMD_SHIFT));
		// paddr_rel = find_dax_ptep(rt, curptr, &dax_pmdp, NULL);
		// if(paddr_rel != 0){
		// 	// it's HUGE
		// 	for(j = 0; j < PTRS_PER_PMD; j++){
		// 		// pfn = phys_to_pfn_t(, dax_region->pfn_flags);
		// 		vmf_insert_pfn_prot(rt->current_vma, curptr + (j << PAGE_SHIFT), DAX_REL2PFN(paddr_rel) + j, PAGE_SHARED);
		// 	}
		// }
		// else{
		// 	// it's not HUGE
		// 	for(j = 0; j < PTRS_PER_PMD; j++){
		// 		dax_ptep = DAX_REL2ABS(*dax_pmdp);
		// 		vmf_insert_pfn_prot(rt->current_vma, curptr + (j << PAGE_SHIFT), DAX_REL2PFN(dax_ptep[j]), PAGE_SHARED);
		// 	}
		// }
		// curptr += PMD_SIZE;
	}
#ifdef PSWAP_DEBUG
	printk("DAX Prefault: end");
#endif
	mutex_unlock(&dax_lock);
	return 0;
}

long dax_handle_ready(struct file * filp, unsigned long ptr){
	struct dev_dax *dev_dax;
	phys_addr_t dax_start_addr;
	dax_master_page_t * m_page_p;
	int ret = 0;

	dev_dax = filp->private_data;
	dax_start_addr = dax_pgoff_to_phys(dev_dax, 0, PMD_SIZE)+ DAX_PSWAP_SHIFT;
	m_page_p = (void *)dax_start_addr + __PAGE_OFFSET;
	if(ptr == 0){
		return 0;
	}
	if(MASTER_NOT_INIT(m_page_p)){
#ifdef PSWAP_DEBUG 
		printk("DAX Ready: NOT INITIALIZED\n");
#endif
		ret = 0;
		copy_to_user((void*)ptr, &ret, sizeof(int));
		return 0;
	}
	else{
#ifdef PSWAP_DEBUG
		printk("DAX Ready: INITIALIZED\n");
#endif
		ret = 1;
		copy_to_user((void*)ptr, &ret, sizeof(int));
		return 0;
	}
}

long dax_ioctl(struct file * filp, unsigned int type, unsigned long ptr){
#if PSWAP_DEBUG > 1
	printk("DAX IOCTL: type: %u, ptr: %#lx", type, ptr);
#endif
	switch (type){
		case DAX_IOCTL_PSWAP:
			return dax_handle_pswap(ptr);
			break;
		case DAX_IOCTL_RESET:
			return dax_handle_reset(filp);
			break;
		case DAX_IOCTL_INIT:
			return dax_handle_init(filp, ptr);
			break;
		case DAX_IOCTL_COW:
			return dax_handle_pcow(ptr);
			break;
		case DAX_IOCTL_READY:
			return dax_handle_ready(filp, ptr);
			break;
		case DAX_IOCTL_PREFAULT:
			return dax_handle_prefault(ptr);
			break;
		case DAX_IOCTL_COPYTEST:
			{
				unsigned i;
				void *target;
				struct dev_dax *dev_dax;
				phys_addr_t dax_start_addr;
				dax_master_page_t * m_page_p;

				dev_dax = filp->private_data;
				dax_start_addr = dax_pgoff_to_phys(dev_dax, 0, PMD_SIZE)+ DAX_PSWAP_SHIFT;
				m_page_p = (void *)dax_start_addr + __PAGE_OFFSET;
				target = (void*)(m_page_p->rt.start_paddr + __PAGE_OFFSET + (unsigned long)12*1024*1024*1024);
				printk("TEST: copy from %#lx to %#lx, size: %d", (unsigned long)ptr, (unsigned long)target, (unsigned)1024* 1024*1024);
				if((i = __copy_user_nocache(target, (void __user*)ptr, (unsigned)1024 * 1024*1024, 0))){
					printk("COPY failed, %d uncopied!\n", i);
				}

			}
			return 0;
		default:
			return -1;
	}
}

#endif

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
module_init(dax_init);
module_exit(dax_exit);
MODULE_ALIAS_DAX_DEVICE(0);
