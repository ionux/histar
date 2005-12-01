#include <inc/syscall.h>
#include <inc/assert.h>
#include <inc/stdio.h>

int
main(int ac, char **av)
{
    // root container, always 0 for now (sequential alloc)
    int rc = 0;

    int sci = sys_container_alloc(rc);
    if (sci < 0)
	panic("cannot allocate sub-container: %d", sci);

    int64_t sc = sys_container_get_c_idx(COBJ(rc, sci));
    if (sc < 0)
	panic("cannot get sub-container: %d", sc);

    int r = sys_container_store_cur_thread(sc);
    if (r < 0)
	panic("cannot addref current thread: %d", r);

    for (int i = 1; i < 10; i++) {
	container_object_type t = sys_container_get_type(COBJ(rc, i));
	cprintf("<%d:%d> type %s\n", rc, i,
				     t == cobj_thread ? "thread" :
				     t == cobj_container ? "container" :
				     t == cobj_pmap ? "pmap" :
				     t == cobj_none ? "none" : "other");

	if (i != sci)
	    sys_container_unref(COBJ(rc, i));
    }
    cprintf("ct_unref now dropping sub-container\n");
    sys_container_unref(COBJ(rc, sci));

    panic("ct_unref: still alive, strange");
}
