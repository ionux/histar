#include <kern/arch.h>
#include <kern/lib.h>
#include <kern/part.h>
#include <kern/kobj.h>
#include <kern/sched.h>
#include <kern/pstate.h>
#include <kern/uinit.h>
#include <kern/prof.h>
#include <machine/trap.h>
#include <machine/pmap.h>
#include <machine/sparc-common.h>
#include <dev/apbucons.h>
#include <dev/amba.h>
#include <dev/irqmp.h>
#include <dev/gptimer.h>
#include <dev/greth.h>

char boot_cmdline[256];

static void
mmu_init(void)
{
    for (uint32_t i = 64; i < 128; i++)
	bootpt.pm1_ent[i] = 0;
    tlb_flush_all();
}

static void
bss_init (void)
{
    extern char sbss[], ebss[];
    memset(sbss, 0, ebss - sbss);
}

void __attribute__((noreturn))
init (void)
{
    int r;

    mmu_init();
    bss_init();

    amba_init();
    irqmp_init();

    gptimer_init();

    apbucons_init();
    amba_print();

    page_init();

    r = greth_init();
    if (r < 0)
	cprintf("init: greth_init error: %s\n", e2s(r));
    
    kobject_init();
    sched_init();
    pstate_init();
    prof_init();

    user_init();

    cprintf("=== kernel ready, calling thread_run() ===\n");
    thread_run();
}
