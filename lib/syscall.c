#include <inc/types.h>
#include <inc/syscall.h>
#include <inc/syscall_num.h>

int
sys_cons_puts(const char *s, uint64_t size)
{
    return syscall(SYS_cons_puts, (uint64_t) s, size);
}

int
sys_cons_getc(void)
{
    return syscall(SYS_cons_getc);
}

int64_t
sys_net_wait(uint64_t waiter_id, int64_t waitgen)
{
    return syscall(SYS_net_wait, waiter_id, waitgen);
}

int
sys_net_buf(struct cobj_ref seg, uint64_t offset, netbuf_type type)
{
    return syscall(SYS_net_buf, seg, offset, type);
}

int
sys_net_macaddr(uint8_t *addrbuf)
{
    return syscall(SYS_net_macaddr, (uint64_t) addrbuf);
}

int64_t
sys_container_alloc(uint64_t parent)
{
    return syscall(SYS_container_alloc, parent);
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
    return syscall(SYS_obj_get_label, o, (uint64_t) l);
}

int
sys_obj_get_name(struct cobj_ref o, char *name)
{
    return syscall(SYS_obj_get_name, o, (uint64_t) name);
}

int
sys_obj_set_name(struct cobj_ref o, char *name)
{
    return syscall(SYS_obj_set_name, o, (uint64_t) name);
}

int64_t
sys_container_nslots(uint64_t container)
{
    return syscall(SYS_container_nslots, container);
}

int64_t
sys_gate_create(uint64_t container, struct thread_entry *te,
		struct ulabel *el, struct ulabel *tl)
{
    return syscall(SYS_gate_create, container, (uint64_t) te,
		   (uint64_t) el, (uint64_t) tl);
}

int
sys_gate_enter(struct cobj_ref gate, uint64_t a1, uint64_t a2)
{
    return syscall(SYS_gate_enter, gate, a1, a2);
}

int64_t
sys_thread_create(uint64_t container)
{
    return syscall(SYS_thread_create, container);
}

int
sys_thread_start(struct cobj_ref thread, struct thread_entry *entry)
{
    return syscall(SYS_thread_start, thread, (uint64_t) entry);
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

void
sys_thread_sleep(uint64_t msec)
{
    syscall(SYS_thread_sleep, msec);
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
    return syscall(SYS_thread_get_as, (uint64_t) as_obj);
}

int64_t
sys_segment_create(uint64_t container, uint64_t num_pages)
{
    return syscall(SYS_segment_create, container, num_pages);
}

int
sys_segment_resize(struct cobj_ref seg, uint64_t num_pages)
{
    return syscall(SYS_segment_resize, seg, num_pages);
}

int64_t
sys_segment_get_npages(struct cobj_ref seg)
{
    return syscall(SYS_segment_get_npages, seg);
}

int64_t
sys_as_create(uint64_t container)
{
    return syscall(SYS_as_create, container);
}

int
sys_as_get(struct cobj_ref as, struct u_address_space *uas)
{
    return syscall(SYS_as_get, as, (uint64_t) uas);
}

int
sys_as_set(struct cobj_ref as, struct u_address_space *uas)
{
    return syscall(SYS_as_set, as, (uint64_t) uas);
}
