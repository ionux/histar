
#include <machine/pmap.h>
#include <machine/x86.h>
#include <machine/thread.h>
#include <machine/trap.h>
#include <kern/lib.h>
#include <dev/console.h>
#include <dev/disk.h>
#include <dev/pci.h>
#include <dev/picirq.h>
#include <dev/kclock.h>
#include <kern/sched.h>
#include <kern/container.h>

/*
 * Variable panicstr contains argument to first call to panic; used as flag
 * to indicate that the kernel has already called panic.
 */
static const char *panicstr;

/*
 * Panic is called on unresolvable fatal errors.
 * It prints "panic: mesg", and then enters the kernel monitor.
 */
void
_panic (const char *file, int line, const char *fmt, ...)
{
  va_list ap;

  if (panicstr)
    goto dead;
  panicstr = fmt;

  va_start (ap, fmt);
  cprintf ("[%p] kpanic: %s:%d: ", cur_thread, file, line);
  vcprintf (fmt, ap);
  cprintf ("\n");
  va_end (ap);

#if 0
  mon_backtrace (0, 0, 0);

dead:
  /* break into the kernel monitor */
  while (1)
    monitor (NULL);
#else
 dead:
  abort ();
#endif
}

void
abort (void)
{
  outw (0x8A00, 0x8A00);
  outw (0x8A00, 0x8AE0);
  for (;;)
    ;
}

void
flush_tlb_hard (void)
{
  uint64_t cr3;
  uint64_t cr4;
  asm volatile ("movq %%cr3,%0":"=a" (cr3));
  asm volatile ("movq %%cr4,%0":"=a" (cr4));
  asm volatile ("movq %0,%%cr4"::"a" (cr4 & ~CAST64 (CR4_PGE)));
  asm volatile ("movq %0,%%cr3"::"a" (cr3));
  asm volatile ("movq %0,%%cr4"::"a" (cr4));
}

static void
mmu_init (void)
{
  /* Move gdt to kernel memory and reload */
  gdtdesc.pd_base = (uintptr_t) & gdt;
  asm volatile ("lgdt (%0)"::"r" (&gdtdesc.pd_lim));

  /* Nuke identically mapped physical memory */
  bootpml4[0] = 0;
  flush_tlb_hard ();

  /* Load TSS */
  gdt[(GD_TSS >> 3)] = (SEG_TSSA | SEG_P | SEG_BASELO (&tss)
			| SEG_LIM (sizeof (tss) - 1));
  gdt[(GD_TSS >> 3) + 1] = SEG_BASEHI (&tss);
  asm volatile ("ltr %w0"::"r" (GD_TSS));
}

static void
bss_init (void)
{
  extern char edata[], end[];
  memset (edata, 0, end - edata);
}

void
init (void)
{
  mmu_init ();
  bss_init ();
  idt_init ();
  cons_init ();
  pic_init ();
  kclock_init ();
  pmap_init ();
  pci_init ();

  disk_test ();

  struct Container *rc;
  int r = container_alloc(&rc);
  if (r < 0)
    panic("cannot allocate root container");

  THREAD_CREATE_EMBED(rc, user_idle);
  THREAD_CREATE_EMBED(rc, user_hello);
  THREAD_CREATE_EMBED(rc, user_chatter2);
  THREAD_CREATE_EMBED(rc, user_ct_unref);

  schedule();
}
