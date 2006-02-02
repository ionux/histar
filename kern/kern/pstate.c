#include <machine/pmap.h>
#include <machine/thread.h>
#include <machine/x86.h>
#include <machine/stackwrap.h>
#include <dev/disk.h>
#include <kern/pstate.h>
#include <kern/uinit.h>
#include <kern/handle.h>
#include <kern/lib.h>
#include <inc/error.h>
#include <lib/btree/btree_traverse.h>


// verbose flags
static int pstate_init_debug = 0;
static int pstate_swapin_debug = 0;
static int pstate_swapout_debug = 0;
static int pstate_swapout_stats = 0;

// Authoritative copy of the header that's actually on disk.
static struct pstate_header stable_hdr;

// assumed to be atomic
static struct freelist flist ;

// lits of objects to be loaded at startup
struct btree_default iobjlist ;
#define IOBJ_ORDER BTREE_MAX_ORDER1
STRUCT_BTREE_CACHE(iobj_cache, 20, IOBJ_ORDER, 1) ;	

// lits of objects to be loaded at startup
struct btree_default objmap ;
#define OBJMAP_ORDER BTREE_MAX_ORDER1
STRUCT_BTREE_CACHE(objmap_cache, 20, OBJMAP_ORDER, 1) ;	

struct mobject
{
	offset_t off ;
	uint64_t npages ;	
	
} ;

// Scratch-space for a copy of the header used while reading/writing.
#define N_HEADER_PAGES		3
#define PSTATE_BUF_SIZE		(N_HEADER_PAGES * PGSIZE)
static union {
    struct pstate_header hdr;
    char buf[PSTATE_BUF_SIZE];
} pstate_buf;

//////////////////////////////////////////////////
// Object map
//////////////////////////////////////////////////

static void
pstate_kobj_free(struct freelist *f, struct kobject *ko)
{
    
	uint64_t key ;
	struct mobject mobj ;

    int r = btree_search(&objmap.tree, &ko->u.hdr.ko_id, &key, (uint64_t *)&mobj) ;
    if (r == 0) {
    	assert(key == ko->u.hdr.ko_id) ;

		freelist_free(f, mobj.off, mobj.npages) ;
    	btree_delete(&iobjlist.tree, &ko->u.hdr.ko_id) ;
    	btree_delete(&objmap.tree, &ko->u.hdr.ko_id) ;
    }
}

static int64_t
pstate_kobj_alloc(struct freelist *f, struct kobject *ko)
{
    int r ;
    pstate_kobj_free(f, ko);
	

	uint64_t npages = ko->u.hdr.ko_npages + 1;
    int64_t offset = freelist_alloc(f, npages);
	
	if (offset < 0) {
		cprintf("pstate_kobj_alloc: no room for %ld pages\n", npages);
		return offset;
    }

	struct mobject mobj = { offset, npages } ;
	r = btree_insert(&objmap.tree, &ko->u.hdr.ko_id, (uint64_t *)&mobj) ;
	if (r < 0) {
		cprintf("pstate_kobj_alloc: objmap insert failed, disk full?\n") ;
		return r ;
	}

	if ((ko->u.hdr.ko_flags & KOBJ_PIN_IDLE) ||
		(ko->u.hdr.ko_ref == 0) ||
		(ko->u.hdr.ko_type == kobj_thread)) {
		r = btree_insert(&iobjlist.tree, &ko->u.hdr.ko_id, &offset) ;
		if (r < 0) {
			cprintf("pstate_kobj_alloc: iobjlist insert failed, "
				"disk full?\n") ;
			return r ;	
		}
	}
	return offset ;
}

//////////////////////////////////
// Swap-in code
//////////////////////////////////

static int
pstate_swapin_off(offset_t off)
{
    void *p;
    int r = page_alloc(&p);
    if (r < 0) {
		cprintf("pstate_swapin_obj: cannot alloc page: %s\n", e2s(r));
		return r;
    }

    struct kobject *ko = (struct kobject *) p;
    offset_t offset = PGSIZE * off ;
    
    disk_io_status s = stackwrap_disk_io(op_read, p, PGSIZE, offset);
    if (s != disk_io_success) {
		cprintf("pstate_swapin_obj: cannot read object from disk\n");
		return -E_IO;
    }

    pagetree_init(&ko->u.hdr.ko_pt);
    for (uint64_t page = 0; page < ko->u.hdr.ko_npages; page++) {
		r = page_alloc(&p);
		if (r < 0) {
		    cprintf("pstate_swapin_obj: cannot alloc page: %s\n", e2s(r));
		    return r;
		}
	
		offset = (off + page + 1) * PGSIZE;
		s = stackwrap_disk_io(op_read, p, PGSIZE, offset);
		if (s != disk_io_success) {
		    cprintf("pstate_swapin_obj: cannot read page from disk\n");
		    return -E_IO;
		}
	
		assert(0 == pagetree_put_page(&ko->u.hdr.ko_pt, page, p));
    }

    if (pstate_swapin_debug)
	cprintf("pstate_swapin_obj: id %ld npages %ld\n",
			ko->u.hdr.ko_id, ko->u.hdr.ko_npages);

    kobject_swapin(ko);
    return 0;
}

static void
pstate_swapin_stackwrap(void *arg)
{
    static struct Thread_list swapin_waiting;

    if (cur_thread)
	thread_suspend(cur_thread, &swapin_waiting);

    kobject_id_t id = (kobject_id_t) arg;
    kobject_id_t id_found;
    struct mobject mobj;
    int r = btree_search(&objmap.tree, &id, &id_found, (uint64_t *) &mobj);
    if (r == -E_NOT_FOUND) {
	if (pstate_swapin_debug)
	    cprintf("pstate_swapin_stackwrap: id %ld not found\n", id);
	kobject_negative_insert(id);
    } else if (r < 0) {
	cprintf("pstate_swapin_stackwrap: error during lookup: %s\n", e2s(r));
    } else {
	r = pstate_swapin_off(mobj.off);
	if (r < 0)
	    cprintf("pstate_swapin_stackwrap: swapping in: %s\n", e2s(r));
    }

    while (!LIST_EMPTY(&swapin_waiting)) {
	struct Thread *t = LIST_FIRST(&swapin_waiting);
	thread_set_runnable(t);
    }
}

int
pstate_swapin(kobject_id_t id) {
    if (pstate_swapin_debug)
	cprintf("pstate_swapin: object %ld\n", id);

    int r = stackwrap_call(&pstate_swapin_stackwrap, (void *) id);
    if (r < 0) {
	cprintf("pstate_swapin: cannot stackwrap: %s\n", e2s(r));
	return r;
    }

    return -E_RESTART;
}

/////////////////////////////////////
// Persistent-store initialization
/////////////////////////////////////

static int
pstate_init2()
{
    disk_io_status s = stackwrap_disk_io(op_read, &pstate_buf.buf[0], PSTATE_BUF_SIZE, 0);
    if (s != disk_io_success) {
		cprintf("pstate_init2: cannot read header\n");
		return -E_IO;
    }

    memcpy(&stable_hdr, &pstate_buf.hdr, sizeof(stable_hdr));
    if (stable_hdr.ph_magic != PSTATE_MAGIC ||
		stable_hdr.ph_version != PSTATE_VERSION)
    {
		cprintf("pstate_init_hdr: magic/version mismatch\n");

		return -E_INVAL;
    }
	
	// XXX
	memcpy(&flist, &stable_hdr.ph_free, sizeof(flist)) ;
	freelist_setup((uint8_t *)&flist) ;
	memcpy(&iobjlist, &stable_hdr.ph_iobjs, sizeof(iobjlist)) ;
	btree_default_setup(&iobjlist, IOBJ_ORDER, &flist, &iobj_cache) ;
	memcpy(&objmap, &stable_hdr.ph_map, sizeof(objmap)) ;
	btree_default_setup(&objmap, OBJMAP_ORDER, &flist, &iobj_cache) ;

	struct btree_traversal trav ;
	btree_init_traversal(&iobjlist.tree, &trav) ;
	
	if (pstate_init_debug)
		btree_pretty_print(&iobjlist.tree, iobjlist.tree.root, 0);
	
	while (btree_next_entry(&trav)) {
		
		uint64_t id = *trav.key ;
		offset_t off = *trav.val ;
		if (pstate_init_debug)
			cprintf("pstate_init2: paging in kobj %ld\n", id) ;
		
		int r = pstate_swapin_off(off) ;
		
		if (r < 0) {
			cprintf("pstate_init2: cannot swapin offset %ld: %s\n",
					id, e2s(r)) ;
			return r ;
		}
	}
		
    handle_counter   = stable_hdr.ph_handle_counter;
    user_root_handle = stable_hdr.ph_user_root_handle;

    if (pstate_init_debug)
		cprintf("pstate_init2: handle_counter %ld root_handle %ld\n",
			handle_counter, user_root_handle);

    return 1;
}

static void
pstate_init_stackwrap(void *arg)
{
    int *donep = (int *) arg;

    *donep = pstate_init2();
}

void
pstate_reset(void)
{
    memset(&stable_hdr, 0, sizeof(stable_hdr));
}

int
pstate_init(void)
{
    pstate_reset();

    int done = 0;
    int r = stackwrap_call(&pstate_init_stackwrap, &done);
    if (r < 0) {
		cprintf("pstate_init: cannot stackwrap: %s\n", e2s(r));
		return r;
    }

    uint64_t ts_start = read_tsc();
    int warned = 0;
    while (!done) {
		uint64_t ts_now = read_tsc();
		if (warned == 0 && ts_now - ts_start > 1024*1024*1024) {
		    cprintf("pstate_init: wedged for %ld\n", ts_now - ts_start);
		    warned = 1;
		}
		ide_intr();
    }

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

    int64_t off = pstate_kobj_alloc(&flist, snap) ;
    if (off < 0) {
		cprintf("pstate_sync_kobj: cannot allocate space: %s\n", e2s(off));
		return off;
    }

    disk_io_status s =
	stackwrap_disk_io(op_write, snap, sizeof(*snap),
			  		  off * PGSIZE);
    if (s != disk_io_success) {
		cprintf("pstate_sync_kobj: error during disk io\n");
		return -E_IO;
    }

    for (uint64_t page = 0; page < snap->u.hdr.ko_npages; page++) {
		uint64_t offset = (off + page + 1) * PGSIZE;
		void *p;
		int r = kobject_get_page(&snap->u.hdr, page, &p, page_ro);
		if (r < 0)
		    panic("pstate_sync_kobj: cannot get page: %s", e2s(r));
	
		s = stackwrap_disk_io(op_write, p, PGSIZE, offset);
		if (s != disk_io_success) {
		    cprintf("pstate_sync_kobj: error during disk io for page\n");
		    return -E_IO;
		}
	
		stats->written_pages++;
    }

    if (pstate_swapout_debug)
		cprintf("pstate_sync_kobj: id %ld npages %ld\n",
			snap->u.hdr.ko_id, snap->u.hdr.ko_npages);

    kobject_snapshot_release(ko);
    stats->written_kobj++;
    return 0;
}

static int
pstate_sync_loop(struct pstate_header *hdr,
		 struct swapout_stats *stats)
{
    struct kobject_hdr *ko;
    LIST_FOREACH(ko, &ko_list, ko_link) {
		if (!(ko->ko_flags & KOBJ_SNAPSHOTING))
		    continue;
	
		struct kobject *snap = kobject_get_snapshot(ko);
		if (snap->u.hdr.ko_type == kobj_dead) {
		    pstate_kobj_free(&flist, snap);
		    stats->dead_kobj++;
		    continue;
		}

		int r = pstate_sync_kobj(hdr, stats, ko);
		if (r < 0)
		    return r;
    }
	
	// XXX
	memcpy(&hdr->ph_free, &flist, sizeof(flist)) ;
	memcpy(&hdr->ph_iobjs, &iobjlist, sizeof(iobjlist)) ;
	memcpy(&hdr->ph_map, &objmap, sizeof(objmap)) ;
  
    disk_io_status s = stackwrap_disk_io(op_write, hdr, PSTATE_BUF_SIZE, 0);
    if (s == disk_io_success) {
		memcpy(&stable_hdr, hdr, sizeof(stable_hdr));
		return 0;
    } else {
		cprintf("pstate_sync_stackwrap: error writing header\n");
		return -E_IO;
    }
}

static void
pstate_sync_stackwrap(void *arg)
{
    static int swapout_active;

    if (swapout_active) {
		cprintf("pstate_sync: another sync still active\n");
		return;
    }
    swapout_active = 1;

    // If we don't have a valid header on disk, init the freelist
    if (stable_hdr.ph_magic != PSTATE_MAGIC) {
		uint64_t disk_pages = disk_bytes / PGSIZE;
		assert(disk_pages > N_HEADER_PAGES);
	
		if (pstate_swapout_debug)
		    cprintf("pstate_sync: %ld disk pages\n", disk_pages);
	
		freelist_init(&flist,
			      N_HEADER_PAGES,
			      disk_pages - N_HEADER_PAGES);
		btree_default_init(&iobjlist, IOBJ_ORDER, 1, 1, &flist, &iobj_cache) ;
		btree_default_init(&objmap, OBJMAP_ORDER, 1, 2, &flist, &objmap_cache) ;
    }

    static_assert(sizeof(pstate_buf.hdr) <= PSTATE_BUF_SIZE);
    static_assert(BTREE_NODE_SIZE(IOBJ_ORDER, 1) <= PGSIZE) ;
	static_assert(BTREE_NODE_SIZE(OBJMAP_ORDER, 1) <= PGSIZE) ;
	
    struct pstate_header *hdr = &pstate_buf.hdr;
    memcpy(hdr, &stable_hdr, sizeof(stable_hdr));
    
    hdr->ph_magic = PSTATE_MAGIC;
    hdr->ph_version = PSTATE_VERSION;
    hdr->ph_handle_counter = handle_counter;
    hdr->ph_user_root_handle = user_root_handle;

    struct swapout_stats stats;
    memset(&stats, 0, sizeof(stats));

    struct kobject_hdr *ko, *ko_next;
    LIST_FOREACH(ko, &ko_list, ko_link) {
		stats.total_kobj++;
		if ((ko->ko_flags & KOBJ_DIRTY)) {
		    kobject_snapshot(ko);
		    ko->ko_flags |= KOBJ_SNAPSHOT_DIRTY;
		    stats.snapshoted_kobj++;
		}
    }

    int r = pstate_sync_loop(hdr, &stats);
    if (r < 0)
		cprintf("pstate_sync_stackwrap: cannot sync: %s\n", e2s(r));

    for (ko = LIST_FIRST(&ko_list); ko; ko = ko_next) {
		ko_next = LIST_NEXT(ko, ko_link);
	
		if ((ko->ko_flags & KOBJ_SNAPSHOT_DIRTY)) {
		    ko->ko_flags &= ~KOBJ_SNAPSHOT_DIRTY;
		    if (r < 0)
				ko->ko_flags |= KOBJ_DIRTY;
		}
	
		if ((ko->ko_flags & KOBJ_SNAPSHOTING)) {
		    struct kobject *snap = kobject_get_snapshot(ko);
		    kobject_snapshot_release(ko);
	
		    if (r == 0 && snap->u.hdr.ko_type == kobj_dead)
				kobject_swapout(kobject_h2k(ko));
		}
    }

    if (pstate_swapout_stats) {
		cprintf("pstate_sync: total %ld snap %ld dead %ld wrote %ld pages %ld\n",
			stats.total_kobj, stats.snapshoted_kobj, stats.dead_kobj,
			stats.written_kobj, stats.written_pages);
		cprintf("pstate_sync: pages used %ld avail %ld allocs %ld fail %ld\n",
			page_stats.pages_used, page_stats.pages_avail,
			page_stats.allocations, page_stats.failures);
    }
    swapout_active = 0;
}


void
pstate_sync(void)
{
    int r = stackwrap_call(&pstate_sync_stackwrap, 0);
    if (r < 0)
		cprintf("pstate_sync: cannot stackwrap: %s\n", e2s(r));
}
