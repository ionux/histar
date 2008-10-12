#include <kern/lib.h>
#include <kern/console.h>
#include <kern/intr.h>
#include <kern/arch.h>
#include <kern/timer.h>

#include <machine/leon3.h>
#include <machine/leon.h>
#include <machine/sparc-config.h>

#include <dev/amba.h>
#include <dev/ambapp.h>
#include <dev/apbucons.h>

enum { baud_rate = 38400 };

static void
serial_putc(void *arg, int c, cons_source src)
{
    if (c == '\n')
	serial_putc(arg, '\r', src);

    LEON3_APBUART_Regs_Map *uart_regs = (LEON3_APBUART_Regs_Map *) arg;
    
    uint32_t i = 0;
    while (!(uart_regs->status & LEON_REG_UART_STATUS_THE) && (i < 256)) {
	i++;
	timer_delay(2000);
    }

    uart_regs->data = c;

    i = 0;
    while (!(uart_regs->status & LEON_REG_UART_STATUS_TSE) && (i < 256)) {
	i++;
	timer_delay(2000);
    }
}

static int
serial_proc_data(void *arg)
{
    LEON3_APBUART_Regs_Map *uart_regs = (LEON3_APBUART_Regs_Map *)arg;    
    if (uart_regs->status & LEON_REG_UART_STATUS_DR)
	return uart_regs->data & 0xFF; 
    return -1;
}

static void
serial_intr(void *arg)
{
    LEON3_APBUART_Regs_Map *uart_regs = (LEON3_APBUART_Regs_Map *)arg;
    cons_intr(serial_proc_data, uart_regs);
}

void
apbucons_init(void)
{    
    struct amba_apb_device dev;
    uint32_t r = amba_apbslv_device(VENDOR_GAISLER, GAISLER_APBUART, &dev, 0);
    if (!r)
	return;
    
    /* Only register serial port A */
    if (dev.irq != LEON_INTERRUPT_UART_1_RX_TX) {
	r = amba_apbslv_device(VENDOR_GAISLER, GAISLER_APBUART, &dev, 1);
	if (r && dev.irq != LEON_INTERRUPT_UART_1_RX_TX)
	    return;
    }

    LEON3_APBUART_Regs_Map *uart_regs = pa2kva(dev.start);
    uart_regs->scaler = (CLOCK_FREQ_KHZ * 1000) / (baud_rate * 8);

    /* enable rx, tx, and rx interrupts */
    uart_regs->ctrl = LEON_REG_UART_CTRL_RE | LEON_REG_UART_CTRL_TE |
		      LEON_REG_UART_CTRL_RI | LEON_REG_UART_CTRL_FL;
    
    static struct interrupt_handler ih = { .ih_func = &serial_intr };
    ih.ih_arg = uart_regs;
    ih.ih_irq = dev.irq;
    irq_register(&ih);

    static struct cons_device cd = {
	.cd_pollin = &serial_proc_data,
	.cd_output = &serial_putc,
    };

    cd.cd_arg = uart_regs;
    cons_register(&cd);
}
