#ifndef JOS_KERN_NETDEV_H
#define JOS_KERN_NETDEV_H

#include <machine/types.h>
#include <kern/thread.h>
#include <kern/segment.h>
#include <kern/kobjhdr.h>
#include <inc/netdev.h>

struct net_device {
    void *arg;

    int  (*add_buf_rx) (void *a, const struct Segment *sg,
			struct netbuf_hdr *nb, uint16_t size);
    int  (*add_buf_tx) (void *a, const struct Segment *sg,
			struct netbuf_hdr *nb, uint16_t size);
    void (*buffer_reset) (void *a);

    uint8_t mac_addr[6];

    uint64_t waiter_id;
    int64_t wait_gen;
    struct Thread_list wait_list;
};

enum { netdevs_max = 32 };
extern struct net_device *netdevs[netdevs_max];
extern uint64_t netdevs_num;

// The API for netdev_thread_wait is as follows:
//
//  * If the last call to thread_wait was with a different waiter
//    value (or this is the first time thread_wait is called and
//    waiter != 0), then all buffers for the card are cleared and
//    -E_AGAIN is returned.
//
//  * Otherwise, if the value gen is the same as the one returned
//    by the previous call to thread_wait, the thread is put to
//    sleep, and -E_RESTART is returned.
//
//  * Otherwise, a new generation number is returned (positive
//    64-bit signed int).

int64_t netdev_thread_wait(struct net_device *ndev, const struct Thread *t,
			   uint64_t waiter, int64_t gen);
void	netdev_thread_wakeup(struct net_device *ndev);

void	netdev_macaddr(struct net_device *ndev, uint8_t *addrbuf);
int	netdev_add_buf(struct net_device *ndev, const struct Segment *sg,
		       uint64_t offset, netbuf_type type)
	__attribute__ ((warn_unused_result));

void	netdev_register(struct net_device *netdev);

#endif
