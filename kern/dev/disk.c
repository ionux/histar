#include <dev/disk.h>

// PIO-based driver, copied from jos/fs/ide.c

#include <machine/x86.h>
#include <kern/lib.h>
#include <dev/ide.h>
#include <inc/error.h>
#include <dev/picirq.h>
#include <machine/pmap.h>

struct ide_op {
    disk_op op;
    void *buf;
    uint32_t num_bytes;
    uint64_t byte_offset;
    disk_callback cb;
    void *cbarg;

    // Align to 256 bytes to avoid spanning a 64K boundary.
    // 17 slots is enough for up to 64K data (16 pages), the max for IDE.
    struct ide_prd bm_prd[17] __attribute__((aligned (256)));
};

struct ide_channel {
    uint32_t cmd_addr;
    uint32_t ctl_addr;
    uint32_t bm_addr;
    uint32_t irq;

    // 1-deep command queue
    struct ide_op current_op;
};

static int
ide_wait_ready(struct ide_channel *idec)
{
    uint8_t r;

    uint64_t ts_start = read_tsc();
    for (;;) {
	r = inb(idec->ctl_addr);
	if ((r & (IDE_STAT_BSY | IDE_STAT_DRDY)) == IDE_STAT_DRDY)
	    break;

	uint64_t ts_diff = read_tsc() - ts_start;
	if (ts_diff > 1024 * 1024 * 1024) {
	    cprintf("ide_wait_ready: stuck for %ld cycles, status %02x\n", ts_diff, r);
	    return -E_BUSY;
	}
    }

    if ((r & (IDE_STAT_DF | IDE_STAT_ERR))) {
	cprintf("IDE error: %02x\n", r);
	return -1;
    }

    return 0;
}

static void
ide_select_sectors(struct ide_channel *idec, uint32_t diskno,
		   uint32_t start_sector, uint32_t num_sectors)
{
    assert(num_sectors <= 256);

    // 28-bit addressing mode
    outb(idec->cmd_addr + IDE_REG_SECTOR_COUNT, num_sectors & 0xff);
    outb(idec->cmd_addr + IDE_REG_LBA_LOW, start_sector & 0xff);
    outb(idec->cmd_addr + IDE_REG_LBA_MID, (start_sector >> 8) & 0xff);
    outb(idec->cmd_addr + IDE_REG_LBA_HI, (start_sector >> 16) & 0xff);
    outb(idec->cmd_addr + IDE_REG_DEVICE, IDE_DEV_LBA |
					  (diskno << 4) |
					  ((start_sector >> 24) & 0x0f));
}

static int
ide_pio_in(struct ide_channel *idec, void *buf, uint32_t num_sectors)
{
    for (; num_sectors > 0; num_sectors--, buf += 512) {
	int r = ide_wait_ready(idec);
	if (r < 0)
	    return r;

	insl(idec->cmd_addr + IDE_REG_DATA, buf, 512 / 4);
    }

    return 0;
}

static int __attribute__((__unused__))
ide_pio_out(struct ide_channel *idec, void *buf, uint32_t num_sectors)
{
    for (; num_sectors > 0; num_sectors--, buf += 512) {
	int r = ide_wait_ready(idec);
	if (r < 0)
	    return r;

	outsl(idec->cmd_addr + IDE_REG_DATA, buf, 512 / 4);
    }

    return 0;
}

static void
ide_complete(struct ide_channel *idec, disk_io_status stat)
{
    idec->current_op.op = op_none;
    idec->current_op.cb(stat,
			idec->current_op.buf,
			idec->current_op.num_bytes,
			idec->current_op.byte_offset,
			idec->current_op.cbarg);
}

static void
ide_prd_fill(struct ide_prd *prdt, void *buf, uint64_t bytes)
{
    int slot = 0;
    while (bytes > 0) {
	int page_off = PGOFF(buf);
	int page_bytes = PGSIZE - page_off;
	if (page_bytes > bytes)
	    page_bytes = bytes;

	prdt[slot].addr = kva2pa(buf);
	prdt[slot].count = page_bytes;

	bytes -= page_bytes;
	buf += page_bytes;

	if (bytes == 0)
	    prdt[slot].count |= IDE_PRD_EOT;

	slot++;
    }
}

static int
ide_send(struct ide_channel *idec, uint32_t diskno)
{
    // IDE DMA can only handle up to 64K
    assert(idec->current_op.num_bytes <= (1 << 16));

    uint32_t num_sectors = idec->current_op.num_bytes / 512;

    int r = ide_wait_ready(idec);
    if (r < 0)
	return r;

    ide_select_sectors(idec, diskno, idec->current_op.byte_offset / 512, num_sectors);

    // Create the physical region descriptor table
    ide_prd_fill(&idec->current_op.bm_prd[0],
		 idec->current_op.buf,
		 idec->current_op.num_bytes);

    // Load table address
    outl(idec->bm_addr + IDE_BM_PRDT_REG, kva2pa(&idec->current_op.bm_prd[0]));

    // Clear DMA interrupt/error flags, enable DMA for disks
    outb(idec->bm_addr + IDE_BM_STAT_REG,
	 IDE_BM_STAT_D0_DMA | IDE_BM_STAT_D1_DMA |
	 IDE_BM_STAT_INTR | IDE_BM_STAT_ERROR);

    // Issue command to disk & DMA controller; clears IDE INTRQ
    disk_op op = idec->current_op.op;
    outb(idec->cmd_addr + IDE_REG_CMD,
	 (op == op_read) ? IDE_CMD_READ_DMA : IDE_CMD_WRITE_DMA);
    outb(idec->bm_addr + IDE_BM_CMD_REG,
	 IDE_BM_CMD_START | ((op == op_read) ? IDE_BM_CMD_WRITE : 0));

    return 0;
}

// One global IDE channel and drive on it, for now
static struct ide_channel the_ide_channel;
static uint32_t the_ide_drive;

void
ide_intr(int polling)
{
    struct ide_channel *idec = &the_ide_channel;
    disk_io_status iostat = disk_io_success;

    // Ack IRQ by reading the status register
    uint8_t ide_status = inb(idec->cmd_addr + IDE_REG_STATUS);
    if ((ide_status & (IDE_STAT_BSY | IDE_STAT_DRDY)) != IDE_STAT_DRDY) {
	if (!polling)
	    cprintf("spurious IDE interrupt, status %02x\n", ide_status);
	return;
    }

    // Ack bus-master interrupt
    uint8_t dma_status = inb(idec->bm_addr + IDE_BM_STAT_REG);
    outb(idec->bm_addr + IDE_BM_STAT_REG, dma_status);

    // Report any error conditions
    if ((ide_status & (IDE_STAT_DF | IDE_STAT_ERR))) {
	cprintf("IDE error: status %02x (DMA %02x)\n", ide_status, dma_status);
	iostat = disk_io_failure;
    }

    if ((dma_status & (IDE_BM_STAT_ERROR | IDE_BM_STAT_ACTIVE))) {
	cprintf("IDE DMA funny state: %02x (IDE status %02x)\n", dma_status, ide_status);
	iostat = disk_io_failure;
    }

    // Stop DMA engine
    outb(idec->bm_addr + IDE_BM_CMD_REG, 0);

    // Check that we had a command queued
    if (idec->current_op.op == op_none)
	return;

    ide_complete(idec, iostat);
}

static void
ide_string_shuffle(char *s, int len)
{
    for (int i = 0; i < len; i += 2) {
	char c = s[i+1];
	s[i+1] = s[i];
	s[i] = c;
    }
}

static union {
    struct identify_device id;
    char buf[512];
} identify_buf;

static void
ide_init(struct ide_channel *idec, uint32_t diskno)
{
    ide_wait_ready(idec);

    outb(idec->cmd_addr + IDE_REG_DEVICE, diskno << 4);
    outb(idec->cmd_addr + IDE_REG_CMD, IDE_CMD_IDENTIFY);

    cprintf("Trying to identify IDE disk\n");
    if (ide_pio_in(idec, &identify_buf, 1) < 0) {
	cprintf("Unable to identify disk device\n");
	return;
    }

    ide_string_shuffle(identify_buf.id.serial, sizeof(identify_buf.id.serial));
    ide_string_shuffle(identify_buf.id.model, sizeof(identify_buf.id.model));
    ide_string_shuffle(identify_buf.id.firmware, sizeof(identify_buf.id.firmware));

    cprintf("IDE device (%d sectors, UDMA %s%s): %1.40s\n",
	    identify_buf.id.lba_sectors,
	    ((identify_buf.id.udma_mode & (1 << 5)) ? "5" :
	     (identify_buf.id.udma_mode & (1 << 4)) ? "4" :
	     (identify_buf.id.udma_mode & (1 << 3)) ? "3" :
	     (identify_buf.id.udma_mode & (1 << 2)) ? "2" :
	     (identify_buf.id.udma_mode & (1 << 1)) ? "1" :
	     (identify_buf.id.udma_mode & (1 << 0)) ? "0" : "none"),
	    idec->bm_addr ? ", bus-master" : "",
	    identify_buf.id.model);

    disk_bytes = identify_buf.id.lba_sectors;
    disk_bytes *= 512;

    uint8_t bm_status = inb(idec->bm_addr + IDE_BM_STAT_REG);
    if (bm_status & IDE_BM_STAT_SIMPLEX)
	cprintf("Simplex-mode IDE bus master, potential problems later..\n");

    // Enable interrupts (clear the IDE_CTL_NIEN bit)
    outb(idec->ctl_addr, 0);
    irq_setmask_8259A (irq_mask_8259A & ~(1 << idec->irq));
}

// Disk interface, from disk.h
uint64_t disk_bytes;

void
disk_init(struct pci_func *pcif)
{
    static int disk_init_done = 0;
    if (disk_init_done++) {
	cprintf("Additional IDE controllers found -- ignoring\n");
	return;
    }

    struct ide_channel *idec = &the_ide_channel;

    // Use the first IDE channel on the IDE controller
    idec->cmd_addr = pcif->reg_size[0] ? pcif->reg_base[0] : 0x1f0;
    idec->ctl_addr = pcif->reg_size[1] ? pcif->reg_base[1] + 2 : 0x3f6;
    idec->bm_addr = pcif->reg_base[4];
    idec->irq = 14;	// PCI IRQ routing is too complicated

    // Use the second IDE drive on the channel
    the_ide_drive = 1;

    // Now initialize the chosen drive/channel
    ide_init(idec, the_ide_drive);
}

int
disk_io(disk_op op, void *buf,
	uint32_t num_bytes, uint64_t byte_offset,
	disk_callback cb, void *cbarg)
{
    struct ide_channel *idec = &the_ide_channel;
    struct ide_op *curop = &idec->current_op;

    if (curop->op != op_none) {
	cprintf("Disk busy, dropping IO request?\n");
	return -E_BUSY;
    }

    curop->op = op;
    curop->buf = buf;
    curop->num_bytes = num_bytes;
    curop->byte_offset = byte_offset;
    curop->cb = cb;
    curop->cbarg = cbarg;

    int r = ide_send(idec, the_ide_drive);
    if (r < 0)
	curop->op = op_none;
    return r;
}
