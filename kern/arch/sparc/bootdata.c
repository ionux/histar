#include <machine/pmap.h>
#include <machine/tag.h>

/*
 * Boot page tables
 */

#define PTATTR __attribute__ ((aligned (4096), section (".data")))
#define KPT_BITS ((PT_ET_PTE << PT_ET_SHIFT) | \
		  (PTE_ACC_SUPER << PTE_ACC_SHIFT))

#define DO_8(_start, _macro)				\
  _macro (((_start) + 0)) _macro (((_start) + 1))	\
  _macro (((_start) + 2)) _macro (((_start) + 3))	\
  _macro (((_start) + 4)) _macro (((_start) + 5))	\
  _macro (((_start) + 6)) _macro (((_start) + 7))

#define DO_16(_start, _macro)					\
  DO_8 ((_start) + 0, _macro) DO_8 ((_start) + 8, _macro)

#define DO_64(_start, _macro)					\
  DO_8 ((_start) + 0, _macro) DO_8 ((_start) + 8, _macro)	\
  DO_8 ((_start) + 16, _macro) DO_8 ((_start) + 24, _macro)	\
  DO_8 ((_start) + 32, _macro) DO_8 ((_start) + 40, _macro)	\
  DO_8 ((_start) + 48, _macro) DO_8 ((_start) + 56, _macro)

#define TRANS16MEG_C(n) \
    (PTE_C | KPT_BITS | (((0x1000000UL * (n)) >> PGSHIFT) << PTE_PPN_SHIFT)),
#define TRANS16MEG_NC(n) \
    (KPT_BITS | (((0x1000000UL * (n)) >> PGSHIFT) << PTE_PPN_SHIFT)),

/*
 * See memlayout.h for kernel memory layout.
 */
struct Pagemap bootpt PTATTR = {
  .pm1_ent = {
    [64] = DO_64(64, TRANS16MEG_C)
    [128] = DO_64(64, TRANS16MEG_C)
    [224] = DO_16(128, TRANS16MEG_NC)
    [240] = DO_16(240, TRANS16MEG_NC)
  }
};

/*
 * Context table, inited during boot
 */
struct Contexttable bootct __attribute__ ((aligned (4096))) __krw__;

struct Trapcode idt[0x100];

