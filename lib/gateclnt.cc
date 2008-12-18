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
return_setup(cobj_ref *g, jos_jmp_buf *jb, uint64_t return_i, uint64_t ct,
	     void (*returncb)(void*), void *cbarg)
{
    label verify;
    verify.add(return_i);

    thread_entry te;
    memset(&te, 0, sizeof(te));
    te.te_entry = (void *) &return_stub;
    te.te_stack = (char *) tls_stack_top - 8;
    te.te_arg[0] = (uintptr_t) jb;
    te.te_arg[1] = thread_id();
    te.te_arg[2] = (uintptr_t) returncb;
    te.te_arg[3] = (uintptr_t) cbarg;
    error_check(sys_self_get_as(&te.te_as));

    int64_t id = sys_gate_create(ct, &te, 0, 0, 0, verify.to_ulabel(),
				 "return gate");
    if (id < 0)
	throw error(id, "return_setup: creating return gate");

    *g = COBJ(ct, id);
}

gate_call::gate_call(cobj_ref gate, const label *owner, const label *clear)
    : call_taint_(0), call_grant_(0), gate_(gate), call_ct_obj_(COBJ(0, 0)),
      return_gate_(COBJ(0, 0)), owner_(0), clear_(0)
{
    // Create a handle for the duration of this call
    error_check(call_taint_ = category_alloc(1));
    scope_guard<void, uint64_t> drop_star1(thread_drop_star, call_taint_);

    error_check(call_grant_ = category_alloc(0));
    scope_guard<void, uint64_t> drop_star2(thread_drop_star, call_grant_);

    // Compute the target labels
    owner_ = new label();
    scope_guard<void, label*> del_o(delete_obj, owner_);
    get_label_retry_obj(owner_, sys_obj_get_ownership, gate_);
    if (owner)
	owner_->add(*owner);

    clear_ = new label();
    scope_guard<void, label*> del_c(delete_obj, clear_);
    get_label_retry_obj(clear_, sys_obj_get_clearance, gate_);
    if (clear)
	clear_->add(*clear);

    owner_->add(call_taint_);
    owner_->add(call_grant_);

    // Create a container to hold data across the gate call
    label call_obj_label_;
    thread_cur_label(&call_obj_label_);
    call_obj_label_.add(call_taint_);
    call_obj_label_.add(call_grant_);

    if (gate_client_debug)
	cprintf("Gate call container label: %s\n", call_obj_label_.to_string());

    int64_t call_ct = sys_container_alloc(start_env->shared_container,
					  call_obj_label_.to_ulabel(),
					  "gate call container", 0, CT_QUOTA_INF);
    if (call_ct < 0)
	throw error(call_ct, "gate_call: creating call container");

    call_ct_obj_ = COBJ(start_env->shared_container, call_ct);

    // Let the destructor clean up after this point
    drop_star1.dismiss();
    drop_star2.dismiss();
    del_o.dismiss();
    del_c.dismiss();
}

void __attribute__((noinline))
gate_call::set_verify(const label *vo, const label *vc)
{
    // Set the verify label; prove ownership of call handles
    label new_vo(vo ? *vo : label());
    label new_vc(vc ? *vc : label());

    new_vo.add(call_grant_);
    new_vo.add(call_taint_);

    error_check(sys_self_set_verify(new_vo.to_ulabel(), new_vc.to_ulabel()));
}

void
gate_call::call(gate_call_data *gcd_param, const label *vo, const label *vc,
		void (*returncb)(void*), void *cbarg, bool setup_return_gate)
{
    set_verify(vo, vc);

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
    d->taint_container = call_ct_obj_.object;
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
	    cprintf("[%"PRIu64"] gate_call: invoking with owner %s, clear %s\n",
		    thread_id(), owner_->to_string(), clear_->to_string());

	error_check(sys_gate_enter(gate_, owner_->to_ulabel(),
					  clear_->to_ulabel(), 0));
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

	if (owner_)
	    delete owner_;
	if (clear_)
	    delete clear_;

	thread_drop_starpair(call_taint_, call_grant_);
    } catch (std::exception &e) {
	cprintf("gate_call::~gate_call: %s\n", e.what());
    }
}
