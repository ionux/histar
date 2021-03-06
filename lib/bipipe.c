#include <inc/fd.h>
#include <inc/syscall.h>
#include <inc/atomic.h>
#include <inc/lib.h>
#include <inc/bipipe.h>
#include <inc/labelutil.h>
#include <inc/gateparam.h>
#include <inc/stdio.h>
#include <inc/syscall.h>
#include <inc/error.h>
#include <inc/assert.h>
#include <inc/ioctl.h>

#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/socket.h>

static int
errno_val(uint64_t e)
{
    errno = e;
    return -1;
}

#define BIPIPE_DEF_CT start_env->shared_container
#define BIPIPE_CT(fd) (fd->fd_bipipe.bipipe_ct ? fd->fd_bipipe.bipipe_ct \
                      : BIPIPE_DEF_CT)
#define BIPIPE_JCOMM(fd) JCOMM(BIPIPE_CT(fd), fd->fd_bipipe.bipipe_jc)

static int
bipipe_addref(struct Fd *fd, uint64_t ct)
{
    int r = jcomm_addref(BIPIPE_JCOMM(fd), ct);
    if (r < 0)
	cprintf("bipipe_addref: <%"PRIu64".%"PRIu64"> error: %s\n",
		fd->fd_bipipe.bipipe_ct, fd->fd_bipipe.bipipe_jc.segment,
		e2s(r));
    return r;
}

static int
bipipe_unref(struct Fd *fd)
{
    int r = jcomm_unref(BIPIPE_JCOMM(fd));
    if (r < 0)
	cprintf("bipipe_unref: <%"PRIu64".%"PRIu64"> error: %s\n",
		fd->fd_bipipe.bipipe_ct, fd->fd_bipipe.bipipe_jc.segment,
		e2s(r));
    return r;
}

int
bipipe_fd(struct jcomm_ref jr, int fd_mode, uint64_t grant, uint64_t taint)
{
    struct Fd *fd;
    int r = fd_alloc(&fd, "bipipe");
    if (r < 0) {
        errno = ENOMEM;
        return -1;
    }
    fd->fd_dev_id = devbipipe.dev_id;
    fd->fd_omode = O_RDWR | fd_mode;
    fd->fd_bipipe.bipipe_jc = jr.jc;
    if (jr.container != BIPIPE_DEF_CT)
	fd->fd_bipipe.bipipe_ct = jr.container;
    else
	fd->fd_bipipe.bipipe_ct = 0;

    fd_set_extra_handles(fd, grant, taint);
    return fd2num(fd);
}

int
bipipe(int type, int fv[2])
{
    return bipipe_label(type, fv, 0);
}

int
bipipe_label(int type, int fv[2], struct ulabel *ul)
{
    int r;
    
    uint64_t taint = handle_alloc();
    uint64_t grant = handle_alloc();
    
    uint64_t label_ent[64];
    struct ulabel label = { .ul_size = 64, .ul_ent = &label_ent[0] };
    if (ul) {
	label.ul_default = ul->ul_default;
	label.ul_nent = ul->ul_nent;

	assert(ul->ul_nent <= sizeof(label_ent) / sizeof(label_ent[0]));
	memcpy(&label_ent[0], ul->ul_ent, ul->ul_nent * sizeof(ul->ul_ent[0]));
    } else {
	r = sys_obj_get_label(COBJ(0, thread_id()), &label);
	if (r < 0)
	    return r;

	for (uint32_t i = 0; i < label.ul_nent; i++)
	    if (LB_LEVEL(label.ul_ent[i]) == LB_LEVEL_STAR)
		label.ul_ent[i] = LB_CODE(LB_HANDLE(label.ul_ent[i]),
					  label.ul_default);
    }

    assert(0 == label_set_level(&label, taint, 3, 1));
    assert(0 == label_set_level(&label, grant, 0, 1));

    struct jcomm_ref jr0, jr1;

    int16_t mode = 0;
    if (type == SOCK_DGRAM)
	mode = JCOMM_PACKET;

    if ((r = jcomm_alloc(BIPIPE_DEF_CT, &label, mode, &jr0, &jr1)) < 0) {
	errno = ENOMEM;
        return -1;
    }

    r = bipipe_fd(jr0, O_RDWR, grant, taint);
    if (r < 0) {
	jcomm_unref(jr0);
	return r;
    }
    fv[0] = r;

    r = bipipe_fd(jr1, O_RDWR, grant, taint);
    if (r < 0) {
	// unrefs seg
	close(fv[0]);
	return r;
    }
    fv[1] = r;

    /* bipipe_fd() does not increment the refcount, and jcomm_alloc()
     * gives us 1, but we need 2 because of the two FDs..
     */
    r = jcomm_addref(jr0, BIPIPE_DEF_CT);
    assert(r == 0);

    return 0;
}

static ssize_t
bipipe_read(struct Fd *fd, void *buf, size_t count, off_t offset)
{
    int r = jcomm_read(BIPIPE_JCOMM(fd), buf, count,
		       !(fd->fd_omode & O_NONBLOCK));

    if (r == -E_AGAIN)
	return errno_val(EAGAIN);
    else if (r == -E_EOF)
	    return 0;
    else if (r < 0)
	return errno_val(EIO);

    return r;
}

static ssize_t
bipipe_write(struct Fd *fd, const void *buf, size_t count, off_t offset)
{
    int r = jcomm_write(BIPIPE_JCOMM(fd), buf, count,
			!(fd->fd_omode & O_NONBLOCK));

    if (r == -E_AGAIN)
	return errno_val(EAGAIN);
    else if (r == -E_EOF)
	/* XXX deliver SIGPIPE */
	return errno_val(EPIPE);
    else if (r < 0)
	return errno_val(EIO);
    
    return r;
}

static int
bipipe_probe(struct Fd *fd, dev_probe_t probe)
{
    return jcomm_probe(BIPIPE_JCOMM(fd), probe);
}

static int
bipipe_close(struct Fd *fd)
{
    int r = jcomm_shut(BIPIPE_JCOMM(fd), JCOMM_SHUT_RD | JCOMM_SHUT_WR);
    if (r < 0)
	cprintf("bipipe_close: jcomm_shut error: %s\n", e2s(r));

    return bipipe_unref(fd);
}

static int
bipipe_shutdown(struct Fd *fd, int how)
{
    int16_t h = 0;
    if (how & SHUT_RD)
	h |= JCOMM_SHUT_RD;
    if (how & SHUT_WR)
	h |= JCOMM_SHUT_WR;
    if (how & SHUT_RDWR)
	h |= JCOMM_SHUT_RD | JCOMM_SHUT_WR;
    int r = jcomm_shut(BIPIPE_JCOMM(fd), h);
    if (r < 0)
	cprintf("bipipe_shutdown: jcomm_shut error: %s\n", e2s(r));
    return r;    
}

static int
bipipe_statsync(struct Fd *fd, dev_probe_t probe,
		struct wait_stat *wstat, int wslot_avail)
{
    return jcomm_multisync(BIPIPE_JCOMM(fd), probe, wstat, wslot_avail);
}

static int
bipipe_getsockopt(struct Fd *fd, int level, int optname,
		  void *optval, socklen_t *optlen)
{
    errno = ENOTSOCK;
    return -1;
}

struct Dev devbipipe = {
    .dev_id = 'b',
    .dev_name = "bipipe",
    .dev_read = &bipipe_read,
    .dev_write = &bipipe_write,
    .dev_probe = &bipipe_probe,
    .dev_close = &bipipe_close,
    .dev_shutdown = &bipipe_shutdown,
    .dev_statsync = &bipipe_statsync,
    .dev_addref = &bipipe_addref,
    .dev_unref = &bipipe_unref,
    .dev_getsockopt = &bipipe_getsockopt,
    .dev_ioctl = &jos_ioctl,
};
