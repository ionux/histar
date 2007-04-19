#ifndef JOS_INC_PMAP_H
#define JOS_INC_PMAP_H

#include <machine/mmu.h>
#include <machine/memlayout.h>
#ifndef __ASSEMBLER__
#include <kern/lib.h>
#include <kern/arch.h>
#include <inc/queue.h>
#endif /* !__ASSEMBLER__ */

#define GD_KT	    (0x08 | 0x00)	/* Kernel text */
#define GD_TSS	    (0x10 | 0x00)	/* Task segment selector */
#define GD_TSS2	    (0x18 | 0x00)	/* TSS is a 16-byte descriptor */
#define GD_UD	    (0x20 | 0x03)	/* User data segment for iretq */
#define GD_UT_NMASK (0x28 | 0x03)	/* User text, traps not masked */
#define GD_UT_MASK  (0x30 | 0x03)	/* User text, traps masked */

/* bootdata.c */
#ifndef __ASSEMBLER__

/* pmap.c */
struct Pagemapent {
    void *va;
    uint64_t pte;
};

#define NPME 2
struct Pagemap {
    struct Pagemapent pme[NPME];
};

void page_init(uint64_t lower_kb, uint64_t upper_kb);

int  page_map_alloc(struct Pagemap **pm_store)
    __attribute__ ((warn_unused_result));
void page_map_free(struct Pagemap *pgmap);

typedef void (*page_map_traverse_cb)(const void *arg, uint64_t *ptep, void *va);
int  page_map_traverse(struct Pagemap *pgmap, const void *first, const void *last,
		       int create, page_map_traverse_cb cb, const void *arg)
    __attribute__ ((warn_unused_result));

int  pgdir_walk(struct Pagemap *pgmap, const void *va,
	       int create, uint64_t **pte_store)
    __attribute__ ((warn_unused_result));

void *pa2kva(physaddr_t pa);
physaddr_t kva2pa(void *kva);
ppn_t pa2ppn(physaddr_t pa);
physaddr_t ppn2pa(ppn_t pn);

#endif /* !__ASSEMBLER__ */

#endif /* !JOS_INC_PMAP_H */