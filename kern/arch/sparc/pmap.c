#include <kern/arch.h>
#include <kern/lib.h>
#include <kern/pageinfo.h>
#include <machine/psr.h>
#include <machine/sparc-common.h>
#include <inc/error.h>
#include <inc/safeint.h>

static struct Contexttable bootct
    __attribute__ ((aligned (4096), section (".data")));
static struct Pagemap bootpt
    __attribute__ ((aligned (4096), section (".data")));
static struct Pagemap2 bootpt2s[192 * (1 + 64)]
    __attribute__ ((aligned (512), section (".data")));

static int
pmap_alloc_pmap2(struct Pagemap2fl *fl, struct Pagemap2 **pgmap)
{
    struct Pagemap2 *pm = LIST_FIRST(fl);
    if (pm) {
	LIST_REMOVE(pm, pm2_link);
	*pgmap = pm;
	return 0;
    } 

    void *p;
    int r = page_alloc(&p);
    if (r < 0)
	return r;
    memset(p, 0, PGSIZE);

    pm = (struct Pagemap2 *)p;
    *pgmap = pm;
    struct Pagemap2 *end = pm + (PGSIZE / sizeof(struct Pagemap2));
    for (pm = pm + 1; pm != end; pm++)
	LIST_INSERT_HEAD(fl, pm, pm2_link);

    return 0;
}

int
page_map_alloc(struct Pagemap **pm_store)
{
    void *pmap;
    int r = page_alloc(&pmap);
    if (r < 0)
	return r;

    memcpy(pmap, &bootpt, sizeof(bootpt));
    *pm_store = (struct Pagemap *) pmap;
    return 0;
}

static void
page_map_free_level(ptent_t *pgmap, int pmlevel, struct Pagemap2fl *fl)
{
    /* For higher-level page tables, free their lower-level page tables */
    if (pmlevel > 0) {
	/* Skip the kernel half of the address space */
	int maxi = (pmlevel == NPTLVLS ? NPTENTRIES(pmlevel)/2 : NPTENTRIES(pmlevel));

	for (int i = 0; i < maxi; i++) {
	    ptent_t ptent = pgmap[i];
	    if (PT_ET(ptent) == PT_ET_PTD) {
		ptent_t *pm = pa2kva(PTD_ADDR(ptent));
		page_map_free_level(pm, pmlevel - 1, fl);
	    }
	}
    }

    /* For non-top-level PTs, put them back onto the free list */
    if (pmlevel < NPTLVLS)
	LIST_INSERT_HEAD(fl, (struct Pagemap2 *) pgmap, pm2_link);
}

void
page_map_free(struct Pagemap *pgmap)
{
    page_map_free_level(&pgmap->pm1_ent[0], NPTLVLS, &pgmap->fl);

    struct Pagemap2 *pm2, *npm2;
    for (pm2 = LIST_FIRST(&pgmap->fl); pm2; pm2 = npm2) {
	npm2 = LIST_NEXT(pm2, pm2_link);
	if (PGOFF(pm2))
	    LIST_REMOVE(pm2, pm2_link);
    }

    for (pm2 = LIST_FIRST(&pgmap->fl); pm2; pm2 = npm2) {
	npm2 = LIST_NEXT(pm2, pm2_link);
	page_free(pm2);
    }

    page_free(pgmap);
}

static int
page_map_traverse_internal(ptent_t *pgmap, int pmlevel, struct Pagemap2fl *fl,
			   const void *first, const void *last,
			   int create,
			   page_map_traverse_cb cb, const void *arg,
			   void *va_base)
{
    int r;
    assert(pmlevel >= 0 && pmlevel <= NPTLVLS);

    uint32_t first_idx = PDX(pmlevel, first);
    uint32_t last_idx  = PDX(pmlevel, last);

    for (uint32_t idx = first_idx; idx <= last_idx; idx++) {
	ptent_t *pm_entp = &pgmap[idx];
	ptent_t pm_ent = *pm_entp;

	void *ent_va = va_base + (idx << PDSHIFT(pmlevel));

	if (pmlevel == 0) {
	    if (create || (PT_ET(pm_ent) == PT_ET_PTE))
		cb(arg, pm_entp, ent_va);
	    continue;
	}

	if ((pmlevel == 1 || pmlevel == 2) && (PT_ET(pm_ent) == PT_ET_PTE)) {
	    panic("XXX");
	    cb(arg, pm_entp, ent_va);
	    continue;
	}

	if (PT_ET(pm_ent) == PT_ET_NONE) {
	    if (!create)
		continue;

	    struct Pagemap2 *pgmap2 = 0;
	    if ((r = pmap_alloc_pmap2(fl, &pgmap2)) < 0)
		return r;

	    pm_ent = PTD_ENTRY(kva2pa(pgmap2));
	    *pm_entp = pm_ent;
	}

	uint32_t *pm_next = pa2kva(PTD_ADDR(pm_ent));
	const void *first_next = (idx == first_idx) ? first : 0;
	const void *last_next  = (idx == last_idx)  ? last  : (const void *) ~0;
	r = page_map_traverse_internal(pm_next, pmlevel - 1, fl,
				       first_next, last_next,
				       create, cb, arg, ent_va);
	if (r < 0)
	    return r;
    }

    return 0;
}

int
page_map_traverse(struct Pagemap *pgmap, const void *first, const void *last,
		  int create, page_map_traverse_cb cb, const void *arg)
{
    if (last >= (const void *) ULIM)
	last = (const void *) ULIM - PGSIZE;
    return page_map_traverse_internal(&pgmap->pm1_ent[0], NPTLVLS, &pgmap->fl, 
				      first, last, create, cb, arg, 0);
}

static void
pgdir_walk_cb(const void *arg, ptent_t *ptep, void *va)
{
    ptent_t **pte_store = (ptent_t **) arg;
    *pte_store = ptep;
}

int
pgdir_walk(struct Pagemap *pgmap, const void *va,
	   int create, ptent_t **pte_store)
{
    *pte_store = 0;
    int r = page_map_traverse(pgmap, va, va, create, &pgdir_walk_cb, pte_store);
    if (r < 0)
	return r;
    if (create && !*pte_store)
	return -E_INVAL;
    return 0;
}

static void *
page_lookup(struct Pagemap *pgmap, void *va, ptent_t **pte_store)
{
    /* LEON3 MMU is missing probe */
    if ((uintptr_t) va >= ULIM)
	panic("page_lookup: va %p over ULIM", va);

    ptent_t *ptep;
    int r = pgdir_walk(pgmap, va, 0, &ptep);
    if (r < 0)
	panic("pgdir_walk(%p, create=0) failed: %d", va, r);

    if (pte_store)
	*pte_store = ptep;

    if (ptep == 0 || PT_ET(*ptep) == PT_ET_NONE)
	return 0;

    return pa2kva(PTE_ADDR(*ptep));
}

int
check_user_access2(const void *ptr, uint64_t nbytes,
		   uint32_t reqflags, int alignbytes)
{
    assert(cur_thread);
    if (!cur_as) {
	int r = thread_load_as(cur_thread);
	if (r < 0)
	    return r;

	as_switch(cur_thread->th_as);
	assert(cur_as);
    }

    ptent_t pte_flags = 0;
    if (reqflags & SEGMAP_WRITE)
	pte_flags |= PTE_ACC_W;
    
    int aspf = 0;
    if (nbytes > 0) {
	int overflow = 0;
	uintptr_t iptr = (uintptr_t) ptr;
	uintptr_t start = ROUNDDOWN(iptr, PGSIZE);
	uintptr_t end = ROUNDUP(safe_addptr(&overflow, iptr, nbytes), PGSIZE);

	if (iptr & (alignbytes - 1))
	    return -E_INVAL;

	if (end <= start || overflow)
	    return -E_INVAL;

	for (uintptr_t va = start; va < end; va += PGSIZE) {
	    if (va >= ULIM)
		return -E_INVAL;

	    ptent_t *ptep;
	    if (cur_as->as_pgmap &&
		page_lookup(cur_as->as_pgmap, (void *) va, &ptep) &&
		(PTE_ACC(*ptep) & pte_flags) == pte_flags)
		continue;

	    aspf = 1;
	    int r = as_pagefault(cur_as, (void *) va, reqflags);
	    if (r < 0)
		return r;
	}
    }

    // Flush any stale TLB entries that might have arisen from as_pagefault()
    if (aspf)
	as_switch(cur_as);

    return 0;
}

void
pmap_set_current(struct Pagemap *pm)
{
    if (!pm)
	pm = &bootpt;

    physaddr_t pa = kva2pa(pm);
    ctxptr_t ptd = ((pa >> 4) & PTD_PTP_MASK) | PT_ET_PTD;
    bootct.ct_ent[0] = ptd;
    tlb_flush_all();
}

/*
 * Page table traversal callbacks
 */

void
as_arch_collect_dirty_bits(const void *arg, ptent_t *ptep, void *va)
{
    uint64_t pte = *ptep;
    if (!(PT_ET(pte) == PT_ET_PTE) || !(pte & PTE_M))
	return;

    struct page_info *pi = page_to_pageinfo(pa2kva(PTE_ADDR(pte)));
    pi->pi_dirty = 1;
    *ptep &= ~PTE_M;
}

void
as_arch_page_invalidate_cb(const void *arg, ptent_t *ptep, void *va)
{
    as_arch_collect_dirty_bits(arg, ptep, va);

    uint32_t pte = *ptep;
    if (PT_ET(pte) == PT_ET_PTE) {
	if (PTE_ACC(pte) & PTE_ACC_W)
	    pagetree_decpin_write(pa2kva(PTE_ADDR(pte)));
	else
	    pagetree_decpin_read(pa2kva(PTE_ADDR(pte)));
	*ptep = 0;
    }
}

void
as_arch_page_map_ro_cb(const void *arg, ptent_t *ptep, void *va)
{
    as_arch_collect_dirty_bits(arg, ptep, va);
    
    uint32_t pte = *ptep;
    if ((PT_ET(pte) == PT_ET_PTE) && (PTE_ACC(pte) & PTE_ACC_W)) {
	pagetree_decpin_write(pa2kva(PTE_ADDR(pte)));
	pagetree_incpin_read(pa2kva(PTE_ADDR(pte)));
	*ptep &= ~(PTE_ACC_W << PTE_ACC_SHIFT);
    }
}

int
as_arch_putpage(struct Pagemap *pgmap, void *va, void *pp, uint32_t flags)
{
    uint32_t ptflags = 0;
    if (flags & SEGMAP_WRITE)
	ptflags |= PTE_ACC_W;
    if (flags & SEGMAP_EXEC)
	ptflags |= PTE_ACC_X;

    ptent_t *ptep;
    int r = pgdir_walk(pgmap, va, 1, &ptep);
    if (r < 0) {
	if (r != -E_RESTART)
	    cprintf("XXX pgdir_walk error: %s\n", e2s(r));
	return r;
    }

    as_arch_page_invalidate_cb(pgmap, ptep, va);
    *ptep = PTE_ENTRY(kva2pa(pp), ptflags);
    if (ptflags & PTE_ACC_W)
	pagetree_incpin_write(pp);
    else
	pagetree_incpin_read(pp);

    return 0;
}

/*
 * Page addressing
 */

void *
pa2kva(physaddr_t pa)
{
    if (pa >= PA_MEMBASE)
	return (void *) pa;

    panic("pa2kva called with invalid pa 0x%x", pa);
}

physaddr_t
kva2pa(void *kva)
{
    physaddr_t va = (physaddr_t) kva;
    if (va >= (uintptr_t) PHYSBASE && va < (uintptr_t) PHYSEND)
	return va - LOAD_OFFSET;

    panic("kva2pa called with invalid kva %p", kva);
}

ppn_t
pa2ppn(physaddr_t pa)
{
    return ((pa - PA_MEMBASE) >> PGSHIFT);
}

physaddr_t
ppn2pa(ppn_t pn)
{
    return (pn << PGSHIFT) + PA_MEMBASE;
}

/*
 * MMU initialization
 */

void
mmu_init(void)
{
    uint32_t n2 = 0;

    for (uint32_t i = 64; i < 256; i++) {
	uint32_t cflag = (i >= 128) ? 0 : PTE_C;

	struct Pagemap2 *pt2 = &bootpt2s[n2++];
	bootpt.pm1_ent[i] = PTD_ENTRY(kva2pa(pt2));

	for (uint32_t j = 0; j < 64; j++) {
	    struct Pagemap2 *pt3 = &bootpt2s[n2++];
	    pt2->pm2_ent[j] = PTD_ENTRY(kva2pa(pt3));

	    for (uint32_t k = 0; k < 64; k++) {
		uint32_t ppn = i * (1 << 12) + j * (1 << 6) + k;
		pt3->pm2_ent[k] = (ppn << PTE_PPN_SHIFT) |
				  (PT_ET_PTE << PT_ET_SHIFT) |
				  (PTE_ACC_SUPER << PTE_ACC_SHIFT) |
				  cflag;
	    }
	}
    }

    assert(n2 == sizeof(bootpt2s) / sizeof(*bootpt2s));

    bootct.ct_ent[0] = (kva2pa(&bootpt) >> 4) | (PT_ET_PTD << PT_ET_SHIFT);
    sta(SRMMU_CTXTBL_PTR, kva2pa(&bootct) >> 4, ASI_MMUREGS);
    sta(SRMMU_CTX_REG,    0, ASI_MMUREGS);
    sta(SRMMU_CTRL_REG,   SRMMU_CTRL_E | lda(SRMMU_CTRL_REG, ASI_MMUREGS),
			  ASI_MMUREGS);
    __asm volatile("nop; nop; nop;");
    tlb_flush_all();
}
