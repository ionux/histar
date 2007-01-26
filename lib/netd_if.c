#include <inc/lib.h>
#include <inc/netd.h>
#include <inc/fs.h>
#include <inc/error.h>
#include <inc/stdio.h>
#include <lwip/netif.h>
#include <net/if.h>
#include <errno.h>

#define NETIF_SEG_MAP(__seg, __va)				\
    do {							\
	int __r;						\
	__r = segment_map(__seg, 0,				\
			  SEGMAP_READ,				\
			  (void **)(__va), 0, 0);		\
	if (__r < 0) {						\
	    cprintf("%s: cannot segment_map: %s\n",		\
		    __FUNCTION__, e2s(__r));			\
	    return __r;						\
	}							\
    } while(0)

#define NETIF_SEG_UNMAP(__va) segment_unmap_delayed((__va), 1)
#define NETIF_CALL(fn, ...)					\
    ({								\
	int __r;						\
	struct cobj_ref __o;					\
	if ((__r = netd_defif(&__o)) < 0)			\
	    return __r;						\
	__r = netd_##fn (__o, __VA_ARGS__);			\
	__r;							\
    })

static void
flags_lwip_to_libc(uint8_t *lwip, uint16_t *netd)
{
    *netd = 0;
    *netd |= *lwip & NETIF_FLAG_UP ? (IFF_UP|IFF_RUNNING) : 0;
    *netd |= *lwip & NETIF_FLAG_BROADCAST ? (IFF_BROADCAST) : 0;
    *netd |= *lwip & NETIF_FLAG_POINTTOPOINT ? (IFF_POINTOPOINT) : 0;
}

static int
netd_defif(struct cobj_ref *ref)
{
    struct fs_inode netd_ct_ino;
    int r = fs_namei("/netd", &netd_ct_ino);
    if (r < 0) {
	cprintf("netd_defif: fs_namei /netd: %s\n", e2s(r));
	return r;
    }

    uint64_t netd_ct = netd_ct_ino.obj.object;
    int64_t seg_id = container_find(netd_ct, kobj_segment, "netif");
    if (seg_id < 0)
	return seg_id;

    *ref = COBJ(netd_ct, seg_id);
    return 0;
}

static int
netd_ifip(struct cobj_ref r, struct netd_sockaddr_in *nsin)
{
    struct netif *nif = 0;
    NETIF_SEG_MAP(r, &nif);
    nsin->sin_addr = nif->ip_addr.addr;
    nsin->sin_port = 0;
    NETIF_SEG_UNMAP(nif);
    return 0;
}

int 
netd_ip(struct netd_sockaddr_in *nsin)
{
    return NETIF_CALL(ifip, nsin);
}

static int
netd_ifmask(struct cobj_ref r, struct netd_sockaddr_in *nsin)
{
    struct netif *nif = 0;
    NETIF_SEG_MAP(r, &nif);
    nsin->sin_addr = nif->netmask.addr;
    nsin->sin_port = 0;
    NETIF_SEG_UNMAP(nif);
    return 0;
}

int 
netd_netmask(struct netd_sockaddr_in *nsin)
{
    return NETIF_CALL(ifmask, nsin);
}

static int
netd_ifname(struct cobj_ref r, char *buf)
{
    struct netif *nif = 0;
    NETIF_SEG_MAP(r, &nif);
    buf[0] = nif->name[0];
    buf[1] = nif->name[1];
    NETIF_SEG_UNMAP(nif);
    return 0;
}

int
netd_name(char *buf)
{
    return NETIF_CALL(ifname, buf);
}

static int
netd_ifflags(struct cobj_ref r, uint16_t *flags)
{
    struct netif *nif = 0;
    NETIF_SEG_MAP(r, &nif);
    flags_lwip_to_libc(&nif->flags, flags);
    NETIF_SEG_UNMAP(nif);
    return 0;
}

int
netd_flags(uint16_t *flags)
{
    return NETIF_CALL(ifflags, flags);
}
