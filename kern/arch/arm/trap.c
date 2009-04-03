#include <kern/arch.h>
#include <kern/lib.h>
#include <kern/prof.h>
#include <kern/kobj.h>
#include <kern/thread.h>
#include <kern/sched.h>
#include <kern/utrap.h>
#include <inc/error.h>
#include <machine/arm.h>
#include <machine/mmu.h>
#include <machine/trap.h>
#include <machine/trapcodes.h>
#include <dev/goldfish_irq.h>

static uint64_t trap_user_iret_tsc;
static const struct Thread *trap_thread;
static int trap_thread_syscall_writeback;

static void trapframe_print(const struct Trapframe *);

//XXX- notes
static void
trap_thread_set(const struct Thread *t)
{
	if (trap_thread) {
		kobject_unpin_hdr(&trap_thread->th_ko);
		trap_thread = NULL;
	}

	if (t) {
		kobject_pin_hdr(&t->th_ko);
		trap_thread = t;
	}
}

// Actually pop the stackframe in t->th_tf, executing the thread.
// The address space will have already been changed, if necessary.
void
thread_arch_run(const struct Thread *t)
{
	trap_thread_set(t);
	trap_user_iret_tsc = karch_get_tsc();

	if (t->th_fp_enabled) {
		panic("%s: arm fp handling!", __func__);
	}

	trapframe_pop(&t->th_tf);
}

// Run the idle loop. There's no thread context.
void
thread_arch_idle(void)
{
	trap_thread_set(NULL);
	trap_user_iret_tsc = karch_get_tsc();
	cpsr_set(cpsr_get() & ~CPSR_IRQ_OFF);

	// must be a more efficient means of sleeping...
	while (1)
		;
}

// Used by sys_self_get_entry_args() to get the thread's entry arguments on
// 32-bit archs.
int
thread_arch_get_entry_args(const struct Thread *t,
    struct thread_entry_args *targ)
{
	memcpy(targ, &t->th_tfa.tfa_entry_args, sizeof(*targ));
	return (0);
}

// Populate a new thread's trapframe, readying it for execution.
void
thread_arch_jump(struct Thread *t, const struct thread_entry *te)
{
	if (t == trap_thread)
		trap_thread_syscall_writeback = 0;

	memset(&t->th_tf, 0, sizeof(t->th_tf));
	t->th_md.mt_utrap_mask = UT_NMASK;
	t->th_tf.tf_spsr = CPSR_MODE_USR | CPSR_ISET_ARM;
	t->th_tf.tf_pc = (uintptr_t)te->te_entry;
	t->th_tf.tf_sp = (uintptr_t)te->te_stack;
	t->th_tf.tf_r0 = te->te_arg[0];
	t->th_tf.tf_r1 = te->te_arg[1];
	t->th_tf.tf_r2 = te->te_arg[2];
	t->th_tf.tf_r3 = te->te_arg[3];
	t->th_tf.tf_r4 = te->te_arg[4];
	t->th_tf.tf_r5 = te->te_arg[5];

	for (uint32_t i = 0; i < thread_entry_narg; i++)
		t->th_tfa.tfa_entry_args.te_arg[i] = te->te_arg[i];

	static_assert(thread_entry_narg == 6);
}

// Return non-0 if user traps are masked for this thread.
int
thread_arch_is_masked(const struct Thread *t)
{
	return (t->th_md.mt_utrap_mask == UT_MASK);
}

// If 'mask', mask user traps for this thread. Return non-0 if user traps were
// previously masked.
int
thread_arch_set_mask(const struct Thread *t, int mask)
{
	int wasmasked = (t->th_md.mt_utrap_mask == UT_MASK);
	kobject_dirty(&trap_thread->th_ko)->th.th_md.mt_utrap_mask = mask;
	return (wasmasked);
}

// XXX- notes
int
thread_arch_utrap(struct Thread *t, uint32_t src, uint32_t num, uint64_t arg)
{
	void *stacktop;
	uint32_t sp = t->th_tf.tf_sp;

	if (sp >  t->th_as->as_utrap_stack_base &&
	    sp <= t->th_as->as_utrap_stack_top)
		stacktop = (void *)(uintptr_t)sp;
	else
		stacktop = (void *)t->th_as->as_utrap_stack_top;

	struct UTrapframe t_utf;

	t_utf.utf_trap_src = src;
	t_utf.utf_trap_num = num;
	t_utf.utf_trap_arg = arg;
	#define UTF_COPY(r) t_utf.utf_##r = t->th_tf.tf_##r
	UTF_COPY(r0);  UTF_COPY(r1);  UTF_COPY(r2);  UTF_COPY(r3);
	UTF_COPY(r4);  UTF_COPY(r5);  UTF_COPY(r6);  UTF_COPY(r7);
	UTF_COPY(r8);  UTF_COPY(r9);  UTF_COPY(r10); UTF_COPY(r11);
	UTF_COPY(r12); UTF_COPY(r13); UTF_COPY(r14); UTF_COPY(r15);
	UTF_COPY(spsr);
	#undef UTF_COPY

	struct UTrapframe *utf = stacktop - sizeof(*utf);
	int r = check_user_access(utf, sizeof(*utf), SEGMAP_WRITE);
	if (r < 0) {
		if ((uintptr_t) utf <= t->th_as->as_utrap_stack_base)
			cprintf("thread_arch_utrap: utrap stack overflow\n");
		return (r);
	}

	if (t == trap_thread && trap_thread_syscall_writeback) {
		trap_thread_syscall_writeback = 0;
		t_utf.utf_r0 = 0;
		t_utf.utf_r1 = 0;
	}

	memcpy(utf, &t_utf, sizeof(*utf));
	t->th_tf.tf_sp = (uintptr_t) utf;
	t->th_tf.tf_pc = t->th_as->as_utrap_entry;
	t->th_tf.tf_spsr = CPSR_MODE_USR | CPSR_ISET_ARM;
	t->th_md.mt_utrap_mask = UT_MASK;

	return (0);
}

void
thread_arch_rebooting(struct Thread *t)
{
}

static void
trapframe_print(const struct Trapframe *tf)
{
	cprintf("\n----- TRAPFRAME: -----\n");

	cprintf("r0:   0x%08x   ", tf->tf_r0);
	cprintf("r1:   0x%08x   ", tf->tf_r1);
	cprintf("r2:   0x%08x   ", tf->tf_r2);
	cprintf("r3:   0x%08x\n",  tf->tf_r3);

	cprintf("r4:   0x%08x   ", tf->tf_r4);
	cprintf("r5:   0x%08x   ", tf->tf_r5);
	cprintf("r6:   0x%08x   ", tf->tf_r6);
	cprintf("r7:   0x%08x\n",  tf->tf_r7);

	cprintf("r8:   0x%08x   ", tf->tf_r8);
	cprintf("r9:   0x%08x   ", tf->tf_r9);
	cprintf("r10:  0x%08x   ", tf->tf_r10);
	cprintf("r11:  0x%08x\n",  tf->tf_r11);

	cprintf("r12:  0x%08x   ", tf->tf_r12);
	cprintf("r13:  0x%08x   ", tf->tf_r13);
	cprintf("r14:  0x%08x   ", tf->tf_r14);
	cprintf("r15:  0x%08x\n",  tf->tf_r15);

	cprintf("spsr: 0x%08x\n\n", tf->tf_spsr);
}

// Handle a system call.
static void
swi_handler(struct Trapframe *tf)
{
	int64_t r;
	uint32_t sysnum = tf->tf_r0;
	uint64_t *args = (uint64_t *)tf->tf_r1;

	trap_thread_syscall_writeback = 1;

	r = check_user_access(args, sizeof(uint64_t) * 7, 0);
	if (r >= 0)
		r = kern_syscall(sysnum, args[0], args[1], args[2],
		    args[3], args[4], args[5], args[6]);

	if (trap_thread_syscall_writeback) {
		trap_thread_syscall_writeback = 0;
		struct Thread *t = &kobject_dirty(&trap_thread->th_ko)->th;
		if (r != -E_RESTART) {
			t->th_tf.tf_r0 = (uint64_t)r & 0xffffffff;
			t->th_tf.tf_r1 = (uint64_t)r >> 32;
		}
	} else {
		assert(r == 0);
	}
}

static void
page_fault(const struct Thread *t, const struct Trapframe *tf, const int code)
{
	void *fault_va;
    	uint32_t reqflags = 0;
	uint32_t fsr, far;

	/*
	 * Ugh. Android's ARM emulator appears to be broken.
	 *
	 * The old FAR and FSR `Combined/Data registers' (opcode2 == 0) are
	 * supposed to be always updated. With newer chips, the user has the
	 * option of using additional registers. However, this isn't the case
	 * with Android. In addition, the code Google's using mistakes the IFAR
	 * for the WFAR and doesn't even allow accesses to the real IFAR!.
	 * Things appear better in the QEMU 0.9.1 source, but we have what we
	 * have.
	 *
	 * I don't know how Linux would work without taking this damage into
	 * account and I haven't looked. Anyway, we will work around this.
	 */
	fsr = cp15_fsr_get();
	far = cp15_far_get();

	/* clear so we can differentiate between bogus Android and real */
	cp15_fsr_set(0);
	cp15_far_set(0);

	assert(code == T_PA || fsr != 0);

	/* if both zero, assume broken Android emulator instruction miss */
	if (code == T_PA && fsr == 0 && far == 0) {
		static int warn = 0;
		uint32_t zero = 0;

		if (!warn) {
			cprintf("NOTICE: Assuming Android emulation\n");
			warn++;
		}

		__asm__ __volatile__("mrc p15, 0, %0, c5, c0, 1" : "=r" (fsr));
		__asm__ __volatile__("mrc p15, 0, %0, c6, c0, 1" : "=r" (far));
		__asm__ __volatile__("mcr p15, 0, %0, c5, c0, 1" : "=r" (zero));
		__asm__ __volatile__("mcr p15, 0, %0, c6, c0, 1" : "=r" (zero));
	}

	fault_va = (void*)far;

	/*
	 * Older ARMs (pre-VMSAv6, with which we're trying to be compatible)
	 * don't let us know whether the attempt was a load or store.
	 */
	if (code == T_PA)
		reqflags |= SEGMAP_EXEC;

	//XXX what to do about SEGMAP_WRITE?

	if (CPSR_MODE(tf->tf_spsr) != CPSR_MODE_USR) {
		cprintf("kernel page fault: va=%p\n", fault_va);
		trapframe_print(tf);
		panic("kernel page fault");
	} else {
		int r;

		// see if we're emulating the dirty bit first
		if (code == T_DA) {
			struct Pagemap *pgmap;

			pgmap = (struct Pagemap *)pa2kva(cp15_ttbr_get());
		    	r = arm_dirtyemu(pgmap, fault_va);
			if (r == 0)
				return;
		}

		r = thread_pagefault(t, fault_va, reqflags);
		if (r == 0 || r == -E_RESTART)
			return;

		r = thread_utrap(t, UTRAP_SRC_HW, code, (uintptr_t)fault_va);
		if (r == 0 || r == -E_RESTART)
			return;

		cprintf("user page fault: thread %" PRIu64 " (%s), "
		    "as %" PRIu64 " (%s), "
		    "va=%p: r15/pc=0x%08x, r14/lr=0x%08x, r13/sp=0x%08x: %s\n",
		    t->th_ko.ko_id, t->th_ko.ko_name,
		    t->th_as ? t->th_as->as_ko.ko_id : 0,
		    t->th_as ? t->th_as->as_ko.ko_name : "null",
		    fault_va, tf->tf_r15, tf->tf_r14, tf->tf_r13, e2s(r));

		thread_halt(t);
	}
}

static void
exception_dispatch(int trapcode, struct Trapframe *tf)
{
	if (trap_thread == NULL && trapcode != T_IRQ && trapcode != T_FIQ) {
		trapframe_print(tf);
		panic("non-{IRQ,FIQ} trap while idle");
	}

	switch (trapcode) {
	case T_RESET:
		panic("T_RESET");
		break;

	case T_SWI:
		assert(CPSR_MODE(cpsr_get()) == CPSR_MODE_SVC);
		swi_handler(tf);
		break;

	case T_PA:
		assert(CPSR_MODE(cpsr_get()) == CPSR_MODE_ABT);
		page_fault(trap_thread, tf, trapcode);
		break;

	case T_DA:
		assert(CPSR_MODE(cpsr_get()) == CPSR_MODE_ABT);
		page_fault(trap_thread, tf, trapcode);
		break;

	case T_UNUSED:
		panic("T_UNUSED");
		break;

	case T_IRQ:
		assert(CPSR_MODE(cpsr_get()) == CPSR_MODE_IRQ);
		irq_arch_handle();	
		break;

	case T_FIQ:
		assert(CPSR_MODE(cpsr_get()) == CPSR_MODE_FIQ);
		panic("T_FIQ");
		break;

	case T_UI:
		assert(CPSR_MODE(cpsr_get()) == CPSR_MODE_UND);
		panic("T_UI");
		break;

	default:
		panic("bad trapcode: %d\n", trapcode);
		break;
	}
}

/*
 * Jumped into from assembly to handle an exception. There is no kernel
 * preemption whatsoever (interrupts off in kernel).
 */
void __attribute__((__noreturn__, no_instrument_function))
exception_handler(int trapcode, struct Trapframe *tf, uint32_t sp)
{
	/* paranoia: ensure the sp coming in was 8-byte aligned */
	assert((sp & 7) == 0);

	if (trap_thread) {
		struct Thread *t = &kobject_dirty(&trap_thread->th_ko)->th;
		sched_stop(t, karch_get_tsc() - trap_user_iret_tsc);

		// Save the thread's trapframe.
		t->th_tf = *tf;
		if (t->th_fp_enabled)
			panic("%s: cannot handle fp (arm has fp?)", __func__);
	}

	uint64_t start = karch_get_tsc();
	if (trap_thread) {
		prof_user(0, start - trap_user_iret_tsc);
		prof_thread(trap_thread, start - trap_user_iret_tsc);
	} else {
		prof_user(1, start - trap_user_iret_tsc);
	}

	exception_dispatch(trapcode, tf);
	prof_trap(trapcode, karch_get_tsc() - start);

	thread_run();

	panic("exception_handler returning!");
}
