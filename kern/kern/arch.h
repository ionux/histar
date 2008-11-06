#ifndef JOS_KERN_ARCH_H
#define JOS_KERN_ARCH_H

#include <machine/param.h>
#include <machine/types.h>
#include <machine/memlayout.h>
#include <machine/pmap.h>
#include <machine/io.h>
#include <machine/setjmp.h>
#include <kern/thread.h>
#include <kern/param.h>
#include <inc/alignmacro.h>

/*
 * Page table (Pagemap) handling
 */
int  page_map_alloc(struct Pagemap **pm_store)
    __attribute__ ((warn_unused_result));
void page_map_free(struct Pagemap *pgmap);

/* Traverse [first .. last]; clamps last down to ULIM-PGSIZE */
typedef void (*page_map_traverse_cb)(const void *arg, ptent_t *ptep, void *va);
int  page_map_traverse(struct Pagemap *pgmap, const void *first,
		       const void *last, int create,
		       page_map_traverse_cb cb, const void *arg)
    __attribute__ ((warn_unused_result));

/* Get (and possibly create) the PTE entry for va; clamps down to ULIM-PGSIZE */
int  pgdir_walk(struct Pagemap *pgmap, const void *va,
		int create, ptent_t **pte_store)
    __attribute__ ((warn_unused_result));

/* Physical address handling */
void *pa2kva(physaddr_t pa);
physaddr_t kva2pa(void *kva);
ppn_t pa2ppn(physaddr_t pa);
physaddr_t ppn2pa(ppn_t pn);

/* XXX temporary hack */
int uva2pa(struct Pagemap *pgmap, void *va, physaddr_t *pa);

/*
 * Miscellaneous
 */
extern char boot_cmdline[];
void machine_reboot(void) __attribute__((noreturn));
uintptr_t karch_get_sp(void);
uint64_t karch_get_tsc(void);
void karch_jmpbuf_init(struct jos_jmp_buf *jb, void *fn, void *stackbase);
void karch_fp_init(struct Fpregs *fpreg);

/* Returns a trap number */
void	 irq_arch_enable(trapno_t trapno);
void	 irq_arch_disable(uint32_t trapno);
void	 irq_arch_eoi(uint32_t trapno);
trapno_t irq_arch_init(uint32_t irqno, tbdp_t tbdp, bool_t user);
void	 irq_arch_umask_enable(void);
void	 irq_arch_umask_disable(void);

int	 arch_out_port(uint64_t port, uint8_t width, uint8_t *val, uint64_t n);
int	 arch_in_port(uint64_t port, uint8_t width, uint8_t *val, uint64_t n);

/*
 * Multiprocessor
 */
uint32_t arch_cpu(void);

/*
 * Page map manipulation
 */
struct Pagemap;
void pmap_set_current(struct Pagemap *pm);
void as_arch_collect_dirty_bits(const void *arg, ptent_t *ptep, void *va);
void as_arch_page_invalidate_cb(const void *arg, ptent_t *ptep, void *va);
void as_arch_page_map_ro_cb(const void *arg, ptent_t *ptep, void *va);
int  as_arch_putpage(struct Pagemap *pmap, void *va, void *pp, uint32_t flags);

/*
 * Checks that [ptr .. ptr + nbytes) is valid user memory,
 * and makes sure the address is paged in (might return -E_RESTART).
 * Checks for writability if (reqflags & SEGMAP_WRITE).
 * Checks for alignment to alignbytes, if needed for architecture.
 */
int  check_user_access2(const void *ptr, uint64_t nbytes,
			uint32_t reqflags, int alignbytes)
    __attribute__ ((warn_unused_result));

#define check_user_access(ptr, nbytes, reqflags)			\
	check_user_access2(ptr, nbytes, reqflags,			\
			   __jos_alignment_of(__typeof__(*(ptr))))

/*
 * Threads and traps
 */
void thread_arch_run(const struct Thread *t)
    __attribute__((__noreturn__));
void thread_arch_idle(void)
    __attribute__((__noreturn__));
int  thread_arch_utrap(struct Thread *t, 
		       uint32_t src, uint32_t num, uint64_t arg)
    __attribute__ ((warn_unused_result));
int  thread_arch_get_entry_args(const struct Thread *t,
				struct thread_entry_args *targ)
    __attribute__ ((warn_unused_result));
void thread_arch_jump(struct Thread *t, const struct thread_entry *te);
int  thread_arch_is_masked(const struct Thread *t);
void thread_arch_rebooting(struct Thread *t);

#endif
