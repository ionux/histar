extern "C" {
#include <inc/authd.h>
#include <inc/gateparam.h>
#include <inc/error.h>
#include <inc/stdio.h>
#include <inc/syscall.h>
#include <inc/memlayout.h>
#include <inc/auth.h>

#include <string.h>
#include <stdio.h>
#include <pwd.h>
}

#include <inc/gateclnt.hh>
#include <inc/authclnt.hh>
#include <inc/error.hh>
#include <inc/labelutil.hh>
#include <inc/scopeguard.hh>
#include <inc/cooperate.hh>

enum { auth_debug = 1 };

int
jos64_login(const char *user, const char *pass)
{
    uint64_t ug, ut;
    try {
	auth_login(user, pass, &ug, &ut);
    } catch (std::exception &e) {
	return 0;
    }

    start_env->user_taint = ut;
    start_env->user_grant = ug;
    return 1;
}

void
auth_login(const char *user, const char *pass, uint64_t *ug, uint64_t *ut)
{
    gate_call_data gcd;
    auth_dir_req      *dir_req      = (auth_dir_req *)      &gcd.param_buf[0];
    auth_dir_reply    *dir_reply    = (auth_dir_reply *)    &gcd.param_buf[0];
    auth_user_req     *user_req     = (auth_user_req *)     &gcd.param_buf[0];
    auth_user_reply   *user_reply   = (auth_user_reply *)   &gcd.param_buf[0];
    auth_uauth_req    *uauth_req    = (auth_uauth_req *)    &gcd.param_buf[0];
    auth_uauth_reply  *uauth_reply  = (auth_uauth_reply *)  &gcd.param_buf[0];
    auth_ugrant_reply *ugrant_reply = (auth_ugrant_reply *) &gcd.param_buf[0];

    fs_inode auth_dir_gt;
    error_check(fs_namei_flags("/uauth/auth_dir/authdir", &auth_dir_gt,
			       NAMEI_LEAF_NOEVAL));

    strcpy(&dir_req->user[0], user);
    dir_req->op = auth_dir_lookup;

    if (auth_debug)
	cprintf("auth_login: calling directory gate\n");

    gate_call(auth_dir_gt.obj, 0, 0, 0).call(&gcd);
    error_check(dir_reply->err);
    cobj_ref user_gate = dir_reply->user_gate;

    // Construct session container, etc.
    int64_t pw_taint, session_grant;
    error_check(pw_taint = handle_alloc());
    scope_guard<void, uint64_t> drop1(thread_drop_star, pw_taint);

    error_check(session_grant = handle_alloc());
    scope_guard<void, uint64_t> drop2(thread_drop_star, session_grant);

    label session_label(1);
    session_label.set(session_grant, 0);

    int64_t session_ct;
    error_check(session_ct =
	sys_container_alloc(start_env->shared_container,
			    session_label.to_ulabel(),
			    "login session container", 0, 65536));
    scope_guard<int, cobj_ref>
	session_drop(sys_obj_unref,
		     COBJ(start_env->shared_container, session_ct));

    // Invoke the user gate to get the user's taint and grant categories
    user_req->req_cats = 1;
    gate_call(user_gate, 0, 0, 0).call(&gcd);
    error_check(user_reply->err);

    uint64_t user_grant = user_reply->ug_cat;
    uint64_t user_taint = user_reply->ut_cat;

    // Construct a cooperative gate for creating the retry count segment
    label retry_seg_l(1);
    retry_seg_l.set(user_grant, 0);
    retry_seg_l.set(user_taint, 3);
    retry_seg_l.set(pw_taint, 3);

    label coop_gate_label(1);
    coop_gate_label.set(session_grant, LB_LEVEL_STAR);

    label coop_gate_clear(2);
    coop_gate_clear.set(pw_taint, 3);

    label coop_gate_verify(3);
    coop_gate_verify.set(session_grant, 0);

    coop_sysarg coop_vals[8];
    memset(&coop_vals[0], 0, sizeof(coop_vals));

    char coop_freemask[8];
    memset(&coop_freemask[0], 0, sizeof(coop_freemask));

    coop_vals[0].u.i = SYS_segment_copy;
    coop_vals[1].u.i = session_ct;
    coop_freemask[2] = 1;
    coop_vals[3].u.i = session_ct;
    coop_vals[4].u.l = &retry_seg_l;
    coop_vals[4].is_label = 1;

    cobj_ref coop_gate =
	coop_gate_create(session_ct, &coop_gate_label, &coop_gate_clear,
			 &coop_gate_verify, coop_vals, coop_freemask);

    // Invoke the user gate
    label user_auth_ds(3);
    user_auth_ds.set(session_grant, LB_LEVEL_STAR);

    user_req->req_cats = 0;
    user_req->pw_taint = pw_taint;
    user_req->session_ct = session_ct;
    user_req->session_grant = session_grant;
    user_req->coop_gate = coop_gate.object;

    if (auth_debug)
	cprintf("auth_login: calling user gate\n");

    gate_call(user_gate, 0, &user_auth_ds, 0).call(&gcd);
    error_check(user_reply->err);
    cobj_ref uauth_gate  = COBJ(session_ct, user_reply->uauth_gate);
    cobj_ref ugrant_gate = COBJ(session_ct, user_reply->ugrant_gate);
    uint64_t xh = user_reply->xh;

    scope_guard<void, uint64_t> xdrop(thread_drop_star, xh);

    // Call the user auth gate to check password
    label uauth_cs(0);
    uauth_cs.set(pw_taint, 3);

    label uauth_dr(0);
    uauth_dr.set(pw_taint, 3);

    label cur_label, cur_clear;
    thread_cur_label(&cur_label);
    thread_cur_clearance(&cur_clear);

    strcpy(&uauth_req->pass[0], pass);
    uauth_req->change_pw = 0;
    uauth_req->session_ct = session_ct;

    if (auth_debug)
	cprintf("auth_login: calling authentication gate\n");

    gate_call(uauth_gate, &uauth_cs, 0, &uauth_dr).call(&gcd, &cur_label);
    error_check(uauth_reply->err);

    // Try to be really paranoid here about not accidentally revealing
    // any extra information from uauth_gate.
    memset(&gcd, 0, sizeof(gcd));
    memset(((char *) tls_top) - PGSIZE, 0, PGSIZE);
    tls_revalidate();

    char buf[KOBJ_META_LEN];
    memset(&buf[0], 0, sizeof(buf));
    error_check(sys_obj_set_meta(COBJ(0, thread_id()), 0, &buf[0]));
    error_check(sys_self_fp_disable());

    label vl(3), vc(0);
    sys_self_set_verify(vl.to_ulabel(), vc.to_ulabel());

    label cur_label2, cur_clear2;
    thread_cur_label(&cur_label2);
    thread_cur_clearance(&cur_clear2);
    cur_label.set(xh, LB_LEVEL_STAR);
    cur_clear.set(xh, 3);
    error_check(cur_label2.compare(&cur_label, label::eq));
    error_check(cur_clear2.compare(&cur_clear, label::eq));

    // Done scrubbing the thread state.

    label grant_dr(0);
    grant_dr.set(xh, 2);

    if (auth_debug)
	cprintf("auth_login: calling grant gate\n");

    gate_call(ugrant_gate, 0, 0, &grant_dr).call(&gcd);

    *ug = ugrant_reply->user_grant;
    *ut = ugrant_reply->user_taint;
}

void
auth_chpass(const char *user, const char *pass, const char *npass)
{
    gate_call_data gcd;
    auth_dir_req      *dir_req      = (auth_dir_req *)      &gcd.param_buf[0];
    auth_dir_reply    *dir_reply    = (auth_dir_reply *)    &gcd.param_buf[0];
    auth_user_req     *user_req     = (auth_user_req *)     &gcd.param_buf[0];
    auth_user_reply   *user_reply   = (auth_user_reply *)   &gcd.param_buf[0];
    auth_uauth_req    *uauth_req    = (auth_uauth_req *)    &gcd.param_buf[0];
    auth_uauth_reply  *uauth_reply  = (auth_uauth_reply *)  &gcd.param_buf[0];

    fs_inode auth_dir_gt;
    error_check(fs_namei_flags("/uauth/auth_dir/authdir", &auth_dir_gt,
			       NAMEI_LEAF_NOEVAL));

    strcpy(&dir_req->user[0], user);
    dir_req->op = auth_dir_lookup;

    if (auth_debug)
	cprintf("auth_chpass: calling directory gate\n");

    gate_call(auth_dir_gt.obj, 0, 0, 0).call(&gcd);
    error_check(dir_reply->err);
    cobj_ref user_gate = dir_reply->user_gate;

    // Construct session container, etc.
    int64_t pw_taint, session_grant;
    error_check(pw_taint = handle_alloc());
    scope_guard<void, uint64_t> drop1(thread_drop_star, pw_taint);

    label cur_clear;
    thread_cur_clearance(&cur_clear);
    cur_clear.set(pw_taint, 3);
    thread_set_clearance(&cur_clear);

    error_check(session_grant = handle_alloc());
    scope_guard<void, uint64_t> drop2(thread_drop_star, session_grant);

    label session_label(1);
    session_label.set(session_grant, 0);

    int64_t session_ct;
    error_check(session_ct =
	sys_container_alloc(start_env->shared_container,
			    session_label.to_ulabel(),
			    "login session container", 0, 65536));
    scope_guard<int, cobj_ref>
	session_drop(sys_obj_unref,
		     COBJ(start_env->shared_container, session_ct));

    // Invoke the user gate to get the user's taint and grant categories
    user_req->req_cats = 1;
    gate_call(user_gate, 0, 0, 0).call(&gcd);
    error_check(user_reply->err);

    uint64_t user_grant = user_reply->ug_cat;
    uint64_t user_taint = user_reply->ut_cat;

    // Construct a cooperative gate for creating the retry count segment
    label retry_seg_l(1);
    retry_seg_l.set(user_grant, 0);
    retry_seg_l.set(user_taint, 3);
    retry_seg_l.set(pw_taint, 3);

    label coop_gate_label(1);
    coop_gate_label.set(session_grant, LB_LEVEL_STAR);

    label coop_gate_clear(2);
    coop_gate_clear.set(pw_taint, 3);

    label coop_gate_verify(3);
    coop_gate_verify.set(session_grant, 0);

    coop_sysarg coop_vals[8];
    memset(&coop_vals[0], 0, sizeof(coop_vals));

    char coop_freemask[8];
    memset(&coop_freemask[0], 0, sizeof(coop_freemask));

    coop_vals[0].u.i = SYS_segment_copy;
    coop_vals[1].u.i = session_ct;
    coop_freemask[2] = 1;
    coop_vals[3].u.i = session_ct;
    coop_vals[4].u.l = &retry_seg_l;
    coop_vals[4].is_label = 1;

    cobj_ref coop_gate =
	coop_gate_create(session_ct, &coop_gate_label, &coop_gate_clear,
			 &coop_gate_verify, coop_vals, coop_freemask);

    // Invoke user gate
    label user_auth_ds(3);
    user_auth_ds.set(session_grant, LB_LEVEL_STAR);

    user_req->req_cats = 0;
    user_req->pw_taint = pw_taint;
    user_req->session_ct = session_ct;
    user_req->session_grant = session_grant;
    user_req->coop_gate = coop_gate.object;

    if (auth_debug)
	cprintf("auth_chpass: calling user gate\n");
cprintf("-------------> PRE USER GATE\n");
int pc;
__asm__ __volatile__("mov %0, pc" : "=r" (pc));
cprintf("CALLING GATE FROM 0x%08x\n", pc);
    gate_call(user_gate, 0, &user_auth_ds, 0).call(&gcd);
cprintf("-------------> POST USER GATE\n");
    error_check(user_reply->err);
    cobj_ref uauth_gate = COBJ(session_ct, user_reply->uauth_gate);
    uint64_t xh = user_reply->xh;

    scope_guard<void, uint64_t> xdrop(thread_drop_star, xh);

    // Call the user auth gate to check password
    label uauth_ds(3);
    uauth_ds.set(pw_taint, LB_LEVEL_STAR);

    label cur_label;
    thread_cur_label(&cur_label);

    strcpy(&uauth_req->pass[0], pass);
    strcpy(&uauth_req->npass[0], npass);
    uauth_req->change_pw = 1;
    uauth_req->session_ct = session_ct;

    if (auth_debug)
	cprintf("auth_chpass: calling authentication gate\n");

    gate_call(uauth_gate, 0, &uauth_ds, 0).call(&gcd, &cur_label);
    error_check(uauth_reply->err);
}

void
auth_log(const char *msg)
{
    gate_call_data gcd;
    uint32_t len = MIN(strlen(msg) + 1, sizeof(gcd.param_buf));
    memcpy(&gcd.param_buf[0], msg, len);

    fs_inode log_gt;
    error_check(fs_namei_flags("/uauth/auth_log/authlog", &log_gt,
			       NAMEI_LEAF_NOEVAL));

    gate_call(log_gt.obj, 0, 0, 0).call(&gcd);
}
