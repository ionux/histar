#ifndef JOS_MACHINE_ATOMIC64_H
#define JOS_MACHINE_ATOMIC64_H

/*
 * If we support SMP at some point, we should enable this.
 */
#if 0
#define ATOMIC_LOCK "lock ; "
#else
#define ATOMIC_LOCK ""
#endif

typedef struct { volatile uint64_t counter; } jos_atomic64_t;

static __inline__ void
jos_atomic_set64(jos_atomic64_t *v, uint64_t i)
{
    v->counter = i;
}

/*
 * Atomically compare the value in "v" with "old", and set "v" to "newv"
 * if equal.
 *
 * Return value is the previous value of "v".  So if return value is same
 * as "old", the swap occurred, otherwise it did not.
 */
static __inline__ uint64_t
jos_atomic_compare_exchange64(jos_atomic64_t *v, uint64_t old, uint64_t newv)
{
    uint32_t new_lo = newv & 0xffffffff;
    uint32_t new_hi = newv >> 32;

    __asm__ __volatile__(
	"pushl %%ebx\n"
	"movl %3, %%ebx\n"
	ATOMIC_LOCK "cmpxchg8b %1\n"
	"popl %%ebx\n"
	: "+A" (old), "+m" (v->counter)
	: "c" (new_hi), "g" (new_lo)
	: "cc");
    return old;
}

#undef ATOMIC_LOCK
#endif
