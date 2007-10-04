#ifndef JOS_MACHINE_PARAM_H
#define JOS_MACHINE_PARAM_H

#include <kern/param.h>

#define JOS_ARCH_BITS	32
#define JOS_ARCH_ENDIAN	JOS_BIG_ENDIAN
#define JOS_ARCH_RETADD	1

#ifdef JOS_KERNEL
#define MAX_IRQS	16

enum { kobj_hash_size = 8192 };
enum { kobj_neg_size = 2 };
enum { kobj_neg_hash = 64 };
enum { part_enable = 1 };
#endif

#endif
