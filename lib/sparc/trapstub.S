#include <machine/asm.h>
#include <machine/pmap.h>
#include <machine/mmu.h>
#include <lib/utrapasm.h>


ENTRY(utrap_entry_asm)
	/* Save space for the register window */
	sub	%sp, 0x40, %sp
	call	utrap_entry
	 add	%sp, 0x40, %o0

.section .utrap_masked, "ax", @progbits
.globl utrap_ret
.type utrap_ret, %function
utrap_ret:
	mov	%o0, %g7
	ld	[%g7 + 136], %g6
	mov	%g6, %y

	LOAD_REG_SET(i, g7 + TF_I0)
	LOAD_REG_SET(l, g7 + TF_L0)
	LOAD_REG_SET(o, g7 + TF_O0)

	ld	[%g7 + TF_G1], %g1
	ldd	[%g7 + TF_G2], %g2
	ldd	[%g7 + TF_G4], %g4

	ldd	[%g7 + 128], %g6

	jmp	%g6
	 jmp	%g7

