#include <inc/memlayout.h>
#include <inc/lib.h>
#include <inc/setjmp.h>
#include <inc/stdio.h>
#include <inc/syscall.h>
#include <inc/thread.h>
#include <inc/assert.h>
#include <inc/error.h>
#include <inc/string.h>

#define NMAPPINGS	16

void
segment_map_print(struct u_address_space *uas)
{
    cprintf("segment  start  npages  f  va\n");
    for (int i = 0; i < uas->nent; i++) {
	if (uas->ents[i].flags == 0)
	    continue;
	cprintf("%3ld.%-3ld  %5ld  %6ld  %ld  %p\n",
		uas->ents[i].segment.container,
		uas->ents[i].segment.object,
		uas->ents[i].start_page,
		uas->ents[i].num_pages,
		uas->ents[i].flags,
		uas->ents[i].va);
    }
}

int
segment_unmap(void *va)
{
    struct segment_mapping ents[NMAPPINGS];
    struct u_address_space uas = { .size = NMAPPINGS, .ents = &ents[0] };

    struct cobj_ref as_ref;
    int r = sys_thread_get_as(&as_ref);
    if (r < 0)
	return r;

    r = sys_as_get(as_ref, &uas);
    if (r < 0)
	return r;

    for (int i = 0; i < uas.nent; i++) {
	if (uas.ents[i].va == va && uas.ents[i].flags) {
	    uas.ents[i].flags = 0;
	    return sys_as_set(as_ref, &uas);
	}
    }

    return -E_INVAL;
}

int
segment_map(struct cobj_ref seg, uint64_t flags,
	    void **va_store, uint64_t *bytes_store)
{
    if (!(flags & SEGMAP_READ)) {
	cprintf("segment_map: unreadable mappings not supported\n");
	return -E_INVAL;
    }

    int64_t npages = sys_segment_get_npages(seg);
    if (npages < 0)
	return npages;
    uint64_t bytes = npages * PGSIZE;

    struct cobj_ref as_ref;
    int r = sys_thread_get_as(&as_ref);
    if (r < 0)
	return r;

    struct segment_mapping ents[NMAPPINGS];
    memset(&ents, 0, sizeof(ents));

    struct u_address_space uas = { .size = NMAPPINGS, .ents = &ents[0] };
    r = sys_as_get(as_ref, &uas);
    if (r < 0)
	return r;

    int free_segslot = uas.nent;
    char *va_start = (char *) 0x100000000;
    char *va_end;

retry:
    va_end = va_start + bytes;
    for (int i = 0; i < uas.nent; i++) {
	if (uas.ents[i].flags == 0) {
	    free_segslot = i;
	    continue;
	}

	char *m_start = uas.ents[i].va;
	char *m_end = m_start + uas.ents[i].num_pages * PGSIZE;

	if (m_start <= va_end && m_end >= va_start) {
	    va_start = m_end + PGSIZE;
	    goto retry;
	}
    }

    if (va_end >= (char*)ULIM) {
	cprintf("out of virtual address space!\n");
	return -E_NO_MEM;
    }

    if (free_segslot >= NMAPPINGS) {
	cprintf("out of segment map slots\n");
	return -E_NO_MEM;
    }

    uas.ents[free_segslot].segment = seg;
    uas.ents[free_segslot].start_page = 0;
    uas.ents[free_segslot].num_pages = npages;
    uas.ents[free_segslot].flags = flags;
    uas.ents[free_segslot].va = va_start;
    uas.nent = NMAPPINGS;

    r = sys_as_set(as_ref, &uas);
    if (r < 0)
	return r;

    if (bytes_store)
	*bytes_store = bytes;
    if (va_store)
	*va_store = va_start;
    return 0;
}

int
segment_alloc(uint64_t container, uint64_t bytes,
	      struct cobj_ref *cobj, void **va_p)
{
    uint64_t npages = ROUNDUP(bytes, PGSIZE) / PGSIZE;
    int64_t id = sys_segment_create(container, npages);
    if (id < 0)
	return id;

    *cobj = COBJ(container, id);

    if (va_p) {
	uint64_t mapped_bytes;
	int r = segment_map(*cobj, SEGMAP_READ | SEGMAP_WRITE,
			    va_p, &mapped_bytes);
	if (r < 0) {
	    sys_obj_unref(*cobj);
	    return r;
	}

	if (mapped_bytes != npages * PGSIZE) {
	    segment_unmap(*va_p);
	    sys_obj_unref(*cobj);
	    return -E_AGAIN;	// race condition maybe..
	}
    }

    return 0;
}
