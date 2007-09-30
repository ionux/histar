extern "C" {
#include <inc/lib.h>
#include <inc/syscall.h>
#include <inc/labelutil.h>
#include <inc/error.h>
#include <inc/argv.h>
#include <inttypes.h>
}

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <inc/error.hh>
#include <inc/errno.hh>
#include <inc/spawn.hh>
#include <inc/fs_dir.hh>
#include <inc/scopeguard.hh>

arg_desc cmdarg[] = {
    { "http_auth_enable", "1" },
    { "http_dj_enable", "0" },

    { "dj_app_server_count", "1" },
    { "dj_user_server_count", "1" },
        
    { 0, "" }
};

static char inetd_enable = 1;
static char ssl_enable = 1;
static char ssl_privsep_enable = 1;
static char ssl_eproc_enable = 1;

static const char* httpd_root_path = "/www";

static struct fs_inode
fs_inode_for(const char *pn)
{
    struct fs_inode ino;
    int r = fs_namei(pn, &ino);
    if (r < 0)
	throw error(r, "cannot fs_lookup %s", pn);
    return ino;
}

static void
segment_copy_to_file(struct cobj_ref seg, struct fs_inode dir, 
		     const char *fn, label *l)
{
    struct fs_inode ino;
    error_check(fs_create(dir, fn, &ino, l->to_ulabel()));

    void *va = 0;
    uint64_t bytes = 0;
    error_check(segment_map(seg, 0, SEGMAP_READ, &va, &bytes, 0));
    scope_guard<int, void *> unmap(segment_unmap, va);
    error_check(fs_pwrite(ino, va, bytes, 0));
}

int
main (int ac, const char **av)
{
    static const char *default_server_pem = "/bin/server.pem";
    static const char *default_servkey_pem = "/bin/servkey.pem";
    static const char *default_dh_pem = "/bin/dh.pem";

    static const char *ssld_server_pem = "/httpd/ssld-priv/server.pem";
    static const char *ssld_dh_pem = "/httpd/ssld-priv/dh.pem";

    static const char *ssld_servkey_pem = "/httpd/servkey-priv/servkey.pem";

    error_check(argv_parse(ac, av, cmdarg));

    if (mkdir(httpd_root_path, 0) < 0 && errno != EEXIST)
	throw basic_exception("unable to create %s\n", httpd_root_path);
    
    int64_t ssld_taint = handle_alloc();
    int64_t eprocd_taint = ssl_eproc_enable ? handle_alloc() : ssld_taint;
    int64_t access_grant = handle_alloc();
    error_check(ssld_taint);
    error_check(eprocd_taint);
    error_check(access_grant);
    label ssld_label(1);
    ssld_label.set(ssld_taint, 3);
    ssld_label.set(access_grant, 0);
    label eprocd_label(1);
    eprocd_label.set(eprocd_taint, 3);
    eprocd_label.set(access_grant, 0);
    
    int64_t httpd_ct = sys_container_alloc(start_env->root_container, 0,
					   "httpd", 0, CT_QUOTA_INF);
    error_check(httpd_ct);

    char httpd_symlink[64];
    sprintf(&httpd_symlink[0], "#%"PRIu64, httpd_ct);
    symlink(httpd_symlink, "/httpd");

    struct fs_inode httpd_dir_ino;
    httpd_dir_ino.obj = COBJ(start_env->root_container, httpd_ct);

    struct fs_inode ssld_dir_ino;
    error_check(fs_mkdir(httpd_dir_ino, "ssld-priv", &ssld_dir_ino, 
			 ssld_label.to_ulabel()));

    struct fs_inode eprocd_dir_ino;
    error_check(fs_mkdir(httpd_dir_ino, "servkey-priv", &eprocd_dir_ino, 
			 eprocd_label.to_ulabel()));
        
    struct fs_inode server_pem_ino = fs_inode_for(default_server_pem);
    struct fs_inode dh_pem_ino = fs_inode_for(default_dh_pem);

    struct fs_inode servkey_pem_ino = fs_inode_for(default_servkey_pem);

    segment_copy_to_file(server_pem_ino.obj, ssld_dir_ino, "server.pem", 
			 &ssld_label);
    segment_copy_to_file(dh_pem_ino.obj, ssld_dir_ino, "dh.pem", 
			 &ssld_label);

    segment_copy_to_file(servkey_pem_ino.obj, eprocd_dir_ino, "servkey.pem", 
			 &eprocd_label);

    label ssld_ds(3), ssld_dr(0);
    ssld_ds.set(ssld_taint, LB_LEVEL_STAR);
    ssld_dr.set(ssld_taint, 3);

    char verify_arg[32];
    snprintf(verify_arg, sizeof(verify_arg), "%lu", start_env->user_grant);
    
    const char *ssld_pn = "/bin/ssld";
    struct fs_inode ssld_ino = fs_inode_for(ssld_pn);
    const char *ssld_argv[] = { ssld_pn, verify_arg, ssld_server_pem,
				ssld_dh_pem, ssld_servkey_pem };
    struct child_process cp = spawn(httpd_ct, ssld_ino,
				    0, 0, 0,
				    ssl_eproc_enable ? 4 : 5, &ssld_argv[0],
				    0, 0,
				    0, &ssld_ds, 0, &ssld_dr, 0,
				    SPAWN_NO_AUTOGRANT);
    int64_t exit_code = 0;
    process_wait(&cp, &exit_code);
    if (exit_code)
	throw error(exit_code, "error starting ssld");
    
    if (ssl_eproc_enable) {
	uint64_t keylen;
	error_check(fs_getsize(servkey_pem_ino, &keylen));

	char *keydata = (char *) malloc(keylen + 1);
	error_check(fs_pread(servkey_pem_ino, keydata, keylen, 0));
	keydata[keylen] = '\0';
	scope_guard<void, void*> freekey(free, keydata);

	label eprocd_ds(3), eprocd_dr(0);
	eprocd_ds.set(eprocd_taint, LB_LEVEL_STAR);
	eprocd_dr.set(eprocd_taint, 3);
	
	const char *eprocd_pn = "/bin/ssl_eprocd";
	struct fs_inode eprocd_ino = fs_inode_for(eprocd_pn);
	const char *eprocd_argv[] = { eprocd_pn, verify_arg, keydata };
	cp = spawn(httpd_ct, eprocd_ino,
		   0, 0, 0,
		   3, &eprocd_argv[0],
		   0, 0,
		   0, &eprocd_ds, 0, &eprocd_dr, 0,
		   SPAWN_NO_AUTOGRANT);
	exit_code = 0;
	process_wait(&cp, &exit_code);
	if (exit_code)
	    throw error(exit_code, "error starting ssl_eprocd");
    }
	
    label httpd_ds(3);
    httpd_ds.set(access_grant, LB_LEVEL_STAR);
    label httpd_dr(0);
    httpd_dr.set(access_grant, 3);

    char ssle_buf[4], sslp_buf[4], ssle2_buf[4];
    snprintf(ssle_buf, sizeof(ssle_buf), "%d", ssl_enable);
    snprintf(sslp_buf, sizeof(sslp_buf), "%d", ssl_privsep_enable);
    snprintf(ssle2_buf, sizeof(ssle2_buf), "%d", ssl_eproc_enable);

    if (inetd_enable) {
	const char *inetd_pn = "/bin/inetd";
	struct fs_inode inetd_ino = fs_inode_for(inetd_pn);
	const char *inetd_av[] = { inetd_pn, 
				   "--ssl_privsep_enable", sslp_buf,
				   "--ssl_eproc_enable", ssle2_buf,
				   "--http_auth_enable", 
				   arg_val(cmdarg, "http_auth_enable"),
				   "--http_dj_enable", 
				   arg_val(cmdarg, "http_dj_enable"),
				   "--httpd_root_path", "/www",
				   "--dj_app_server_count", 
				   arg_val(cmdarg, "dj_app_server_count"),
				   "--dj_user_server_count", 
				   arg_val(cmdarg, "dj_user_server_count") };

	int inetd_ac = sizeof(inetd_av) / sizeof(inetd_av[0]);
	spawn(httpd_ct, inetd_ino,
	      0, 0, 0,
	      inetd_ac, &inetd_av[0],
	      0, 0,
	      0, &httpd_ds, 0, &httpd_dr, 0);
    } else {
	const char *httpd_pn = "/bin/httpd";
	struct fs_inode httpd_ino = fs_inode_for(httpd_pn);
	const char *httpd_av[] = { httpd_pn, 
				   "--ssl_enable", ssle_buf,
				   "--ssl_privsep_enable", sslp_buf,
				   "--ssl_eproc_enable", ssle2_buf,
				   "--http_auth_enable", 
				   arg_val(cmdarg, "http_auth_enable"),
				   "--httpd_root_path", "/www" };
	int httpd_ac = sizeof(httpd_av) / sizeof(httpd_av[0]);
	spawn(httpd_ct, httpd_ino,
	      0, 0, 0,
	      httpd_ac, &httpd_av[0],
	      0, 0,
	      0, &httpd_ds, 0, &httpd_dr, 0);
    }
    return 0;
}
