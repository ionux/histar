#ifndef JOS_KERN_ARCH_H
#define JOS_KERN_ARCH_H

#include <machine/types.h>
#include <machine/memlayout.h>

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
 * Miscellaneous
 */
extern char boot_cmdline[];
void machine_reboot(void);

/*
 * Page map manipulation
 */
struct Pagemap;
void pmap_tlb_invlpg(const void *va);
void pmap_set_current(struct Pagemap *pm, int flush_tlb);

#endif
