#include <machine/pmap.h>
#include <dev/hpet.h>
#include <dev/hpetreg.h>

struct hpet_state {
    struct hpet_reg *reg;
    uint32_t freq_hz;
    uint16_t min_tick;
};

static struct hpet_state the_hpet;

static void
hpet_schedule(struct hpet_state *hpet, uint32_t timer, uint64_t usec)
{
    uint64_t ticks = usec * (hpet->freq_hz / 1000000);
    if (ticks < hpet->min_tick)
	ticks = hpet->min_tick;

    uint64_t now = hpet->reg->counter;
    uint64_t then = now + ticks;
    hpet->reg->timer[timer].compare = then;

    now = hpet->reg->counter;
    int64_t diff = then - now;
    if (diff < 0) {
	hpet->reg->timer[timer].compare = now + hpet->freq_hz;
	cprintf("hpet_schedule: lost a tick, recovering in a second\n");
    }
}

void
hpet_attach(struct acpi_table_hdr *th)
{
    struct hpet_state *hpet = &the_hpet;
    struct hpet_acpi *t = (struct hpet_acpi *) th;
    hpet->reg = pa2kva(t->base.addr);

    uint64_t cap = hpet->reg->cap;
    if (!(cap & HPET_CAP_LEGACY) || !(cap & HPET_CAP_64BIT)) {
	cprintf("HPET: incompetent chip: %"PRIx64"\n", cap);
	return;
    }

    uint64_t period = cap >> 32;
    hpet->freq_hz = UINT64(1000000000000000) / period;
    hpet->min_tick = t->min_tick;
    cprintf("HPET: %d Hz, min tick %d\n", hpet->freq_hz, hpet->min_tick);

    hpet->reg->config = HPET_CONFIG_ENABLE | HPET_CONFIG_LEGACY;
    hpet->reg->timer[0].config = HPET_TIMER_ENABLE;


    cprintf("HPET: currently at %"PRIu64"\n", hpet->reg->counter);
    hpet_schedule(hpet, 0, 1000);
}
