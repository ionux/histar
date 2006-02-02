#include <machine/stackwrap.h>
#include <machine/pmap.h>
#include <machine/x86.h>
#include <inc/setjmp.h>
#include <inc/error.h>

#define STACKWRAP_MAGIC	0xabcd9262deed1713

struct stackwrap_state {
    stackwrap_fn fn;
    void *fn_arg;

    void *stackbase;
    struct jmp_buf entry_cb;
    struct jmp_buf task_state;

    int alive;
    uint64_t magic;
};

static struct stackwrap_state *
stackwrap_cur(void)
{
    void *rsp = (void *) read_rsp();
    void *base = ROUNDDOWN(rsp, PGSIZE);
    struct stackwrap_state *ss = (struct stackwrap_state *) base;

    assert(ss->magic == STACKWRAP_MAGIC);
    assert(ss->alive);

    return ss;
}

static void __attribute__((noreturn))
stackwrap_entry(void)
{
    struct stackwrap_state *ss = stackwrap_cur();

    ss->fn(ss->fn_arg);
    ss->alive = 0;
    longjmp(&ss->entry_cb, 1);
}

static void
stackwrap_check(struct stackwrap_state *ss)
{
    if (ss->alive == 0)
	page_free(ss->stackbase);
}

static void
stackwrap_wakeup(struct stackwrap_state *ss)
{
    if (setjmp(&ss->entry_cb) == 0)
	longjmp(&ss->task_state, 1);

    stackwrap_check(ss);
}

static void
stackwrap_sleep(struct stackwrap_state *ss)
{
    if (setjmp(&ss->task_state) == 0)
	longjmp(&ss->entry_cb, 1);
}

int
stackwrap_call(stackwrap_fn fn, void *fn_arg)
{
    void *stackbase;
    int r = page_alloc(&stackbase);
    if (r < 0)
	return r;

    struct stackwrap_state *ss = (struct stackwrap_state *) stackbase;
    ss->fn = fn;
    ss->fn_arg = fn_arg;
    ss->stackbase = stackbase;
    ss->alive = 1;
    ss->magic = STACKWRAP_MAGIC;
    ss->task_state.jb_rip = (uint64_t) &stackwrap_entry;
    ss->task_state.jb_rsp = (uint64_t) stackbase + PGSIZE;

    stackwrap_wakeup(ss);
    return 0;
}

// Blocking disk I/O support

struct disk_io_request {
    struct stackwrap_state *ss;
    disk_io_status status;
    LIST_ENTRY(disk_io_request) link;
};

static void
disk_io_cb(disk_io_status status, void *b, uint32_t c, uint64_t o, void *arg)
{
    struct disk_io_request *ds = (struct disk_io_request *) arg;
    ds->status = status;
    stackwrap_wakeup(ds->ss);
}

disk_io_status
stackwrap_disk_io(disk_op op, void *buf, uint32_t count, uint64_t offset)
{
    struct stackwrap_state *ss = stackwrap_cur();
    struct disk_io_request ds = { .ss = ss };

    static LIST_HEAD(disk_waiters_list, disk_io_request) disk_waiters;

    for (;;) {
	int r = disk_io(op, buf, count, offset, &disk_io_cb, &ds);
	if (r == 0) {
	    stackwrap_sleep(ss);
	    break;
	} else if (r == -E_BUSY) {
	    LIST_INSERT_HEAD(&disk_waiters, &ds, link);
	    stackwrap_sleep(ss);
	} else if (r < 0) {
	    cprintf("stackwrap_disk_io: unexpected error: %s\n", e2s(r));
	    ds.status = disk_io_failure;
	    break;
	}
    }

    while (!LIST_EMPTY(&disk_waiters)) {
	struct disk_io_request *rq = LIST_FIRST(&disk_waiters);
	LIST_REMOVE(rq, link);
	stackwrap_wakeup(rq->ss);
    }

    return ds.status;
}
