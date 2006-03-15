extern "C" {
#include <inc/lib.h>
#include <inc/syscall.h>
#include <inc/setjmp.h>
#include <inc/taint.h>
#include <inc/memlayout.h>

#include <string.h>
}

#include <inc/gateclnt.hh>
#include <inc/gateparam.hh>
#include <inc/gateinvoke.hh>
#include <inc/cpplabel.hh>
#include <inc/scopeguard.hh>
#include <inc/error.hh>
#include <inc/labelutil.hh>

static void __attribute__((noreturn))
return_stub(struct jos_jmp_buf *jb)
{
    struct gate_call_data *gcd = (struct gate_call_data *) UTLS;
    taint_cow(gcd->mlt_like_container);
    jos_longjmp(jb, 1);
}

static void
return_setup(struct cobj_ref *g, struct jos_jmp_buf *jb,
	     void *tls, uint64_t return_handle)
{
    label clear;
    thread_cur_clearance(&clear);
    clear.set(return_handle, 0);

    label label;
    thread_cur_label(&label);

    struct thread_entry te;
    te.te_entry = (void *) &return_stub;
    te.te_stack = (char *) tls + PGSIZE - 8;
    te.te_arg = (uint64_t) jb;
    error_check(sys_self_get_as(&te.te_as));

    uint64_t ct = kobject_id_thread_ct;
    int64_t id = sys_gate_create(ct, &te,
				 clear.to_ulabel(),
				 label.to_ulabel(),
				 "return gate");
    if (id < 0)
	throw error(id, "return_setup: creating return gate");

    *g = COBJ(ct, id);
}

static void
make_mlt_like_ct(label *cs, label *ds, label *dr,
		 label *tgt_s, label *tgt_r,
		 void *arg)
{
    uint64_t *ctp = (uint64_t *) arg;

    label mlt_like_label(*tgt_s);
    label thread_label;
    thread_cur_label(&thread_label);

    // XXX should grant a preset handle at * so others cannot use it
    mlt_like_label.merge_with(&thread_label, label::max, label::leq_starlo);
    mlt_like_label.transform(label::star_to, 1);

    int64_t id = sys_container_alloc(start_env->proc_container,
				     mlt_like_label.to_ulabel(),
				     "gate call mlt-like thing");
    if (id < 0)
	throw error(id, "make_mlt_like_ct: creating container");

    *ctp = id;
}

void
gate_call(struct cobj_ref gate, struct gate_call_data *gcd_param,
	  label *cs, label *ds, label *dr)
{
    void *tls = (void *) UTLS;

    int64_t return_handle = sys_handle_create();
    error_check(return_handle);
    scope_guard<void, uint64_t> g1(thread_drop_star, return_handle);

    struct cobj_ref return_gate;
    struct jos_jmp_buf back_from_call;
    return_setup(&return_gate, &back_from_call, tls, return_handle);
    scope_guard<int, struct cobj_ref> g2(sys_obj_unref, return_gate);

    label new_ds(ds ? *ds : label());
    new_ds.set(return_handle, LB_LEVEL_STAR);

    struct gate_call_data *d = (struct gate_call_data *) tls;
    if (gcd_param)
	memcpy(d, gcd_param, sizeof(*d));
    d->return_gate = return_gate;

    if (jos_setjmp(&back_from_call) == 0)
	gate_invoke(gate, cs, &new_ds, dr,
		    &make_mlt_like_ct, &d->mlt_like_container);

    if (gcd_param)
	memcpy(gcd_param, d, sizeof(*d));
}
