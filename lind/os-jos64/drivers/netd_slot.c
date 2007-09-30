#include <stdint.h>
#include <inc/jthread.h>
#include <inc/assert.h>
#include <string.h>

#include "netduser.h"

struct sock_slot slots[128];
jthread_mutex_t slot_alloc_mu;

struct sock_slot *
slot_alloc(void)
{
    int i;
    jthread_mutex_lock(&slot_alloc_mu);
    for(i = 0; i < sizeof(slots) / sizeof(struct sock_slot); i++) {
	if (!slots[i].used) {
	    memset(&slots[i], 0, sizeof(slots[i]));
	    slots[i].sock = -1;
	    slots[i].used = 1;
	    jthread_mutex_unlock(&slot_alloc_mu);
	    return &slots[i];
	}
    }
    jthread_mutex_unlock(&slot_alloc_mu);
    return 0;
}

int
slot_to_id(struct sock_slot *ss)
{
    return ((uintptr_t)ss - (uintptr_t)&slots[0]) / sizeof(struct sock_slot);
}

struct sock_slot *
slot_from_id(int id)
{
    assert(id < sizeof(slots) / sizeof(struct sock_slot));
    return &slots[id];
}

void
slot_free(struct sock_slot *ss)
{
    ss->used = 0;
}

void
slot_for_each(void (*op)(struct sock_slot*, void*), void *arg)
{
    int i;
    for (i = 0; i < sizeof(slots) / sizeof(struct sock_slot); i++)
	if (slots[i].used)
	    op(&slots[i], arg);
}

static void
init_slot(struct sock_slot *s, void *x)
{
    s->sock = -1;
}

void
slot_init(void)
{
    memset(slots, 0, sizeof(slots));
    slot_for_each(&init_slot, 0);
}
