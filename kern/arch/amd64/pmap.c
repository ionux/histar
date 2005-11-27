/* See COPYRIGHT for copyright information. */

#include <machine/x86.h>
#include <machine/pmap.h>
#include <machine/thread.h>
#include <inc/error.h>
#include <inc/stdarg.h>

#include <dev/kclock.h>
#include <kern/lib.h>
#include <machine/atomic.h>
#if 0
#include <kern/labels.h>
#include <kern/env.h>
#include <kern/asbestos.h>
#endif

// These variables are set by i386_detect_memory()
static physaddr_t maxpa;	// Maximum physical address
size_t npage;			// Amount of physical memory (in pages)
static size_t basemem;		// Amount of base memory (in bytes)
static size_t extmem;		// Amount of extended memory (in bytes)

// These variables are set in i386_vm_init()
static char *boot_freemem;	// Pointer to next byte of free mem
struct Page *pages;		// Virtual address of physical page array
static struct Page_list page_free_list;
				// Free list of physical pages

static int
nvram_read (int r)
{
  return mc146818_read (NULL, r) | (mc146818_read (NULL, r + 1) << 8);
}

static void
i386_detect_memory (void)
{
  // CMOS tells us how many kilobytes there are
  basemem = ROUNDDOWN (nvram_read (NVRAM_BASELO) * 1024, PGSIZE);
  extmem = ROUNDDOWN (nvram_read (NVRAM_EXTLO) * 1024, PGSIZE);

  // Calculate the maxmium physical address based on whether
  // or not there is any extended memory.  See comment in ../inc/mmu.h.
  if (extmem)
    maxpa = EXTPHYSMEM + extmem;
  else
    maxpa = basemem;

  npage = maxpa / PGSIZE;

  cprintf ("Physical memory: %dK available, ", (int) (maxpa / 1024));
  cprintf ("base = %dK, extended = %dK\n", (int) (basemem / 1024),
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
boot_alloc (uint32_t n, uint32_t align)
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

  // Your code here:
  //      Step 1: round boot_freemem up to be aligned properly
  //      Step 2: save current value of boot_freemem as allocated chunk
  //      Step 3: increase boot_freemem to record allocation
  //      Step 4: return allocated chunk

  boot_freemem = (char *) ROUNDUP (boot_freemem, align);
  if (boot_freemem + n < boot_freemem
      || boot_freemem + n > (char *) (maxpa + KERNBASE))
    panic ("out of memory during i386_vm_init");
  v = boot_freemem;
  boot_freemem += n;
  return v;
}

//
// Return a page to the free list.
// (This function should only be called when pp->pp_ref reaches 0.)
//
void
page_free (struct Page *pp)
{
  if (pp->pp_ref) {
    cprintf ("page_free: attempt to free mapped page - nothing done");
    return;	   /* be conservative and assume page is still used */
  }

  int ppn = page2ppn (pp);
  LIST_INSERT_HEAD (&page_free_list, &pages[ppn], pp_link);
}

//
// Initialize a Page structure.
// The result has null links and 0 refcount.
// Note that the corresponding physical page is NOT initialized!
//
static void
page_initpp (struct Page *pp)
{
  memset (pp, 0, sizeof (*pp));
}

//
// Allocates physical page(s).
// Does NOT set the contents of the physical page to zero -
// the caller must do that if necessary.
//
// *pp -- is set to point to the first Page struct of the newly allocated
// page(s)
//
// RETURNS 
//   0 -- on success
//   -E_NO_MEM -- otherwise 
//
// Hint: pp_ref should not be incremented
int
page_alloc (struct Page **pp_store)
{
  *pp_store = LIST_FIRST(&page_free_list);
  if (*pp_store) {
    LIST_REMOVE (*pp_store, pp_link);
    page_initpp (*pp_store);
    return 0;
  }

  cprintf("page alloc returned no mem\n");
  return -E_NO_MEM;
}

//  
// Initialize page structure and memory free list.  After this point,
// ONLY allocate and deallocate physical memory via the
// page_free_list, and NEVER use boot_alloc() or the related boot-time
// functions above.
//
static void
page_init (void)
{
  int inuse;
  ptrdiff_t i;

  size_t n = npage * sizeof (struct Page);
  pages = boot_alloc (n, PGSIZE);
  memset (pages, 0, n);

  // Align boot_freemem to page boundary.
  boot_alloc (0, PGSIZE);

  for (i = 0; i < npage; i++) {
    // Off-limits until proven otherwise.
    inuse = 1;

    // The bottom basemem bytes are free except page 0.
    if (i != 0 && i < basemem / PGSIZE)
      inuse = 0;

    // The IO hole and the kernel abut.

    // The memory past the kernel is free.
    if (i >= RELOC (boot_freemem) / PGSIZE)
      inuse = 0;

    pages[i].pp_ref = inuse;

    if (!inuse)
      page_free(&pages[i]);
  }
}

//
// Decrement the reference count on a page, freeing it if there are no more refs.
//
void
page_decref (struct Page *pp)
{
  if (--pp->pp_ref == 0)
    page_free (pp);
}

void
pmap_init (void)
{
  i386_detect_memory ();
  page_init ();
}

static int
page_map_clone_level (struct Pagemap *pgmap, struct Pagemap **pm_store, int pmlevel)
{
    struct Page *p;
    int r = page_alloc(&p);
    if (r < 0)
	return r;
    p->pp_ref++;

    struct Pagemap *clone = page2kva(p);
    *pm_store = clone;

    // COW only the user half of the address space
    int maxcow = (pmlevel == 3 ? NPTENTRIES/2 : NPTENTRIES);
    int i;

    for (i = 0; i < NPTENTRIES; i++) {
	if (i < maxcow && (pgmap->pm_ent[i] & PTE_P)) {
	    struct Page *entp = pa2page(PTE_ADDR(pgmap->pm_ent[i]));
	    entp->pp_ref++;
	    if ((pgmap->pm_ent[i] & (PTE_W | PTE_COW)))
		pgmap->pm_ent[i] = (pgmap->pm_ent[i] & ~PTE_W) | PTE_COW;
	}
	clone->pm_ent[i] = pgmap->pm_ent[i];
    }

    return 0;
}

int
page_map_clone (struct Pagemap *pgmap, struct Pagemap **pm_store)
{
    return page_map_clone_level (pgmap, pm_store, 3);
}

//
// Stores address of page table entry in *ppte.
// Stores 0 if there is no such entry or on error.
// 
// RETURNS: 
//   0 on success
//   -E_NO_MEM, if page table couldn't be allocated
//
static int
pgdir_walk (struct Pagemap *pgmap, int pmlevel, const void *va, int create, int do_cow, uint64_t **pte_store)
{
    assert(pmlevel >= 0 && pmlevel <= 3);

    uint64_t *pm_entp = &pgmap->pm_ent[PDX(pmlevel, va)];

    // If we made it all the way down, return the PTE
    if (pmlevel == 0) {
	*pte_store = pm_entp;
	return 0;
    }

    // If an intermediate page map is missing, allocate it
    if (!(*pm_entp & PTE_P)) {
	if (!create) {
	    *pte_store = 0;
	    return 0;
	}

	struct Page *pp;
	int r = page_alloc(&pp);
	if (r < 0)
	    return r;
	pp->pp_ref++;

	memset(page2kva(pp), 0, PGSIZE);
	*pm_entp = page2pa(pp) | PTE_P | PTE_U | PTE_W;
    }

    // If the intermediate map is COW (and do_cow is set), then do the copy.
    struct Pagemap *pm_next = page2kva(pa2page(PTE_ADDR(*pm_entp)));
    if (do_cow && (*pm_entp & PTE_COW)) {
	struct Page *old_pm_page = pa2page(kva2pa(pm_next));
	if (old_pm_page->pp_ref > 1) {
	    int r = page_map_clone_level(pm_next, &pm_next, pmlevel - 1);
	    if (r < 0)
		return r;

	    page_decref(old_pm_page);
	}
	*pm_entp = kva2pa(pm_next) | (PTE_FLAGS(*pm_entp) & ~PTE_COW);
    }

    return pgdir_walk(pm_next, pmlevel-1, va, create, do_cow, pte_store);
}

//
// Return the page mapped at virtual address 'va'.
// If pte_store is not zero, then we store in it the address
// of the pte for this page.  This is used by page_remove
// but should not be used by other callers.
//
// Return 0 if there is no page mapped at va.
//
// Hint: the TA solution uses pgdir_walk and pa2page.
//
struct Page *
page_lookup (struct Pagemap *pgmap, void *va, uint64_t **pte_store)
{
    uint64_t *ptep;
    int r = pgdir_walk(pgmap, 3, va, 0, 0, &ptep);
    if (r < 0)
	panic("pgdir_walk(create=0) failed: %d", r);

    if (pte_store)
	*pte_store = ptep;

    if (ptep == 0 || !(*ptep & PTE_P))
	return 0;

    return pa2page (PTE_ADDR (*ptep));
}

//
// Invalidate a TLB entry, but only if the page tables being
// edited are the ones currently in use by the processor.
//
static void
tlb_invalidate (struct Pagemap *pgmap, void *va)
{
    // Flush the entry only if we're modifying the current address space.
    if (cur_thread == 0 || cur_thread->th_pgmap == pgmap)
	invlpg(va);
}

//
// Unmaps the physical page at virtual address 'va'.
//
// Details:
//   - The ref count on the physical page should decrement.
//   - The physical page should be freed if the refcount reaches 0.
//   - The pg table entry corresponding to 'va' should be set to 0.
//     (if such a PTE exists)
//   - The TLB must be invalidated if you remove an entry from
//         the pg dir/pg table.
//
// Hint: The TA solution is implemented using page_lookup,
//      tlb_invalidate, and page_decref.
//
void
page_remove (struct Pagemap *pgmap, void *va)
{
    uint64_t *ptep;
    struct Page *pp = page_lookup(pgmap, va, &ptep);
    if (pp == 0)
	return;

    *ptep = 0;
    tlb_invalidate (pgmap, va);
    page_decref (pp);
}

//
// Map the physical page 'pp' at virtual address 'va'.
// The permissions (the low 12 bits) of the page table
//  entry should be set to 'perm|PTE_P'.
//
// Details
//   - If there is already a page mapped at 'va', it is page_remove()d.
//   - If necesary, on demand, allocates a page table and inserts it into 'pgdir'.
//   - pp->pp_ref should be incremented if the insertion succeeds
//
// RETURNS: 
//   0 on success
//   -E_NO_MEM, if page table couldn't be allocated
//
// Hint: The TA solution is implemented using
//   pgdir_walk() and and page_remove().
//
int
page_insert (struct Pagemap *pgmap, struct Page *pp, void *va, uint64_t perm)
{
    uint64_t *ptep;
    int r = pgdir_walk(pgmap, 3, va, 1, 1, &ptep);
    if (r < 0)
	return r;

    // We must increment pp_ref before page_remove, so that
    // if pp is already mapped at va (we're just changing perm),
    // we don't lose the page when we decref in page_remove.
    pp->pp_ref++;

    if (*ptep & PTE_P)
	page_remove (pgmap, va);

    *ptep = page2pa (pp) | perm | PTE_P;
    return 0;
}

static void
page_map_decref_level (struct Pagemap *pgmap, int pmlevel)
{
    struct Page *pgmap_p = pa2page(kva2pa(pgmap));
    if (--pgmap_p->pp_ref == 0) {
	// Skip the kernel half of the address space
	int maxi = (pmlevel == 3 ? NPTENTRIES/2 : NPTENTRIES);
	int i;

	for (i = 0; i < maxi; i++) {
	    uint64_t ptent = pgmap->pm_ent[i];
	    if (!(ptent & PTE_P))
		continue;

	    struct Page *p = pa2page(PTE_ADDR(ptent));
	    if (pmlevel == 0)
		page_decref(p);
	    else
		page_map_decref_level(page2kva(p), pmlevel - 1);
	}

	page_free (pgmap_p);
    }
}

void
page_map_decref (struct Pagemap *pgmap)
{
    page_map_decref_level (pgmap, 3);
}

int
page_cow (struct Pagemap *pgmap, void *va)
{
    uint64_t *ptep;
    int r = pgdir_walk(pgmap, 3, va, 0, 1, &ptep);
    if (r < 0)
	return r;

    if (ptep == 0 || !(*ptep & PTE_COW) || !(*ptep & PTE_P))
	return -E_INVAL;

    struct Page *old_page = pa2page(PTE_ADDR(*ptep));
    if (old_page->pp_ref > 1) {
	struct Page *new_page;
	r = page_alloc(&new_page);
	if (r < 0)
	    return r;
	new_page->pp_ref++;

	memcpy(page2kva(new_page), page2kva(old_page), PGSIZE);
	page_decref(old_page);
	*ptep = page2pa(new_page) | PTE_FLAGS(*ptep);
    }

    *ptep = (*ptep & ~PTE_COW) | PTE_W;
    return 0;
}
