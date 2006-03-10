extern "C" {
#include <inc/stdio.h>
#include <inc/lib.h>
#include <inc/assert.h>
#include <inc/string.h>
#include <inc/netd.h>
#include <inc/fs.h>
#include <inc/syscall.h>
#include <inc/error.h>
#include <inc/fd.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
}

#include <inc/nethelper.hh>
#include <inc/error.hh>
#include <inc/scopeguard.hh>

static int reqs;

static void
http_client(void *arg)
{
    int s = (int64_t) arg;

    try {
	tcpconn tc(s);
	lineparser lp(&tc);

	const char *req = lp.read_line();
	if (req == 0 || strncmp(req, "GET ", 4))
	    throw basic_exception("bad http request: %s", req);

	const char *pn_start = req + 4;
	char *space = strchr(pn_start, ' ');
	if (space == 0)
	    throw basic_exception("no space in http req: %s", req);

	char pnbuf[512];
	strncpy(&pnbuf[0], pn_start, space - pn_start);
	pnbuf[sizeof(pnbuf) - 1] = '\0';

	while (req[0] != '\0') {
	    req = lp.read_line();
	    if (req == 0)
		throw basic_exception("client EOF");
	}

	char buf[1024];
	snprintf(buf, sizeof(buf),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/html\r\n"
		"\r\n"
		"<h1>jos64 web server: request %d</h1><p><hr><pre>\n",
		reqs++);
	tc.write(buf, strlen(buf));

	try {
	    snprintf(buf, sizeof(buf), "Trying to lookup %s\n", pnbuf);
	    tc.write(buf, strlen(buf));

	    struct fs_inode ino;
	    int r = fs_namei(pnbuf, &ino);
	    if (r < 0)
		throw error(r, "fs_namei");

	    int type = sys_obj_get_type(ino.obj);
	    if (type < 0)
		throw error(r, "sys_obj_get_type");

	    if (type == kobj_segment) {
		snprintf(buf, sizeof(buf), "segment\n");
		tc.write(buf, strlen(buf));

		uint64_t sz;
		r = fs_getsize(ino, &sz);
		if (r < 0)
		    throw error(r, "fs_getsize");

		for (uint64_t off = 0; off < sz; off += sizeof(buf)) {
		    size_t cc = MIN(sizeof(buf), sz - off);
		    r = fs_pread(ino, &buf[0], cc, off);
		    if (r < 0)
			throw error(r, "fs_pread");
		    tc.write(buf, cc);
		}
	    } else if (type == kobj_container || type == kobj_mlt) {
		snprintf(buf, sizeof(buf), "directory or mlt\n");
		tc.write(buf, strlen(buf));

		struct fs_readdir_state rs;
		error_check(fs_readdir_init(&rs, ino));
		scope_guard<void, fs_readdir_state *> g(fs_readdir_close, &rs);

		for (;;) {
		    struct fs_dent de;
		    r = fs_readdir_dent(&rs, &de, 0);
		    if (r < 0)
			throw error(r, "fs_readdir_dent");
		    if (r == 0)
			break;

		    snprintf(buf, sizeof(buf),
			     "<a href=\"%s%s%s\">%s/%s</a>\n",
			     pnbuf,
			     pnbuf[strlen(pnbuf) - 1] == '/' ? "" : "/",
			     &de.de_name[0],
			     pnbuf, &de.de_name[0]);
		    tc.write(buf, strlen(buf));
		}
	    } else {
		snprintf(buf, sizeof(buf), "type %d\n", type);
		tc.write(buf, strlen(buf));
	    }
	} catch (std::exception &e) {
	    const char *what = e.what();
	    tc.write(what, strlen(what));
	}
    } catch (std::exception &e) {
	printf("http_client: %s\n", e.what());
    }
}

static void __attribute__((noreturn))
http_server(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        panic("cannot create socket: %d\n", s);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(80);
    int r = bind(s, (struct sockaddr *)&sin, sizeof(sin));
    if (r < 0)
        panic("cannot bind socket: %d\n", r);

    r = listen(s, 5);
    if (r < 0)
        panic("cannot listen on socket: %d\n", r);

    printf("httpd: server on port 80\n");
    for (;;) {
        socklen_t socklen = sizeof(sin);
        int ss = accept(s, (struct sockaddr *)&sin, &socklen);
        if (ss < 0) {
            printf("cannot accept client: %d\n", ss);
            continue;
        }

	struct cobj_ref t;
	r = thread_create(start_env->proc_container, &http_client,
			  (void*) (int64_t) ss, &t, "http client");
	if (r < 0) {
	    printf("cannot spawn client thread: %s\n", e2s(r));
	    close(ss);
	} else {
	    fd_give_up_privilege(ss);
	}
    }
}

int
main(int ac, char **av)
{
    http_server();
}
