#include <machine/x86.h>
#include <machine/boot.h>
#include <inc/elf32.h>
#include <inc/elf64.h>

/**********************************************************************
 * This a dirt simple boot loader, whose sole job is to boot
 * an elf kernel image from the first IDE hard disk.
 *
 * DISK LAYOUT
 *  * This program(boot.S and main.c) is the bootloader.  It should
 *    be stored in the first sector of the disk.
 *
 *  * The 2nd sector holds a Linux style setup header (setup.S) for 
 *    SYSLINUX loaders.
 *
 *  * The 3rd sector onward hols the kernel image.  The kernel image  
 *    must be in ELF format.
 *
 * BOOT UP STEPS	
 *  * when the CPU boots it loads the BIOS into memory and executes it
 *
 *  * the BIOS intializes devices, sets of the interrupt routines, and
 *    reads the first sector of the boot device(e.g., hard-drive) 
 *    into memory and jumps to it.
 *
 *  * Assuming this boot loader is stored in the first sector of the
 *    hard-drive, this code takes over...
 *
 *  * control starts in boot.S -- which sets up protected mode,
 *    and a stack so C code then run, then calls cmain()
 *
 *  * cmain() in this file takes over, reads in the kernel and jumps to it.
 **********************************************************************/

#if defined(JOS_KARCH_i386)
#define ELF_EHDR Elf32_Ehdr
#define ELF_PHDR Elf32_Phdr
#define ELF_MACH ELF_MACH_386
#elif defined(JOS_KARCH_amd64)
#define ELF_EHDR Elf64_Ehdr
#define ELF_PHDR Elf64_Phdr
#define ELF_MACH ELF_MACH_AMD64
#else
#error Boot loader does not support K_ARCH
#endif

#define SECTSIZE	512
#define ELFHDR		((ELF_EHDR *) 0x10000)	// scratch space

void readsect(void *, uint32_t);
void readseg(uint32_t, uint32_t, uint32_t);

void
cmain(uint32_t extmem_kb)
{
    ELF_PHDR *ph;
    int i;

    // read 1st page off disk
    readseg((uint32_t) ELFHDR, SECTSIZE * 8, ELFOFF);

    if (ELFHDR->e_magic != ELF_MAGIC_LE ||	/* Invalid ELF */
	ELFHDR->e_machine != ELF_MACH)		/* Wrong machine */
	goto bad;

    // load each program segment (ignores ph flags)
    ph = (ELF_PHDR *) ((uint8_t *) ELFHDR + ELFHDR->e_phoff);
    for (i = ELFHDR->e_phnum; i != 0; i--) {
	readseg(ph->p_vaddr, ph->p_memsz, ph->p_offset + ELFOFF);
	ph = (ELF_PHDR *) ((uint8_t *) ph + ELFHDR->e_phentsize);
    }

    // call the entry point from the ELF header, passing in extmem_kb
    // note: does not return!
    uint32_t eax = DIRECT_BOOT_EAX_MAGIC;
    uint32_t ebx = extmem_kb;
    uint32_t ecx = ELFHDR->e_entry & 0xFFFFFF;
    __asm__("jmp *%%ecx": :"a"(eax), "b"(ebx), "c"(ecx));

 bad:
    outw(0x8A00, 0x8A00);
    outw(0x8A00, 0x8AE0);
    for (;;)
	;
}

// Read 'count' bytes at 'offset' from kernel into virtual address 'va'.
// Might copy more than asked
void
readseg(uint32_t va, uint32_t count, uint32_t offset)
{
    uint32_t end_va;

    va &= 0xFFFFFF;
    end_va = va + count;

    // round down to sector boundary
    va &= ~(SECTSIZE - 1);

    // translate from bytes to sectors, and kernel starts at sector 1
    offset = (offset / SECTSIZE);

    // If this is too slow, we could read lots of sectors at a time.
    // We'd write more to memory than asked, but it doesn't matter --
    // we load in increasing order.
    while (va < end_va) {
	readsect((uint8_t *) va, offset);
	va += SECTSIZE;
	offset++;
    }
}

void
waitdisk(void)
{
    // wait for disk reaady
    while ((inb(0x1F7) & 0xC0) != 0x40)
	/* do nothing */ ;
}

void
readsect(void *dst, uint32_t offset)
{
    // wait for disk to be ready
    waitdisk();

    outb(0x1F2, 1);		// count = 1
    outb(0x1F3, offset);
    outb(0x1F4, offset >> 8);
    outb(0x1F5, offset >> 16);
    outb(0x1F6, (offset >> 24) | 0xE0);
    outb(0x1F7, 0x20);		// cmd 0x20 - read sectors

    // wait for disk to be ready
    waitdisk();

    // read a sector
    insl(0x1F0, dst, SECTSIZE / 4);
}
