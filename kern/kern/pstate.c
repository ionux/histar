#include <kern/disk.h>
#include <kern/thread.h>
#include <kern/arch.h>
#include <kern/disklayout.h>
#include <kern/pstate.h>
#include <kern/uinit.h>
#include <kern/handle.h>
#include <kern/timer.h>
#include <kern/lib.h>
#include <kern/log.h>
#include <kern/stackwrap.h>
#include <kern/part.h>
#include <btree/cache.h>
#include <inc/error.h>

// verbose flags
enum { pstate_load_debug = 0 };
enum { pstate_swapin_debug = 0 };
enum { pstate_swapout_debug = 0 };
enum { pstate_swapout_object_debug = 0 };
enum { pstate_swapout_stats = 0 };

enum { scrub_disk_pages = 0 };
enum { commit_panic = 0 };

// Disk partition to use
struct part_desc *pstate_part;

// Lock serializing all disk access
static struct lock pstate_lock;
static struct Thread_list pstate_waiting;

// Authoritative copy of the header that's actually on disk.
static struct pstate_header stable_hdr;

// Global freelist for disk
struct freelist freelist;

struct mobject {
    offset_t off;
    uint64_t nbytes;
};

// A zero page (not necessarily aligned), for memcmp()
static uint8_t zero_page[PGSIZE];

//////////////////////////////////////////////////
// Encrypted pstate timestamps
//////////////////////////////////////////////////

static uint64_t pstate_counter;

uint64_t
pstate_ts_alloc(void)
{
    return bf61_encipher(&pstate_key_ctx, pstate_counter++);
}

static uint64_t
pstate_ts_decrypt(uint64_t v)
{
    return bf61_decipher(&pstate_key_ctx, v);
}

//////////////////////////////////////////////////
// Delayed pstate operations
//////////////////////////////////////////////////

enum {
    op_swapin = 1,
    op_swapout_obj,
    op_swapout_global,
    op_swapout_global_gc
};

static struct pstate_op_t {
    uint32_t prio;
    stackwrap_fn fn;
    uint64_t arg[3];
} pstate_op_queue;

static char pstate_op_stack[PGSIZE]
    __attribute__((aligned(PGSIZE), section(".data")));

void
pstate_op_check(void)
{
    while (pstate_op_queue.prio) {
	struct pstate_op_t op = pstate_op_queue;
	pstate_op_queue.prio = 0;

	if (op.prio == op_swapout_global_gc) {
	    /* Garbage-collect before writing garbage to disk */
	    kobject_gc_scan();
	    kobject_reclaim();
	}

	if (lock_try_acquire(&pstate_lock) < 0)
	    return;

	stackwrap_call_stack(&pstate_op_stack[0], 0, op.fn,
			     op.arg[0], op.arg[1], op.arg[2]);
    }
}

static void
pstate_op_schedule(uint32_t prio, stackwrap_fn fn,
		   uint64_t a0, uint64_t a1, uint64_t a2)
{
    if (prio > pstate_op_queue.prio) {
	pstate_op_queue.prio = prio;
	pstate_op_queue.fn = fn;
	pstate_op_queue.arg[0] = a0;
	pstate_op_queue.arg[1] = a1;
	pstate_op_queue.arg[2] = a2;
    }
}

//////////////////////////////////////////////////
// Object map
//////////////////////////////////////////////////

static void
pstate_kobj_free(struct freelist *f, struct kobject *ko)
{
    uint64_t key;
    struct mobject mobj;

    int r = btree_search(BTREE_OBJMAP, &ko->hdr.ko_id, &key, (uint64_t *)&mobj);
    if (r == 0) {
	assert(key == ko->hdr.ko_id);

	if (scrub_disk_pages) {
	    static uint8_t dummy_sector[512];
	    memset(&dummy_sector[0], 0xc4, PGSIZE);

	    for (uint32_t i = 0; i < mobj.nbytes; i += 512) {
		disk_io_status s =
		    stackwrap_disk_io(op_write, pstate_part,
				      &dummy_sector[0], 512,
				      mobj.off + i * 512);
		if (!SAFE_EQUAL(s, disk_io_success))
		    cprintf("pstate_kobj_free: cannot scrub sector %"PRIu64"\n",
			    mobj.off + i * 512);
	    }
	}

	freelist_free_later(f, mobj.off, mobj.nbytes);
	assert(0 == btree_delete(BTREE_OBJMAP, &ko->hdr.ko_id));

	r = btree_delete(BTREE_IOBJ, &ko->hdr.ko_id);
	assert(r == 0 || r == -E_NOT_FOUND);
    }
}

static int64_t
pstate_kobj_alloc(struct freelist *f, struct kobject *ko, uint64_t objbytes)
{
    int r;
    pstate_kobj_free(f, ko);

    uint64_t nbytes = KOBJ_DISK_SIZE + ROUNDUP(objbytes, PGSIZE);
    int64_t offset = freelist_alloc(f, nbytes);

    if (offset < 0) {
	cprintf("pstate_kobj_alloc: no room for %"PRIu64" bytes\n", nbytes);
	return offset;
    }

    struct mobject mobj = { offset, nbytes };
    r = btree_insert(BTREE_OBJMAP, &ko->hdr.ko_id, (uint64_t *)&mobj);
    if (r < 0) {
	cprintf("pstate_kobj_alloc: objmap insert failed, disk full?\n");
	return r;
    }

    if (kobject_initial(ko)) {
	uint64_t dummy = 0;
	r = btree_insert(BTREE_IOBJ, &ko->hdr.ko_id, &dummy);
	if (r < 0) {
	    cprintf("pstate_kobj_alloc: iobjlist insert failed, disk full?\n");
	    return r;
	}
    }

    return offset;
}

//////////////////////////////////////////////////
// Scatter-gather buffering logic
//////////////////////////////////////////////////

struct pstate_iov_collector {
    struct kiovec iov_buf[DISK_REQMAX / PGSIZE];
    uint32_t iov_cnt;
    uint32_t iov_bytes;

    uint64_t flush_off;
    disk_op flush_op;
};

static int
pstate_iov_flush(struct pstate_iov_collector *x)
{
    if (x->iov_bytes > 0) {
	disk_io_status s =
	    stackwrap_disk_iov(x->flush_op, pstate_part,
			       x->iov_buf, x->iov_cnt, x->flush_off);
	if (!SAFE_EQUAL(s, disk_io_success)) {
	    cprintf("pstate_iov_flush: error during disk io\n");
	    return -E_IO;
	}
    }

    x->flush_off += x->iov_bytes;
    x->iov_bytes = 0;
    x->iov_cnt = 0;
    return 0;
}

static int
pstate_iov_append(struct pstate_iov_collector *x, void *buf, uint32_t size)
{
    uint32_t iov_max = sizeof(x->iov_buf) / sizeof(x->iov_buf[0]);

    if (x->iov_cnt == iov_max) {
	int r = pstate_iov_flush(x);
	if (r < 0)
	    return r;
    }

    x->iov_buf[x->iov_cnt].iov_base = buf;
    x->iov_buf[x->iov_cnt].iov_len = size;
    x->iov_cnt++;
    x->iov_bytes += size;
    return 0;
}

//////////////////////////////////
// Swap-in code
//////////////////////////////////

static int
pstate_swapin_mobj(struct mobject mobj, kobject_id_t id)
{
    void *p;
    int r = page_alloc(&p);
    if (r < 0)
	return r;

    struct kobject *ko = (struct kobject *) p;
    memset(ko, 0, sizeof(*ko));
    pagetree_init(&ko->ko_pt, ko);

    struct pstate_iov_collector x;
    memset(&x, 0, sizeof(x));

    x.flush_off = mobj.off;
    x.flush_op = op_read;

    r = pstate_iov_append(&x, p, KOBJ_DISK_SIZE);
    if (r < 0)
	goto err;

    uint64_t ko_bytes = mobj.nbytes - KOBJ_DISK_SIZE;
    for (uint64_t page = 0; page < ROUNDUP(ko_bytes, PGSIZE) / PGSIZE; page++) {
	r = page_alloc(&p);
	if (r < 0)
	    goto err;

	r = pagetree_put_page(&ko->ko_pt, page, p);
	if (r < 0) {
	    page_free(p);
	    goto err;
	}

	uint32_t pagebytes = JMIN(ROUNDUP(ko_bytes - page * PGSIZE,
					  512), (uint32_t) PGSIZE);
	if (pagebytes != PGSIZE)
	    memset(p, 0, PGSIZE);

	r = pstate_iov_append(&x, p, pagebytes);
	if (r < 0)
	    goto err;
    }

    r = pstate_iov_flush(&x);
    if (r < 0)
	goto err;

    if (pstate_swapin_debug)
	cprintf("pstate_swapin_obj: id %"PRIu64" nbytes %"PRIu64"\n",
			ko->hdr.ko_id, ko->hdr.ko_nbytes);

    if (ko->hdr.ko_id != id) {
	cprintf("pstate_swapin_mobj: requested %"PRIu64" (%"PRIu64" @ %"PRIu64"), got %"PRIu64"\n",
		id, mobj.nbytes, mobj.off, ko->hdr.ko_id);

	kobject_id_t id_found;
	r = btree_search(BTREE_OBJMAP, &ko->hdr.ko_id, &id_found, (uint64_t *) &mobj);
	if (r >= 0)
	    cprintf("pstate_swapin_mobj: %"PRIu64" maps to %"PRIu64" @ %"PRIu64"\n",
		    id_found, mobj.nbytes, mobj.off);

	panic("pstate_swapin_mobj: disk state corrupted");
    }

    kobject_swapin(ko);
    return 0;

err:
    pagetree_free(&ko->ko_pt, 0);
    page_free(ko);
    return r;
}

static int
pstate_swapin_id(kobject_id_t id)
{
    kobject_id_t id_found;
    struct mobject mobj;

    if (stable_hdr.ph_magic != PSTATE_MAGIC) {
	if (pstate_swapin_debug)
	    cprintf("pstate_swapin_id: disk not initialized\n");
	kobject_negative_insert(id);
	return -E_NOT_FOUND;
    }

    int r = btree_search(BTREE_OBJMAP, &id, &id_found, (uint64_t *) &mobj);
    if (r == -E_NOT_FOUND) {
	if (pstate_swapin_debug)
	    cprintf("pstate_swapin_id: id %"PRIu64" not found\n", id);
	kobject_negative_insert(id);
    } else if (r < 0) {
	cprintf("pstate_swapin_stackwrap: error during lookup: %s\n", e2s(r));
    } else {
	r = pstate_swapin_mobj(mobj, id);
	if (r < 0 && r != -E_RESTART)
	    cprintf("pstate_swapin_id: swapping in: %s\n", e2s(r));
    }

    return r;
}

static void
pstate_swapin_stackwrap(uint64_t arg, uint64_t arg1, uint64_t arg2)
{
    kobject_id_t id = (kobject_id_t) arg;
    if (kobject_incore(id) < 0) {
	cur_waitlist = &pstate_waiting;
	pstate_swapin_id(id);
    }

    lock_release(&pstate_lock);
    while (!LIST_EMPTY(&pstate_waiting))
	thread_set_runnable(LIST_FIRST(&pstate_waiting));
}

int
pstate_swapin(kobject_id_t id)
{
    if (!pstate_part) {
	kobject_negative_insert(id);
	return -E_RESTART;
    }

    if (pstate_swapin_debug)
	cprintf("pstate_swapin: object %"PRIu64"\n", id);

    thread_suspend_cur(&pstate_waiting);
    pstate_op_schedule(op_swapin, &pstate_swapin_stackwrap, id, 0, 0);
    return -E_RESTART;
}

/////////////////////////////////////
// Persistent-store initialization
/////////////////////////////////////

static int
pstate_apply_disk_log(void)
{
    log_init(stable_hdr.ph_log_max);

    int r = log_apply_disk(stable_hdr.ph_log_blocks);
    if (r < 0) {
	cprintf("pstate_apply_disk_log: cannot apply: %s\n", e2s(r));
	return r;
    }

    disk_io_status s = stackwrap_disk_io(op_flush, pstate_part, 0, 0, 0);
    if (!SAFE_EQUAL(s, disk_io_success)) {
	cprintf("pstate_apply_disk_log: cannot flush\n");
	return -E_IO;
    }

    stable_hdr.ph_log_blocks = 0;
    s = stackwrap_disk_io(op_write, pstate_part, &stable_hdr.ph_buf[0],
			  PSTATE_BUF_SIZE, 0);
    if (!SAFE_EQUAL(s, disk_io_success)) {
	cprintf("pstate_apply_disk_log: cannot write out header\n");
	return -E_IO;
    }

    s = stackwrap_disk_io(op_flush, pstate_part, 0, 0, 0);
    if (!SAFE_EQUAL(s, disk_io_success))
	panic("pstate_apply_disk_log: cannot flush header");

    return 0;
}

static int
pstate_load2(void)
{
    disk_io_status s = stackwrap_disk_io(op_read, pstate_part,
					 &stable_hdr.ph_buf[0],
					 PSTATE_BUF_SIZE, 0);
    if (!SAFE_EQUAL(s, disk_io_success)) {
	cprintf("pstate_load2: cannot read header\n");
	return -E_IO;
    }

    if (stable_hdr.ph_magic != PSTATE_MAGIC ||
	stable_hdr.ph_version != PSTATE_VERSION)
    {
	cprintf("pstate_load2: magic/version mismatch\n");
	return -E_INVAL;
    }

    int r = pstate_apply_disk_log();
    if (r < 0) {
	cprintf("pstate_load2: cannot apply log: %s\n", e2s(r));
	return r;
    }

    freelist_deserialize(&freelist, &stable_hdr.ph_free);
    btree_manager_deserialize(&stable_hdr.ph_btrees);

    struct btree_traversal trav;
    r = btree_init_traversal(BTREE_IOBJ, &trav);
    if (r < 0)
	return r;

    while (btree_next_entry(&trav)) {
	uint64_t id = *trav.key;
	if (pstate_load_debug)
	    cprintf("pstate_load2: paging in kobj %"PRIu64"\n", id);

	r = pstate_swapin_id(id);
	if (r < 0) {
	    cprintf("pstate_load2: cannot swapin %"PRIu64": %s\n", id, e2s(r));
	    return r;
	}
    }

    memcpy(&system_key[0], &stable_hdr.ph_system_key, SYSTEM_KEY_SIZE);
    key_derive();

    handle_counter = stable_hdr.ph_handle_counter;
    user_root_ct   = stable_hdr.ph_user_root_ct;
    pstate_counter = pstate_ts_decrypt(stable_hdr.ph_sync_ts);

    uint64_t now = timer_user_nsec();
    if (now < stable_hdr.ph_user_nsec)
	timer_user_nsec_offset = stable_hdr.ph_user_nsec - now;

    if (pstate_load_debug)
	cprintf("pstate_load2: handle_ctr %"PRIu64" root_ct %"PRIu64" nsec %"PRIu64"\n",
		handle_counter, user_root_ct, now);

    return 1;
}

static void
pstate_load_stackwrap(uint64_t arg, uint64_t arg1, uint64_t arg2)
{
    int *donep = (int *) (uintptr_t) arg;
    *donep = pstate_load2();
}

static void
pstate_reset(void)
{
    memset(&stable_hdr, 0, sizeof(stable_hdr));
    if (pstate_part)
	log_init(JMIN(pstate_part->pd_size / 65536, MAX_LOG_PAGES));
}

int
pstate_load(void)
{
    if (!pstate_part)
	return -E_INVAL;

    int done = 0;
    int r = stackwrap_call(&pstate_load_stackwrap, (uintptr_t) &done, 0, 0);
    if (r < 0) {
	cprintf("pstate_load: cannot stackwrap: %s\n", e2s(r));
	return r;
    }

    uint64_t ts_start = karch_get_tsc();
    int warned = 0;
    while (!done) {
	uint64_t ts_now = karch_get_tsc();
	if (warned == 0 && ts_now - ts_start > 1024*1024*1024) {
	    cprintf("pstate_load: wedged for %"PRIu64"\n", ts_now - ts_start);
	    warned = 1;
	}
	disk_poll(pstate_part->pd_dk);
    }

    if (done < 0)
	pstate_reset();

    return done;
}

//////////////////////////////////////////////////
// Swap-out code
//////////////////////////////////////////////////

struct swapout_stats {
    uint64_t written_kobj;
    uint64_t written_pages;
    uint64_t snapshoted_kobj;
    uint64_t dead_kobj;
    uint64_t total_kobj;
};

static int
pstate_sync_kobj(struct pstate_header *hdr,
		 struct swapout_stats *stats,
		 struct kobject_hdr *ko)
{
    struct kobject *snap = kobject_get_snapshot(ko);
    snap->hdr.ko_sync_ts = hdr->ph_sync_ts;
    snap->hdr.ko_flags |= KOBJ_ON_DISK;

    uint64_t objbytes = snap->hdr.ko_nbytes;
    uint64_t pgoff = PGOFF(objbytes);
    if (pgoff) {
	/* Verify the user hasn't scribbled on the tail of the last page */
	uint64_t lastpage = ROUNDDOWN(objbytes, PGSIZE) / PGSIZE;
	void *p;
	int r = kobject_get_page(&snap->hdr, lastpage, &p, page_shared_ro);
	if (r < 0)
	    panic("pstate_sync_kobj: cannot get last page: %s", e2s(r));

	if (memcmp(p + pgoff, &zero_page[0], PGSIZE - pgoff) != 0) {
	    if (pstate_swapout_debug)
		cprintf("pstate_sync_kobj: tail garbage: %"PRIu64" (%s)\n",
			snap->hdr.ko_id, snap->hdr.ko_name);
	    objbytes += PGSIZE - pgoff;
	}
    }

    int64_t off = pstate_kobj_alloc(&freelist, snap, objbytes);
    if (off < 0) {
	cprintf("pstate_sync_kobj: cannot allocate space: %s\n", e2s(off));
	return off;
    }

    struct pstate_iov_collector x;
    memset(&x, 0, sizeof(x));

    x.flush_off = off;
    x.flush_op = op_write;

    int r = pstate_iov_append(&x, snap, KOBJ_DISK_SIZE);
    if (r < 0)
	return r;

    for (uint64_t page = 0; page < kobject_npages(&snap->hdr); page++) {
	void *p;
	r = kobject_get_page(&snap->hdr, page, &p, page_shared_ro);
	if (r < 0)
	    panic("pstate_sync_kobj: cannot get page: %s", e2s(r));

	uint32_t pagebytes = JMIN(ROUNDUP(objbytes - page * PGSIZE,
					  512), (uint32_t) PGSIZE);
	r = pstate_iov_append(&x, p, pagebytes);
	if (r < 0)
	    return r;

	stats->written_pages++;
    }

    r = pstate_iov_flush(&x);
    if (r < 0)
	return r;

    if (pstate_swapout_debug)
	cprintf("pstate_sync_kobj: id %"PRIu64" nbytes %"PRIu64"\n",
		snap->hdr.ko_id, snap->hdr.ko_nbytes);

    ko->ko_flags |= KOBJ_ON_DISK;
    kobject_snapshot_release(ko);
    stats->written_kobj++;
    return 0;
}

static int
pstate_sync_loop(struct pstate_header *hdr,
		 struct swapout_stats *stats)
{
    struct kobject *ko;
    LIST_FOREACH(ko, &ko_list, ko_link) {
	if (!(ko->hdr.ko_flags & KOBJ_SNAPSHOTING))
	    continue;

	struct kobject *snap = kobject_get_snapshot(&ko->hdr);
	if (snap->hdr.ko_type == kobj_dead && (snap->hdr.ko_flags & KOBJ_ON_DISK)) {
	    pstate_kobj_free(&freelist, snap);
	    stats->dead_kobj++;
	    continue;
	}

	int r = pstate_sync_kobj(hdr, stats, &ko->hdr);
	if (r < 0)
	    return r;
    }

    int r = freelist_commit(&freelist);
    if (r < 0) {
	cprintf("pstate_sync_loop: cannot commit freelist: %s\n", e2s(r));
	return r;
    }

    btree_lock_all();

    freelist_serialize(&hdr->ph_free, &freelist);
    btree_manager_serialize(&hdr->ph_btrees);

    int64_t flush_blocks = log_flush();
    if (flush_blocks < 0) {
	cprintf("pstate_sync_loop: unable to flush: %s\n", e2s(flush_blocks));
	btree_unlock_all();
	return flush_blocks;
    }

    disk_io_status s = stackwrap_disk_io(op_flush, pstate_part, 0, 0, 0);
    if (!SAFE_EQUAL(s, disk_io_success)) {
	cprintf("pstate_sync_loop: unable to flush disk\n");
	btree_unlock_all();
	return -E_IO;
    }

    hdr->ph_log_blocks = flush_blocks;
    s = stackwrap_disk_io(op_write, pstate_part, hdr, PSTATE_BUF_SIZE, 0);
    if (!SAFE_EQUAL(s, disk_io_success)) {
	cprintf("pstate_sync_loop: unable to commit header\n");
	btree_unlock_all();
	return -E_IO;
    }

    s = stackwrap_disk_io(op_flush, pstate_part, 0, 0, 0);
    if (!SAFE_EQUAL(s, disk_io_success))
	panic("pstate_sync_loop: unable to flush commit record");

    static int commit_count = 0;
    if (commit_panic && ++commit_count == commit_panic)
	panic("commit test");

    if (hdr->ph_log_blocks > hdr->ph_log_max / 2 || log_must_apply()) {
	if (pstate_swapout_debug)
	    cprintf("pstate_sync_loop: applying on-disk log\n");

	do {
	    r = log_apply_mem();
	    if (r < 0)
		cprintf("pstate_sync_loop: unable to apply, retry: %s\n", e2s(r));
	} while (r < 0);

	hdr->ph_log_blocks = 0;
	do {
	    s = stackwrap_disk_io(op_write, pstate_part, hdr, PSTATE_BUF_SIZE, 0);
	    if (!SAFE_EQUAL(s, disk_io_success))
		cprintf("pstate_sync_loop: unable to rewrite header, retrying\n");
	} while (!SAFE_EQUAL(s, disk_io_success));

	s = stackwrap_disk_io(op_flush, pstate_part, 0, 0, 0);
	if (!SAFE_EQUAL(s, disk_io_success))
	    panic("pstate_sync_loop: unable to flush applied log");
    }

    btree_unlock_all();
    memcpy(&stable_hdr, hdr, sizeof(stable_hdr));
    return 0;
}

static void
pstate_sync_stackwrap(uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    if (!pstate_part)
	return;

    int *rvalp = 0;
    if (arg0)
	rvalp = (int *) (uintptr_t) arg0;

    uint64_t log_max = stable_hdr.ph_log_max;

    // If we don't have a valid header on disk, init the freelist
    if (stable_hdr.ph_magic != PSTATE_MAGIC) {
	log_max = JMIN(pstate_part->pd_size / 65536, MAX_LOG_PAGES);

	uint64_t part_pages = pstate_part->pd_size / PGSIZE;
	uint64_t reserved_pages = HEADER_PAGES + log_max;

	if (part_pages <= reserved_pages) {
	    cprintf("pstate_sync_stackwrap: %"PRIu64" disk pages "
		    "is too small, need %"PRIu64"\n",
		    part_pages, reserved_pages);
	    if (rvalp)
		*rvalp = -E_NO_MEM;
	    return;
	}

	if (pstate_swapout_debug)
	    cprintf("pstate_sync: %"PRIu64" disk pages\n", part_pages);

	log_init(log_max);
	btree_manager_init();
	freelist_init(&freelist, reserved_pages * PGSIZE,
		      (part_pages - reserved_pages) * PGSIZE);
    }

    static_assert(sizeof(struct pstate_header) == PSTATE_BUF_SIZE);

    static struct pstate_header pstate_scratch_buf;
    struct pstate_header *hdr = &pstate_scratch_buf;
    memcpy(hdr, &stable_hdr, sizeof(stable_hdr));

    hdr->ph_magic = PSTATE_MAGIC;
    hdr->ph_version = PSTATE_VERSION;
    hdr->ph_sync_ts = pstate_ts_alloc();
    hdr->ph_handle_counter = handle_counter;
    hdr->ph_user_root_ct = user_root_ct;
    hdr->ph_user_nsec = timer_user_nsec();
    hdr->ph_log_max = log_max;
    memcpy(&hdr->ph_system_key[0], &system_key[0], SYSTEM_KEY_SIZE);

    struct swapout_stats stats;
    memset(&stats, 0, sizeof(stats));

    struct kobject *ko, *ko_next;
    LIST_FOREACH(ko, &ko_list, ko_link) {
	stats.total_kobj++;
	if ((ko->hdr.ko_flags & KOBJ_DIRTY_LATER))
	    kobject_dirty_eval(ko);

	if ((ko->hdr.ko_flags & KOBJ_DIRTY)) {
	    kobject_snapshot(&ko->hdr);
	    ko->hdr.ko_flags |= KOBJ_SNAPSHOT_DIRTY;
	    stats.snapshoted_kobj++;
	}
    }

    int r = pstate_sync_loop(hdr, &stats);
    if (r < 0) {
	cprintf("pstate_sync_stackwrap: cannot sync: %s\n", e2s(r));

	// Flush btree caches
	for (uint64_t i = 0; i < BTREE_COUNT; i++) {
	    struct cache *c = btree_cache(i);
	    cache_flush(c);
	}

	// Reset the un-committed log by applying the committed on-disk one.
	assert(0 == pstate_apply_disk_log());

	// Restore the freelist and btree structures to their on-disk state
	freelist_deserialize(&freelist, &stable_hdr.ph_free);
	btree_manager_deserialize(&stable_hdr.ph_btrees);

	if (rvalp && !*rvalp)
	    *rvalp = r;
    }

    for (ko = LIST_FIRST(&ko_list); ko; ko = ko_next) {
	ko_next = LIST_NEXT(ko, ko_link);

	if ((ko->hdr.ko_flags & KOBJ_SNAPSHOT_DIRTY)) {
	    ko->hdr.ko_flags &= ~KOBJ_SNAPSHOT_DIRTY;
	    if (r < 0)
		ko->hdr.ko_flags |= KOBJ_DIRTY;
	    else
		ko->hdr.ko_sync_ts = hdr->ph_sync_ts;
	}

	if ((ko->hdr.ko_flags & KOBJ_SNAPSHOTING)) {
	    struct kobject *snap = kobject_get_snapshot(&ko->hdr);
	    kobject_snapshot_release(&ko->hdr);
    
	    if (r == 0 && snap->hdr.ko_type == kobj_dead)
		ko->hdr.ko_flags &= ~KOBJ_ON_DISK;
	}
    }

    if (pstate_swapout_stats) {
	cprintf("pstate_sync: total %"PRIu64" snap %"PRIu64" dead %"PRIu64" wrote %"PRIu64" pages %"PRIu64"\n",
		stats.total_kobj, stats.snapshoted_kobj, stats.dead_kobj,
		stats.written_kobj, stats.written_pages);
	cprintf("pstate_sync: pages used %"PRIu64" avail %"PRIu64" allocs %"PRIu64" fail %"PRIu64"\n",
		page_stats.pages_used, page_stats.pages_avail,
		page_stats.allocations, page_stats.failures);
    }

    while (!LIST_EMPTY(&pstate_waiting))
	thread_set_runnable(LIST_FIRST(&pstate_waiting));

    lock_release(&pstate_lock);
    if (rvalp && !*rvalp)
	*rvalp = 1;
}

static void
pstate_sync_schedule(int gc)
{
    pstate_op_schedule(gc ? op_swapout_global_gc : op_swapout_global,
		       &pstate_sync_stackwrap, 0, 0, 0);
}

void
pstate_sync(int gc)
{
    if (!pstate_part)
	return;

    thread_suspend_cur(&pstate_waiting);
    pstate_sync_schedule(gc);
}

int
pstate_sync_now(void)
{
    assert(cur_thread);

    if (!pstate_part)
	return 0;

    if (lock_try_acquire(&pstate_lock) < 0) {
	cprintf("pstate_sync_now: another sync still active\n");

	thread_suspend_cur(&pstate_waiting);
	return -E_RESTART;
    }

    int r = 0;
    stackwrap_call_stack(&pstate_op_stack[0], 0, &pstate_sync_stackwrap,
			 (uintptr_t) &r, 0, 0);
    while (r == 0)
	disk_poll(pstate_part->pd_dk);

    return r < 0 ? r : 0;
}

//////////////////////////////////////////////////
// User-initiated sync-to-disk
//////////////////////////////////////////////////

static void
pstate_sync_object_stackwrap(uint64_t arg, uint64_t start, uint64_t nbytes)
{
    // Casting to non-const, but it's OK here.
    struct kobject *ko = (struct kobject *) (uintptr_t) arg;

    // kobject_snapshot() clears KOBJ_DIRTY, but we don't want that here..
    uint64_t dirty = (ko->hdr.ko_flags & KOBJ_DIRTY);
    kobject_snapshot(&ko->hdr);
    ko->hdr.ko_flags |= dirty;
    struct kobject *snap = kobject_get_snapshot(&ko->hdr);
    uint64_t sync_ts = pstate_ts_alloc();

    kobject_id_t id_found, id = snap->hdr.ko_id;
    struct mobject mobj;
    int r = btree_search(BTREE_OBJMAP, &id, &id_found, (uint64_t *) &mobj);
    if (r < 0) {
	if (r != -E_NOT_FOUND)
	    cprintf("pstate_sync_object_stackwrap: lookup: %s\n", e2s(r));
	goto fallback;
    }

    uint64_t sync_end = JMIN(start + nbytes, snap->hdr.ko_nbytes);
    uint64_t req_disk_bytes = KOBJ_DISK_SIZE + ROUNDUP(sync_end, 512);
    if (req_disk_bytes > mobj.nbytes) {
	if (pstate_swapout_object_debug)
	    cprintf("pstate_sync_object_stackwrap: object grew, fallback\n");
	goto fallback;
    }

    struct pstate_iov_collector x;
    memset(&x, 0, sizeof(x));
    x.flush_off = mobj.off + KOBJ_DISK_SIZE + ROUNDDOWN(start, PGSIZE);
    x.flush_op = op_write;

    for (uint64_t page = start / PGSIZE; page < ROUNDUP(sync_end, PGSIZE) / PGSIZE; page++) {
	void *p;
	r = kobject_get_page(&snap->hdr, page, &p, page_shared_ro);
	if (r < 0)
	    goto fallback;

	uint32_t pagebytes = JMIN(ROUNDUP(sync_end - page * PGSIZE, 512),
				  (uint32_t) PGSIZE);
	r = pstate_iov_append(&x, p, pagebytes);
	if (r < 0)
	    goto fallback;
    }

    r = pstate_iov_flush(&x);
    if (r < 0)
	goto fallback;

    if (!SAFE_EQUAL(stackwrap_disk_io(op_flush, pstate_part,
				      0, 0, 0), disk_io_success))
	goto fallback;

    if (pstate_swapout_object_debug)
	cprintf("pstate_sync_object_stackwrap: incremental sync OK\n");
    ko->hdr.ko_sync_ts = sync_ts;

    while (!LIST_EMPTY(&pstate_waiting))
	thread_set_runnable(LIST_FIRST(&pstate_waiting));
    kobject_snapshot_release(&ko->hdr);
    lock_release(&pstate_lock);
    return;

fallback:
    kobject_snapshot_release(&ko->hdr);
    lock_release(&pstate_lock);
    pstate_sync_schedule(0);
}

int
pstate_sync_object(uint64_t timestamp, const struct kobject *ko,
		   uint64_t start, uint64_t nbytes)
{
    assert(cur_thread);

    if (!pstate_part)
	return 0;

    if (stable_hdr.ph_magic != PSTATE_MAGIC)
	goto fallback;

    if (ko->hdr.ko_sync_ts &&
	pstate_ts_decrypt(ko->hdr.ko_sync_ts) > pstate_ts_decrypt(timestamp))
	return 0;

    thread_suspend_cur(&pstate_waiting);
    pstate_op_schedule(op_swapout_obj, &pstate_sync_object_stackwrap,
		       (uintptr_t) ko, start, nbytes);
    return -E_RESTART;

fallback:
    thread_suspend_cur(&pstate_waiting);
    pstate_sync_schedule(0);
    return -E_RESTART;
}

int
pstate_sync_user(uint64_t timestamp)
{
    assert(cur_thread);

    if (!pstate_part)
	return 0;

    if (stable_hdr.ph_magic == PSTATE_MAGIC &&
	pstate_ts_decrypt(stable_hdr.ph_sync_ts) > pstate_ts_decrypt(timestamp))
	return 0;

    thread_suspend_cur(&pstate_waiting);
    pstate_sync_schedule(0);
    return -E_RESTART;
}

static void
pstate_sync_periodic(void)
{
    pstate_sync_schedule(0);
}

void
pstate_init(void)
{
    if (!pstate_part)
	return;

    pstate_reset();

    static struct periodic_task sync_pt =
	{ .pt_fn = &pstate_sync_periodic, .pt_interval_sec = 300 };
    timer_add_periodic(&sync_pt);
}
