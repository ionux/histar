#include <inc/sockfd.h>
#include <inc/uds.h>
#include <inc/bipipe.h>
#include <inc/stdio.h>

#include <sys/socket.h>
#include <errno.h>

libc_hidden_proto(socket)

int
socket(int domain, int type, int protocol)
{
    switch(domain) {
    case PF_INET:
	return netd_socket(domain, type, protocol);
    case PF_UNIX:
	return uds_socket(domain, type, protocol);
    default:
	cprintf("socket: unimplemented domain: %d\n", domain);
	errno = ENOSYS;
	return -1;
    }
}

int
socketpair(int domain, int type, int protocol, int sv[2])
{
    // fudge the socketpair
    return bipipe(type, sv);
}

libc_hidden_def(socket)

