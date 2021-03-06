#ifndef JOS_INC_LABEL_H
#define JOS_INC_LABEL_H

#include <inc/types.h>
#include <inc/intmacro.h>

struct ulabel {
    uint32_t ul_size;
    uint32_t ul_nent;

    uint8_t ul_default;
    uint32_t ul_needed;

    uint64_t *ul_ent;
};

#define LB_HANDLE(ent)		((ent) & UINT64(0x1fffffffffffffff))
#define LB_LEVEL(ent)		((uint8_t) ((ent) >> 61))

#define LB_LEVEL_STAR		4
#define LB_LEVEL_UNDEF		5
#define LB_CODE(h, level)	((h) | (((uint64_t) (level)) << 61))

typedef uint8_t level_t;

#endif
