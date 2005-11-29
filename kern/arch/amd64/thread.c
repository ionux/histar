#include <machine/thread.h>
#include <machine/types.h>
#include <machine/pmap.h>
#include <machine/x86.h>
#include <machine/trap.h>
#include <inc/elf64.h>
#include <inc/error.h>
#include <kern/segment.h>

struct Thread *cur_thread;
struct Thread_list thread_list;

int
thread_load_elf(struct Thread *t, struct Label *l, uint8_t *binary, size_t size)
{
    int r = page_map_alloc(&t->th_pgmap);
    if (r < 0)
	return r;

    page_map_addref(t->th_pgmap);
    t->th_cr3 = kva2pa(t->th_pgmap);

    // Switch to target address space to populate it
    lcr3(t->th_cr3);

    Elf64_Ehdr *elf = (Elf64_Ehdr *) binary;
    if (elf->e_magic != ELF_MAGIC || elf->e_ident[0] != 2) {
	cprintf("ELF magic mismatch\n");
	return -E_INVAL;
    }

    int i;
    Elf64_Phdr *ph = (Elf64_Phdr *) (binary + elf->e_phoff);
    for (i = 0; i < elf->e_phnum; i++, ph++) {
	if (ph->p_type != 1)
	    continue;
	if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) {
	    cprintf("ELF segment overflow\n");
	    return -E_INVAL;
	}
	if (ph->p_vaddr + ph->p_memsz > ULIM) {
	    cprintf("ELF segment over ULIM\n");
	    return -E_INVAL;
	}

	struct Segment *sg;
	int r = segment_alloc(&sg);
	if (r < 0)
	    return r;

	Elf64_Addr pg_start = ROUNDDOWN(ph->p_vaddr, PGSIZE);
	Elf64_Addr pg_end = ROUNDUP(ph->p_vaddr + ph->p_memsz, PGSIZE);
	uint64_t num_pages = (pg_end - pg_start) / PGSIZE;
	r = segment_set_npages(sg, num_pages);
	if (r < 0) {
	    segment_free(sg);
	    return r;
	}

	r = segment_map(t->th_pgmap, sg, (void*) pg_start,
			0, num_pages, segment_map_rw);
	if (r < 0) {
	    segment_free(sg);
	    return r;
	}

	// Pages are already referenced by page table, so this is safe.
	segment_free(sg);
    }

    // Two passes so that map_segment() doesn't drop a partially-filled
    // page from a previous ELF segment.
    ph = (Elf64_Phdr *) (binary + elf->e_phoff);
    for (i = 0; i < elf->e_phnum; i++, ph++) {
	if (ph->p_type != 1)
	    continue;

	memcpy((void*) ph->p_vaddr, binary + ph->p_offset, ph->p_filesz);
    }

    // Map a stack
    struct Segment *sg;
    r = segment_alloc(&sg);
    if (r < 0)
	return r;

    r = segment_set_npages(sg, 1);
    if (r < 0)
	return r;

    r = segment_map(t->th_pgmap, sg, (void*) (ULIM - PGSIZE),
		    0, 1, segment_map_rw);
    if (r < 0)
	return r;

    segment_free(sg);

    return thread_jump(t, l, t->th_pgmap, (void*) elf->e_entry, (void*) ULIM, 0);
}

void
thread_set_runnable(struct Thread *t)
{
    t->th_status = thread_runnable;
}

void
thread_suspend(struct Thread *t)
{
    t->th_status = thread_not_runnable;
}

int
thread_alloc(struct Thread **tp)
{
    struct Page *thread_pg;
    int r = page_alloc(&thread_pg);
    if (r < 0)
	return r;

    struct Thread *t = page2kva(thread_pg);

    memset(t, 0, sizeof(*t));
    LIST_INSERT_HEAD(&thread_list, t, th_link);
    t->th_status = thread_not_runnable;

    *tp = t;
    return 0;
}

void
thread_decref(struct Thread *t)
{
    if (--t->th_ref == 0)
	thread_free(t);
}

void
thread_free(struct Thread *t)
{
    thread_halt(t);

    LIST_REMOVE(t, th_link);
    if (t->th_pgmap)
	page_map_decref(t->th_pgmap);
    page_free(pa2page(kva2pa(t)));
}

void
thread_run(struct Thread *t)
{
    if (t->th_status != thread_runnable)
	panic("trying to run a non-runnable thread %p", t);

    cur_thread = t;
    lcr3(t->th_cr3);
    trapframe_pop(&t->th_tf);
}

void
thread_halt(struct Thread *t)
{
    t->th_status = thread_not_runnable;
    if (cur_thread == t)
	cur_thread = 0;
}

int
thread_jump(struct Thread *t, struct Label *label,
	    struct Pagemap *pgmap, void *entry,
	    void *stack, uint64_t arg)
{
    struct Label *newl;
    int r = label_copy(label, &newl);
    if (r < 0)
	return r;

    if (t->th_label)
	label_free(t->th_label);
    t->th_label = newl;

    page_map_addref(pgmap);
    if (t->th_pgmap)
	page_map_decref(t->th_pgmap);

    t->th_pgmap = pgmap;
    t->th_cr3 = kva2pa(t->th_pgmap);

    memset(&t->th_tf, 0, sizeof(t->th_tf));
    t->th_tf.tf_rflags = FL_IF;
    t->th_tf.tf_cs = GD_UT | 3;
    t->th_tf.tf_ss = GD_UD | 3;
    t->th_tf.tf_rip = (uint64_t) entry;
    t->th_tf.tf_rsp = (uint64_t) stack;
    t->th_tf.tf_rdi = arg;

    return 0;
}

void
thread_syscall_restart(struct Thread *t)
{
    t->th_tf.tf_rip -= 2;
}
