#include <machine/types.h>
#include <machine/pmap.h>
#include <machine/x86.h>
#include <dev/pci.h>
#include <dev/e1000.h>
#include <dev/e1000reg.h>
#include <kern/segment.h>
#include <kern/lib.h>
#include <kern/kobj.h>
#include <kern/intr.h>
#include <kern/netdev.h>
#include <kern/timer.h>
#include <kern/arch.h>
#include <inc/queue.h>
#include <inc/netdev.h>
#include <inc/error.h>

#define E1000_RX_SLOTS	64
#define E1000_TX_SLOTS	64

struct e1000_buffer_slot {
    struct netbuf_hdr *nb;
    const struct Segment *sg;
};

// Static allocation ensures contiguous memory.
struct e1000_tx_descs {
    struct wiseman_txdesc txd[E1000_TX_SLOTS] __attribute__((aligned (16)));
};

struct e1000_rx_descs {
    struct wiseman_rxdesc rxd[E1000_RX_SLOTS] __attribute__((aligned (16)));
};

struct e1000_card {
    uint32_t membase;
    uint32_t iobase;
    uint8_t irq_line;
    uint16_t pci_dev_id;
    struct interrupt_handler ih;

    struct e1000_tx_descs *txds;
    struct e1000_rx_descs *rxds;

    struct e1000_buffer_slot tx[E1000_TX_SLOTS];
    struct e1000_buffer_slot rx[E1000_RX_SLOTS];

    int rx_head;	// card receiving into rx_head, -1 if none
    int rx_nextq;	// next slot for rx buffer

    int tx_head;	// card transmitting from tx_head, -1 if none
    int tx_nextq;	// next slot for tx buffer

    struct net_device netdev;
};

static uint32_t
e1000_io_read(struct e1000_card *c, uint32_t reg)
{
    physaddr_t pa = c->membase + reg;
    volatile uint32_t *ptr = pa2kva(pa);
    return *ptr;
}

static void
e1000_io_write(struct e1000_card *c, uint32_t reg, uint32_t val)
{
    physaddr_t pa = c->membase + reg;
    volatile uint32_t *ptr = pa2kva(pa);
    *ptr = val;
}

static void
e1000_io_write_flush(struct e1000_card *c, uint32_t reg, uint32_t val)
{
    e1000_io_write(c, reg, val);
    e1000_io_read(c, WMREG_STATUS);
}

static void
e1000_eeprom_uwire_out(struct e1000_card *c, uint16_t data, uint16_t count)
{
    uint32_t mask = 1 << (count - 1);
    uint32_t eecd = e1000_io_read(c, WMREG_EECD) & ~(EECD_DO | EECD_SK);

    do {
	if (data & mask)
	    eecd |= EECD_DI;
	else
	    eecd &= ~(EECD_DI);

	e1000_io_write_flush(c, WMREG_EECD, eecd);
	timer_delay(50000);

	e1000_io_write_flush(c, WMREG_EECD, eecd | EECD_SK);
	timer_delay(50000);

	e1000_io_write_flush(c, WMREG_EECD, eecd);
	timer_delay(50000);

	mask = mask >> 1;
    } while (mask);

    e1000_io_write_flush(c, WMREG_EECD, eecd & ~(EECD_DI));
}

static uint16_t
e1000_eeprom_uwire_in(struct e1000_card *c, uint16_t count)
{
    uint32_t data = 0;
    uint32_t eecd = e1000_io_read(c, WMREG_EECD) & ~(EECD_DO | EECD_DI);

    for (uint16_t i = 0; i < count; i++) {
	data = data << 1;

	e1000_io_write_flush(c, WMREG_EECD, eecd | EECD_SK);
	timer_delay(50000);

	eecd = e1000_io_read(c, WMREG_EECD) & ~(EECD_DI);
	if (eecd & EECD_DO)
	    data |= 1;

	e1000_io_write_flush(c, WMREG_EECD, eecd & ~EECD_SK);
	timer_delay(50000);
    }

    return data;
}

static int32_t
e1000_eeprom_uwire_read(struct e1000_card *c, uint16_t off)
{
    /* Make sure this is microwire */
    uint32_t eecd = e1000_io_read(c, WMREG_EECD);
    if (eecd & EECD_EE_TYPE) {
	cprintf("e1000_eeprom_read: EERD timeout, SPI not supported\n");
	return -1;
    }

    uint32_t abits = (eecd & EECD_EE_SIZE) ? 8 : 6;

    /* Get access to the EEPROM */
    eecd |= EECD_EE_REQ;
    e1000_io_write_flush(c, WMREG_EECD, eecd);
    for (uint32_t t = 0; t < 100; t++) {
	timer_delay(50000);
	eecd = e1000_io_read(c, WMREG_EECD);
	if (eecd & EECD_EE_GNT)
	    break;
    }

    if (!(eecd & EECD_EE_GNT)) {
	cprintf("e1000_eeprom_read: cannot get EEPROM access\n");
	e1000_io_write_flush(c, WMREG_EECD, eecd & ~EECD_EE_REQ);
	return -1;
    }

    /* Turn on the EEPROM */
    eecd &= ~(EECD_DI | EECD_SK);
    e1000_io_write_flush(c, WMREG_EECD, eecd);

    eecd |= EECD_CS;
    e1000_io_write_flush(c, WMREG_EECD, eecd);

    /* Read the bits */
    e1000_eeprom_uwire_out(c, UWIRE_OPC_READ, 3);
    e1000_eeprom_uwire_out(c, off, abits);
    uint16_t v = e1000_eeprom_uwire_in(c, 16);

    /* Turn off the EEPROM */
    eecd &= ~(EECD_CS | EECD_DI | EECD_SK);
    e1000_io_write_flush(c, WMREG_EECD, eecd);

    e1000_io_write_flush(c, WMREG_EECD, eecd | EECD_SK);
    timer_delay(50000);

    e1000_io_write_flush(c, WMREG_EECD, eecd & ~EECD_EE_REQ);
    timer_delay(50000);

    return v;
}

static int32_t
e1000_eeprom_eerd_read(struct e1000_card *c, uint16_t off)
{
    e1000_io_write(c, WMREG_EERD, (off << EERD_ADDR_SHIFT) | EERD_START);

    uint32_t reg;
    for (int x = 0; x < 100; x++) {
	reg = e1000_io_read(c, WMREG_EERD);
	if (!(reg & EERD_DONE))
	    timer_delay(5000);
    }

    if (reg & EERD_DONE)
	return (reg & EERD_DATA_MASK) >> EERD_DATA_SHIFT;
    return -1;
}

static int
e1000_eeprom_read(struct e1000_card *c, uint16_t *buf, int off, int count)
{
    for (int i = 0; i < count; i++) {
	int32_t r = e1000_eeprom_eerd_read(c, off + i);
	if (r < 0)
	    r = e1000_eeprom_uwire_read(c, off + i);

	if (r < 0) {
	    cprintf("e1000_eeprom_read: cannot read\n");
	    return -1;
	}

	buf[i] = r;
    }

    return 0;
}

static void __attribute__((unused))
e1000_dump_stats(struct e1000_card *c)
{
    for (uint32_t i = 0; i <= 0x124; i += 4) {
	uint32_t v = e1000_io_read(c, 0x4000 + i);
	if (v != 0)
	    cprintf("%x:%x ", i, v);
    }
    cprintf("\n");
}

static void
e1000_reset(struct e1000_card *c)
{
    e1000_io_write(c, WMREG_RCTL, 0);
    e1000_io_write(c, WMREG_TCTL, 0);

    // Allocate the card's packet buffer memory equally between rx, tx
    uint32_t pba = e1000_io_read(c, WMREG_PBA);
    uint32_t rxtx = ((pba >> PBA_RX_SHIFT) & PBA_RX_MASK) +
		    ((pba >> PBA_TX_SHIFT) & PBA_TX_MASK);
    e1000_io_write(c, WMREG_PBA, rxtx / 2);

    // Reset PHY, card
    uint32_t ctrl = e1000_io_read(c, WMREG_CTRL);
    e1000_io_write(c, WMREG_CTRL, ctrl | CTRL_PHY_RESET);
    timer_delay(5 * 1000 * 1000);

    e1000_io_write(c, WMREG_CTRL, ctrl | CTRL_RST);
    timer_delay(10 * 1000 * 1000);

    for (int i = 0; i < 1000; i++) {
	if ((e1000_io_read(c, WMREG_CTRL) & CTRL_RST) == 0)
	    break;
	timer_delay(20000);
    }

    if (e1000_io_read(c, WMREG_CTRL) & CTRL_RST)
	cprintf("e1000_reset: card still resetting, odd..\n");

    e1000_io_write(c, WMREG_CTRL, ctrl | CTRL_SLU | CTRL_ASDE);

    // Make sure the management hardware is not hiding any packets
    if (c->pci_dev_id == 0x108c || c->pci_dev_id == 0x109a) {
	uint32_t manc = e1000_io_read(c, WMREG_MANC);
	manc &= ~MANC_ARP_REQ;
	manc |= MANC_MNG2HOST;

	e1000_io_write(c, WMREG_MANC2H, MANC2H_PORT_623 | MANC2H_PORT_664);
	e1000_io_write(c, WMREG_MANC, manc);
    }

    // Setup RX, TX rings
    uint64_t rptr = kva2pa(&c->rxds->rxd[0]);
    e1000_io_write(c, WMREG_RDBAH, rptr >> 32);
    e1000_io_write(c, WMREG_RDBAL, rptr & 0xffffffff);
    e1000_io_write(c, WMREG_RDLEN, sizeof(c->rxds->rxd));
    e1000_io_write(c, WMREG_RDH, 0);
    e1000_io_write(c, WMREG_RDT, 0);
    e1000_io_write(c, WMREG_RDTR, 0);
    e1000_io_write(c, WMREG_RADV, 0);

    uint64_t tptr = kva2pa(&c->txds->txd[0]);
    e1000_io_write(c, WMREG_TDBAH, tptr >> 32);
    e1000_io_write(c, WMREG_TDBAL, tptr & 0xffffffff);
    e1000_io_write(c, WMREG_TDLEN, sizeof(c->txds->txd));
    e1000_io_write(c, WMREG_TDH, 0);
    e1000_io_write(c, WMREG_TDT, 0);
    e1000_io_write(c, WMREG_TIDV, 1);
    e1000_io_write(c, WMREG_TADV, 1);

    // Disable VLAN
    e1000_io_write(c, WMREG_VET, 0);

    // Flow control junk?
    e1000_io_write(c, WMREG_FCAL, FCAL_CONST);
    e1000_io_write(c, WMREG_FCAH, FCAH_CONST);
    e1000_io_write(c, WMREG_FCT, 0x8808);
    e1000_io_write(c, WMREG_FCRTH, FCRTH_DFLT);
    e1000_io_write(c, WMREG_FCRTL, FCRTL_DFLT);
    e1000_io_write(c, WMREG_FCTTV, FCTTV_DFLT);

    // Interrupts
    e1000_io_write(c, WMREG_IMC, ~0);
    e1000_io_write(c, WMREG_IMS, ICR_TXDW | ICR_RXO | ICR_RXT0);

    // MAC address filters
    e1000_io_write(c, WMREG_CORDOVA_RAL_BASE + 0,
		   (c->netdev.mac_addr[0]) |
		   (c->netdev.mac_addr[1] << 8) |
		   (c->netdev.mac_addr[2] << 16) |
		   (c->netdev.mac_addr[3] << 24));
    e1000_io_write(c, WMREG_CORDOVA_RAL_BASE + 4,
		   (c->netdev.mac_addr[4]) |
		   (c->netdev.mac_addr[5] << 8) | RAL_AV);

    for (int i = 2; i < WM_RAL_TABSIZE * 2; i++)
	e1000_io_write(c, WMREG_CORDOVA_RAL_BASE + i * 4, 0);
    for (int i = 0; i < WM_MC_TABSIZE; i++)
	e1000_io_write(c, WMREG_CORDOVA_MTA + i * 4, 0);

    // Enable RX, TX
    e1000_io_write(c, WMREG_RCTL,
		   RCTL_EN | RCTL_RDMTS_1_2 | RCTL_DPF | RCTL_BAM);
    e1000_io_write(c, WMREG_TCTL,
		   TCTL_EN | TCTL_PSP | TCTL_CT(TX_COLLISION_THRESHOLD) |
		   TCTL_COLD(TX_COLLISION_DISTANCE_FDX));

    for (int i = 0; i < E1000_TX_SLOTS; i++) {
	if (c->tx[i].sg) {
	    kobject_unpin_page(&c->tx[i].sg->sg_ko);
	    pagetree_decpin_write(c->tx[i].nb);
	    kobject_dirty(&c->tx[i].sg->sg_ko);
	}
	c->tx[i].sg = 0;
    }

    for (int i = 0; i < E1000_RX_SLOTS; i++) {
	if (c->rx[i].sg) {
	    kobject_unpin_page(&c->rx[i].sg->sg_ko);
	    pagetree_decpin_write(c->rx[i].nb);
	    kobject_dirty(&c->rx[i].sg->sg_ko);
	}
	c->rx[i].sg = 0;
    }

    c->rx_head = -1;
    c->rx_nextq = 0;

    c->tx_head = -1;
    c->tx_nextq = 0;

    c->netdev.waiter_id = 0;
    netdev_thread_wakeup(&c->netdev);
}

static void
e1000_buffer_reset(void *a)
{
    e1000_reset(a);
}

static void
e1000_intr_rx(struct e1000_card *c)
{
    for (;;) {
	int i = c->rx_head;
	if (i == -1 || !(c->rxds->rxd[i].wrx_status & WRX_ST_DD))
	    break;

	kobject_unpin_page(&c->rx[i].sg->sg_ko);
	pagetree_decpin_write(c->rx[i].nb);
	kobject_dirty(&c->rx[i].sg->sg_ko);
	c->rx[i].sg = 0;
	c->rx[i].nb->actual_count = c->rxds->rxd[i].wrx_len;
	c->rx[i].nb->actual_count |= NETHDR_COUNT_DONE;
	if (c->rxds->rxd[i].wrx_errors)
	    c->rx[i].nb->actual_count |= NETHDR_COUNT_ERR;

	c->rx_head = (i + 1) % E1000_RX_SLOTS;
	if (c->rx_head == c->rx_nextq)
	    c->rx_head = -1;
    }
}

static void
e1000_intr_tx(struct e1000_card *c)
{
    for (;;) {
	int i = c->tx_head;
	if (i == -1 || !(c->txds->txd[i].wtx_fields.wtxu_status & WTX_ST_DD))
	    break;

	kobject_unpin_page(&c->tx[i].sg->sg_ko);
	pagetree_decpin_write(c->tx[i].nb);
	kobject_dirty(&c->tx[i].sg->sg_ko);
	c->tx[i].sg = 0;
	c->tx[i].nb->actual_count |= NETHDR_COUNT_DONE;

	c->tx_head = (i + 1) % E1000_TX_SLOTS;
	if (c->tx_head == c->tx_nextq)
	    c->tx_head = -1;
    }
}

static void
e1000_intr(void *arg)
{
    struct e1000_card *c = arg;
    uint32_t icr = e1000_io_read(c, WMREG_ICR);

    if (icr & ICR_TXDW)
	e1000_intr_tx(c);

    if (icr & ICR_RXT0)
	e1000_intr_rx(c);

    if (icr & ICR_RXO) {
	cprintf("e1000_intr: receiver overrun\n");
	e1000_reset(c);
    }

    netdev_thread_wakeup(&c->netdev);
}

static int
e1000_add_txbuf(void *arg, const struct Segment *sg,
		struct netbuf_hdr *nb, uint16_t size)
{
    struct e1000_card *c = arg;
    int slot = c->tx_nextq;
    int next_slot = (slot + 1) % E1000_TX_SLOTS;

    if (slot == c->tx_head || next_slot == c->tx_head)
	return -E_NO_SPACE;

    if (size > 1522) {
	cprintf("e1000_add_txbuf: oversize buffer, %d bytes\n", size);
	return -E_INVAL;
    }

    c->tx[slot].nb = nb;
    c->tx[slot].sg = sg;
    kobject_pin_page(&sg->sg_ko);
    pagetree_incpin_write(nb);

    c->txds->txd[slot].wtx_addr = kva2pa(c->tx[slot].nb + 1);
    c->txds->txd[slot].wtx_cmdlen = size | WTX_CMD_RS | WTX_CMD_EOP | WTX_CMD_IFCS;
    memset(&c->txds->txd[slot].wtx_fields, 0, sizeof(&c->txds->txd[slot].wtx_fields));

    c->tx_nextq = next_slot;
    if (c->tx_head == -1)
	c->tx_head = slot;

    e1000_io_write(c, WMREG_TDT, next_slot);
    return 0;
}

static int
e1000_add_rxbuf(void *arg, const struct Segment *sg,
	        struct netbuf_hdr *nb, uint16_t size)
{
    struct e1000_card *c = arg;
    int slot = c->rx_nextq;
    int next_slot = (slot + 1) % E1000_RX_SLOTS;

    if (slot == c->rx_head || next_slot == c->rx_head)
	return -E_NO_SPACE;

    // The receive buffer size is hard-coded in the RCTL register as 2K.
    // However, we configure it to reject packets over 1522 bytes long.
    if (size < 1522) {
	cprintf("e1000_add_rxbuf: buffer too small, %d bytes\n", size);
	return -E_INVAL;
    }

    c->rx[slot].nb = nb;
    c->rx[slot].sg = sg;
    kobject_pin_page(&sg->sg_ko);
    pagetree_incpin_write(nb);

    memset(&c->rxds->rxd[slot], 0, sizeof(c->rxds->rxd[slot]));
    c->rxds->rxd[slot].wrx_addr = kva2pa(c->rx[slot].nb + 1);

    c->rx_nextq = next_slot;
    if (c->rx_head == -1)
	c->rx_head = slot;

    e1000_io_write(c, WMREG_RDT, next_slot);
    return 0;
}

int
e1000_attach(struct pci_func *pcif)
{
    struct e1000_card *c;
    int r = page_alloc((void **) &c);
    if (r < 0)
	return r;

    memset(c, 0, sizeof(*c));
    static_assert(PGSIZE >= sizeof(*c));
    static_assert(PGSIZE >= sizeof(*c->txds));
    static_assert(PGSIZE >= sizeof(*c->rxds));

    r = page_alloc((void **) &c->txds);
    if (r < 0) {
	page_free(c);
	return r;
    }

    r = page_alloc((void **) &c->rxds);
    if (r < 0) {
	page_free(c->txds);
	page_free(c);
	return r;
    }

    pci_func_enable(pcif);
    c->irq_line = pcif->irq_line;
    c->membase = pcif->reg_base[0];
    c->iobase = pcif->reg_base[2];
    c->pci_dev_id = pcif->dev_id;

    // Get the MAC address
    uint16_t myaddr[3];
    r = e1000_eeprom_read(c, &myaddr[0], EEPROM_OFF_MACADDR, 3);
    if (r < 0) {
	cprintf("e1000_attach: cannot read EEPROM MAC addr: %s\n", e2s(r));
	return 0;
    }

    for (int i = 0; i < 3; i++) {
	c->netdev.mac_addr[2*i + 0] = myaddr[i] & 0xff;
	c->netdev.mac_addr[2*i + 1] = myaddr[i] >> 8;
    }

    e1000_reset(c);

    // Register card with kernel
    c->ih.ih_func = &e1000_intr;
    c->ih.ih_arg = c;
    irq_register(c->irq_line, &c->ih);

    c->netdev.arg = c;
    c->netdev.add_buf_tx = &e1000_add_txbuf;
    c->netdev.add_buf_rx = &e1000_add_rxbuf;
    c->netdev.buffer_reset = &e1000_buffer_reset;
    netdev_register(&c->netdev);

    // All done
    cprintf("e1000: irq %d io 0x%x mac %02x:%02x:%02x:%02x:%02x:%02x\n",
	    c->irq_line, c->iobase,
	    c->netdev.mac_addr[0], c->netdev.mac_addr[1],
	    c->netdev.mac_addr[2], c->netdev.mac_addr[3],
	    c->netdev.mac_addr[4], c->netdev.mac_addr[5]);
    return 1;
}
