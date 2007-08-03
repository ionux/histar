#ifndef JOS_MACHINE_UTRAP_H
#define JOS_MACHINE_UTRAP_H

#include <inc/types.h>
#include <machine/mmu.h>

#define JOS_UTRAP_GCCATTR

struct UTrapframe {
#define utf_stackptr utf_reg.sp
    struct Regs utf_reg;
    uint32_t utf_pc;
    uint32_t utf_npc;
    uint32_t utf_y;
    
    uint32_t utf_trap_src;
    uint32_t utf_trap_num;
    uint64_t utf_trap_arg;
};

#define UTRAP_SRC_HW	1
#define UTRAP_SRC_USER	2

#define UT_MASK 0
#define UT_NMASK 1

#endif
