#ifndef JOS_MACHINE_PARAM_H
#define JOS_MACHINE_PARAM_H

#include <kern/param.h>

#define JOS_ARCH_BITS	32
#define JOS_ARCH_ENDIAN	JOS_BIG_ENDIAN

#ifdef JOS_KERNEL
#define MAX_IRQS	16

enum { kobj_hash_size = 1 };
enum { kobj_neg_size = 1 };
enum { kobj_neg_hash = 1 };
enum { part_enable = 0 };
#endif

#endif
