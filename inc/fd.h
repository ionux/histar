#ifndef JOS_INC_FD_H
#define JOS_INC_FD_H

#include <stdint.h>
#include <inc/atomic.h>
#include <inc/container.h>
#include <inc/types.h>
#include <inc/fs.h>
#include <inc/jthread.h>
#include <inc/label.h>
#include <inc/multisync.h>
#include <inc/pty.h>
#include <inc/uds.h>
#include <inc/lfs.h>

#include <dirent.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <termios/kernel_termios.h>

struct stat64;

/* Maximum number of file descriptors a program may hold open concurrently */
#define MAXFD		64

/* pre-declare for forward references */
struct Fd;
struct Dev;

struct Dev
{
    uint8_t dev_id;
    const char *dev_name;

    ssize_t (*dev_read)(struct Fd *fd, void *buf, size_t len, off_t offset);
    ssize_t (*dev_write)(struct Fd *fd, const void *buf, size_t len, off_t offset);
    ssize_t (*dev_recvfrom)(struct Fd *fd, void *buf, size_t len, int flags, 
			struct sockaddr *from, socklen_t *fromlen);
    ssize_t (*dev_sendto)(struct Fd *fd, const void *buf, size_t len, int flags, 
			  const struct sockaddr *to, socklen_t tolen);
    ssize_t (*dev_sendmsg)(struct Fd *fd, const struct msghdr *msg, int flags);
    int (*dev_open)(struct fs_inode ino, int flags, uint32_t dev_opt);
    int (*dev_close)(struct Fd *fd);
    int (*dev_seek)(struct Fd *fd, off_t pos);
    int (*dev_trunc)(struct Fd *fd, off_t length);
    int (*dev_stat)(struct Fd *fd, struct stat64 *buf);
    int (*dev_probe)(struct Fd *fd, dev_probe_t probe);
    int (*dev_sync)(struct Fd *fd);
    int (*dev_statsync)(struct Fd *fd, dev_probe_t probe, struct wait_stat *wstat, int wslots_avail);

    ssize_t (*dev_getdents)(struct Fd *fd, struct dirent *dirbuf, size_t nbytes);
    int64_t (*dev_getdents64)(struct Fd *fd, struct dirent64 *dirbuf, uint64_t nbytes);

    int (*dev_bind)(struct Fd *fd, const struct sockaddr *addr, socklen_t addrlen);
    int (*dev_listen)(struct Fd *fd, int backlog);
    int (*dev_accept)(struct Fd *fd, struct sockaddr *addr, socklen_t *addrlen);
    int (*dev_connect)(struct Fd *fd, const struct sockaddr *addr, socklen_t addrlen);
    int (*dev_getsockname)(struct Fd *fd, struct sockaddr *name, socklen_t *namelen);
    int (*dev_getpeername)(struct Fd *fd, struct sockaddr *name, socklen_t *namelen);
    int (*dev_setsockopt)(struct Fd *fd, int level, int optname,
			  const void *optval, socklen_t optlen);
    int (*dev_getsockopt)(struct Fd *fd, int level, int optname,
			  void *optval, socklen_t *optlen);
    int (*dev_shutdown)(struct Fd *fd, int how);
    int (*dev_ioctl)(struct Fd *fd, uint64_t cmd, va_list ap);

    int (*dev_addref)(struct Fd *fd, uint64_t ct);
    int (*dev_unref)(struct Fd *fd);
};

enum {
    fd_handle_grant = 0,
    fd_handle_taint,
    fd_handle_extra_grant,
    fd_handle_extra_taint,
    fd_handle_max
};

struct Fd
{
    uint8_t fd_dev_id;
    uint8_t fd_immutable;
    uint8_t fd_isatty;
    uint8_t fd_private;
    uint32_t fd_omode;
    off_t fd_offset;

    /* handles for this fd */
    uint64_t fd_handle[fd_handle_max];

    union {
	jos_atomic_t fd_ref;
	uint64_t fd_ref64;
    };

    union {
	struct {
	} fd_dev_state;

	struct {
	    uint64_t pgid;
	    struct cobj_ref fbcons_seg;
	    struct __kernel_termios ios;
	    struct winsize ws;

	    char pending[16];
	    uint32_t pending_count;

	    char outbuf[16];
	    uint32_t outbuf_count;
	} fd_cons;

	struct {
	    jthread_mutex_t mu;
	    struct cobj_ref netd_gate;
	    int s;
	    int type;
	    char extra[128];
	} fd_sock;

	struct {
	    struct fs_inode ino;
	    struct fs_readdir_pos readdir_pos;
	} fd_file;

	struct {
	    uint64_t bytes;	/* # bytes in circular buffer */
	    uint32_t read_ptr;	/* read at this offset */
	    char reader_waiting;
	    char writer_waiting;
	    jthread_mutex_t mu;
	    char buf[4000];
	} fd_pipe;

	struct {
	    struct jcomm bipipe_jc;
	    uint64_t bipipe_ct;
	} fd_bipipe;
	
	struct {
	    struct jcomm pty_jc;
	    uint64_t pty_seg;

	    /* for KDGETMODE/KDSETMODE */
	    int64_t pty_cons_mode_req;
	    int64_t pty_cons_mode_cur;
	} fd_pty;

	struct {
	    struct cobj_ref tun_seg;
	    int tun_a;
	} fd_tun;

	struct {
	    int uds_type;
	    struct cobj_ref uds_gate;
	    
	    union {
		struct {
		    /* connected state */
		    char connect;
		    struct jcomm jc;
		    
		    /* listener state */
		    uint16_t backlog;
                    uint64_t backlogged;
		    char listen;
		    
		    struct uds_slot slots[16];
		    jthread_mutex_t mu;
		} s; /* stream */
		struct {
		    char bound;
		    struct cobj_ref jl;
		    char dst[128];
		} d; /* dgram */
	    };
	} fd_uds;

	struct {
	    struct lfs_descriptor ld;
	} fd_lfs;

	struct {
	    char buf[4000];
	} fd_cust;
    };
};

char*	fd2data(struct Fd *fd);
int	fd2num(struct Fd *fd);
int	fd_alloc(struct Fd **fd_store, const char *name);
int	jos_fd_close(struct Fd *fd);
int	fd_lookup(int fdnum, struct Fd **fd_store, struct cobj_ref *objp, uint64_t *flagsp);
int	fd_setflags(struct Fd *fd, struct cobj_ref obj, uint64_t newflags);
void	fd_give_up_privilege(int fdnum);
int	fd_set_isatty(int fdnum, int isit);
void	dev_register(struct Dev *d);
int	dev_lookup(uint8_t devid, struct Dev **dev_store);
void	fd_set_extra_handles(struct Fd *fd, uint64_t eg, uint64_t et);

/* Allocates individual handles for this FD */
int	fd_make_public(int fdnum, struct ulabel *taint);

extern struct Dev devcons;	/* type 'c' */
extern struct Dev devsock;	/* type 's' */
extern struct Dev devfile;	/* type 'f' */
extern struct Dev devpipe;	/* type 'p' */
extern struct Dev devtun;	/* type 't' */
extern struct Dev devbipipe;	/* type 'b' */
extern struct Dev devrand;	/* type 'r' */
extern struct Dev devzero;	/* type 'z' */
extern struct Dev devnull;	/* type 'n' */
extern struct Dev devtty;       /* type 'w' */
extern struct Dev devptm;	/* type 'x' */
extern struct Dev devpts;	/* type 'y' */
extern struct Dev devuds;	/* type 'u' */
extern struct Dev devsymlink;	/* type 'l' */
extern struct Dev devlfs;       /* type 'o' */
extern struct Dev devfb;	/* type 'F' */
extern struct Dev devfbcons;	/* type 'C' */

int	dup2_as(int oldfd, int newfd,
		struct cobj_ref target_as, uint64_t target_ct);
void	close_all(void);
ssize_t	readn(int fd, void *buf, size_t nbytes);

#endif
