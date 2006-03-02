#include <machine/thread.h>
#include <machine/pmap.h>
#include <machine/as.h>
#include <kern/container.h>
#include <kern/segment.h>
#include <kern/gate.h>
#include <kern/kobj.h>
#include <kern/pstate.h>
#include <kern/handle.h>
#include <kern/timer.h>
#include <kern/ht.h>
#include <inc/error.h>
#include <inc/cksum.h>

struct kobject_list ko_list;
struct Thread_list kobj_snapshot_waiting;

static HASH_TABLE(kobject_hash, struct kobject_list, 8191) ko_hash;
static struct kobject_list ko_gc_list;

static int kobject_reclaim_debug = 1;
static int kobject_checksum_pedantic = 0;
static int kobject_print_sizes = 0;

struct kobject *
kobject_h2k(struct kobject_hdr *kh)
{
    return (struct kobject *) kh;
}

static struct kobject *
kobject_const_h2k(const struct kobject_hdr *kh)
{
    return (struct kobject *) kh;
}

static uint64_t
kobject_cksum(const struct kobject_hdr *ko)
{
    assert(ko);
    uint64_t sum = 0;

    // Compute checksum on the persistent parts of kobject_hdr
    sum = cksum(sum, ko, offsetof(struct kobject_hdr, ko_cksum));
    sum = cksum(sum, (uint8_t *) ko + sizeof(struct kobject_hdr),
		     sizeof(struct kobject_persistent) -
		     sizeof(struct kobject_hdr));

    for (uint64_t i = 0; i < ko->ko_nbytes; i += PGSIZE) {
	void *p;
	assert(0 == kobject_get_page(ko, i / PGSIZE, &p, page_ro));
	assert(p);
	sum = cksum(sum, p, MIN((uint32_t) PGSIZE, ko->ko_nbytes - i * PGSIZE));
    }

    return sum;
}

static int
kobject_iflow_check(struct kobject_hdr *ko, info_flow_type iflow)
{
    int r;

    if (cur_thread == 0)
	return 0;

    if (SAFE_EQUAL(iflow, iflow_read)) {
	r = label_compare(&ko->ko_label, &cur_thread->th_ko.ko_label,
			  label_leq_starok);
    } else if (SAFE_EQUAL(iflow, iflow_write)) {
	r = label_compare(&cur_thread->th_ko.ko_label, &ko->ko_label,
			  label_leq_starlo);
    } else if (SAFE_EQUAL(iflow, iflow_rw)) {
	r = kobject_iflow_check(ko, iflow_read) ? :
	    kobject_iflow_check(ko, iflow_write);
    } else if (SAFE_EQUAL(iflow, iflow_none)) {
	r = 0;
    } else {
	panic("kobject_get: unknown iflow type %d\n", SAFE_UNWRAP(iflow));
    }

    return r;
}

int
kobject_get(kobject_id_t id, const struct kobject **kp, info_flow_type iflow)
{
    if (id == kobject_id_thread_ct)
	id = cur_thread->th_ct;

    if (id == kobject_id_thread_sg)
	id = cur_thread->th_sg;

    struct kobject *ko;
    LIST_FOREACH(ko, HASH_SLOT(&ko_hash, id), ko_hash) {
	if (ko->hdr.ko_id == id) {
	    int r = kobject_iflow_check(&ko->hdr, iflow);
	    if (r < 0)
		return r;

	    *kp = ko;
	    return 0;
	}
    }

    // Should match the code returned by container_find() when trying
    // to get an object of the wrong type.
    if (kobject_negative_contains(id))
	return -E_INVAL;

    return pstate_swapin(id);
}

int
kobject_alloc(kobject_type_t type, const struct Label *l,
	      struct kobject **kp)
{
    int r;

    if (cur_thread) {
	r = label_compare(&cur_thread->th_ko.ko_label, l, label_leq_starlo);
	if (r < 0)
	    return r;
    }

    void *p;
    r = page_alloc(&p);
    if (r < 0)
	return r;

    struct kobject_pair *ko_pair = (struct kobject_pair *) p;
    static_assert(sizeof(struct kobject) == KOBJ_MEM_SIZE);
    static_assert(sizeof(struct kobject_persistent) == KOBJ_DISK_SIZE);
    static_assert(sizeof(*ko_pair) <= PGSIZE);

    struct kobject *ko = &ko_pair->active;
    memset(ko, 0, sizeof(*ko));

    struct kobject_hdr *kh = &ko->hdr;
    kh->ko_type = type;
    kh->ko_id = handle_alloc();
    kh->ko_label = *l;
    kh->ko_flags = KOBJ_DIRTY;
    pagetree_init(&ko->ko_pt);

    kobject_negative_remove(kh->ko_id);
    LIST_INSERT_HEAD(&ko_list, ko, ko_link);
    LIST_INSERT_HEAD(&ko_gc_list, ko, ko_gc_link);
    LIST_INSERT_HEAD(HASH_SLOT(&ko_hash, kh->ko_id), ko, ko_hash);

    *kp = ko;
    return 0;
}

int
kobject_get_page(const struct kobject_hdr *kp, uint64_t npage, void **pp, page_rw_mode rw)
{
    if (npage >= kobject_npages(kp))
	return -E_INVAL;

    if ((kp->ko_flags & KOBJ_SNAPSHOTING) && SAFE_EQUAL(rw, page_rw) && kp->ko_pin_pg) {
	thread_suspend(cur_thread, &kobj_snapshot_waiting);
	return -E_RESTART;
    }

    if (SAFE_EQUAL(rw, page_rw))
	kobject_dirty(kp);

    int r = pagetree_get_page(&kobject_const_h2k(kp)->ko_pt,
			      npage, pp, rw);
    if (r == 0 && *pp == 0)
	panic("kobject_get_page: id %ld (%s) type %d npage %ld null",
	      kp->ko_id, kp->ko_name, kp->ko_type, npage);
    return r;
}

uint64_t
kobject_npages(const struct kobject_hdr *kp)
{
    return ROUNDUP(kp->ko_nbytes, PGSIZE) / PGSIZE;
}

int
kobject_set_nbytes(struct kobject_hdr *kp, uint64_t nbytes)
{
    if (nbytes < kp->ko_min_bytes)
	return -E_RANGE;

    struct kobject *ko = kobject_h2k(kp);

    uint64_t npages = ROUNDUP(nbytes, PGSIZE) / PGSIZE;
    if (npages > pagetree_maxpages())
	return -E_RANGE;

    for (uint64_t i = npages; i < kobject_npages(kp); i++) {
	int r = pagetree_put_page(&ko->ko_pt, i, 0);
	if (r < 0) {
	    cprintf("XXX this leaves a hole in the kobject\n");
	    return r;
	}
    }

    for (uint64_t i = kobject_npages(kp); i < npages; i++) {
	void *p;
	int r = page_alloc(&p);
	if (r == 0)
	    r = pagetree_put_page(&ko->ko_pt, i, p);

	if (r < 0) {
	    // free all the pages we allocated up to now
	    for (uint64_t j = kobject_npages(kp); j < i; j++)
		assert(0 == pagetree_put_page(&ko->ko_pt, j, 0));
	    return r;
	}

	memset(p, 0, PGSIZE);
    }

    kp->ko_nbytes = nbytes;
    return 0;
}

struct kobject *
kobject_dirty(const struct kobject_hdr *kh)
{
    struct kobject *ko = kobject_const_h2k(kh);
    ko->hdr.ko_flags |= KOBJ_DIRTY;
    return ko;
}

void
kobject_swapin(struct kobject *ko)
{
    uint64_t sum1 = ko->hdr.ko_cksum;
    uint64_t sum2 = kobject_cksum(&ko->hdr);

    if (sum1 != sum2)
	cprintf("kobject_swapin: %ld (%s) checksum mismatch: 0x%lx != 0x%lx\n",
		ko->hdr.ko_id, ko->hdr.ko_name, sum1, sum2);

    struct kobject *kx;
    LIST_FOREACH(kx, &ko_list, ko_link)
	if (ko->hdr.ko_id == kx->hdr.ko_id)
	    panic("kobject_swapin: duplicate %ld (%s)",
		  ko->hdr.ko_id, ko->hdr.ko_name);

    kobject_negative_remove(ko->hdr.ko_id);
    LIST_INSERT_HEAD(&ko_list, ko, ko_link);
    if (ko->hdr.ko_ref == 0)
	LIST_INSERT_HEAD(&ko_gc_list, ko, ko_gc_link);
    LIST_INSERT_HEAD(HASH_SLOT(&ko_hash, ko->hdr.ko_id), ko, ko_hash);

    ko->hdr.ko_pin = 0;
    ko->hdr.ko_pin_pg = 0;
    ko->hdr.ko_flags &= ~(KOBJ_SNAPSHOTING |
			    KOBJ_DIRTY |
			    KOBJ_SNAPSHOT_DIRTY);

    if (ko->hdr.ko_type == kobj_thread)
	thread_swapin(&ko->th);
    if (ko->hdr.ko_type == kobj_address_space)
	as_swapin(&ko->as);
    if (ko->hdr.ko_type == kobj_segment)
	segment_swapin(&ko->sg);
}

void
kobject_incref(const struct kobject_hdr *kh)
{
    struct kobject *ko = kobject_dirty(kh);
    if (ko->hdr.ko_ref == 0)
	LIST_REMOVE(ko, ko_gc_link);
    ko->hdr.ko_ref++;
}

void
kobject_decref(const struct kobject_hdr *kh)
{
    struct kobject *ko = kobject_dirty(kh);
    ko->hdr.ko_ref--;
    if (ko->hdr.ko_ref == 0)
	LIST_INSERT_HEAD(&ko_gc_list, ko, ko_gc_link);
}

void
kobject_pin_hdr(const struct kobject_hdr *ko)
{
    struct kobject_hdr *m = &kobject_const_h2k(ko)->hdr;
    ++m->ko_pin;
}

void
kobject_unpin_hdr(const struct kobject_hdr *ko)
{
    struct kobject_hdr *m = &kobject_const_h2k(ko)->hdr;
    --m->ko_pin;

    if (m->ko_pin == (uint32_t) -1)
	panic("kobject_unpin_hdr: underflow for object %ld (%s)",
	      m->ko_id, m->ko_name);
}

void
kobject_pin_page(const struct kobject_hdr *ko)
{
    struct kobject_hdr *m = &kobject_const_h2k(ko)->hdr;
    ++m->ko_pin_pg;

    kobject_pin_hdr(ko);
}

void
kobject_unpin_page(const struct kobject_hdr *ko)
{
    struct kobject_hdr *m = &kobject_const_h2k(ko)->hdr;
    --m->ko_pin_pg;

    kobject_unpin_hdr(ko);
}

static void
kobject_gc(struct kobject *ko)
{
    int r = 0;

    switch (ko->hdr.ko_type) {
    case kobj_thread:
	r = thread_gc(&ko->th);
	break;

    case kobj_container:
	r = container_gc(&ko->ct);
	break;

    case kobj_address_space:
	r = as_gc(&ko->as);
	break;

    case kobj_mlt:
	r = mlt_gc(&ko->mt);
	break;

    case kobj_gate:
    case kobj_segment:
    case kobj_netdev:
	break;

    default:
	panic("kobject_free: unknown kobject type %d", ko->hdr.ko_type);
    }

    if (r == -E_RESTART)
	return;
    if (r < 0)
	cprintf("kobject_free: cannot GC type %d: %d\n", ko->hdr.ko_type, r);

    pagetree_free(&ko->ko_pt);
    ko->hdr.ko_nbytes = 0;
    ko->hdr.ko_type = kobj_dead;
}

static void
kobject_gc_scan(void)
{
    // Clear cur_thread to avoid putting it to sleep on behalf of
    // our swapped-in objects.
    const struct Thread *t = cur_thread;
    cur_thread = 0;

    struct kobject *ko;
    LIST_FOREACH(ko, &ko_gc_list, ko_gc_link) {
	if (ko->hdr.ko_ref == 0 && ko->hdr.ko_type != kobj_dead) {
	    if (ko->hdr.ko_type == kobj_thread)
		thread_zero_refs(&ko->th);
	    if (ko->hdr.ko_pin == 0)
		kobject_gc(kobject_dirty(&ko->hdr));
	}
    }

    cur_thread = t;
}

void
kobject_swapout(struct kobject *ko)
{
    if (kobject_checksum_pedantic) {
	uint64_t sum1 = ko->hdr.ko_cksum;
	uint64_t sum2 = kobject_cksum(&ko->hdr);

	if (sum1 != sum2)
	    cprintf("kobject_swapout: %ld (%s) checksum mismatch: 0x%lx != 0x%lx\n",
		    ko->hdr.ko_id, ko->hdr.ko_name, sum1, sum2);
    }

    assert(ko->hdr.ko_pin == 0);
    assert(!(ko->hdr.ko_flags & KOBJ_SNAPSHOTING));

    if (ko->hdr.ko_type == kobj_thread)
	thread_swapout(&ko->th);
    if (ko->hdr.ko_type == kobj_address_space)
	as_swapout(&ko->as);

    LIST_REMOVE(ko, ko_link);
    if (ko->hdr.ko_ref == 0)
	LIST_REMOVE(ko, ko_gc_link);
    LIST_REMOVE(ko, ko_hash);
    pagetree_free(&ko->ko_pt);
    page_free(ko);
}

static struct kobject *
kobject_get_snapshot_internal(struct kobject_hdr *ko)
{
    struct kobject_pair *kp = (struct kobject_pair *) ko;
    return &kp->snapshot;
}

struct kobject *
kobject_get_snapshot(struct kobject_hdr *ko)
{
    assert((ko->ko_flags & KOBJ_SNAPSHOTING));
    struct kobject *snap = kobject_get_snapshot_internal(ko);

    if (kobject_checksum_pedantic) {
	uint64_t sum = kobject_cksum(&snap->hdr);
	if (sum != ko->ko_cksum)
	    cprintf("kobject_get_snapshot(%ld, %s): cksum changed 0x%lx -> 0x%lx\n",
		    ko->ko_id, ko->ko_name, ko->ko_cksum, sum);
    }

    return snap;
}

void
kobject_snapshot(struct kobject_hdr *ko)
{
    assert(!(ko->ko_flags & KOBJ_SNAPSHOTING));

    if (ko->ko_type == kobj_segment)
	segment_snapshot(&kobject_h2k(ko)->sg);

    ko->ko_flags &= ~KOBJ_DIRTY;
    ko->ko_cksum = kobject_cksum(ko);
    kobject_pin_hdr(ko);

    struct kobject *snap = kobject_get_snapshot_internal(ko);
    memcpy(snap, ko, KOBJ_DISK_SIZE);
    pagetree_copy(&kobject_h2k(ko)->ko_pt, &snap->ko_pt);

    ko->ko_flags |= KOBJ_SNAPSHOTING;
}

void
kobject_snapshot_release(struct kobject_hdr *ko)
{
    struct kobject *snap = kobject_get_snapshot(ko);

    ko->ko_flags &= ~KOBJ_SNAPSHOTING;
    kobject_unpin_hdr(ko);
    pagetree_free(&snap->ko_pt);

    while (!LIST_EMPTY(&kobj_snapshot_waiting)) {
	struct Thread *t = LIST_FIRST(&kobj_snapshot_waiting);
	thread_set_runnable(t);
    }
}

// Negative kobject id cache (objects that don't exist)
enum { kobject_neg_nent = 1024 };
static struct {
    int inited;
    int next;
    kobject_id_t ents[kobject_neg_nent];
} kobject_neg;

void
kobject_negative_insert(kobject_id_t id)
{
    if (!kobject_neg.inited)
	for (int i = 0; i < kobject_neg_nent; i++)
	    kobject_neg.ents[i] = kobject_id_null;

    kobject_neg.inited = 1;
    kobject_neg.ents[kobject_neg.next] = id;
    kobject_neg.next = (kobject_neg.next + 1) % kobject_neg_nent;
}

void
kobject_negative_remove(kobject_id_t id)
{
    if (!kobject_neg.inited)
	return;

    for (int i = 0; i < kobject_neg_nent; i++)
	if (kobject_neg.ents[i] == id)
	    kobject_neg.ents[i] = kobject_id_null;
}

bool_t
kobject_negative_contains(kobject_id_t id)
{
    if (!kobject_neg.inited)
	return 0;

    for (int i = 0; i < kobject_neg_nent; i++)
	if (kobject_neg.ents[i] == id)
	    return 1;

    return 0;
}

bool_t
kobject_initial(const struct kobject *ko)
{
    if ((ko->hdr.ko_flags & KOBJ_PIN_IDLE))
	return 1;

    if (ko->hdr.ko_ref == 0)
	return 1;

    if (ko->hdr.ko_type == kobj_thread)
	return SAFE_EQUAL(ko->th.th_status, thread_runnable) ||
	       SAFE_EQUAL(ko->th.th_status, thread_suspended);

    return 0;
}

static void
kobject_reclaim(void)
{
    // A rather simple heuristic for when to clean up
    if (page_stats.pages_avail > global_npages / 4)
	return;

    const struct Thread *t = cur_thread;
    cur_thread = 0;

    struct kobject *next;
    for (struct kobject *ko = LIST_FIRST(&ko_list); ko != 0; ko = next) {
	next = LIST_NEXT(ko, ko_link);

	if (ko->hdr.ko_pin || kobject_initial(ko))
	    continue;
	if ((ko->hdr.ko_flags & (KOBJ_DIRTY | KOBJ_SNAPSHOT_DIRTY)))
	    continue;

	if (kobject_reclaim_debug)
	    cprintf("kobject_reclaim: swapping out %ld (%s)\n",
		    ko->hdr.ko_id, ko->hdr.ko_name);

	kobject_swapout(ko);
    }

    cur_thread = t;
}

void
kobject_init(void)
{
    static struct periodic_task gc_pt =
	{ .pt_fn = &kobject_gc_scan, .pt_interval_ticks = 1 };
    timer_add_periodic(&gc_pt);

    static struct periodic_task reclaim_pt =
	{ .pt_fn = &kobject_reclaim };
    reclaim_pt.pt_interval_ticks = kclock_hz;
    timer_add_periodic(&reclaim_pt);

    if (kobject_print_sizes) {
	cprintf("kobject sizes:\n");
	cprintf("hdr %ld label %ld\n",
		sizeof(struct kobject_hdr), sizeof(struct Label));
	cprintf("ct %ld th %ld gt %ld as %ld sg %ld mlt %ld\n",
		sizeof(struct Container),
		sizeof(struct Thread),
		sizeof(struct Gate),
		sizeof(struct Address_space),
		sizeof(struct Segment),
		sizeof(struct Mlt));
    }
}
