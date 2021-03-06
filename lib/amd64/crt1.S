#include <machine/asm.h>

ENTRY(_start)
	.cfi_startproc

	// Terminate stack unwinding
	pushq	$0

	// AMD64 requires 16-byte stack alignment before the call
	pushq	$0

	// Save our three arguments
	movq	%rdi, %r12
	movq	%rsi, %r13
	movq	%rdx, %r14

	// Call setup_env with our args
	call	setup_env

	// crtbegin, crtend, and whatever else
	call	_init

	// Call libmain with the right args
	movq	%r12, %rdi
	movq	%r13, %rsi
	movq	%r14, %rdx
	movq	$main, %rcx
	call	libmain
	.cfi_endproc

