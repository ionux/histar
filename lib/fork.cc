extern "C" {
#include <inc/lib.h>
#include <inc/stdio.h>
#include <inc/syscall.h>
#include <inc/fd.h>
#include <inc/setjmp.h>
#include <inc/memlayout.h>
#include <inc/wait.h>
#include <inc/debug_gate.h>
#include <inc/error.h>
#include <inc/process.h>

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <inttypes.h>
}

#include <inc/error.hh>
#include <inc/labelutil.hh>
#include <inc/cpplabel.hh>
#include <inc/scopeguard.hh>
#include <inc/jthread.hh>

static int fork_debug = 0;
jthread_mutex_t fork_mu;

libc_hidden_proto(fork)

static void
signal_unmask(const sigset_t *sigset)
{
    sigprocmask(SIG_SETMASK, sigset, 0);
}

static pid_t
do_fork()
{
    char namebuf[KOBJ_NAME_LEN];

    // Make all FDs independent of the process taint/grant handles
    for (int i = 0; i < MAXFD; i++)
	fd_make_public(i, 0);

    // New process gets the same label as this process,
    // except we take away our process taint&grant * and
    // instead give it its own process taint&grant *.
    label thread_clearance, thread_contaminate;
    thread_cur_clearance(&thread_clearance);
    thread_cur_label(&thread_contaminate);

    if (fork_debug)
	cprintf("fork: current labels %s / %s\n",
		thread_contaminate.to_string(),
		thread_clearance.to_string());

    thread_contaminate.set(start_env->process_grant, 1);
    thread_contaminate.set(start_env->process_taint, 1);

    // Compute the mandatory contamination for objects
    label integrity_label(thread_contaminate);
    integrity_label.transform(label::star_to, integrity_label.get_default());

    label secret_label(integrity_label);

    // Generate handles for new process
    int64_t process_grant = handle_alloc();
    error_check(process_grant);
    scope_guard<void, uint64_t> pgrant_cleanup(thread_drop_star, process_grant);

    int64_t process_taint = handle_alloc();
    error_check(process_taint);
    scope_guard2<void, uint64_t, uint64_t> ptaint_cleanup(thread_drop_starpair, process_taint, process_grant);
    pgrant_cleanup.dismiss();

    // Grant process handles to the new process
    thread_contaminate.set(process_grant, LB_LEVEL_STAR);
    thread_contaminate.set(process_taint, LB_LEVEL_STAR);
    thread_clearance.set(process_grant, 3);
    thread_clearance.set(process_taint, 3);
    secret_label.set(process_grant, 0);
    secret_label.set(process_taint, 3);
    integrity_label.set(process_grant, 0);

    // Start creating the new process
    char forkname[KOBJ_NAME_LEN];
    char *fname = strrchr(jos_progname, '/');
    snprintf(&forkname[0], sizeof(forkname), "fork.%s",
	     fname ? fname + 1 : jos_progname);
    forkname[KOBJ_NAME_LEN - 1] = '\0';

    int64_t top_ct = sys_container_alloc(start_env->process_pool,
					 integrity_label.to_ulabel(),
					 &forkname[0], 0, CT_QUOTA_INF);
    if (top_ct < 0) {
	int64_t procpool = sys_container_alloc(start_env->shared_container,
					       0, "procpool", 0, CT_QUOTA_INF);
	error_check(procpool);

	start_env->process_pool = procpool;
	top_ct = sys_container_alloc(start_env->process_pool,
				     integrity_label.to_ulabel(),
				     &forkname[0], 0, CT_QUOTA_INF);
    }
    error_check(top_ct);

    struct cobj_ref top_ref = COBJ(start_env->shared_container, top_ct);
    scope_guard<int, struct cobj_ref> top_drop(sys_obj_unref, top_ref);

    int64_t proc_ct = sys_container_alloc(top_ct, secret_label.to_ulabel(),
					  "process", 0, CT_QUOTA_INF);
    error_check(proc_ct);

    // Create a process status segment for it
    struct cobj_ref proc_status_seg;
    struct process_state *procstat = 0;
    error_check(segment_alloc(top_ct, sizeof(struct process_state),
			      &proc_status_seg, (void **) &procstat, 0,
			      "process status"));
    strncpy(&procstat->procname[0], jos_progname, sizeof(procstat->procname));
    segment_unmap_delayed(procstat, 1);

    // Create a process gid for it
    struct cobj_ref process_gid_seg;
    uint64_t *child_pgid = 0;
    label pgid_label(1);
    pgid_label.set(start_env->user_grant, 0);
    try {
	error_check(segment_alloc(top_ct, sizeof(uint64_t),
				  &process_gid_seg, (void **) &child_pgid, 
				  pgid_label.to_ulabel(), "process gid"));
    } catch (error &e) {
	if (e.err() != -E_LABEL)
	    throw e;

	label tmp, out;
	thread_cur_label(&tmp);
	pgid_label.set(start_env->user_grant, 1);
	pgid_label.set(process_grant, 0);
	pgid_label.merge(&tmp, &out, label::max, label::leq_starlo);
	pgid_label = out;
	error_check(segment_alloc(top_ct, sizeof(uint64_t),
				  &process_gid_seg, (void **) &child_pgid, 
				  pgid_label.to_ulabel(), "process gid"));
    }
    scope_guard<int, void *> pgid_unmap(segment_unmap, child_pgid);

    *child_pgid = getpgrp();

    /*
     * Mask signals so we get a more consistent copy of our address space.
     */
    sigset_t oldset, maskset;
    sigfillset(&maskset);
    sigprocmask(SIG_SETMASK, &maskset, &oldset);
    scope_guard<void, const sigset_t*> unmask_sig(signal_unmask, &oldset);

    /*
     * Lock fork_mu to prevent gate exit code from calling malloc & free.
     */
    scoped_jthread_lock l(&fork_mu);

    /* 
     * uas.ents will be malloc'ed in the new process.  Allocate before 
     * jos_setjmp so the scope_guard will free it in the new process.
     */
    struct u_address_space uas;
    uas.size = 32;
    uas.ents = (u_segment_mapping *) malloc(uas.size * sizeof(uas.ents[0]));
    if (!uas.ents)
	throw error(-E_NO_MEM, "cannot allocate uas.ents");

    scope_guard<void, u_segment_mapping **> uas_ents_free(free_indir, &uas.ents);
    segment_unmap_flush();

    struct cobj_ref cur_as;
    error_check(sys_self_get_as(&cur_as));
    int r = sys_as_get(cur_as, &uas);
    while (r == -E_NO_SPACE) {
	uas.size *= 2;
	void *p = realloc(uas.ents, uas.size * sizeof(uas.ents[0]));
	if (!p)
	    throw error(-E_NO_MEM, "cannot realloc uas.ents, size %"PRIu64,
			uas.size);
	uas.ents = (u_segment_mapping *) p;

	r = sys_as_get(cur_as, &uas);
    }
    error_check(r);

    // Prepare a setjmp buffer for the new thread, before we copy our stack!
    struct jos_jmp_buf jb;
    if (jos_setjmp(&jb) != 0) {
	if (fork_debug)
	    cprintf("fork: new thread running\n");

	start_env->ppid = start_env->shared_container;

	start_env->shared_container = top_ct;
	start_env->proc_container = proc_ct;

	start_env->process_status_seg = proc_status_seg;
	start_env->process_gid_seg = process_gid_seg;

	start_env->process_grant = process_grant;
	start_env->process_taint = process_taint;

	pgrant_cleanup.dismiss();
	ptaint_cleanup.dismiss();
	top_drop.dismiss();

	signal_init();
	child_clear();
	debug_gate_reset();
	debug_gate_init();
	
	return 0;
    }

    // Copy the address space, much like taint_cow() in lib/taint.c
    void *fd_base = (void *) UFDBASE;
    void *fd_end = ((char *) fd_base) + MAXFD * PGSIZE;

    for (uint32_t i = 0; i < uas.nent; i++) {
	if (uas.ents[i].flags == 0)
	    continue;
	if (uas.ents[i].segment.container == (uint64_t) proc_ct)
	    continue;

	// FDs are a special case
	// XXX the fd refcounts are not garbage-collected on failure..
	void *va = uas.ents[i].va;
	if (va >= fd_base && va < fd_end) {
	    error_check(sys_segment_addref(uas.ents[i].segment, top_ct));
	    uas.ents[i].segment.container = top_ct;

	    struct Fd *fd = (struct Fd *) va;
	    if (!fd->fd_immutable)
		jos_atomic_inc(&fd->fd_ref);

	    struct Dev *dev;
	    if (dev_lookup(fd->fd_dev_id, &dev) == 0 && dev->dev_addref)
		(*dev->dev_addref)(fd, top_ct);
	}

	// What gets copied across fork() and what stays shared?
	// Our heuristic is that anything in the process container
	// gets copied (heap, stacks, data); everything else stays
	// shared.  This works well for FDs which are in the shared
	// container.
	if (uas.ents[i].segment.container != start_env->proc_container)
	    continue;

	error_check(sys_obj_get_name(uas.ents[i].segment, &namebuf[0]));

	int64_t id = sys_segment_copy(uas.ents[i].segment, proc_ct,
				      0, &namebuf[0]);
	error_check(id);

	uint64_t old_id = uas.ents[i].segment.object;
	for (uint32_t j = 0; j < uas.nent; j++)
	    if (uas.ents[j].segment.object == old_id)
		uas.ents[j].segment = COBJ(proc_ct, id);
    }

    // Construct the new AS object and a non-running thread
    sys_obj_get_name(cur_as, &namebuf[0]);
    int64_t id = sys_as_create(proc_ct, 0, &namebuf[0]);
    error_check(id);

    if (fork_debug) {
	cprintf("fork: new AS:\n");
	segment_map_print(&uas);
    }

    struct cobj_ref new_as = COBJ(proc_ct, id);
    error_check(sys_as_set(new_as, &uas));

    sys_obj_get_name(COBJ(0, thread_id()), &namebuf[0]);
    id = sys_thread_create(proc_ct, &namebuf[0]);
    error_check(id);
    struct cobj_ref new_th = COBJ(proc_ct, id);

    error_check(sys_container_move_quota(proc_ct, id, thread_quota_slush));
    error_check(sys_obj_set_fixedquota(new_th));

    // Create a new thread that jumps into the new AS
    struct thread_entry te;
    memset(&te, 0, sizeof(te));
    te.te_as = new_as;
    te.te_entry = (void *) &jos_longjmp;
    te.te_stack = 0;
    te.te_arg[0] = (uintptr_t) &jb;
    te.te_arg[1] = 1;

    if (fork_debug)
	cprintf("fork: new thread labels %s / %s\n",
		thread_contaminate.to_string(),
		thread_clearance.to_string());

    error_check(sys_thread_start(new_th, &te,
				 thread_contaminate.to_ulabel(),
				 thread_clearance.to_ulabel()));

    top_drop.dismiss();

    child_add(top_ct, proc_status_seg);
    return top_ct;
}

pid_t
fork(void) __THROW
{
    try {
	return do_fork();
    } catch (std::exception &e) {
	cprintf("fork: %s\n", e.what());
	__set_errno(ENOMEM);
	return -1;
    }
}

pid_t
vfork(void) __THROW
{
    return fork();
}

libc_hidden_def(fork)

