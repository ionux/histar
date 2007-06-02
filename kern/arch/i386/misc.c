#include <machine/x86.h>
#include <kern/arch.h>

uintptr_t
karch_get_sp(void)
{
    return read_esp();
}

uint64_t
karch_get_tsc(void)
{
    return read_tsc();
}

void
karch_jmpbuf_init(struct jos_jmp_buf *jb,
		  void *fn, void *stackbase)
{
    jb->jb_eip = (uintptr_t) fn;
    jb->jb_esp = (uintptr_t) ROUNDUP(stackbase, PGSIZE);
}
