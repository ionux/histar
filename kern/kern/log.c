#include <kern/log.h>
#include <kern/disklayout.h>
#include <kern/freelist.h>
#include <kern/disklayout.h>
#include <kern/stackwrap.h>
#include <kern/arch.h>
#include <kern/pstate.h>
#include <inc/hashtable.h>
#include <inc/queue.h>
#include <inc/error.h>

static struct {
    uint64_t byteoff;
    uint64_t npages;
    uint64_t max_mem;

    uint64_t in_mem;
    uint8_t tailq_inited;
    uint8_t must_apply;
    struct node_list nodes;
    struct node_list free_nodes;

    uint64_t on_disk;
    struct hashtable disk_map;
    struct hashentry *map_back[MAX_LOG_PAGES / (PGSIZE / sizeof(struct hashentry)) + 1];
} the_log;

static int __attribute__((warn_unused_result))
log_write_to_disk(struct node_list *nodes, uint64_t *count)
{
    disk_io_status s;
    uint64_t n = 0;

    struct btree_node *node;
    TAILQ_FOREACH(node, nodes, node_log_link) {
	assert(node->block.offset < the_log.byteoff ||
	       node->block.offset >= the_log.byteoff + the_log.npages * PGSIZE);

	s = stackwrap_disk_io(op_write, pstate_part, node, BTREE_BLOCK_SIZE,
			      node->block.offset);
	if (!SAFE_EQUAL(s, disk_io_success)) {
	    *count = n;
	    return -E_IO;
	}
	n++;
    }
    *count = n;
    return 0;
}

static int __attribute__((warn_unused_result))
log_write_to_log(struct node_list *nodes, uint64_t *count, offset_t off)
{
    disk_io_status s;
    uint64_t n = 0;
    assert(the_log.byteoff <= off);

    struct btree_node *node;
    TAILQ_FOREACH(node, nodes, node_log_link) {
	if (!node->block.is_dirty)
	    continue;

	assert(off + n * PGSIZE < the_log.npages * PGSIZE + the_log.byteoff);
	s = stackwrap_disk_io(op_write, pstate_part, node, BTREE_BLOCK_SIZE,
			      off + n * PGSIZE);
	if (!SAFE_EQUAL(s, disk_io_success)) {
	    *count = n;
	    return -E_IO;
	}
	hash_put(&the_log.disk_map, node->block.offset, off + n * PGSIZE);
	node->block.is_dirty = 0;
	n++;
    }
    *count = n;
    return 0;
}

static uint64_t
log_free_list(struct node_list *nodes)
{
    uint64_t n = 0;
    struct btree_node *prev, *next;

    for (prev = TAILQ_FIRST(nodes); prev; prev = next, n++) {
	next = TAILQ_NEXT(prev, node_log_link);
	TAILQ_REMOVE(nodes, prev, node_log_link);
	TAILQ_INSERT_HEAD(&the_log.free_nodes, prev, node_log_link);
    }
    return n;
}

static int __attribute__((warn_unused_result))
log_read_log(offset_t off, uint64_t n_nodes, struct node_list *nodes)
{
    struct btree_node *node;
    disk_io_status s;

    for (uint64_t i = 0; i < n_nodes; i++, off += BTREE_BLOCK_SIZE) {
	node = TAILQ_FIRST(&the_log.free_nodes);
	assert(node);
	TAILQ_REMOVE(&the_log.free_nodes, node, node_log_link);

	s = stackwrap_disk_io(op_read, pstate_part, node, BTREE_BLOCK_SIZE, off);
	if (!SAFE_EQUAL(s, disk_io_success))
	    return -E_IO;
	TAILQ_INSERT_TAIL(nodes, node, node_log_link);
    }
    return 0;
}

static int64_t __attribute__((warn_unused_result))
log_read_map(struct hashtable *map, uint64_t max_nodes, struct node_list *nodes)
{
    struct btree_node *node;
    disk_io_status s;
    struct hashiter iter;

    uint64_t count = 0;
    for (hashiter_init(map, &iter); hashiter_next(&iter); ) {
	node = TAILQ_FIRST(&the_log.free_nodes);
	assert(node);
	TAILQ_REMOVE(&the_log.free_nodes, node, node_log_link);

	offset_t off = iter.hi_val;
	s = stackwrap_disk_io(op_read, pstate_part,
			      node, BTREE_BLOCK_SIZE, off);
	if (!SAFE_EQUAL(s, disk_io_success))
	    return -E_IO;
	assert(node->block.offset == iter.hi_key);
	TAILQ_INSERT_HEAD(nodes, node, node_log_link);

	count++;
	if (count >= max_nodes)
	    break;
    }

    return count;
}

int
log_try_read(offset_t byteoff, void *page)
{
    struct btree_node *node;
    TAILQ_FOREACH(node, &the_log.nodes, node_log_link) {
	if (byteoff == node->block.offset) {
	    memcpy(page, node, PGSIZE);
	    return 0;
	}
    }

    // look in on-disk log
    offset_t log_off;
    if (hash_get(&the_log.disk_map, byteoff, &log_off) < 0)
	return -E_NOT_FOUND;
    disk_io_status s = stackwrap_disk_io(op_read, pstate_part,
					 page, BTREE_BLOCK_SIZE, log_off);
    if (!SAFE_EQUAL(s, disk_io_success))
	return -E_IO;

    return 0;
}

int
log_write(struct btree_node *node)
{
    int r;
    char found = 0;
    struct btree_node *store;

    TAILQ_FOREACH(store, &the_log.nodes, node_log_link) {
	if (store->block.offset == node->block.offset) {
	    TAILQ_REMOVE(&the_log.nodes, store, node_log_link);
	    found = 1;
	    break;
	}
    }

    if (!found) {
	if (the_log.in_mem == the_log.max_mem) {
	    if ((r = log_flush()) < 0)
		return r;
	    uint64_t count = log_free_list(&the_log.nodes);
	    the_log.in_mem -= count;
	    assert(the_log.in_mem == 0);
	}

	store = TAILQ_FIRST(&the_log.free_nodes);
	assert(store);
	TAILQ_REMOVE(&the_log.free_nodes, store, node_log_link);

	memset(store, 0, PGSIZE);
	the_log.in_mem++;
    }

    memcpy(store, node,
	   BTREE_NODE_SIZE(node->tree->order, node->tree->s_key));
    TAILQ_INSERT_TAIL(&the_log.nodes, store, node_log_link);
    store->block.is_dirty = 1;
    return 0;
}

void
log_free(offset_t byteoff)
{
    struct btree_node *node;
    TAILQ_FOREACH(node, &the_log.nodes, node_log_link) {
	if (byteoff == node->block.offset) {
	    TAILQ_REMOVE(&the_log.nodes, node, node_log_link);
	    TAILQ_INSERT_HEAD(&the_log.free_nodes, node, node_log_link);
	    the_log.in_mem--;
	    break;
	}
    }

    offset_t log_off;
    if (hash_get(&the_log.disk_map, byteoff, &log_off) >= 0) {
	// We freed some block that's present in our on-disk the_log..
	// To avoid overwriting any new data that will be written to
	// this soon-to-be-free space, the log must be flushed right
	// after the free list is committed.
	the_log.must_apply = 1;
	hash_del(&the_log.disk_map, byteoff);
    }
}

int64_t
log_flush(void)
{
    int r;

    if (the_log.npages <= the_log.in_mem + the_log.on_disk + 1)
	panic("log_flush: out of log space: %"PRIu64" %"PRIu64" %"PRIu64,
	      the_log.in_mem, the_log.on_disk, the_log.npages);

    uint64_t count;
    uint64_t off = the_log.byteoff + the_log.on_disk * PGSIZE;
    if ((r = log_write_to_log(&the_log.nodes, &count, off)) < 0)
	return r;

    assert(count <= the_log.in_mem);
    the_log.on_disk += count;

    return the_log.on_disk;
}

int
log_apply_disk(uint64_t n_nodes)
{
    int r;
    struct node_list nodes;
    uint64_t count, n;
    offset_t off = the_log.byteoff;

    /* We're rolling back to the on-disk state, discard in-memory data */
    log_init(the_log.npages);

    while (n_nodes) {
	n = JMIN(n_nodes, the_log.max_mem);
	n_nodes -= n;
	TAILQ_INIT(&nodes);
	if ((r = log_read_log(off, n, &nodes)) < 0) {
	    log_free_list(&nodes);
	    return r;
	}
	if ((r = log_write_to_disk(&nodes, &count)) < 0) {
	    log_free_list(&nodes);
	    return r;
	}
	assert(count == n);
	log_free_list(&nodes);
	off += n * PGSIZE;
    }

    log_init(the_log.npages);
    return 0;
}

int
log_apply_mem(void)
{
    int64_t r;
    uint64_t count;
    if ((r = log_write_to_disk(&the_log.nodes, &count)) < 0)
	return r;

    struct btree_node *node;
    TAILQ_FOREACH(node, &the_log.nodes, node_log_link)
	hash_del(&the_log.disk_map, node->block.offset);

    assert(count == log_free_list(&the_log.nodes));
    the_log.in_mem -= count;
    assert(the_log.in_mem == 0);

    while (the_log.disk_map.size) {
	struct node_list in_log;
	TAILQ_INIT(&in_log);
	r = log_read_map(&the_log.disk_map, the_log.max_mem, &in_log);
	assert(r != 0);
	if (r < 0)
	    return r;

	r = log_write_to_disk(&in_log, &count);
	if (r < 0)
	    return r;

	TAILQ_FOREACH(node, &in_log, node_log_link)
	    hash_del(&the_log.disk_map, node->block.offset);
	log_free_list(&in_log);
    }

    log_init(the_log.npages);
    return 0;
}

int
log_must_apply()
{
    return the_log.must_apply;
}

void
log_init(uint64_t log_pages)
{
    if (!the_log.tailq_inited) {
	TAILQ_INIT(&the_log.nodes);
	TAILQ_INIT(&the_log.free_nodes);
	the_log.tailq_inited = 1;

	for (uint64_t i = 0; i < LOG_MEMORY; i++) {
	    struct btree_node *node;
	    int r = page_alloc((void **) &node);
	    if (r < 0)
		panic("log_init: cannot allocate buffers: %s\n", e2s(r));
	    TAILQ_INSERT_TAIL(&the_log.free_nodes, node, node_log_link);
	}
    }

    for (uint32_t i = 0;
	 i < log_pages / (PGSIZE / sizeof(struct hashentry)) + 1; i++)
    {
	if (i >= sizeof(the_log.map_back) / sizeof(the_log.map_back[0]))
	    panic("log size exceeds MAX_LOG_PAGES");
	if (!the_log.map_back[i]) {
	    int r = page_alloc((void **) &the_log.map_back[i]);
	    if (r < 0)
		panic("cannot allocate log: %s", e2s(r));
	}
	memset(the_log.map_back[i], 0, PGSIZE);
    }

    log_free_list(&the_log.nodes);
    hash_init2(&the_log.disk_map, the_log.map_back, log_pages, PGSIZE);
    the_log.in_mem = 0;
    the_log.on_disk = 0;
    the_log.byteoff = LOG_OFFSET * PGSIZE;
    the_log.npages = log_pages;
    the_log.max_mem = LOG_MEMORY;
    the_log.must_apply = 0;
}
