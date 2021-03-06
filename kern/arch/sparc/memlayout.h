#ifndef JOS_MACHINE_MEMLAYOUT_H
#define JOS_MACHINE_MEMLAYOUT_H

#include <machine/mmu.h>

/*
 * Virtual memory map
 *   2^32 --------->  +---------------------------------------------+
 *                    |        AHB plug & play and I/O banks        |
 *       AHBPNPIO ->  +---------------------------------------------
 *                    |                 AHB memory                  |
 *        AHBBASE ->  +---------------------------------------------+
 *                    |                                             |
 *                    |         invalid virtual addresses           |
 *                    |                                             |
 *        PHYSEND ->  +---------------------------------------------+
 *                    |                                             |
 *                    |     0GB..1GB of physical address space      |
 *                    |                                             |
 *   PHYSBASE/ULM ->  +---------------------------------------------+
 *                    |                 user stack                  |
 *                    |                 user data                   |
 *                    |                 user text                   |
 *   0 ------------>  +---------------------------------------------+
 */

/* The physical address ranges for AHB plug & play, I/O and memory are less
 * than 256M, but we use 256M mappings out of convenience.
 */
#define AHBPNPIO        0xF0000000
#define AHBBASE         0xE0000000
#define PHYSBASE	0x40000000
#define PHYSEND         ((uintptr_t) PHYSBASE + 0x40000000)
#define KERNBASE	PHYSBASE

#define ULIM		0x40000000

/* User-mode (below ULIM) address space layout conventions. */
#define UTRAPMASKED     0x3FFFD000 /* keep syncrhonized with */
#define UWINOVERFLOW    0x3FFFE000 /* lib/sparc/Makefrag     */
#define UWINUNDERFLOW   0x3FFFF000 /*                        */
#define USTACKTOP	0x3FFFC000
#define UMMAPBASE	0x20000000
#define UHEAP		0x30000000
#define UHEAPTOP	0x38000000
#define USTARTENVRO	0x38001000
#define UTLSBASE	0x38002000
#define UTLSTOP		0x38fff000
#define UFDBASE		0x39000000
#define USEGMAPENTS	0x3a000000
#define ULDSO		0x3b000000

/* libgcc code for stack unwinding uses lots of stack space... */
#define UTLS_DEFSIZE	(2 * PGSIZE)

/* Default physical memory layout for the LEON3 */
#define PA_MEMBASE      0x40000000
#define PA_MEMEND       0x80000000

#define PA_AHBBASE      0x80000000
#define PA_APBREG       0x80000000
#define PA_APBREGEND    0x800FF000
#define PA_APBPNP       0x800FF000
#define PA_APBPNPEND    0x800FFFFF
#define PA_AHBEND       0x800FFFFF

#define PA_AHBIO        0xFFF00000
#define PA_AHBIOEND     0xFFFFF000

#define PA_AHBPNP       0xFFFFF000
#define PA_AHBPNPEND    0xFFFFFFFF

/*
 * The linker script uses PHYSLOAD_ADDR and VIRTLOAD_ADDR.
 */
#define PHYSLOAD_ADDR   KERN_LOADADDR
#define VIRTLOAD_ADDR   KERNBASE
#define LOAD_OFFSET     (VIRTLOAD_ADDR - PHYSLOAD_ADDR)

#ifndef __ASSEMBLER__
#define RELOC(x)        ((uint32_t)x - LOAD_OFFSET)
#else
#define RELOC(x)        (x - LOAD_OFFSET)
#endif

#endif
