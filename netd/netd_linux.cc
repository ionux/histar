extern "C" {
#include <inc/lib.h>
#include <inc/netd.h>
#include <inc/syscall.h>
#include <inc/stdio.h>
#include <inc/assert.h>
#include <inc/fd.h>
#include <inc/error.h>
#include <netd/netdlinux.h>

#include <errno.h>
#include <inttypes.h>
}

#include <inc/cpplabel.hh>
#include <inc/labelutil.hh>
#include <inc/gatesrv.hh>
#include <inc/gobblegateclnt.hh>
#include <inc/jthread.hh>
#include <netd/netdsrv.hh>

static int
gate_lookup(const char *bn, const char *gn, struct cobj_ref *ret)
{
    struct fs_inode netd_ct_ino;
    int r = fs_namei(bn, &netd_ct_ino);
    if (r == 0) {
	uint64_t netd_ct = netd_ct_ino.obj.object;
	
	int64_t gate_id = container_find(netd_ct, kobj_gate, gn);
	if (gate_id > 0) {
	    *ret = COBJ(netd_ct, gate_id);
	    return 0;
	}
    }
    return -E_NOT_FOUND;
}

static void
netd_linux_gate_entry(uint64_t a, struct gate_call_data *gcd, gatesrv_return *rg)
{
    netd_socket_handler h = (netd_socket_handler) (uintptr_t) a;
    socket_conn *sr = (socket_conn *)gcd->param_buf;
    h(sr);
}

int
netd_linux_server_init(netd_socket_handler h, uint64_t inet_taint)
{
    try {
	label owner, clear, guard;

	thread_cur_ownership(&owner);
	thread_cur_clearance(&clear);

#if 0	/* XXX have netd taint responses on gate return? */
	if (inet_taint)
	    l.set(inet_taint, 2);
#endif
	
	gatesrv_descriptor gd;
	gd.gate_container_ = start_env->shared_container;
	gd.owner_ = &owner;
	gd.clear_ = &clear;
	gd.guard_ = &guard;

	gd.arg_ = (uintptr_t) h;
	gd.name_ = "netd-linux";
	gd.func_ = &netd_linux_gate_entry;
	gd.flags_ = GATESRV_KEEP_TLS_STACK;
	cobj_ref gate = gate_create(&gd);

#if 0	/* XXX pre-taint all of netd if inet_taint? */
	thread_set_label(&l);
#endif
	signal_grow_stack = 0;
    } catch (std::exception &e) {
	cprintf("netd_linux_server_init: %s\n", e.what());
	return -1;
    }
    return 0;
}

static int
setup_socket_conn(cobj_ref gate, struct socket_conn *client_conn,
		  int sock_id, int dgram)
{
    int r;
    /* allocate some args */
    uint64_t taint = category_alloc(1);
    uint64_t grant = category_alloc(0);

    label l;
    thread_cur_label(&l);
    l.add(taint);
    l.add(grant);
    
    int64_t ct = sys_container_alloc(start_env->shared_container, l.to_ulabel(),
				     "socket-store", 0, CT_QUOTA_INF);
    if (ct < 0)
	return ct;
    jcomm_ref ctrl_comm0, ctrl_comm1;
    r = jcomm_alloc(ct, l.to_ulabel(), JCOMM_PACKET, 
		    &ctrl_comm0, &ctrl_comm1);
    if (r < 0)
	return r;

    jcomm_ref data_comm0, data_comm1;
    r = jcomm_alloc(ct, l.to_ulabel(), dgram ? JCOMM_PACKET : 0,
		    &data_comm0, &data_comm1);
    if (r < 0) {
	jcomm_unref(ctrl_comm0);
	return r;
    }
    
    gate_call_data gcd;
    socket_conn *sc = (socket_conn *)gcd.param_buf;
    
    sc->container = ct;
    sc->taint = taint;
    sc->grant = grant;
    sc->ctrl_comm = ctrl_comm1;
    sc->data_comm = data_comm1;
    sc->init_magic = NETD_LINUX_MAGIC;
    sc->sock_id = sock_id;
    sc->dgram = dgram;
    
    label owner;
    owner.add(taint);
    owner.add(grant);

    try {
	/* clean up thread artifacts in destructor */
	gobblegate_call gc(gate, &owner, 0, 1);
	gc.call(&gcd, 0, 0);

	client_conn->container = ct;
	client_conn->taint = taint;
	client_conn->grant = grant;
	client_conn->ctrl_comm = ctrl_comm0;
	client_conn->data_comm = data_comm0;
	client_conn->dgram = dgram;
	
	/* need to wait for thread signal, so can safely cleanup */
	int64_t z = jcomm_read(ctrl_comm0, &r, sizeof(r), 1);
	if (z < 0) { 
	    cprintf("setup_socket_conn: jcomm_read error: %"PRIu64"\n", z);
	    return z;
	}
	else if (r < 0) {
	    cprintf("setup_socket_conn: gobble thread error: %s\n", e2s(r));
	    return r;
	}
    } catch (std::exception &e) {
	cprintf("setup_socket_conn: gobblegate call error: %s\n", e.what());
	return -1;
    }
    client_conn->init_magic = NETD_LINUX_MAGIC;
    return 0;
}

int
netd_linux_client_init(struct cobj_ref *gate)
{
    return gate_lookup("/netd", "netd-linux", gate);
}

int 
netd_linux_call(struct Fd *fd, struct netd_op_args *a)
{
    int r;
    int64_t z;
    struct socket_conn *client_conn = (struct socket_conn *) fd->fd_sock.extra;
    scoped_jthread_lock l(&fd->fd_sock.mu);

    if (client_conn->init_magic != NETD_LINUX_MAGIC) {
	/* if we aren't creating a new socket we probably are socket 
	 * created by accept that hasn't been used yet
	 */
	if (a->op_type == netd_op_socket)
	    r = setup_socket_conn(fd->fd_sock.netd_gate, client_conn,
				  -1,
				  (a->socket.type == SOCK_DGRAM) ? 1 : 0);
	else
	    r = setup_socket_conn(fd->fd_sock.netd_gate, client_conn,
				  fd->fd_sock.s,
				  (fd->fd_sock.type == SOCK_DGRAM) ? 1 : 0);

	if (r < 0)
	    return r;
	fd_set_extra_handles(fd, client_conn->grant, client_conn->taint);
    }

    switch(a->op_type) {
    case netd_op_close:
    {
	l.release();

	/* Push through all written data, before deallocating jcomm buffers */
	jcomm_write_flush(client_conn->data_comm);

	/* Linux doesn't send a response on close.  We send the close 
	 * operation over the jcomm to pop Linux out of multisync.
	 */
	z = jcomm_write(client_conn->ctrl_comm, a, a->size, 1);
	assert(z == a->size);
	r = jcomm_shut(client_conn->ctrl_comm, JCOMM_SHUT_RD | JCOMM_SHUT_WR);
	if (r < 0)
	    cprintf("netd_linux_call: jcomm_shut error: %s\n", e2s(r));
	jcomm_shut(client_conn->data_comm, JCOMM_SHUT_RD | JCOMM_SHUT_WR);
	r = sys_obj_unref(COBJ(start_env->shared_container,
			       client_conn->container));
	if (r < 0)
	    cprintf("netd_linux_call: sys_obj_unref error: %s\n", e2s(r));
	return 0;
    }

    case netd_op_probe:
	/* XXX how to handle selecting on a listening socket */
	return jcomm_probe(client_conn->data_comm, a->probe.how);

    case netd_op_statsync:
	return jcomm_multisync(client_conn->data_comm, 
			       a->statsync.how, 
			       &a->statsync.wstat[0],
			       sizeof(a->statsync.wstat) / sizeof(a->statsync.wstat[0]));

    case netd_op_recvfrom:
	l.release();

	if (client_conn->dgram) {
	    r = jcomm_read(client_conn->data_comm,
			   &a->recvfrom.sin,
			   sizeof(a->recvfrom.sin) + a->recvfrom.count,
			   !(a->recvfrom.flags & MSG_DONTWAIT));
	    if (r == -E_AGAIN) {
		errno = EAGAIN;
		return -1;
	    }

	    if (r > 0) {
		assert(r >= (ssize_t) sizeof(a->recvfrom.sin));
		r -= sizeof(a->recvfrom.sin);
	    } else {
		cprintf("netd_linux_call: recvfrom: %s\n", e2s(r));
	    }
	} else {
	    if (a->recvfrom.wantfrom)
		memset(&a->recvfrom.sin, 0, sizeof(a->recvfrom.sin));
	    r = jcomm_read(client_conn->data_comm,
			   &a->recvfrom.buf[0], a->recvfrom.count,
			   !(a->recvfrom.flags & MSG_DONTWAIT));
	    if (r == -E_AGAIN) {
		errno = EAGAIN;
		return -1;
	    }
	}

	if (r < 0) {
	    cprintf("netd_linux_call: jcomm_read error: %s\n", e2s(r));
	    errno = EAGAIN;
	    return -1;
	}

	return r;

    case netd_op_accept:
	l.release();

	r = jcomm_read(client_conn->data_comm, &a->accept, sizeof(a->accept), 1);
	if (r < 0) {
	    cprintf("netd_linux_call: jcomm_read error: %s\n", e2s(r));
	    errno = ENOSYS;
	    return -1;
	}
	return a->accept.fd;

    case netd_op_send:
	if (a->send.flags & ~MSG_DONTWAIT)
	    break;

	l.release();
	r = jcomm_write(client_conn->data_comm, &a->send.buf[0], a->send.count, !(a->send.flags & MSG_DONTWAIT));
	if (r == -E_AGAIN) {
	    errno = EAGAIN;
	    return -1;
	}

	if (r < 0) {
	    cprintf("netd_linux_call: jcomm_write error: %s\n", e2s(r));
	    errno = ENOSYS;
	    return -1;
	}

	return r;

    default:
	break;
    }

    /* write operation request */
    z = jcomm_write(client_conn->ctrl_comm, a, a->size, 1);
    assert(z == a->size);

    /* read return value */
    z = jcomm_read(client_conn->ctrl_comm, a, sizeof(*a), 1);
    assert(z == a->size);
    if (a->rval < 0) {
	errno = -1 * a->rval;
	return -1;
    }
    return a->rval;
}
