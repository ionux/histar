#ifndef JOS_INC_THREAD_H
#define JOS_INC_THREAD_H

#include <inc/types.h>
#include <inc/segment.h>

enum { thread_entry_narg = 6 };

struct thread_entry {
    struct cobj_ref te_as;
    void *te_entry;
    void *te_stack;

    uint64_t te_arg[thread_entry_narg];
};

struct thread_entry_args {
    uint64_t te_arg[thread_entry_narg];
};

// energy billing types
enum { THREAD_BILL_ENERGY_NET_SEND, THREAD_BILL_ENERGY_NET_RECV };

#endif
