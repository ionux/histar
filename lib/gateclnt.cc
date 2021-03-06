extern "C" {
#include <inc/lib.h>
#include <inc/syscall.h>
#include <inc/setjmp.h>
#include <inc/taint.h>
#include <inc/memlayout.h>
#include <inc/gateparam.h>
#include <inc/stdio.h>
#include <inc/signal.h>
#include <inc/features.h>

#include <string.h>
#include <inttypes.h>
}

#include <inc/gateclnt.hh>
#include <inc/gateinvoke.hh>
#include <inc/cpplabel.hh>
#include <inc/scopeguard.hh>
#include <inc/error.hh>
#include <inc/labelutil.hh>

enum { gate_client_debug = 0 };

static void __attribute__((noreturn))
return_stub(jos_jmp_buf *jb, uint64_t tid, void (*returncb)(void*), void *cbarg)
{
    if (!__jos_entry_allregs) {
	struct thread_entry_args targ;
	sys_self_get_entry_args(&targ);

	jb = (jos_jmp_buf *) (uintptr_t) targ.te_arg[0];
	tid = targ.te_arg[1];
	returncb = (void (*)(void*)) (uintptr_t) targ.te_arg[2];
	cbarg = (void *) (uintptr_t) targ.te_arg[3];
    }

    uint64_t mytid = sys_self_id();
    if (mytid != tid) {
	cprintf("return_stub: wrong thread id %"PRIu64" vs %"PRIu64"\n",
		mytid, tid);
	sys_self_halt();
    }

    if (returncb)
	returncb(cbarg);

    // Note: cannot use tls_data variable since it's in RW-mapped space
    gate_call_data *gcd = &TLS_DATA->tls_gate_args;
    taint_cow(gcd->taint_container, gcd->declassify_gate);

    if (gate_client_debug)
	cprintf("[%"PRIu64"] gateclnt: return_stub\n", sys_self_id());

    jos_longjmp(jb, 1);
}

static void
return_setup(cobj_ref *g, jos_jmp_buf *jb, uint64_t return_handle, uint64_t ct,
	     void (*returncb)(void*), void *cbarg)
{
    label verify(3);
    verify.set(return_handle, 0);

    thread_entry te;
    memset(&te, 0, sizeof(te));
    te.te_entry = (void *) &return_stub;
    te.te_stack = (char *) tls_stack_top - 8;
    te.te_arg[0] = (uintptr_t) jb;
    te.te_arg[1] = thread_id();
    te.te_arg[2] = (uintptr_t) returncb;
    te.te_arg[3] = (uintptr_t) cbarg;
    error_check(sys_self_get_as(&te.te_as));

    int64_t id = sys_gate_create(ct, &te, 0, 0, verify.to_ulabel(),
				 "return gate", 0);
    if (id < 0)
	throw error(id, "return_setup: creating return gate");

    *g = COBJ(ct, id);
}

gate_call::gate_call(cobj_ref gate,
		     const label *cs, const label *ds, const label *dr)
    : call_taint_(0), call_grant_(0),
      gate_(gate), call_ct_obj_(COBJ(0, 0)),
      taint_ct_obj_(COBJ(0, 0)), return_gate_(COBJ(0, 0)),
      tgt_label_(0), tgt_clear_(0)
{
    // Create a handle for the duration of this call
    error_check(call_taint_ = handle_alloc());
    scope_guard<void, uint64_t> drop_star1(thread_drop_star, call_taint_);

    error_check(call_grant_ = handle_alloc());
    scope_guard<void, uint64_t> drop_star2(thread_drop_star, call_grant_);

    // Compute the target labels
    label new_ds(ds ? *ds : label(3));
    new_ds.set(call_taint_, LB_LEVEL_STAR);
    new_ds.set(call_grant_, LB_LEVEL_STAR);

    label new_dr(dr ? *dr : label(0));
    new_dr.set(call_taint_, 3);
    new_dr.set(call_grant_, 3);

    tgt_label_ = new label();
    scope_guard<void, label*> del_tl(delete_obj, tgt_label_);
    tgt_clear_ = new label();
    scope_guard<void, label*> del_tc(delete_obj, tgt_clear_);
    gate_compute_labels(gate, cs, &new_ds, &new_dr, tgt_label_, tgt_clear_);

    // Create a container to hold data across the gate call
    label call_obj_label_;
    thread_cur_label(&call_obj_label_);
    call_obj_label_.transform(label::star_to, call_obj_label_.get_default());
    if (cs) {
	label tmp;
	call_obj_label_.merge(cs, &tmp, label::max, label::leq_starlo);
	call_obj_label_ = tmp;
    }
    call_obj_label_.set(call_taint_, 3);
    call_obj_label_.set(call_grant_, 0);

    if (gate_client_debug)
	cprintf("Gate call container label: %s\n", call_obj_label_.to_string());

    int64_t call_ct = sys_container_alloc(start_env->shared_container,
					  call_obj_label_.to_ulabel(),
					  "gate call container", 0, CT_QUOTA_INF);
    if (call_ct < 0)
	throw error(call_ct, "gate_call: creating call container");

    call_ct_obj_ = COBJ(start_env->shared_container, call_ct);
    scope_guard<int, cobj_ref> call_ct_cleanup(sys_obj_unref, call_ct_obj_);

    // Create an MLT-like container for tainted data to live in
    label taint_obj_label_;
    call_obj_label_.merge(tgt_label_, &taint_obj_label_,
			  label::max, label::leq_starlo);

    if (gate_client_debug)
	cprintf("Gate taint container label: %s\n", taint_obj_label_.to_string());

    int64_t taint_ct = sys_container_alloc(call_ct, taint_obj_label_.to_ulabel(),
					   "gate call taint", 0, CT_QUOTA_INF);
    if (taint_ct < 0)
	throw error(taint_ct, "gate_call: creating tainted container");

    taint_ct_obj_ = COBJ(call_ct, taint_ct);

    // Let the destructor clean up after this point
    drop_star1.dismiss();
    drop_star2.dismiss();
    del_tl.dismiss();
    del_tc.dismiss();
    call_ct_cleanup.dismiss();
}

void __attribute__((noinline))
gate_call::set_verify(const label *vl, const label *vc)
{
    // Set the verify label; prove ownership of call handles
    label new_vl(vl ? *vl : label(3));
    label new_vc(vc ? *vc : label(0));
    new_vl.set(call_grant_, LB_LEVEL_STAR);
    new_vl.set(call_taint_, LB_LEVEL_STAR);
    new_vc.set(call_grant_, 3);
    new_vc.set(call_taint_, 3);
    error_check(sys_self_set_verify(new_vl.to_ulabel(), new_vc.to_ulabel()));
}

void
gate_call::call(gate_call_data *gcd_param, const label *vl, const label *vc,
		void (*returncb)(void*), void *cbarg, bool setup_return_gate)
{
    set_verify(vl, vc);

    // Create a return gate in the taint container
    jos_jmp_buf back_from_call;
    if (setup_return_gate)
	return_setup(&return_gate_, &back_from_call, call_grant_, call_ct_obj_.object,
		     returncb, cbarg);

    // Gate call parameters
    gate_call_data *d = &tls_data->tls_gate_args;
    if (gcd_param)
	memcpy(d, gcd_param, sizeof(*d));
    d->return_gate = return_gate_;
    d->taint_container = taint_ct_obj_.object;
    d->thread_ref_ct = d->taint_container;
    d->call_taint = call_taint_;
    d->call_grant = call_grant_;

    error_check(sys_self_addref(d->thread_ref_ct));
    error_check(sys_self_set_sched_parents(start_env->proc_container,
					   d->thread_ref_ct));

    // Flush delayed unmap segment mappings; if we come back tainted,
    // we won't be able to look at our in-memory delayed unmap cache.
    // Perhaps this should be fixed by allowing a RW-mapping to be
    // mapped RO if that's all that the labeling permits.
    segment_unmap_flush();

    // Off into the gate!
    if (jos_setjmp(&back_from_call) == 0) {
	if (gate_client_debug)
	    cprintf("[%"PRIu64"] gate_call: invoking with label %s, clear %s\n",
		    thread_id(), tgt_label_->to_string(), tgt_clear_->to_string());

	error_check(sys_gate_enter(gate_, tgt_label_->to_ulabel(),
					  tgt_clear_->to_ulabel(), 0));
	throw basic_exception("gate_call: sys_gate_enter returned 0");
    }

    if (gate_client_debug)
	cprintf("[%"PRIu64"] gate_call: returned\n", thread_id());

    // Restore cached thread ID, just to be safe
    tls_revalidate();

    error_check(sys_self_set_sched_parents(start_env->proc_container, 0));
    thread_label_cache_invalidate();

    // Copy back the arguments
    if (gcd_param)
	memcpy(gcd_param, d, sizeof(*d));

    // Check for pending signals while we were in another address space..
    signal_trap_if_pending();
}

gate_call::~gate_call()
{
    try {
	sys_obj_unref(call_ct_obj_);
	if (return_gate_.object)
	    sys_obj_unref(return_gate_);

	if (tgt_label_)
	    delete tgt_label_;
	if (tgt_clear_)
	    delete tgt_clear_;

	thread_drop_starpair(call_taint_, call_grant_);
    } catch (std::exception &e) {
	cprintf("gate_call::~gate_call: %s\n", e.what());
    }
}
