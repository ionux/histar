#ifndef JOS_KERN_SCHED_H
#define JOS_KERN_SCHED_H

#include <kern/thread.h>

void schedule(void);
void sched_init(void);

void sched_join(struct Thread *t);
void sched_leave(struct Thread *t);

void sched_stop(struct Thread *t, uint64_t elapsed);

#endif
