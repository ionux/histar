#include <inc/memlayout.h>
#include <inc/lib.h>
#include <inc/setjmp.h>
#include <inc/stdio.h>
#include <inc/syscall.h>
#include <inc/thread.h>
#include <inc/assert.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/pthread.h>

#define NMAPPINGS 32
static struct u_segment_mapping ents[NMAPPINGS];
static struct u_address_space uas = { .size = NMAPPINGS, .ents = &ents[0] };
static pthread_mutex_t as_mutex;

static struct ulabel *seg_create_label;

void
segment_set_default_label(struct ulabel *l)
{
    if (seg_create_label && seg_create_label != l)
	label_free(seg_create_label);
    seg_create_label = l;
}

struct ulabel *
segment_get_default_label(void)
{
    return seg_create_label;
}

static void
as_mutex_lock(void) {
    pthread_mutex_lock(&as_mutex);
}

static void
as_mutex_unlock(void) {
    pthread_mutex_unlock(&as_mutex);
}

static void
segment_map_print(struct u_address_space *as)
{
    cprintf("segment  start  npages  f  va\n");
    for (uint64_t i = 0; i < as->nent; i++) {
	if (as->ents[i].flags == 0)
	    continue;
	cprintf("%3ld.%-3ld  %5ld  %6ld  %ld  %p\n",
		as->ents[i].segment.container,
		as->ents[i].segment.object,
		as->ents[i].start_page,
		as->ents[i].num_pages,
		as->ents[i].flags,
		as->ents[i].va);
    }
}

int
segment_unmap(void *va)
{
    struct cobj_ref as_ref;
    int r = sys_thread_get_as(&as_ref);
    if (r < 0)
	return r;

    as_mutex_lock();
    r = sys_as_get(as_ref, &uas);
    if (r < 0) {
	as_mutex_unlock();
	return r;
    }

    for (uint64_t i = 0; i < uas.nent; i++) {
	if (uas.ents[i].va == va && uas.ents[i].flags) {
	    uas.ents[i].flags = 0;
	    r = sys_as_set(as_ref, &uas);
	    as_mutex_unlock();
	    return r;
	}
    }

    as_mutex_unlock();
    return -E_INVAL;
}

int
segment_lookup(void *va, struct cobj_ref *seg, uint64_t *npage)
{
    struct cobj_ref as_ref;
    int r = sys_thread_get_as(&as_ref);
    if (r < 0)
	return r;

    as_mutex_lock();
    r = sys_as_get(as_ref, &uas);
    if (r < 0) {
	as_mutex_unlock();
	return r;
    }

    for (uint64_t i = 0; i < uas.nent; i++) {
	void *va_start = uas.ents[i].va;
	void *va_end = uas.ents[i].va + uas.ents[i].num_pages * PGSIZE;
	if (va >= va_start && va < va_end) {
	    if (seg)
		*seg = uas.ents[i].segment;
	    if (npage)
		*npage = (va - va_start) / PGSIZE;
	    as_mutex_unlock();
	    return 1;
	}
    }

    as_mutex_unlock();
    return 0;
}

int
segment_lookup_obj(uint64_t oid, void **vap)
{
    struct cobj_ref as_ref;
    int r = sys_thread_get_as(&as_ref);
    if (r < 0)
	return r;

    as_mutex_lock();
    r = sys_as_get(as_ref, &uas);
    if (r < 0) {
	as_mutex_unlock();
	return r;
    }

    for (uint64_t i = 0; i < uas.nent; i++) {
	if (uas.ents[i].segment.object == oid) {
	    if (vap)
		*vap = uas.ents[i].va;
	    as_mutex_unlock();
	    return 1;
	}
    }

    as_mutex_unlock();
    return 0;
}

int
segment_map(struct cobj_ref seg, uint64_t flags,
	    void **va_p, uint64_t *bytes_store)
{
    struct cobj_ref as;
    int r = sys_thread_get_as(&as);
    if (r < 0)
	return r;

    return segment_map_as(as, seg, flags, va_p, bytes_store);
}

int
segment_map_as(struct cobj_ref as_ref, struct cobj_ref seg,
	       uint64_t flags, void **va_p, uint64_t *bytes_store)
{
    if (!(flags & SEGMAP_READ)) {
	cprintf("segment_map: unreadable mappings not supported\n");
	return -E_INVAL;
    }

    int64_t npages = sys_segment_get_npages(seg);
    if (npages < 0)
	return npages;
    uint64_t bytes = npages * PGSIZE;

    as_mutex_lock();
    memset(&ents, 0, sizeof(ents));
    int r = sys_as_get(as_ref, &uas);
    if (r < 0) {
	as_mutex_unlock();
	return r;
    }

    int free_segslot = uas.nent;
    char *va_start = (char *) UMMAPBASE;
    char *va_end;

    bool_t fixed_va = 0;
    if (va_p && *va_p) {
	fixed_va = 1;
	va_start = *va_p;

	if (va_start >= (char *) ULIM) {
	    cprintf("segment_map: VA %p over ulim\n", va_start);
	    as_mutex_unlock();
	    return -E_INVAL;
	}
    }

retry:
    va_end = va_start + bytes;
    for (uint64_t i = 0; i < uas.nent; i++) {
	// If it's the same segment we're trying to map, allow remapping
	if (fixed_va && uas.ents[i].flags &&
	    uas.ents[i].segment.object == seg.object &&
	    uas.ents[i].va == va_start &&
	    uas.ents[i].start_page == 0)
	{
	    uas.ents[i].flags = 0;
	}

	if (uas.ents[i].flags == 0) {
	    free_segslot = i;
	    continue;
	}

	char *m_start = uas.ents[i].va;
	char *m_end = m_start + uas.ents[i].num_pages * PGSIZE;

	// Leave unmapped gaps between mappings, when possible
	if (!fixed_va && m_start <= va_end && m_end >= va_start) {
	    va_start = m_end + PGSIZE;
	    goto retry;
	}

	if (fixed_va && m_start < va_end && m_end > va_start) {
	    cprintf("segment_map: fixed VA %p busy\n", va_start);
	    as_mutex_unlock();
	    return -E_NO_MEM;
	}
    }

    if (!fixed_va && va_end >= (char*) USTACKTOP) {
	cprintf("out of virtual address space!\n");
	as_mutex_unlock();
	return -E_NO_MEM;
    }

    if (free_segslot >= NMAPPINGS) {
	cprintf("out of segment map slots\n");
	segment_map_print(&uas);
	as_mutex_unlock();
	return -E_NO_MEM;
    }

    uas.ents[free_segslot].segment = seg;
    uas.ents[free_segslot].start_page = 0;
    uas.ents[free_segslot].num_pages = npages;
    uas.ents[free_segslot].flags = flags;
    uas.ents[free_segslot].va = va_start;
    uas.nent = NMAPPINGS;

    r = sys_as_set(as_ref, &uas);
    as_mutex_unlock();
    if (r < 0)
	return r;

    if (bytes_store)
	*bytes_store = bytes;
    if (va_p)
	*va_p = va_start;
    return 0;
}

int
segment_alloc(uint64_t container, uint64_t bytes,
	      struct cobj_ref *cobj, void **va_p,
	      struct ulabel *label, const char *name)
{
    if (label == 0)
	label = seg_create_label;

    uint64_t npages = ROUNDUP(bytes, PGSIZE) / PGSIZE;
    int64_t id = sys_segment_create(container, npages, label, name);
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
