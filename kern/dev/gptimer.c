#include <kern/lib.h>
#include <kern/timer.h>
#include <kern/intr.h>
#include <kern/arch.h>
#include <kern/sched.h>
#include <machine/leon3.h>
#include <machine/sparc-config.h>
#include <machine/tag.h>
#include <dev/gptimer.h>
#include <dev/ambapp.h>
#include <dev/amba.h>

struct gpt_ts {
    struct time_source gpt_src;
    uint32_t mask;
    uint32_t last_read;
    uint64_t ticks;
    LEON3_GpTimerElem_Regs_Map *regs;
};

struct gpt_sh {
    uint32_t hz;
    uint32_t mask;
    LEON3_GpTimerElem_Regs_Map *regs;
};

static uint32_t
gpt_tsval(struct gpt_ts *gpt)
{
    return gpt->mask - gpt->regs->val;
}

/*
 * XXX
 * This function may need to be a "subsystem" of some sort,
 * which will have privileges over "last_read" & "ticks"..
 */
static uint64_t
gpt_get_ticks(void *arg)
{
    struct gpt_ts *gpt = (struct gpt_ts *)arg;
    uint32_t val = gpt_tsval(gpt);
    uint32_t diff = (val - gpt->last_read) & gpt->mask;
    gpt->last_read = val;
    gpt->ticks += diff;
    
    return gpt->ticks;
}

static void
gpt_delay(void *arg, uint64_t nsec)
{
    struct gpt_ts *gpt = (struct gpt_ts *)arg;
    uint32_t now = gpt_get_ticks(gpt);
    uint64_t diff = timer_convert(nsec, gpt->gpt_src.freq_hz, 1000000000);
    while ((gpt_get_ticks(gpt) - now) < diff)
	;
}

static void
gpt_schedule(void *arg, uint64_t nsec)
{
    struct gpt_sh *gpt = (struct gpt_sh *)arg;
    uint64_t ticks = timer_convert(nsec, gpt->hz, 1000000000);

    if (ticks > gpt->mask) {
	cprintf("gpt_schedule: ticks %"PRIu64" overflow timer\n", ticks);
	ticks = gpt->mask;
    }
    
    gpt->regs->rld = (uint32_t)ticks;
    gpt->regs->ctrl = LEON3_GPTIMER_EN | LEON3_GPTIMER_IRQEN | 
	LEON3_GPTIMER_LD;
}

static void
gpt_intr(void *arg)
{
    LEON3_GpTimerElem_Regs_Map *sh_regs = (LEON3_GpTimerElem_Regs_Map *) arg;
    sh_regs->ctrl &= ~LEON3_GPTIMER_CTRL_PENDING;

    schedule();
}

void
gptimer_init(void)
{
    if (the_timesrc && the_schedtmr)
	return;

    struct amba_apb_device dev;
    uint32_t r = amba_apbslv_device(VENDOR_GAISLER, GAISLER_GPTIMER, &dev, 0);
    if (!r)
	return;

    LEON3_GpTimer_Regs_Map *gpt_regs = pa2kva(dev.start);
    uint32_t irq = GPTIMER_CONFIG_IRQNT(gpt_regs->config);

    gpt_regs->scalar_reload = 0;
    gpt_regs->scalar = 0;

    /* Timer 0 for scheduler */
    LEON3_GpTimerElem_Regs_Map *sh_regs = 
	(LEON3_GpTimerElem_Regs_Map *) &gpt_regs->e[0];

    sh_regs->rld = ~0;

    static struct gpt_sh gpt_sh;
    gpt_sh.hz = CLOCK_FREQ_KHZ * 1000;
    gpt_sh.regs = sh_regs;
    gpt_sh.mask = sh_regs->rld;

    static struct interrupt_handler gpt_ih;
    gpt_ih.ih_func = &gpt_intr;
    gpt_ih.ih_arg = sh_regs;

    static struct preemption_timer gpt_preempt;
    gpt_preempt.schedule_nsec = &gpt_schedule;
    gpt_preempt.arg = &gpt_sh;

    if (!the_schedtmr)
	the_schedtmr = &gpt_preempt;
    irq_register(irq, &gpt_ih);

    /* Timer 1 for time source */
    LEON3_GpTimerElem_Regs_Map *ts_regs = 
	(LEON3_GpTimerElem_Regs_Map *) &(gpt_regs->e[1]);

    ts_regs->rld = ~0;
    ts_regs->ctrl = LEON3_GPTIMER_EN | LEON3_GPTIMER_RL | LEON3_GPTIMER_LD;

    static struct gpt_ts gpt_ts;
    gpt_ts.regs = ts_regs;
    gpt_ts.mask = ts_regs->rld;
    gpt_ts.last_read = gpt_tsval(&gpt_ts);
    gpt_ts.ticks = 0;
    gpt_ts.gpt_src.type = time_source_gpt;
    gpt_ts.gpt_src.freq_hz = CLOCK_FREQ_KHZ * 1000;
    gpt_ts.gpt_src.ticks = &gpt_get_ticks;
    gpt_ts.gpt_src.delay_nsec = &gpt_delay;
    gpt_ts.gpt_src.arg = &gpt_ts;
    if (!the_timesrc)
	the_timesrc = &gpt_ts.gpt_src;

    tag_set(&gpt_ts.ticks, DTAG_KRW, sizeof(gpt_ts.ticks));
    tag_set(&gpt_ts.last_read, DTAG_KRW, sizeof(gpt_ts.last_read));
}
