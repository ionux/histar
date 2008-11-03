#include <machine/x86.h>
#include <machine/io.h>
#include <dev/pci.h>
#include <dev/pcireg.h>
#include <dev/ide.h>
#include <dev/ne2kpci.h>
#include <dev/fxp.h>
#include <dev/pnic.h>
#include <dev/e1000.h>
#include <dev/ahci.h>
#include <kern/lib.h>
#include <kern/udev.h>
#include <inc/error.h>
#include <inc/device.h>
#include <inc/udev.h>
#include <inc/bitops.h>

// Flag to do "lspci" at bootup
static int pci_show_devs = 0;
static int pci_show_addrs = 0;

// PCI "configuration mechanism one"
static uint32_t pci_conf1_addr_ioport = 0x0cf8;
static uint32_t pci_conf1_data_ioport = 0x0cfc;

// Forward declarations
static int pci_bridge_attach(struct pci_func *pcif);

// PCI driver table
struct pci_driver {
    uint32_t key1, key2;
    int (*attachfn) (struct pci_func *pcif);
};

// PCI udevs
enum { pciudevs_max = 32 };
struct pci_udevice {
    struct pci_func func;
    struct udevice  udev;
    uint8_t iomap[PGSIZE * 2]; // enough for 64K IO-space
};
static struct pci_udevice pciudev[pciudevs_max];
static uint64_t pciudev_num;

struct pci_driver pci_attach_class[] = {
    { PCI_CLASS_BRIDGE, PCI_SUBCLASS_BRIDGE_PCI, &pci_bridge_attach },
    { PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_MASS_STORAGE_IDE, &ide_init },
    { PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_MASS_STORAGE_SATA, &ahci_init },
    { 0, 0, 0 },
};

struct pci_driver pci_attach_vendor[] = {
    //    { 0x10ec, 0x8029, &ne2kpci_attach },
    { 0x8086, 0x1229, &fxp_attach },
    { 0xfefe, 0xefef, &pnic_attach },
    { 0x8086, 0x100e, &e1000_attach },
    { 0x8086, 0x100f, &e1000_attach },
    { 0x8086, 0x107c, &e1000_attach },
    { 0x8086, 0x108c, &e1000_attach },
    { 0x8086, 0x109a, &e1000_attach },
    { 0x8086, 0x1079, &e1000_attach },
    { 0, 0, 0 },
};

// PCI udev fuctions
static uint8_t 
pci_class_to_device(uint8_t class)
{
    switch (class) {
    case PCI_CLASS_NETWORK:
	return device_net;
    default:
	return 0xFF;
    }
}

static int
pci_get_base(void *a, uint64_t base, uint64_t *val)
{
    if (base >= 6)
	return -E_INVAL;

    struct pci_udevice *pciud = a;
    *val = pciud->func.reg_base[base];
    return 0;
}

static int
pci_udev_attach(struct pci_func *f)
{
    if (pciudev_num == pciudevs_max) {
	cprintf("pci_attach_match: no enough pciudevs\n");
	return 0;
    }
    
    pci_func_enable(f);

    struct pci_udevice *d = &pciudev[pciudev_num++];    
    
    for (uint32_t i = 0; i < 6; i++)
	if (f->reg_type[i] == pci_res_io)
	    for (uint32_t k = 0; k < f->reg_size[i]; k++)
		bit_set(d->iomap, k + f->reg_base[i]);

    memcpy(&d->func, f, sizeof(d->func));
    d->udev.arg = d;
    d->udev.key = MK_PCIKEY(pci_class_to_device(PCI_CLASS(f->dev_class)),
			    PCI_VENDOR(f->dev_id), 
			    (uint64_t)PCI_PRODUCT(f->dev_id));
    d->udev.get_base = pci_get_base;
    d->udev.iomap = d->iomap;
    d->udev.iomax = 8 * PGSIZE * 2;
    
    d->udev.ih.ih_tbdp = f->tbdp;
    d->udev.ih.ih_irq = f->irq_line;
    /* udev_register fills out the rest of ih */

    udev_register(&d->udev);
    return 1;
}

static void
pci_conf1_set_addr(uint32_t bus,
		   uint32_t dev,
		   uint32_t func,
		   uint32_t offset)
{
    assert(bus < 256);
    assert(dev < 32);
    assert(func < 8);
    assert(offset < 256);
    assert((offset & 0x3) == 0);

    uint32_t v = (1 << 31) |		// config-space
		 (bus << 16) | (dev << 11) | (func << 8) | (offset);
    outl(pci_conf1_addr_ioport, v);
}

static uint32_t
pci_conf_read(struct pci_func *f, uint32_t off)
{
    pci_conf1_set_addr(f->bus->busno, f->dev, f->func, off);
    return inl(pci_conf1_data_ioport);
}

static void
pci_conf_write(struct pci_func *f, uint32_t off, uint32_t v)
{
    pci_conf1_set_addr(f->bus->busno, f->dev, f->func, off);
    outl(pci_conf1_data_ioport, v);
}

static int __attribute__((warn_unused_result))
pci_attach_match(uint32_t key1, uint32_t key2,
		 struct pci_driver *list, struct pci_func *pcif)
{
    for (uint32_t i = 0; list[i].attachfn; i++) {
	if (list[i].key1 == key1 && list[i].key2 == key2) {
	    int r = list[i].attachfn(pcif);
	    if (r > 0)
		return r;
	    if (r < 0)
		cprintf("pci_attach_match: attaching %x.%x (%p): %s\n",
			key1, key2, list[i].attachfn, e2s(r));
	}
    }

    return 0;
}

static int
pci_attach(struct pci_func *f)
{
    int r =
	pci_attach_match(PCI_CLASS(f->dev_class), PCI_SUBCLASS(f->dev_class),
			 &pci_attach_class[0], f) ||
	pci_attach_match(PCI_VENDOR(f->dev_id), PCI_PRODUCT(f->dev_id),
			 &pci_attach_vendor[0], f);
    
    if (r)
	return r;

    return pci_udev_attach(f);
}

static void
pci_scan_bus(struct pci_bus *bus)
{
    struct pci_func df;
    memset(&df, 0, sizeof(df));
    df.bus = bus;

    for (df.dev = 0; df.dev < 32; df.dev++) {
	uint32_t bhlc = pci_conf_read(&df, PCI_BHLC_REG);
	if (PCI_HDRTYPE_TYPE(bhlc) > 1)	    // Unsupported or no device
	    continue;

	struct pci_func f = df;
	for (f.func = 0; f.func < (PCI_HDRTYPE_MULTIFN(bhlc) ? 8 : 1);
			 f.func++) {
	    struct pci_func af = f;

	    af.dev_id = pci_conf_read(&f, PCI_ID_REG);
	    if (PCI_VENDOR(af.dev_id) == 0xffff)
		continue;

	    uint32_t intr = pci_conf_read(&af, PCI_INTERRUPT_REG);
	    af.irq_line = PCI_INTERRUPT_LINE(intr);
	    af.tbdp = MKBUS(BUS_PCI, bus->busno, df.dev, PCI_INTERRUPT_PIN(intr));
	    af.dev_class = pci_conf_read(&af, PCI_CLASS_REG);
	    if (pci_show_devs)
		cprintf("PCI: %02x:%02x.%d: %04x:%04x: class %x.%x irq %d\n",
			af.bus->busno, af.dev, af.func,
			PCI_VENDOR(af.dev_id), PCI_PRODUCT(af.dev_id),
			PCI_CLASS(af.dev_class), PCI_SUBCLASS(af.dev_class),
			af.irq_line);

	    pci_attach(&af);
	}
    }
}

static int
pci_bridge_attach(struct pci_func *pcif)
{
    uint32_t ioreg  = pci_conf_read(pcif, PCI_BRIDGE_STATIO_REG);
    uint32_t busreg = pci_conf_read(pcif, PCI_BRIDGE_BUS_REG);

    if (PCI_BRIDGE_IO_32BITS(ioreg)) {
	cprintf("PCI: %02x:%02x.%d: 32-bit bridge IO not supported.\n",
		pcif->bus->busno, pcif->dev, pcif->func);
	return 0;
    }

    struct pci_bus nbus;
    memset(&nbus, 0, sizeof(nbus));
    nbus.parent_bridge = pcif;
    nbus.busno = (busreg >> PCI_BRIDGE_BUS_SECONDARY_SHIFT) & 0xff;

    if (pci_show_devs)
	cprintf("PCI: %02x:%02x.%d: bridge to PCI bus %d--%d\n",
		pcif->bus->busno, pcif->dev, pcif->func,
		nbus.busno,
		(busreg >> PCI_BRIDGE_BUS_SUBORDINATE_SHIFT) & 0xff);

    pci_scan_bus(&nbus);
    return 1;
}

// External PCI subsystem interface

void
pci_func_enable(struct pci_func *f)
{
    pci_conf_write(f, PCI_COMMAND_STATUS_REG,
		   PCI_COMMAND_IO_ENABLE |
		   PCI_COMMAND_MEM_ENABLE |
		   PCI_COMMAND_MASTER_ENABLE);

    uint32_t bar_width;
    for (uint32_t bar = PCI_MAPREG_START; bar < PCI_MAPREG_END;
	 bar += bar_width)
    {
	uint32_t oldv = pci_conf_read(f, bar);

	bar_width = 4;
	pci_conf_write(f, bar, 0xffffffff);
	uint32_t rv = pci_conf_read(f, bar);

	if (rv == 0)
	    continue;

	int regnum = PCI_MAPREG_NUM(bar);
	uint32_t base, size, type;
	if (PCI_MAPREG_TYPE(rv) == PCI_MAPREG_TYPE_MEM) {
	    if (PCI_MAPREG_MEM_TYPE(rv) == PCI_MAPREG_MEM_TYPE_64BIT)
		bar_width = 8;

	    size = PCI_MAPREG_MEM_SIZE(rv);
	    base = PCI_MAPREG_MEM_ADDR(oldv);
	    type = pci_res_mem;
	    if (pci_show_addrs)
		cprintf("  mem region %d: %d bytes at 0x%x\n",
			regnum, size, base);
	} else {
	    size = PCI_MAPREG_IO_SIZE(rv);
	    base = PCI_MAPREG_IO_ADDR(oldv);
	    type = pci_res_io;
	    if (pci_show_addrs)
		cprintf("  io region %d: %d bytes at 0x%x\n",
			regnum, size, base);
	}

	pci_conf_write(f, bar, oldv);
	f->reg_base[regnum] = base;
	f->reg_size[regnum] = size;
	f->reg_type[regnum] = type;

	if (size && !base)
	    cprintf("PCI device %02x:%02x.%d (%04x:%04x) may be misconfigured: "
		    "region %d: base 0x%x, size %d\n",
		    f->bus->busno, f->dev, f->func,
		    PCI_VENDOR(f->dev_id), PCI_PRODUCT(f->dev_id),
		    regnum, base, size);
    }
}

void
pci_init(void)
{
    static struct pci_bus root_bus;
    memset(&root_bus, 0, sizeof(root_bus));

    pci_scan_bus(&root_bus);
}
