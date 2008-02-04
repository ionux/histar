extern "C" {
#include <inc/syscall.h>
#include <inc/lib.h>
#include <inc/string.h>
#include <inc/error.h>
#include <inc/assert.h>
#include <inc/fs.h>
#include <inc/fd.h>

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <termios.h>
#include <stdlib.h>
#include <fcntl.h>
}

#include <inc/spawn.hh>
#include <inc/error.hh>
#include <inc/labelutil.hh>

#define MAXARGS	256
static char *cmd_argv[MAXARGS];
static int cmd_argc;
static int cmd_with_privs;

static char separators[] = " \t\n\r";

static void builtin_help(int ac, char **av);

static void
print_cobj(uint64_t ct, uint64_t slot)
{
    int64_t id = sys_container_get_slot_id(ct, slot);
    if (id == -E_NOT_FOUND)
	return;
    if (id < 0) {
	printf("cannot get slot %"PRIu64" in %"PRIu64": %s\n", slot, ct, e2s(id));
	return;
    }

    struct cobj_ref cobj = COBJ(ct, id);
    int type = sys_obj_get_type(cobj);
    if (type < 0) {
	printf("sys_obj_get_type <%"PRIu64".%"PRIu64">: %s\n",
		cobj.container, cobj.object, e2s(type));
	return;
    }

    printf("%20"PRIu64" [%4"PRIu64"]  ", id, slot);

    int r;
    switch (type) {
    case kobj_gate:
	printf("gate");
	break;

    case kobj_thread:
	printf("thread");
	break;

    case kobj_container:
	printf("container");
	break;

    case kobj_segment:
	printf("segment");
	r = sys_segment_get_nbytes(cobj);
	if (r < 0)
	    printf(" (cannot get bytes: %s)", e2s(r));
	else
	    printf(" (%d bytes)", r);
	break;

    case kobj_address_space:
	printf("address space");
	break;

    case kobj_netdev:
	printf("netdev");
	break;

    default:
	printf("unknown (%d)", type);
    }

    char name[KOBJ_NAME_LEN];
    r = sys_obj_get_name(cobj, &name[0]);
    if (r < 0) {
	printf(" (cannot get name: %s)", e2s(r));
    } else if (name[0]) {
	printf(": %s", name);
    }

    int64_t q_total = sys_obj_get_quota_total(cobj);
    int64_t q_avail = sys_obj_get_quota_avail(cobj);
    if (q_total >= 0) {
	if (q_total == CT_QUOTA_INF) {
	    printf(" (infinite quota)");
	} else {
	    if (q_avail >= 0)
		printf(" (quota %"PRIu64", avail %"PRIu64")", q_total, q_avail);
	    else
		printf(" (quota %"PRIu64")", q_total);
	}
    }

    printf("\n");
}

static void
builtin_list_container(int ac, char **av)
{
    if (ac != 0 && ac != 1) {
	printf("Usage: lc [container-id]\n");
	return;
    }

    int r;
    uint64_t ct;
    if (ac == 1) {
	r = strtou64(av[0], 0, 10, &ct);
	if (r < 0) {
	    printf("bad number: %s\n", av[0]);
	    return;
	}
    } else {
	ct = start_env->fs_cwd.obj.object;
    }

    int64_t ct_quota = sys_obj_get_quota_total(COBJ(ct, ct));
    int64_t ct_avail = sys_obj_get_quota_avail(COBJ(ct, ct));

    printf("Container %"PRIu64" (%"PRIu64" bytes, %"PRIu64" avail):\n", ct, ct_quota, ct_avail);
    printf("                  id  slot   object\n");

    int64_t nslots = sys_container_get_nslots(ct);
    if (nslots < 0) {
	printf("sys_container_nslots(%"PRIu64"): %s\n", ct, e2s(nslots));
	return;
    }

    for (int64_t i = 0; i < nslots; i++)
	print_cobj(ct, i);
}

static int64_t
do_spawn(int ac, char **av, struct child_process *childp)
{
    const char *pn = av[0];
    struct fs_inode ino;
    int r = fs_namei(pn, &ino);
    if (r < 0 && pn[0] != '/') {
	char buf[512];
	snprintf(buf, sizeof(buf), "/bin/%s", pn);
	r = fs_namei(buf, &ino);
    }

    if (r < 0) {
	printf("cannot find %s: %s\n", pn, e2s(r));
	return r;
    }

    label my_label;
    thread_cur_label(&my_label);

    try {
	struct child_process child;
        child = spawn(start_env->process_pool, ino,
		      0, 1, 2,
		      ac, (const char **) av,
		      0, 0,
		      0, cmd_with_privs ? &my_label : 0, 0, 0, 0);
	*childp = child;
	return child.container;
    } catch (std::exception &e) {
	printf("spawn: %s\n", e.what());
	return -1;
    }
}

static void
builtin_spawn(int ac, char **av)
{
    if (ac < 1) {
	printf("Usage: spawn <program-name>\n");
	return;
    }

    struct child_process child;
    int64_t ct = do_spawn(ac, av, &child);
    if (ct >= 0)
	printf("Spawned in container %"PRIu64"\n", ct);
}

static void
spawn_and_wait(int ac, char **av)
{
    struct child_process child;
    int64_t ct = do_spawn(ac, av, &child);
    if (ct < 0)
	return;

    int64_t code;
    int r = process_wait(&child, &code);
    if (r < 0) {
	printf("spawn_wait: %s\n", e2s(r));
	return;
    }

    if (r == PROCESS_EXITED) {
	if (code)
	    printf("Exited with code %"PRIu64"\n", code);
	sys_obj_unref(COBJ(start_env->process_pool, ct));
    } else if (r == PROCESS_TAINTED) {
	printf("Process tainted itself, detaching.\n");
    } else if (r == PROCESS_DEAD) {
	printf("Process vanished.\n");
    } else {
	printf("Process reports unknown status %d\n", r);
    }
}

static void
builtin_unref(int ac, char **av)
{
    if (ac != 2) {
	printf("Usage: unref <container> <object>\n");
	return;
    }

    uint64_t c, i;

    int r = strtou64(av[0], 0, 10, &c);
    if (r < 0) {
	printf("bad number: %s\n", av[0]);
	return;
    }

    r = strtou64(av[1], 0, 10, &i);
    if (r < 0) {
	printf("bad number: %s\n", av[1]);
	return;
    }

    r = sys_obj_unref(COBJ(c, i));
    if (r < 0) {
	printf("Cannot unref <%"PRIu64".%"PRIu64">: %s\n", c, i, e2s(r));
	return;
    }

    printf("Dropped <%"PRIu64".%"PRIu64">\n", c, i);
}

static void
builtin_cd(int ac, char **av)
{
    if (ac != 1) {
	printf("cd <pathname>\n");
	return;
    }

    const char *pn = av[0];
    int r = chdir(pn);
    if (r < 0) {
	printf("cannot cd to %s: %s\n", pn, e2s(r));
	return;
    }
}

static void
builtin_exit(int ac, char **av)
{
    close(0);
}

static struct {
    const char *name;
    const char *desc;
    void (*func) (int ac, char **av);
} commands[] = {
    { "help",	"Display the list of commands",	&builtin_help },
    { "lc",	"List a container",		&builtin_list_container },
    { "spawn",	"Run in background",		&builtin_spawn },
    { "unref",	"Drop container object",	&builtin_unref },
    { "exit",	"Exit",				&builtin_exit },
    { "cd",	"Change directory",		&builtin_cd },
};

static void
builtin_help(int ac, char **av)
{
    printf("Commands:\n");
    for (uint32_t i = 0; i < sizeof(commands)/sizeof(commands[0]); i++)
	printf("  %-10s %s\n", commands[i].name, commands[i].desc);
}

static void
parse_cmd(char *cmd)
{
    char *arg_base = cmd;
    char *c = cmd;

    if (*c == '@') {
	cmd_with_privs = 1;
	arg_base++;
	c++;
    } else {
	cmd_with_privs = 0;
    }

    cmd_argc = 0;
    for (;;) {
	int eoc = 0;

	if (*c == '\0')
	    eoc = 1;

	if (eoc || strchr(separators, *c)) {
	    *c = '\0';
	    if (arg_base != c)
		cmd_argv[cmd_argc++] = arg_base;
	    arg_base = c + 1;
	}

	if (eoc)
	    break;
	c++;
    }
}

static void
run_cmd(int ac, char **av)
{
    if (ac == 0)
	return;

    for (uint32_t i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
	if (!strcmp(av[0], commands[i].name)) {
	    commands[i].func(ac-1, av+1);
	    return;
	}
    }

    spawn_and_wait(ac, av);
}

int
main(int ac, char **av)
{
    printf("JOS: shell\n");

    struct fs_inode selfdir;
    fs_get_root(start_env->shared_container, &selfdir);

    fs_unmount(start_env->fs_mtab_seg, start_env->fs_root, "self");
    int r = fs_mount(start_env->fs_mtab_seg, start_env->fs_root, "self", selfdir);
    if (r < 0)
	printf("shell: cannot mount /self: %s\n", e2s(r));

    struct termios term, origterm;
    memset(&term, 0, sizeof(term));
    ioctl(0, TCGETS, &term);
    memcpy(&origterm, &term, sizeof(term));

    term.c_oflag |= ONLCR;
    term.c_lflag &= ~ECHO;
    ioctl(0, TCSETS, &term);

    for (;;) {
	char prompt[64];
	snprintf(prompt, sizeof(prompt), "[jos:%"PRIu64"]> ",
		 start_env->fs_cwd.obj.object);
	char *cmd = readline(prompt, 1);
	if (cmd == 0) {
	    printf("EOF\n");
	    break;
	}

	parse_cmd(cmd);
	run_cmd(cmd_argc, cmd_argv);
    }

    ioctl(0, TCSETS, &origterm);
}
