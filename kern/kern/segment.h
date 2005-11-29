#ifndef JOS_KERN_SEGMENT_H
#define JOS_KERN_SEGMENT_H

#include <machine/types.h>
#include <machine/mmu.h>

struct segment_header {
    uint64_t num_pages;
    struct Label *label;
};

#define NUM_SG_PAGES	((PGSIZE - sizeof(struct segment_header)) / 8)
struct Segment {
    struct segment_header sg_hdr;
    struct Page *sg_page[NUM_SG_PAGES];
};

int  segment_alloc(struct Segment **sgp);
void segment_free(struct Segment *sg);
void segment_addref(struct Segment *sg);
void segment_decref(struct Segment *sg);
int  segment_set_length(struct Segment *sg, uint64_t num_pages);

#endif
