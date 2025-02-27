// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/arm/mm/highmem.c -- ARM highmem support
 *
 * Author:	Nicolas Pitre
 * Created:	september 8, 2008
 * Copyright:	Marvell Semiconductors Inc.
 */

#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <asm/fixmap.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include "mm.h"

static inline void set_fixmap_pte(int idx, pte_t pte)
{
	unsigned long vaddr = __fix_to_virt(idx);
	pte_t *ptep = pte_offset_kernel(pmd_off_k(vaddr), vaddr);

	set_pte_ext(ptep, pte, 0);
	local_flush_tlb_kernel_page(vaddr);
}

static inline pte_t get_fixmap_pte(unsigned long vaddr)
{
	pte_t *ptep = pte_offset_kernel(pmd_off_k(vaddr), vaddr);

	return *ptep;
}

static unsigned int fixmap_idx(int type)
{
	return FIX_KMAP_BEGIN + type + KM_TYPE_NR * smp_processor_id();
}

void *kmap(struct page *page)
{
	might_sleep();
	if (!PageHighMem(page))
		return page_address(page);
	return kmap_high(page);
}
EXPORT_SYMBOL(kmap);

void kunmap(struct page *page)
{
	BUG_ON(in_interrupt());
	if (!PageHighMem(page))
		return;
	kunmap_high(page);
}
EXPORT_SYMBOL(kunmap);

void *kmap_atomic(struct page *page)
{
	pte_t pte = mk_pte(page, kmap_prot);
	unsigned int idx;
	unsigned long vaddr;
	void *kmap;
	int type;

	preempt_disable_nort();
	pagefault_disable();
	if (!PageHighMem(page))
		return page_address(page);

#ifdef CONFIG_DEBUG_HIGHMEM
	/*
	 * There is no cache coherency issue when non VIVT, so force the
	 * dedicated kmap usage for better debugging purposes in that case.
	 */
	if (!cache_is_vivt())
		kmap = NULL;
	else
#endif
		kmap = kmap_high_get(page);
	if (kmap)
		return kmap;

	type = kmap_atomic_idx_push();

	idx = fixmap_idx(type);
	vaddr = __fix_to_virt(idx);
#ifdef CONFIG_DEBUG_HIGHMEM
	/*
	 * With debugging enabled, kunmap_atomic forces that entry to 0.
	 * Make sure it was indeed properly unmapped.
	 */
	BUG_ON(!pte_none(get_fixmap_pte(vaddr)));
#endif
	/*
	 * When debugging is off, kunmap_atomic leaves the previous mapping
	 * in place, so the contained TLB flush ensures the TLB is updated
	 * with the new mapping.
	 */
#ifdef CONFIG_PREEMPT_RT_FULL
	current->kmap_pte[type] = pte;
#endif
	set_fixmap_pte(idx, pte);

	return (void *)vaddr;
}
EXPORT_SYMBOL(kmap_atomic);

void __kunmap_atomic(void *kvaddr)
{
	unsigned long vaddr = (unsigned long) kvaddr & PAGE_MASK;
	int idx, type;

	if (kvaddr >= (void *)FIXADDR_START) {
		type = kmap_atomic_idx();
		idx = fixmap_idx(type);

		if (cache_is_vivt())
			__cpuc_flush_dcache_area((void *)vaddr, PAGE_SIZE);
#ifdef CONFIG_PREEMPT_RT_FULL
		current->kmap_pte[type] = __pte(0);
#endif
#ifdef CONFIG_DEBUG_HIGHMEM
		BUG_ON(vaddr != __fix_to_virt(idx));
#else
		(void) idx;  /* to kill a warning */
#endif
		set_fixmap_pte(idx, __pte(0));
		kmap_atomic_idx_pop();
	} else if (vaddr >= PKMAP_ADDR(0) && vaddr < PKMAP_ADDR(LAST_PKMAP)) {
		/* this address was obtained through kmap_high_get() */
		kunmap_high(pte_page(pkmap_page_table[PKMAP_NR(vaddr)]));
	}
	pagefault_enable();
	preempt_enable_nort();
}
EXPORT_SYMBOL(__kunmap_atomic);

void *kmap_atomic_pfn(unsigned long pfn)
{
	pte_t pte = pfn_pte(pfn, kmap_prot);
	unsigned long vaddr;
	int idx, type;
	struct page *page = pfn_to_page(pfn);

	preempt_disable_nort();
	pagefault_disable();
	if (!PageHighMem(page))
		return page_address(page);

	type = kmap_atomic_idx_push();
	idx = fixmap_idx(type);
	vaddr = __fix_to_virt(idx);
#ifdef CONFIG_DEBUG_HIGHMEM
	BUG_ON(!pte_none(get_fixmap_pte(vaddr)));
#endif
#ifdef CONFIG_PREEMPT_RT_FULL
	current->kmap_pte[type] = pte;
#endif
	set_fixmap_pte(idx, pte);

	return (void *)vaddr;
}
#if defined CONFIG_PREEMPT_RT_FULL
void switch_kmaps(struct task_struct *prev_p, struct task_struct *next_p)
{
	int i;

	/*
	 * Clear @prev's kmap_atomic mappings
	 */
	for (i = 0; i < prev_p->kmap_idx; i++) {
		int idx = fixmap_idx(i);

		set_fixmap_pte(idx, __pte(0));
	}
	/*
	 * Restore @next_p's kmap_atomic mappings
	 */
	for (i = 0; i < next_p->kmap_idx; i++) {
		int idx = fixmap_idx(i);

		if (!pte_none(next_p->kmap_pte[i]))
			set_fixmap_pte(idx, next_p->kmap_pte[i]);
	}
}
#endif
