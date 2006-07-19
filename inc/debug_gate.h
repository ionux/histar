#ifndef _JOS_INC_DEBUG_GATE_H
#define _JOS_INC_DEBUG_GATE_H

struct sigcontext;

typedef enum {
    da_wait = 1,
    da_getregs,
} debug_args_op_t;

struct debug_args 
{
    debug_args_op_t op;
    struct cobj_ref arg_obj;
    // return
    char ret;
    struct cobj_ref ret_cobj;
};

int64_t debug_gate_send(struct cobj_ref gate, struct debug_args *da);
void debug_gate_init(void);
void debug_gate_close(void);

// to be used locally
void debug_gate_signal_stop(char signo, struct sigcontext *sc);

#endif
