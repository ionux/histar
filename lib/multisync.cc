extern "C" {
#include <inc/lib.h>
#include <inc/segment.h>
#include <inc/multisync.h>
#include <inc/stdio.h>
#include <inc/syscall.h>
#include <inc/error.h>
#include <machine/memlayout.h>

#include <errno.h>
#include <string.h>
}

#include <inc/error.hh>

enum { dbg = 0 };

extern "C" int
multisync_wait(struct wait_stat *wstat, uint64_t n, uint64_t nsec)
{
    int r = 0;
    volatile uint64_t *addrs[n];
    uint64_t vals[n];
    uint64_t refcts[n];

    // for assigning wait addresses
    uint64_t spares[n];
    uint64_t spares_used = 0;
    memset(spares, 0, sizeof(spares));

    // store cb1 args
    void *args[n];

    // store mapped segments
    uint64_t *mapped[n];
    uint64_t mapped_count = 0;

    try {
	for (uint64_t i = 0; i < n; i++) {
	    if (WS_ISOBJ(&wstat[i])) {
		// map the segment to pass an address to the kernel, unmap
		// segments when wait returns.
		uint8_t *addr = 0;

		// Map just enough of the segment to get the wait value
		uint64_t mapbytes = PGOFF(wstat[i].ws_off) + sizeof(uint64_t);

		// SEGMAP_WRITE only because it speeds up sys_sync_wait
		error_check(segment_map(wstat[i].ws_seg,
					wstat[i].ws_off - PGOFF(wstat[i].ws_off),
					SEGMAP_READ | SEGMAP_WRITE,
					(void **) &addr, &mapbytes, 0));

		mapped[mapped_count] = (uint64_t *) addr;
		mapped_count++;

		addr += PGOFF(wstat[i].ws_off);
		addrs[i] = (uint64_t *) addr;
		vals[i] = wstat[i].ws_val;
		refcts[i] = wstat[i].ws_seg.container;
	    } else if (WS_ISASS(&wstat[i])) {
		// assign address
		addrs[i] = &spares[spares_used++];
		vals[i] = 0;
		refcts[i] = 0;
	    } else {
		// given address
		addrs[i] = wstat[i].ws_addr;
		vals[i] = wstat[i].ws_val;
		refcts[i] = wstat[i].ws_refct;
	    }
	}

	int cb0_err = 0;
	
	for (uint64_t i = 0; i < n; i++) {
	    if (wstat[i].ws_cb0) {
		r = (*wstat[i].ws_cb0)(&wstat[i], wstat[i].ws_probe, 
				       &addrs[i], &args[i]);
		if (r < 0) {
		    cb0_err = 1;
		    if (dbg)
			cprintf("multisync_wait: cb0 (%p) error: %s\n", 
				wstat[i].ws_cb0, e2s(r));
		}
	    }
	}
	
	r = cb0_err ? 0 : sys_sync_wait_multi(addrs, vals, refcts, n, nsec);
	if (r == -E_NO_SPACE) {
	    error_check(sys_self_set_waitslots(n));
	    error_check(r = sys_sync_wait_multi(addrs, vals, refcts, n, nsec));
	}
	else if (r < 0)
	    throw error(r, "sys_sync_wait_multi error");
    } catch (basic_exception &e) {
	if (dbg)
	    cprintf("multisync_wait: error: %s\n", e.what());
	errno = EIO;
	r = -1;
    }
    
    for (uint64_t i = 0; i < n; i++) {
	if (wstat[i].ws_cb1) {
	    r = (*wstat[i].ws_cb1)(&wstat[i], args[i], wstat[i].ws_probe);
	    if (r < 0 && dbg)
		cprintf("multisync_wait: cb1 (%p) error: %s\n", 
			wstat[i].ws_cb1, e2s(r));
	}
    }
    
    for (uint64_t i = 0; i < mapped_count; i++)
	segment_unmap_delayed(mapped[i], 1);
    
    return r;
}
