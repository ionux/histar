extern "C" {
#include <inc/syscall.h>
#include <inc/fd.h>
#include <inc/bipipe.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
}

#include <inc/cpplabel.hh>
#include <inc/error.hh>
#include <inc/labelutil.hh>
#include <inc/spawn.hh>

static void __attribute__((noreturn))
alarm_handler(int signo)
{
    printf("wrap: timeout\n");
    exit(-1);
}

int
main(int ac, const char **av)
try
{
    if (!start_env->user_taint)
	throw basic_exception("Missing user taint handle -- not logged in?");

    int64_t clam_taint;
    error_check(clam_taint = handle_alloc());

    label bipipe_l(1);
    bipipe_l.set(clam_taint, 3);

    int fds[2];
    if (bipipe_label(SOCK_STREAM, &fds[0], bipipe_l.to_ulabel()) < 0)
	throw basic_exception("cannot create pipe: %s", strerror(errno));

    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd < 0)
	throw basic_exception("cannot open /dev/null: %s", strerror(errno));

    label clear;
    thread_cur_clearance(&clear);
    clear.set(start_env->user_taint, 3);
    clear.set(clam_taint, 3);
    thread_set_clearance(&clear);

    label taint_zero(0);
    taint_zero.set(start_env->user_taint, 3);
    taint_zero.set(clam_taint, 3);

    label taint_star(LB_LEVEL_STAR);
    taint_star.set(start_env->user_taint, 3);
    taint_star.set(clam_taint, 3);

    error_check(fd_make_public(fds[0], taint_zero.to_ulabel()));
    error_check(fd_make_public(fds[1], taint_zero.to_ulabel()));
    error_check(fd_make_public(nullfd, taint_zero.to_ulabel()));

    // Make a private /tmp for clamscan to use
    label tmp_label(1);
    tmp_label.set(start_env->user_taint, 3);
    tmp_label.set(clam_taint, 3);

    fs_inode self_dir;
    fs_get_root(start_env->shared_container, &self_dir);

    fs_inode tmp_dir;
    error_check(fs_mkdir(self_dir, "tmp", &tmp_dir, tmp_label.to_ulabel()));

    // Copy the mount table and mount our /tmp there
    int64_t new_mtab_id;
    error_check(new_mtab_id =
	sys_segment_copy(start_env->fs_mtab_seg, start_env->shared_container,
			 0, "wrap mtab"));
    start_env->fs_mtab_seg = COBJ(start_env->shared_container, new_mtab_id);
    fs_mount(start_env->fs_mtab_seg, start_env->fs_root, "tmp", tmp_dir);

    // Run clamscan
    fs_inode wrapped;
    error_check(fs_namei(av[1], &wrapped));

    int envc = 0;
    while (environ[envc])
	envc++;

    child_process cp =
	spawn(start_env->shared_container, wrapped,
	      nullfd, fds[1], fds[1],
	      ac - 1, &av[1],
	      envc, (const char **) environ,
	      &taint_star, 0, 0, &taint_zero, 0);
    close(fds[1]);
    close(nullfd);

    signal(SIGALRM, &alarm_handler);
    alarm(15);

    char buf[512];
    for (;;) {
	ssize_t cc = read(fds[0], &buf[0], sizeof(buf));
	if (cc < 0)
	    throw basic_exception("cannot read: %s", strerror(errno));

	if (cc == 0)
	    break;

	write(1, &buf[0], cc);
    }
} catch (std::exception &e) {
    printf("wrap: %s\n", e.what());
}
