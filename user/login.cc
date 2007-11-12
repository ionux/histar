extern "C" {
#include <inc/lib.h>
#include <inc/authd.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <inttypes.h>
#include <login_cap.h>
}

#include <inc/error.hh>
#include <inc/authclnt.hh>
#include <inc/spawn.hh>

enum { login_debug = 0 };

static void
login(char *u, char *p)
{
    uint64_t ug, ut;
    try {
	auth_login(u, p, &ug, &ut);
    } catch (std::exception &e) {
	printf("login failed: %s\n", e.what());
	return;
    }

    if (login_debug)
	printf("login: taint %"PRIu64" grant %"PRIu64"\n", ut, ug);

    start_env->user_taint = ut;
    start_env->user_grant = ug;

    const char *user_shell = "/bin/ksh";
    const char *user_home = "/";
    struct passwd *pw = getpwnam(u);
    if (pw) {
	user_shell = pw->pw_shell;
	user_home = pw->pw_dir;
    }

    struct fs_inode fsshell;
    error_check(fs_namei(user_shell, &fsshell));

    char env_user[128], env_home[128];
    sprintf(&env_user[0], "USER=%s", u);
    sprintf(&env_home[0], "HOME=%s", user_home);

    chdir(user_home);

    setusercontext(0, pw, pw->pw_uid, LOGIN_SETALL);

    const char *argv[] = { user_shell };
    const char *envv[] = { "TERM=vt100", "TERMINFO=/x/share/terminfo",
			   &env_user[0], &env_home[0] };
    struct child_process shell = spawn(start_env->process_pool,
                                       fsshell,
                                       0, 1, 2,
                                       sizeof(argv)/sizeof(argv[0]), &argv[0],
                                       sizeof(envv)/sizeof(envv[0]), &envv[0],
                                       0, 0, 0, 0, 0);
    int64_t e;
    process_wait(&shell, &e);
    exit(0);
}

static void
prompt(const char *initial_user)
{
    char user[32], pass[32];
    
    for (;;) {
	if (initial_user) {
	    strcpy(user, initial_user);
	    initial_user = 0;
	} else {
	    char *s = readline("login: ", 1);
	    if (!s)
		return;
	    strcpy(user, s);
	}
    
        char *p = readline("password: ", 0);
	printf("\n");
	if (!p)
	    return;
        strcpy(pass, p);
        
        login(&user[0], &pass[0]);
    }
}

int
main(int ac, char **av)
{
    try {
	prompt(ac >= 2 ? av[1] : 0);
    } catch (std::exception &e) {
	printf("%s\n", e.what());
    }

    return 0;
}
