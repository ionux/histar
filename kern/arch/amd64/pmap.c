/* See COPYRIGHT for copyright information. */

#include <machine/x86.h>
#include <machine/pmap.h>
#include <dev/kclock.h>
#include <kern/lib.h>
#include <kern/kobj.h>
#include <kern/pageinfo.h>
#include <inc/error.h>

static bool_t scrub_free_pages = 0;

// These variables are set by i386_detect_memory()
static physaddr_t maxpa;	// Maximum physical address
size_t global_npages;		// Amount of physical memory (in pages)
static size_t basemem;		// Amount of base memory (in bytes)
static size_t extmem;		// Amount of extended memory (in bytes)

// These variables are set in i386_vm_init()
static char *boot_freemem;	// Pointer to next byte of free mem

struct Page_link {
    TAILQ_ENTRY(Page_link) pp_link;	// free list link
};
static TAILQ_HEAD(Page_list, Page_link) page_free_list;
				// Free list of physical pages

// Global page allocation stats
struct page_stats page_stats;

// Keep track of various page metadata
struct page_info *page_infos;

static int
nvram_read(int r)
{
  return mc146818_read (r) | (mc146818_read (r + 1) << 8);
}

static void
i386_detect_memory(uint64_t lower_kb, uint64_t upper_kb)
{
    // Worse case, CMOS tells us how many kilobytes there are
    if (!lower_kb)
	lower_kb = nvram_read(NVRAM_BASELO);
    if (!upper_kb)
	upper_kb = nvram_read (NVRAM_EXTLO);

    basemem = ROUNDDOWN(lower_kb * 1024, PGSIZE);
    extmem  = ROUNDDOWN(upper_kb * 1024, PGSIZE);

    // Calculate the maxmium physical address based on whether
    // or not there is any extended memory.  See comment in ../inc/mmu.h.
    if (extmem)
	maxpa = EXTPHYSMEM + extmem;
    else
	maxpa = basemem;

    global_npages = maxpa / PGSIZE;

    cprintf("Physical memory: %dK available, ", (int) (maxpa / 1024));
    cprintf("base = %dK, extended = %dK\n", (int) (basemem / 1024),
	    (int) (extmem / 1024));
}

//
// Allocate n bytes of physical memory aligned on an 
// align-byte boundary.  Align must be a power of two.
// Return kernel virtual address.  Returned memory is uninitialized.
//
// If we're out of memory, boot_alloc should panic.
// It's too early to run out of memory.
// This function may ONLY be used during initialization,
// before the page_free_list has been set up.
// 
static void *
boot_alloc(uint32_t n, uint32_t align)
{
  extern char end[];
  void *v;

  // Initialize boot_freemem if this is the first time.
  // 'end' is a magic symbol automatically generated by the linker,
  // which points to the end of the kernel's bss segment -
  // i.e., the first virtual address that the linker
  // did _not_ assign to any kernel code or global variables.
  if (boot_freemem == 0)
    boot_freemem = end;

  boot_freemem = (char *) ROUNDUP (boot_freemem, align);
  if (boot_freemem + n < boot_freemem
      || boot_freemem + n > (char *) (maxpa + KERNBASE))
    panic ("out of memory during i386_vm_init");
  v = boot_freemem;
  boot_freemem += n;
  return v;
}

void
page_free(void *v)
{
    struct Page_link *pl = (struct Page_link *) v;
    if (PGOFF(pl))
	panic("page_free: not a page-aligned pointer %p", pl);

    if (scrub_free_pages)
	memset(v, 0xde, PGSIZE);

    TAILQ_INSERT_TAIL(&page_free_list, pl, pp_link);
    page_stats.pages_avail++;
    page_stats.pages_used--;
}

int
page_alloc(void **vp)
{
    struct Page_link *pl = TAILQ_FIRST(&page_free_list);
    if (pl) {
	TAILQ_REMOVE(&page_free_list, pl, pp_link);
	*vp = pl;
	page_stats.pages_avail--;
	page_stats.pages_used++;
	page_stats.allocations++;

	if (scrub_free_pages)
	    memset(pl, 0xcd, PGSIZE);

	return 0;
    }

    cprintf("page_alloc: returning no mem\n");
    page_stats.failures++;
    return -E_NO_MEM;
}

static void
page_init(void)
{
    TAILQ_INIT(&page_free_list);

    int inuse;

    // Align boot_freemem to page boundary.
    boot_alloc(0, PGSIZE);

    // Allocate space for page status info.
    uint64_t sz = global_npages * sizeof(*page_infos);
    page_infos = boot_alloc(sz, PGSIZE);
    memset(page_infos, 0, sz);

    // Align to another page boundary.
    boot_alloc(0, PGSIZE);

    for (uint64_t i = 0; i < global_npages; i++) {
	// Off-limits until proven otherwise.
	inuse = 1;

	// The bottom basemem bytes are free except page 0.
	if (i != 0 && i < basemem / PGSIZE)
	    inuse = 0;

	// The IO hole and the kernel abut.

	// The memory past the kernel is free.
	if (i >= RELOC (boot_freemem) / PGSIZE)
	    inuse = 0;

	if (!inuse)
	    page_free(pa2kva(i << PGSHIFT));
    }

    page_stats.pages_used = 0;
}

void
pmap_init(uint64_t lower_kb, uint64_t upper_kb)
{
    i386_detect_memory(lower_kb, upper_kb);
    page_init ();
}

int
page_map_alloc(struct Pagemap **pm_store)
{
    void *pmap;
    int r = page_alloc(&pmap);
    if (r < 0)
	return r;

    memcpy(pmap, &bootpml4, PGSIZE);
    *pm_store = (struct Pagemap *) pmap;
    return 0;
}

static void
page_map_free_level(struct Pagemap *pgmap, int pmlevel)
{
    // Skip the kernel half of the address space
    int maxi = (pmlevel == 3 ? NPTENTRIES/2 : NPTENTRIES);
    int i;

    for (i = 0; i < maxi; i++) {
	uint64_t ptent = pgmap->pm_ent[i];
	if (!(ptent & PTE_P))
	    continue;
	if (pmlevel > 0) {
	    struct Pagemap *pm = (struct Pagemap *) pa2kva(PTE_ADDR(ptent));
	    page_map_free_level(pm, pmlevel - 1);
	}
    }

    page_free(pgmap);
}

void
page_map_free(struct Pagemap *pgmap)
{
    page_map_free_level(pgmap, 3);
}

static int
page_map_traverse_internal(struct Pagemap *pgmap, int pmlevel,
			   const void *first, const void *last,
			   int create,
			   page_map_traverse_cb cb, const void *arg,
			   void *va_base)
{
    int r;
    assert(pmlevel >= 0 && pmlevel <= 3);

    uint32_t first_idx = PDX(pmlevel, first);
    uint32_t last_idx  = PDX(pmlevel, last);

    for (uint64_t idx = first_idx; idx <= last_idx; idx++) {
	uint64_t *pm_entp = &pgmap->pm_ent[idx];
	uint64_t pm_ent = *pm_entp;

	void *ent_va = va_base + (idx << PDSHIFT(pmlevel));

	if (pmlevel == 0) {
	    cb(arg, pm_entp, ent_va);
	    continue;
	}

	if (pmlevel == 1 && (pm_ent & PTE_PS)) {
	    cb(arg, pm_entp, ent_va);
	    continue;
	}

	if (!(pm_ent & PTE_P)) {
	    if (!create)
		continue;

	    void *p;
	    if ((r = page_alloc(&p)) < 0)
		return r;

	    memset(p, 0, PGSIZE);
	    pm_ent = kva2pa(p) | PTE_P | PTE_U | PTE_W;
	    *pm_entp = pm_ent;
	}

	struct Pagemap *pm_next = (struct Pagemap *) pa2kva(PTE_ADDR(pm_ent));
	const void *first_next = (idx == first_idx) ? first : 0;
	const void *last_next  = (idx == last_idx)  ? last : (const void *) ~0UL;
	r = page_map_traverse_internal(pm_next, pmlevel - 1, first_next, last_next,
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
    if (first >= (const void *) ULIM)
	first = (const void *) ULIM - PGSIZE;
    if (last >= (const void *) ULIM)
	last = (const void *) ULIM - PGSIZE;
    return page_map_traverse_internal(pgmap, 3, first, last, create, cb, arg, 0);
}

static void
pgdir_walk_cb(const void *arg, uint64_t *ptep, void *va)
{
    uint64_t **pte_store = (uint64_t **) arg;
    *pte_store = ptep;
}

int
pgdir_walk(struct Pagemap *pgmap, const void *va,
	   int create, uint64_t **pte_store)
{
    *pte_store = 0;
    return page_map_traverse(pgmap, va, va, create, &pgdir_walk_cb, pte_store);
}

//
// Return the page mapped at virtual address 'va'.
// If pte_store is not zero, then we store in it the address
// of the pte for this page.  This is used by page_remove
// but should not be used by other callers.
//
// Return 0 if there is no page mapped at va.
//
static void *
page_lookup(struct Pagemap *pgmap, void *va, uint64_t **pte_store)
{
    if ((uintptr_t) va >= ULIM)
	panic("page_lookup: va %p over ULIM", va);

    uint64_t *ptep;
    int r = pgdir_walk(pgmap, va, 0, &ptep);
    if (r < 0)
	panic("pgdir_walk(%p, create=0) failed: %d", va, r);

    if (pte_store)
	*pte_store = ptep;

    if (ptep == 0 || !(*ptep & PTE_P))
	return 0;

    return pa2kva (PTE_ADDR (*ptep));
}

int
check_user_access(const void *ptr, uint64_t nbytes, uint32_t reqflags)
{
    assert(cur_thread && cur_as);

    uintptr_t iptr = (uintptr_t) ptr;
    if (iptr >= ULIM || iptr + nbytes > ULIM)
	return -E_INVAL;

    uint64_t pte_flags = PTE_P | PTE_U;
    if ((reqflags & SEGMAP_WRITE))
	pte_flags |= PTE_W;

    if (nbytes > 0) {
	uintptr_t start = (uintptr_t) ROUNDDOWN(ptr, PGSIZE);
	uintptr_t end = (uintptr_t) ROUNDUP(ptr + nbytes, PGSIZE);
	for (uintptr_t va = start; va < end; va += PGSIZE) {
	    if (va >= ULIM)
		return -E_INVAL;

	    uint64_t *ptep;
	    if (page_lookup(cur_as->as_pgmap, (void *) va, &ptep) &&
		(*ptep & pte_flags) == pte_flags)
		continue;

	    int r = as_pagefault(cur_as, (void *) va, reqflags);
	    if (r < 0)
		return r;
	}
    }

    // Flush any stale TLB entries that might have arisen from as_pagefault()
    as_switch(cur_as);

    return 0;
}
