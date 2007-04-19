#ifndef JOS_KERN_ARCH_H
#define JOS_KERN_ARCH_H

#include <machine/param.h>
#include <machine/types.h>
#include <machine/memlayout.h>
#include <machine/pmap.h>
#include <kern/thread.h>

/*
 * Page allocation
 */
extern size_t global_npages;

extern struct page_stats {
    uint64_t pages_used;
    uint64_t pages_avail;
    uint64_t allocations;
    uint64_t failures;
} page_stats;

int  page_alloc(void **p)
    __attribute__ ((warn_unused_result));
void page_free(void *p);

/*
 * Page table (Pagemap) handling
 */
int  page_map_alloc(struct Pagemap **pm_store)
    __attribute__ ((warn_unused_result));
void page_map_free(struct Pagemap *pgmap);

/* Traverse [first .. last]; clamps last down to ULIM-PGSIZE */
typedef void (*page_map_traverse_cb)(const void *arg, ptent_t *ptep, void *va);
int  page_map_traverse(struct Pagemap *pgmap, const void *first,
		       const void *last, int create,
		       page_map_traverse_cb cb, const void *arg)
    __attribute__ ((warn_unused_result));

/* Get (and possibly create) the PTE entry for va; clamps down to ULIM-PGSIZE */
int  pgdir_walk(struct Pagemap *pgmap, const void *va,
		int create, ptent_t **pte_store)
    __attribute__ ((warn_unused_result));

/* Physical address handling */
void *pa2kva(physaddr_t pa);
physaddr_t kva2pa(void *kva);
ppn_t pa2ppn(physaddr_t pa);
physaddr_t ppn2pa(ppn_t pn);

/*
 * Miscellaneous
 */
extern char boot_cmdline[];
void machine_reboot(void);
uintptr_t karch_get_sp(void);
uintptr_t karch_get_tsc(void);

/*
 * Page map manipulation
 */
struct Pagemap;
void pmap_tlb_invlpg(const void *va);
void pmap_set_current(struct Pagemap *pm, int flush_tlb);

/*
 * Checks that [ptr .. ptr + nbytes) is valid user memory,
 * and makes sure the address is paged in (might return -E_RESTART).
 * Checks for writability if (reqflags & SEGMAP_WRITE).
 */
int  check_user_access(const void *ptr, uint64_t nbytes, uint32_t reqflags)
    __attribute__ ((warn_unused_result));

/*
 * Threads and traps
 */
void thread_arch_run(const struct Thread *t)
     __attribute__((__noreturn__));

#endif
