#ifndef JOS_MACHINE_MEMLAYOUT_H
#define JOS_MACHINE_MEMLAYOUT_H

#include <machine/mmu.h>

#define USYSCALL	0x07003000
#define USCRATCH	0x07002000
#define USPRING		0x07001000
#define ULIM		0x07000000
#define USTACKTOP	0x07000000

#define UMMAPBASE	0x040000000
#define UHEAP		0x050000000
#define UHEAPTOP	0x060000000
#define USTARTENVRO	0x060001000
#define UTLSBASE	0x060002000
#define UTLSTOP		0x060fff000
#define UFDBASE		0x061000000
#define USEGMAPENTS	0x062000000
#define ULDSO		0x063000000
#define UTLS_DEFSIZE	PGSIZE

#endif
