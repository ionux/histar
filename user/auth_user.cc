extern "C" {
#include <inc/lib.h>
#include <inc/authd.h>
#include <inc/error.h>
#include <inc/gateparam.h>
#include <inc/string.h>
#include <inc/sha1.h>
#include <inc/syscall.h>
#include <inc/stdio.h>

#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
}

#include <inc/authclnt.hh>
#include <inc/gatesrv.hh>
#include <inc/cpplabel.hh>
#include <inc/labelutil.hh>
#include <inc/scopeguard.hh>
#include <inc/error.hh>
#include <inc/cooperate.hh>

static uint64_t root_grant;
static int64_t user_grant, user_taint;
static cobj_ref user_password_seg;
static cobj_ref base_as_ref;
static char *respect_root;

struct user_password {
    char pwhash[20];
};

struct retry_seg {
    uint64_t xh;
    uint64_t attempts;
};

static void __attribute__((noreturn))
auth_grant_entry(uint64_t arg, gate_call_data *parm, gatesrv_return *gr)
{
    char log_msg[256];
    snprintf(&log_msg[0], sizeof(log_msg), "authenticated OK");
    auth_log(log_msg);

    auth_ugrant_reply *reply = (auth_ugrant_reply *) &parm->param_buf[0];
    reply->user_grant = user_grant;
    reply->user_taint = user_taint;

    label *ds = new label(3);
    ds->set(user_grant, LB_LEVEL_STAR);
    ds->set(user_taint, LB_LEVEL_STAR);

    label *dr = new label(0);
    dr->set(user_grant, 3);
    dr->set(user_taint, 3);

    gr->ret(0, ds, dr);
}

static void __attribute__((noreturn))
auth_uauth_entry(uint64_t arg, gate_call_data *parm, gatesrv_return *gr)
{
    auth_uauth_req *req = (auth_uauth_req *) &parm->param_buf[0];
    auth_uauth_reply *reply = (auth_uauth_reply *) &parm->param_buf[0];
    uint64_t retry_seg_id = arg;

    req->pass[sizeof(req->pass) - 1] = '\0';
    req->npass[sizeof(req->npass) - 1] = '\0';

    try {
	struct retry_seg *rs = 0;
	uint64_t nbytes = sizeof(*rs);
	error_check(segment_map(COBJ(req->session_ct, retry_seg_id),
				0, SEGMAP_READ | SEGMAP_WRITE,
				(void **) &rs, &nbytes, 0));
	uint64_t xh = rs->xh;
	if (rs->attempts >= 1)
	    throw basic_exception("too many authentication attempts");
	rs->attempts++;

	struct user_password *pw = 0;
	nbytes = sizeof(*pw);
	error_check(segment_map(user_password_seg, 0, SEGMAP_READ,
				(void **) &pw, &nbytes, 0));
    	scope_guard<int, void *> unmap(segment_unmap, pw);

	sha1_ctx sctx;
	sha1_init(&sctx);
	sha1_update(&sctx, (unsigned char *) req->pass, strlen(req->pass));

	char pwhash[20];
	sha1_final((unsigned char *) &pwhash[0], &sctx);

    	if (memcmp(pwhash, pw->pwhash, sizeof(pw->pwhash))) {
	    label vl, vc;
	    thread_cur_verify(&vl, &vc);

	    label v_root(3);
	    v_root.set(root_grant, 0);
	    if (respect_root[0] != '1' || vl.compare(&v_root, label::leq_starlo) < 0)
		throw error(-E_INVAL, "bad password");
	}

    	if (req->change_pw) {
	    struct user_password *pw2 = 0;
	    error_check(segment_map(user_password_seg, 0, SEGMAP_READ | SEGMAP_WRITE,
				    (void **) &pw2, &nbytes, 0));
	    scope_guard<int, void *> unmap2(segment_unmap, pw2);

	    sha1_init(&sctx);
	    sha1_update(&sctx, (unsigned char *) req->npass, strlen(req->npass));
	    sha1_final((unsigned char *) &pw2->pwhash[0], &sctx);
	}

    	reply->err = 0;
    	label *ds = new label(3);
    	ds->set(xh, LB_LEVEL_STAR);
    	gr->ret(0, ds, 0);
    } catch (error &e) {
    	cprintf("authd_user_entry: %s\n", e.what());
    	reply->err = e.err();
    } catch (std::exception &e) {
    	cprintf("authd_user_entry: %s\n", e.what());
    	reply->err = -E_INVAL;
    }
    gr->ret(0, 0, 0);
}

static void __attribute__((noreturn))
auth_user_entry(uint64_t arg, struct gate_call_data *parm, gatesrv_return *gr)
{
    auth_user_req req = *(auth_user_req *) &parm->param_buf[0];
    auth_user_reply reply;
    memset(&reply, 0, sizeof(reply));

    try {
	reply.ug_cat = user_grant;
	reply.ut_cat = user_taint;

	if (req.req_cats)
	    goto ret;

	char log_msg[256];
	snprintf(&log_msg[0], sizeof(log_msg), "attempting login..");
	auth_log(log_msg);

	int64_t xh;
	error_check(xh = handle_alloc());

	// OK, creating the retry counter.  First create an empty segment.

	label retry_l(1);
	retry_l.set(user_grant, 0);
	retry_l.set(user_taint, 3);

	cobj_ref retry_seg;
	struct retry_seg *rs = 0;
	error_check(segment_alloc(req.session_ct, sizeof(*rs),
				  &retry_seg, (void **) &rs,
				  retry_l.to_ulabel(), "retry counter"));
	rs->xh = xh;
	rs->attempts = 0;

	// Now try to copy it with a label of { Pt 3 } using the coop mechanism.

	retry_l.set(req.pw_taint, 3);

	coop_sysarg coop_vals[8];
	memset(&coop_vals[0], 0, sizeof(coop_vals));

	coop_vals[0].u.i = SYS_segment_copy;
	coop_vals[1].u.i = retry_seg.container;
	coop_vals[2].u.i = retry_seg.object;
	coop_vals[3].u.i = req.session_ct;
	coop_vals[4].u.l = &retry_l;
	coop_vals[4].is_label = 1;

	label coop_ds(3);
	coop_ds.set(user_grant, LB_LEVEL_STAR);
	coop_ds.set(user_taint, LB_LEVEL_STAR);

	label coop_dr(0);
	coop_dr.set(user_grant, 3);
	coop_dr.set(user_taint, 3);
	coop_dr.set(req.session_grant, 2);

	int64_t retry_seg_copy_id;
	error_check(retry_seg_copy_id =
	    coop_gate_invoke(COBJ(req.session_ct, req.coop_gate),
			     0, &coop_ds, &coop_dr, coop_vals));

	cobj_ref retry_seg_copy = COBJ(req.session_ct, retry_seg_copy_id);

	label retry_l2;
	obj_get_label(retry_seg_copy, &retry_l2);
	error_check(retry_l.compare(&retry_l2, label::eq));

	// Whew, created that pesky retry segment..

	label verify(3);
	verify.set(req.session_grant, 0);

	gatesrv_descriptor gd;
	gd.gate_container_ = req.session_ct;
	gd.name_ = "user auth gate";
	gd.as_ = base_as_ref;
	gd.label_ = 0;
	gd.clearance_ = 0;
	gd.verify_ = &verify;
	gd.func_ = auth_uauth_entry;
	gd.arg_ = retry_seg_copy.object;
	cobj_ref ga = gate_create(&gd);

	verify.set(xh, 0);
	gd.name_ = "user grant gate";
	gd.func_ = auth_grant_entry;
	gd.arg_ = 0;
	cobj_ref gg = gate_create(&gd);

	reply.err = 0;
	reply.uauth_gate = ga.object;
	reply.ugrant_gate = gg.object;
	reply.xh = xh;
    } catch (error &e) {
    	cprintf("auth_user_entry: %s\n", e.what());
    	reply.err = e.err();
    } catch (std::exception &e) {
    	cprintf("auth_user_entry: %s\n", e.what());
    	reply.err = -E_INVAL;
    }

ret:
    memcpy(&parm->param_buf[0], &reply, sizeof(reply));
    gr->ret(0, 0, 0);
}

static void
auth_user_init(void)
{
    /* Don't need any input, close our stdin */
    close(0);
    open("/dev/null", O_RDWR);

    error_check(sys_self_get_as(&base_as_ref));

    int64_t config_ct = 0;
    error_check(config_ct = sys_container_alloc(start_env->shared_container,
						0, "config", 0, 65536));

    // file which controls whether to respect root authority (val: "0" or "1")
    label user_write_protect(1);
    user_write_protect.set(user_grant, 0);

    cobj_ref respect_root_seg;
    error_check(segment_alloc(config_ct, 2, &respect_root_seg,
			      (void **) &respect_root,
			      user_write_protect.to_ulabel(),
			      "respect-root"));
    respect_root[0] = '1';
    respect_root[1] = '\n';
    error_check(segment_unmap(respect_root));
    error_check(segment_map(respect_root_seg, 0, SEGMAP_READ,
			    (void **) &respect_root, 0, 0));

    // files which export the user's grant and taint categories
    char *s = 0;
    error_check(segment_alloc(config_ct, 128, 0, (void **) &s,
			      0, "user-grant"));
    snprintf(s, 128, "%"PRIu64"\n", user_grant);
    error_check(segment_unmap(s));

    s = 0;
    error_check(segment_alloc(config_ct, 128, 0, (void **) &s,
			      0, "user-taint"));
    snprintf(s, 128, "%"PRIu64"\n", user_taint);
    error_check(segment_unmap(s));

    // password segment
    label pw_ctm(1);
    pw_ctm.set(user_grant, 0);
    pw_ctm.set(user_taint, 3);

    struct user_password *pw = 0;
    error_check(segment_alloc(config_ct,
			      sizeof(struct user_password),
			      &user_password_seg,
			      (void **)&pw, pw_ctm.to_ulabel(), "password"));
    scope_guard<int, void *> unmap(segment_unmap, pw);

    sha1_ctx sctx;
    sha1_init(&sctx);
    sha1_final((unsigned char *) pw->pwhash, &sctx);

    // any error output
    struct cobj_ref log_seg;
    error_check(segment_alloc(config_ct, 0, &log_seg,
			      0, pw_ctm.to_ulabel(), "log"));

    char logpn[64];
    sprintf(&logpn[0], "#%"PRIu64".%"PRIu64, log_seg.container, log_seg.object);
    int logfd = open(logpn, O_RDWR);
    if (logfd < 0) {
	printf("cannot open log file: %d %s\n", logfd, strerror(errno));
    } else {
	dup2(logfd, 1);
	dup2(logfd, 2);
	close(logfd);
    }

    // gate
    label th_ctm, th_clr;
    thread_cur_label(&th_ctm);
    thread_cur_clearance(&th_clr);
    th_ctm.set(start_env->process_grant, 1);

    gate_create(start_env->shared_container,
		"user login gate", &th_ctm, &th_clr, 0,
		&auth_user_entry, 0);

    int64_t new_mtab_id =
	sys_segment_copy(start_env->fs_mtab_seg, start_env->shared_container,
			 0, "private mount table");
    error_check(new_mtab_id);
    start_env->fs_mtab_seg = COBJ(start_env->shared_container, new_mtab_id);

    printf("auth_user: ready\n");
}

int
main(int ac, char **av)
{
    try {
	if (ac != 2 && ac != 4) {
	    cprintf("Usage: %s root-grant-handle [user-grant user-taint]\n", av[0]);
	    return -1;
	}

	error_check(strtou64(av[1], 0, 10, &root_grant));

	if (ac == 4) {
	    error_check(strtou64(av[2], 0, 10, (uint64_t *) &user_grant));
	    error_check(strtou64(av[3], 0, 10, (uint64_t *) &user_taint));
	} else {
	    error_check(user_grant = handle_alloc());
	    error_check(user_taint = handle_alloc());
	}

    	auth_user_init();
	process_report_exit(0, 0);
	sys_self_halt();
    } catch (std::exception &e) {
    	printf("auth_user: %s\n", e.what());
    	return -1;
    }
}
