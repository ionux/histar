#include <kern/lib.h>
#include <kern/part.h>
#include <kern/arch.h>
#include <kern/pstate.h>
#include <kern/disk.h>
#include <inc/error.h>

/*
+--- Bit 7 is the active partition flag, bits 6-0 are
    |    zero (when not zero this byte is also the drive
    |    number of the drive to boot so the active
    |    partition is always found on drive 80H,
    |    the first hard disk).
    |
    |    +--- Starting CHS in INT 13 call format.
    |    |
    |    |      +--- Partition type byte.
    |    |      |
    |    |      |    +--- Ending CHS in INT 13 call format.
    |    |      |    |
    |    |      |    |       +-- Starting LBA.
    |    |      |    |       |
    |    |      |    |       |        +-- Size in sectors.
    |    |      |    |       |        |
    v ---+----  v ---+----   v        v
   0. 1. 2. 3. 4. 5. 6. 7. 8.9.A.B. C.D.E.F.
   DL DH CL CH TB DH CL CH LBA..... SIZE....
   80 01 01 00 06 0e be 94 3e000000 0c610900  1st entry
   00 00 81 95 05 0e fe 7d 4a610900 724e0300  2nd entry
   00 00 00 00 00 00 00 00 00000000 00000000  3rd entry
   00 00 00 00 00 00 00 00 00000000 00000000  4th entry
*/

// offset of on disk partition table in bytes
#define PART_TABLE_OFFSET 446
// an unused partition id
#define JOS64_PART_ID 0xBC

enum { part_debug = 0 };

struct part_entry 
{
    uint8_t pe_active;

    uint8_t pe_cstart;
    uint8_t pe_hstart;
    uint8_t pe_sstart;
    
    uint8_t pe_type;
    
    uint8_t pe_cend;
    uint8_t pe_hend;
    uint8_t pe_send;
    
    uint32_t pe_lbastart;
    uint32_t pe_nsectors;
};

struct part_table { 
    struct part_entry pt_entry[4]; 
};

// the global pstate partition
static struct part_desc the_part;

static void
disk_io_cb(disk_io_status status, void *arg)
{
    uint64_t *done = (uint64_t *) arg;
    if (SAFE_EQUAL(status, disk_io_success))
	*done = 0;
    else
	*done = -E_IO;
}

static int
part_table_read(struct disk *dk, struct part_table *pt) 
{
    static uint8_t sect[512];    
    int64_t blocked = 1;
    struct kiovec iov = { sect, 512 };
    int r = disk_io(dk, op_read, &iov, 1, 0, &disk_io_cb, &blocked);
    if (r < 0)
	return r;

    uint64_t ts_start = karch_get_tsc();
    int warned = 0;
    while (blocked == 1) {
	uint64_t ts_now = karch_get_tsc();
	if (warned == 0 && ts_now - ts_start > 1024*1024*1024) {
	    cprintf("part_table_read: wedged for %"PRIu64"\n", ts_now - ts_start);
	    warned = 1;
	}
	disk_poll(dk);
    }

    if (blocked < 0)
	return blocked;

    if (sect[510] != 0x55 || sect[511] != 0xAA) {
	cprintf("part_table_read: invalid MBR signature 0x%x%x\n",
		sect[510], sect[511]);
	return -E_INVAL;
    }

    memcpy(pt, &sect[PART_TABLE_OFFSET], sizeof(*pt));
    return 0;
}

static void
part_table_print(struct part_table *pt)
{
    for (uint32_t i = 0; i < 4; i++) {
	struct part_entry *pe = &pt->pt_entry[i];
	cprintf(" %d  type %2x  active %2x  LBA %10d size %10d\n", i, 
		pe->pe_type, pe->pe_active, pe->pe_lbastart, pe->pe_nsectors);
    }
}

static int
part_init_disk(struct disk *dk)
{
    struct part_table table;
    int r = part_table_read(dk, &table);
    if (r < 0) {
	cprintf("cannot read partition table: %s\n", e2s(r));
	return 0;
    }

    if (part_debug)
	part_table_print(&table);

    struct part_entry *e = 0;
    const char *store;
    if ((store = strstr(&boot_cmdline[0], "store=/dev/hd"))) {
	const char *spec = store + strlen("store=/dev/hd");
	int i = spec[1] - '1';
	if (i < 0 || i > 3) {
	    cprintf("part store: unknown %s\n", store);
	    return 0;
	}

	if (table.pt_entry[i].pe_type != JOS64_PART_ID) {
	    cprintf("part store: %s (%x) is not type JOS64 (%x)\n",
		    store, table.pt_entry[i].pe_type, JOS64_PART_ID);
	    return 0;
	}
	e = &table.pt_entry[i];
    } else {
	for (uint32_t i = 0; i < 4; i++) {
	    if (table.pt_entry[i].pe_type == JOS64_PART_ID) {
		uint64_t off = table.pt_entry[i].pe_lbastart;
		uint64_t size = table.pt_entry[i].pe_nsectors;
		if (off * 512 + size * 512 > dk->dk_bytes) {
		    cprintf("partition exceeds disk: LBA "
			    "offset %"PRIu64", sectors %"PRIu64"\n",
			    off, size);
		    continue;
		}

		e = &table.pt_entry[i];
		break;
	    }
	}
    }

    if (!e) {
	cprintf("no JOS64 partitions found\n");
	return 0;
    }

    the_part.pd_offset = UINT64(512) * e->pe_lbastart;
    the_part.pd_size = UINT64(512) * e->pe_nsectors;
    the_part.pd_dk = dk;

    pstate_part = &the_part;
    cprintf("partition LBA offset %d, sectors %d\n",
	    e->pe_lbastart, e->pe_nsectors);
    return 1;
}

void
part_init(void)
{
    struct disk *dk;

    if (!part_enable) {
	dk = LIST_FIRST(&disks);
	the_part.pd_offset = 0;
	the_part.pd_size = dk ? dk->dk_bytes : 0;
	the_part.pd_dk = dk;
	pstate_part = &the_part;
	return;
    }

    LIST_FOREACH(dk, &disks, dk_link)
	if (part_init_disk(dk) > 0)
	    return;
}
