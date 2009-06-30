#include <kern/intr.h>
#include <kern/lib.h>
#include <kern/arch.h>
#include <kern/thread.h>
#include <inc/queue.h>
#include <inc/intmacro.h>
#include <inc/error.h>

LIST_HEAD(ih_list, interrupt_handler);
struct ih_list irq_handlers[MAX_IRQS];
static uint64_t irq_warnings[MAX_IRQS];
static int64_t irq_count[MAX_IRQS];
static struct Thread_list irq_waiting[MAX_IRQS];

void
irq_handler(uint32_t irqno)
{
    if (irqno >= MAX_IRQS)
	panic("irq_handler: invalid IRQ %d", irqno);

    irq_count[irqno]++;

    if (LIST_FIRST(&irq_handlers[irqno]) == 0) {
	irq_warnings[irqno]++;
	if (IS_POWER_OF_2(irq_warnings[irqno]))
	    cprintf("IRQ %d not handled (%"PRIu64")\n",
		    irqno, irq_warnings[irqno]);
    }

    struct interrupt_handler *ih;
    LIST_FOREACH(ih, &irq_handlers[irqno], ih_link)
	ih->ih_func(ih->ih_arg);

    while (!LIST_EMPTY(&irq_waiting[irqno])) {
        struct Thread *t = LIST_FIRST(&irq_waiting[irqno]);
        thread_set_runnable(t);
    }
}

void
irq_register(uint32_t irq, struct interrupt_handler *ih)
{
    if (irq >= MAX_IRQS)
	panic("irq_register: invalid IRQ %d", irq);

    LIST_INSERT_HEAD(&irq_handlers[irq], ih, ih_link);
    irq_arch_enable(irq);
}

int64_t
irq_wait_thread(uint32_t irqno, int64_t lastcount)
{
    if (irqno >= MAX_IRQS)
        return -E_INVAL;

    if (irq_count[irqno] <= lastcount)
        thread_suspend(cur_thread, &irq_waiting[irqno]);

    return irq_count[irqno];
}
