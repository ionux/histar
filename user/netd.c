#include <inc/memlayout.h>
#include <inc/syscall.h>
#include <inc/stdio.h>
#include <inc/lib.h>
#include <inc/assert.h>
#include <inc/string.h>

#include <lwip/netif.h>
#include <lwip/stats.h>
#include <lwip/sys.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/dhcp.h>
#include <netif/etharp.h>

#include <jif/jif.h>

static uint64_t container;

struct timer_thread {
    int msec;
    void (*func)();
    struct cobj_ref thread;
};

static void
lwip_init(struct netif *nif)
{
    // lwIP initialization sequence, as suggested by lwip/doc/rawapi.txt
    stats_init();
    sys_init();
    mem_init();
    memp_init();
    pbuf_init();
    etharp_init();
    ip_init();
    udp_init();
    tcp_init();

    struct ip_addr ipaddr, netmask, gateway;
    memset(&ipaddr,  0, sizeof(ipaddr));
    memset(&netmask, 0, sizeof(netmask));
    memset(&gateway, 0, sizeof(gateway));

    if (0 == netif_add(nif, &ipaddr, &netmask, &gateway, 0, jif_init, ip_input))
	panic("lwip_init: error in netif_add\n");

    netif_set_default(nif);
    netif_set_up(nif);
}

static void __attribute__((noreturn))
net_receive(void *arg)
{
    struct netif *nif = arg;

    for (;;)
	jif_input(nif);
}

static void __attribute__((noreturn))
net_timer(void *arg)
{
    struct timer_thread *t = arg;

    for (;;) {
	t->func();
	sys_thread_sleep(t->msec);
    }
}

static void
start_timer(struct timer_thread *t, void (*func)(), int msec)
{
    t->msec = msec;
    t->func = func;
    int r = thread_create(container, &net_timer, t, &t->thread);
    if (r < 0)
	panic("cannot create timer thread: %s", e2s(r));
}

static err_t
cb_sent(void *arg, struct tcp_pcb *c, u16_t len)
{
    int64_t remaining = (int64_t) arg;
    remaining -= len;
    tcp_arg(c, (void*)remaining);

    if (remaining > 0)
	return ERR_OK;

    err_t e = tcp_close(c);
    if (e)
	panic("tcp_close: %d", e);

    return ERR_OK;
}

static err_t
cb_accept(void *arg, struct tcp_pcb *c, err_t err)
{
    static const char *msg = "Hello world.\n";

    if (err)
	panic("cb_accept: %d", err);

    int64_t len = strlen(msg);
    tcp_arg(c, (void*)len);
    tcp_sent(c, &cb_sent);

    err_t e = tcp_write(c, msg, len, 0);
    if (e)
	panic("tcp_write: %d", e);

    return ERR_OK;
}

static void
netd_server()
{
    // testing
    struct tcp_pcb *srv = tcp_new();
    if (!srv)
	panic("tcp_new");

    srv = tcp_listen(srv);
    if (!srv)
	panic("tcp_listen");

    err_t e = tcp_bind(srv, IP_ADDR_ANY, 23);
    if (e)
	panic("tcp_bind: %d", e);

    tcp_accept(srv, &cb_accept);
}

int
main(int ac, char **av)
{
    // container is passed as argument to _start()
    container = start_arg;

    struct netif nif;
    lwip_init(&nif);
    dhcp_start(&nif);

    struct cobj_ref receive_thread;
    int r = thread_create(container, &net_receive, &nif, &receive_thread);
    if (r < 0)
	panic("cannot create receiver thread: %s", e2s(r));

    struct timer_thread t_arp, t_tcpf, t_tcps, t_dhcpf, t_dhcpc;

    start_timer(&t_arp,	    &etharp_tmr,	ARP_TMR_INTERVAL);
    start_timer(&t_tcpf,    &tcp_fasttmr,	TCP_FAST_INTERVAL);
    start_timer(&t_tcps,    &tcp_slowtmr,	TCP_SLOW_INTERVAL);
    start_timer(&t_dhcpf,   &dhcp_fine_tmr,	DHCP_FINE_TIMER_MSECS);
    start_timer(&t_dhcpc,   &dhcp_coarse_tmr,	DHCP_COARSE_TIMER_SECS * 1000);

    cprintf("netd: up and running\n");

    netd_server();
    sys_thread_halt();    
}
