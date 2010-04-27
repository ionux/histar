/* linux/drivers/net/msm_rmnet.c
 *
 * Virtual Ethernet Interface for MSM7K Networking
 *
 * Copyright (C) 2007 Google, Inc.
 * Author: Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

//#define DEBUG WOOHOO

extern "C" {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/mman.h>

#include <inc/assert.h>
#include <inc/error.h>
#include <inc/stdio.h>
#include <inc/queue.h>
#include <inc/syscall.h>
#include <inc/jhexdump.h>
}

#include "msm_smd.h"
#include "smd_rmnet.h"

#define NPKTQUEUE	64

struct ringseg rxsegs[3];

static struct rmnet_private {
	smd_channel_t *ch;
	const char *chname;
	size_t tx_frames, tx_frame_bytes;
	size_t rx_frames, rx_frame_bytes, rx_dropped;
	struct ringseg *rxseg;
	int fast_path;
	pthread_mutex_t mtx;
	pthread_cond_t  cond;
} rmnet_private[3];

static void smd_net_notify(void *_priv, unsigned event)
{
	if (event != SMD_EVENT_DATA)
		return;

	struct rmnet_private *p = (struct rmnet_private *)_priv;
	int sz;
	int fast_wakeup = 0;

	for (;;) {
		sz = smd_cur_packet_size(p->ch);
		if (sz == 0)
			break;
		if (smd_read_avail(p->ch) < sz)
			break;

		if (sz > 1514) {
			cprintf("rmnet_recv() discarding %d len\n", sz);
			if (smd_read(p->ch, NULL, sz) != sz)
				cprintf("rmnet_recv() smd lied about avail?!\n");
			p->rx_dropped++;
		} else {
			struct ringseg *seg = p->rxseg;
			struct ringpkt *rxp = &seg->q[seg->q_head];

			if (smd_read(p->ch, (void *)rxp->buf, sz) != sz) {
				cprintf("rmnet_recv() smd lied about avail?!\n");
				return;
			}

#ifdef DEBUG
			cprintf("RMNET RECEIVED A FRAME (%d bytes)\n", sz);
			jhexdump((const unsigned char *)pktbuf, sz);
#endif

			p->rx_frames++;
			p->rx_frame_bytes += sz;

			if (p->fast_path) {
				rxp->bytes = sz; 
				seg->q_head = (seg->q_head + 1) % NPKTQUEUE;
				if (seg->q_head == seg->q_tail)
					cprintf("RMNET OVERRAN THE WHOLE RING!!\n");
				fast_wakeup = 1;
			} else {
				// XXX- slow ass.
				//      could be better to move the locking out
				//      of the loop, but the gate call overhead
				//      is pretty bad anyway
				pthread_mutex_lock(&p->mtx);
				rxp->bytes = sz;
				seg->q_head = (seg->q_head + 1) % NPKTQUEUE;
				if (seg->q_head == seg->q_tail)
					cprintf("RMNET OVERRAN THE WHOLE RING!!\n");
				pthread_cond_broadcast(&p->cond);
				pthread_mutex_unlock(&p->mtx);
			}
		}
	}

	if (fast_wakeup)
		sys_sync_wakeup(&p->rxseg->q_head);
}

extern "C" int smd_rmnet_open(int which)
{
	int r;

	if (which < 0 || which >= 3)
		return -E_NOT_FOUND;

#ifdef DEBUG
	cprintf("rmnet_open(%d)\n", which);
#endif

	struct rmnet_private *p = &rmnet_private[which]; 
	if (!p->ch) {
		r = smd_open(p->chname, &p->ch, p, smd_net_notify);

		if (r < 0)
			return r;
	}

	return 0;
}

extern "C" void smd_rmnet_close(int which)
{
}

extern "C" int smd_rmnet_xmit(int which, void *buf, int len)
{
	if (which < 0 || which >= 3)
		return -E_NOT_FOUND;

	struct rmnet_private *p = &rmnet_private[which]; 
	if (p->ch == NULL)
		return -E_INVAL;

#ifdef DEBUG
	cprintf("RMNET XMITTING A FRAME: which = %d, %d bytes\n", which, len);
	jhexdump((const unsigned char *)buf, len);
#endif

	// NB: smd_write already does mutual exclusion
	if (smd_write(p->ch, buf, len) != len) {
		cprintf("rmnet fifo full, dropping packet\n");
	} else {
		p->tx_frames++;
		p->tx_frame_bytes += len;	
	}

	return 0;
}

extern "C" int smd_rmnet_recv(int which, void *buf, int len)
{
	if (which < 0 || which >= 3)
		return -E_NOT_FOUND;

	struct rmnet_private *p = &rmnet_private[which]; 
	if (p->ch == NULL)
		return -E_INVAL;

	pthread_mutex_lock(&p->mtx);
	while (p->rxseg->q_tail == p->rxseg->q_head)
		pthread_cond_wait(&p->cond, &p->mtx);

	struct ringpkt *rxp = &p->rxseg->q[p->rxseg->q_tail];
	int recvd = MIN(len, rxp->bytes);
	memcpy(buf, (void *)rxp->buf, recvd);
	rxp->bytes = 0;
	p->rxseg->q_tail = (p->rxseg->q_tail + 1) % NPKTQUEUE;
	pthread_mutex_unlock(&p->mtx);

	return recvd;
}

extern "C" int smd_rmnet_fast_setup(int which, void *tx, int txlen,
    void *rx, int rxlen)
{
	if (which < 0 || which >= 3)
		return -E_NOT_FOUND;

	if (txlen < (int)sizeof(struct ringseg)) {
		cprintf("%s: txlen too small!\n", __func__);
		return -E_INVAL;
	}

	if (rxlen < (int)sizeof(struct ringseg)) {
		cprintf("%s: rxbuf len too small!\n", __func__);
		return -E_INVAL;
	}

	struct rmnet_private *p = &rmnet_private[which]; 
	p->rxseg = (struct ringseg *)rx;
	p->fast_path = 1;
	memset(p->rxseg, 0, sizeof(*p->rxseg));
cprintf("%s: shared memory: rx @ %p, len %d\n", __func__, rx, rxlen); 
	//xxx - do tx later if we care. downstream is what we care about anyhow 

	return 0;
}

extern "C" void smd_rmnet_init()
{
	static const char *names[3] = { "SMD_DATA5", "SMD_DATA6", "SMD_DATA7" };
	for (int i = 0; i < 3; i++) {
		struct rmnet_private *p = &rmnet_private[i]; 
		memset(p, 0, sizeof(*p));
		p->rxseg = &rxsegs[i];
		p->chname = names[i];
		pthread_mutex_init(&p->mtx, NULL);
		pthread_cond_init(&p->cond, NULL);
	}
}
