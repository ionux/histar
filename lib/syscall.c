#include <inc/types.h>
#include <inc/syscall.h>
#include <inc/syscall_num.h>

int
sys_cons_puts(const char *s, uint64_t size)
{
    return syscall(SYS_cons_puts, s, size);
}

int
sys_cons_getc(void)
{
    return syscall(SYS_cons_getc);
}

int64_t
sys_net_create(uint64_t container, struct ulabel *l, const char *name)
{
    return syscall(SYS_net_create, container, l, name);
}

int64_t
sys_net_wait(struct cobj_ref nd, uint64_t waiter_id, int64_t waitgen)
{
    return syscall(SYS_net_wait, nd, waiter_id, waitgen);
}

int
sys_net_buf(struct cobj_ref nd, struct cobj_ref seg, uint64_t offset,
	    netbuf_type type)
{
    return syscall(SYS_net_buf, nd, seg, offset, type);
}

int
sys_net_macaddr(struct cobj_ref nd, uint8_t *addrbuf)
{
    return syscall(SYS_net_macaddr, nd, addrbuf);
}

int64_t
sys_container_alloc(uint64_t parent, struct ulabel *ul, const char *name)
{
    return syscall(SYS_container_alloc, parent, ul, name);
}

int
sys_obj_unref(struct cobj_ref o)
{
    return syscall(SYS_obj_unref, o);
}

int64_t
sys_container_get_slot_id(uint64_t container, uint64_t slot)
{
    return syscall(SYS_container_get_slot_id, container, slot);
}

int64_t
sys_handle_create(void)
{
    return syscall(SYS_handle_create);
}

kobject_type_t
sys_obj_get_type(struct cobj_ref o)
{
    return syscall(SYS_obj_get_type, o);
}

int
sys_obj_get_label(struct cobj_ref o, struct ulabel *l)
{
    return syscall(SYS_obj_get_label, o, l);
}

int
sys_obj_get_name(struct cobj_ref o, char *name)
{
    return syscall(SYS_obj_get_name, o, name);
}

int64_t
sys_container_nslots(uint64_t container)
{
    return syscall(SYS_container_nslots, container);
}

int64_t
sys_gate_create(uint64_t container, struct thread_entry *te,
		struct ulabel *el, struct ulabel *tl,
		const char *name)
{
    return syscall(SYS_gate_create, container, te, el, tl, name);
}

int
sys_gate_enter(struct cobj_ref gate,
	       struct ulabel *l,
	       struct ulabel *clearance,
	       uint64_t a1, uint64_t a2)
{
    return syscall(SYS_gate_enter, gate, l, clearance, a1, a2);
}

int
sys_gate_clearance(struct cobj_ref gate, struct ulabel *ul)
{
    return syscall(SYS_gate_clearance, gate, ul);
}

int64_t
sys_thread_create(uint64_t container, const char *name)
{
    return syscall(SYS_thread_create, container, name);
}

int
sys_thread_start(struct cobj_ref thread, struct thread_entry *entry,
		 struct ulabel *ul, struct ulabel *clearance)
{
    return syscall(SYS_thread_start, thread, entry, ul, clearance);
}

void
sys_thread_yield(void)
{
    syscall(SYS_thread_yield);
}

void
sys_thread_halt(void)
{
    syscall(SYS_thread_halt);
}

int64_t
sys_thread_id(void)
{
    return syscall(SYS_thread_id);
}

int
sys_thread_addref(uint64_t container)
{
    return syscall(SYS_thread_addref, container);
}

int
sys_thread_get_as(struct cobj_ref *as_obj)
{
    return syscall(SYS_thread_get_as, as_obj);
}

int
sys_thread_set_as(struct cobj_ref as_obj)
{
    return syscall(SYS_thread_set_as, as_obj);
}

int
sys_thread_set_label(struct ulabel *l)
{
    return syscall(SYS_thread_set_label, l);
}

int
sys_thread_set_clearance(struct ulabel *l)
{
    return syscall(SYS_thread_set_clearance, l);
}

int
sys_thread_sync_wait(volatile uint64_t *addr, uint64_t val, uint64_t msec)
{
    return syscall(SYS_thread_sync_wait, addr, val, msec);
}

int
sys_thread_sync_wakeup(volatile uint64_t *addr)
{
    return syscall(SYS_thread_sync_wakeup, addr);
}

int64_t
sys_clock_msec(void)
{
    return syscall(SYS_clock_msec);
}

int64_t
sys_segment_create(uint64_t container, uint64_t num_bytes,
		   struct ulabel *l, const char *name)
{
    return syscall(SYS_segment_create, container, num_bytes, l, name);
}

int64_t
sys_segment_copy(struct cobj_ref seg, uint64_t container,
		 struct ulabel *l, const char *name)
{
    return syscall(SYS_segment_copy, seg, container, l, name);
}

int
sys_segment_resize(struct cobj_ref seg, uint64_t num_bytes)
{
    return syscall(SYS_segment_resize, seg, num_bytes);
}

int64_t
sys_segment_get_nbytes(struct cobj_ref seg)
{
    return syscall(SYS_segment_get_nbytes, seg);
}

int64_t
sys_as_create(uint64_t container, struct ulabel *l, const char *name)
{
    return syscall(SYS_as_create, container, l, name);
}

int
sys_as_get(struct cobj_ref as, struct u_address_space *uas)
{
    return syscall(SYS_as_get, as, uas);
}

int
sys_as_set(struct cobj_ref as, struct u_address_space *uas)
{
    return syscall(SYS_as_set, as, uas);
}

int64_t
sys_mlt_create(uint64_t container, const char *name)
{
    return syscall(SYS_mlt_create, container, name);
}

int
sys_mlt_get(struct cobj_ref mlt, uint64_t idx, struct ulabel *l, uint8_t *buf, uint64_t *ct_id)
{
    return syscall(SYS_mlt_get, mlt, idx, l, buf, ct_id);
}

int
sys_mlt_put(struct cobj_ref mlt, struct ulabel *l, uint8_t *buf, uint64_t *ct_id)
{
    return syscall(SYS_mlt_put, mlt, l, buf, ct_id);
}
