#include <machine/param.h>
#include <machine/x86.h>
#include <kern/lib.h>
#include <kern/part.h>
#include <dev/disk.h>
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

// the global partition
struct part_desc the_part = { 0, 0 };

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
part_table_read(struct part_table *pt) 
{
    static uint8_t sect[512];    
    int64_t blocked = 1;
    struct kiovec iov = { sect, 512 };
    int r = disk_io(op_read, &iov, 1, 0, &disk_io_cb, &blocked);
    if (r < 0)
	return r;
    
    uint64_t ts_start = read_tsc();
    int warned = 0;
    while (blocked == 1) {
	uint64_t ts_now = read_tsc();
	if (warned == 0 && ts_now - ts_start > 1024*1024*1024) {
	    cprintf("part_table_read: wedged for %"PRIu64"\n", ts_now - ts_start);
	    warned = 1;
	}
	ide_poke();
    }
    
    memcpy(pt, &sect[PART_TABLE_OFFSET], sizeof(*pt));
    return blocked;
}

static void
part_table_print(struct part_table *pt)
{
    for (uint32_t i = 0; i < 4; i++) {
	struct part_entry *pe = &pt->pt_entry[i];
	cprintf(" %d  type %2x  active%2x  LBA %10d size %10d\n", i, 
		pe->pe_type, pe->pe_active, pe->pe_lbastart, pe->pe_nsectors);
    }
}

int
part_init(void)
{
    if (!part_enable) {
	the_part.pd_offset = 0;
	the_part.pd_size = disk_bytes;
	return 0;
    }

    struct part_table table;
    int r = part_table_read(&table);
    if (r < 0) {
	cprintf("cannot read partition table: %s\n", e2s(r));
	return r;
    }
    part_table_print(&table);
    
    for (uint32_t i = 0; i < 4; i++) {
	if (table.pt_entry[i].pe_type == JOS64_PART_ID) {
	    the_part.pd_offset = table.pt_entry[i].pe_lbastart * 512;
	    the_part.pd_size = table.pt_entry[i].pe_nsectors * 512;

	    cprintf("partition LBA offset %d, sectors %d\n",
		    table.pt_entry[i].pe_lbastart, table.pt_entry[i].pe_nsectors);
	    break;
	}
    }
    
    if (the_part.pd_size == 0) {
	cprintf("no JOS64 partition found\n");
	return -E_INVAL;
    }
    
    return 0;
}
