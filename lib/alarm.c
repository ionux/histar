#include <inc/syscall.h>
#include <inc/atomic.h>
#include <inc/signal.h>
#include <inc/lib.h>
#include <inc/stdio.h>

#include <bits/unimpl.h>

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include <sys/time.h>

libc_hidden_proto(alarm)
libc_hidden_proto(setitimer)
libc_hidden_proto(timer_create)
libc_hidden_proto(timer_delete)
libc_hidden_proto(timer_gettime)
libc_hidden_proto(timer_settime)

static struct cobj_ref alarm_worker_obj;
static uint64_t alarm_worker_ct;

static struct cobj_ref alarm_target_obj;

static jos_atomic64_t alarm_at_nsec;

static void __attribute__((noreturn))
alarm_worker(void *arg)
{
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.si_signo = SIGALRM;

    for (;;) {
	uint64_t now = sys_clock_nsec();
	uint64_t alarm_at = jos_atomic_read(&alarm_at_nsec);

	if (now >= alarm_at) {
	    uint64_t old = jos_atomic_compare_exchange64(&alarm_at_nsec,
							 alarm_at, UINT64(~0));
	    if (old == alarm_at)
		kill_thread_siginfo(alarm_target_obj, &si);
	} else {
	    sys_sync_wait(&jos_atomic_read(&alarm_at_nsec), alarm_at, alarm_at);
	}
    }
}

unsigned int
alarm(unsigned int seconds)
{
    if (alarm_worker_ct != start_env->proc_container) {
	alarm_target_obj = COBJ(start_env->proc_container, thread_id());
	jos_atomic_set64(&alarm_at_nsec, UINT64(~0));

	// Either we forked, or never had an alarm thread to start with.
	int r = thread_create(start_env->proc_container, &alarm_worker, 0,
			      &alarm_worker_obj, "alarm");
	if (r < 0) {
	    cprintf("alarm: cannot create worker thread: %s\n", e2s(r));
	    __set_errno(ENOMEM);
	    return -1;
	}

	alarm_worker_ct = start_env->proc_container;
    }

    uint64_t now = sys_clock_nsec();
    uint64_t nsec = seconds ? now + NSEC_PER_SECOND * seconds : UINT64(~0);
    jos_atomic_set64(&alarm_at_nsec, nsec);
    sys_sync_wakeup(&jos_atomic_read(&alarm_at_nsec));
    return 0;
}

int
setitimer(__itimer_which_t which, const struct itimerval *value,
	  struct itimerval *ovalue)
{
    set_enosys();
    return -1;
}

int
timer_create(clockid_t clock_id, struct sigevent *evp, timer_t *timerid)
{
    set_enosys();
    return -1;
}

int
timer_delete(timer_t timerid)
{
    set_enosys();
    return -1;
}

int
timer_settime(timer_t timerid, int flags,
	      const struct itimerspec *val, struct itimerspec *oval)
{
    set_enosys();
    return -1;
}

int
timer_gettime(timer_t timerid, struct itimerspec *val)
{
    set_enosys();
    return -1;
}

libc_hidden_def(alarm)
libc_hidden_def(setitimer)
libc_hidden_def(timer_create)
libc_hidden_def(timer_delete)
libc_hidden_def(timer_gettime)
libc_hidden_def(timer_settime)

