#include <machine/pmap.h>
#include <machine/lnxpage.h>
#include <kern/arch.h>
#include <kern/sched.h>
#include <inc/error.h>

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>

static struct Pagemap *cur_pm;

static void
lnx64_sigsegv(int signo, siginfo_t *si, void *ctx)
{
    static int recursive;
    recursive++;

    void *va = si->si_addr;
    int code = si->si_code;
    uint32_t reqflags = 0;
    printf("lnx64_sigsegv: faulting address %p, code %d\n", va, code);

    /*
     * simulate hardware handling of this page fault..
     * fall through only if the mapping isn't in Pagemap.
     */
    for (int i = 0; i < NPME; i++) {
	if (cur_pm &&
	    (cur_pm->pme[i].va == ROUNDDOWN(va, PGSIZE)) &&
	    (cur_pm->pme[i].pte & PTE_P) &&
	    (cur_pm->pme[i].pte & PTE_U))
	{
	    int prot = PROT_READ;
	    if (cur_pm->pme[i].pte & PTE_W)
		prot |= PROT_WRITE;
	    if (mmap(va, PGSIZE, prot, MAP_FIXED | MAP_SHARED,
		     physmem_file_fd, PTE_ADDR(cur_pm->pme[i].pte)) < 0) {
		printf("lnx64_sigsegv: mmap: %s\n", strerror(errno));
	    } else {
		recursive--;
		return;
	    }
	}
    }

    /*
     * XXX corner case:
     * if we're faulting because the page is mapped read-only and the
     * "user code" is trying to write to it, we'll keep looping forever.
     * need to check if the page is already mapped RO and we're getting
     * code==SEGV_ACCERR, in which case reqflags|=SEGMAP_WRITE.
     */
    if (recursive > 1) {
	printf("lnx64_sigsegv: recursive unhandled page fault\n");
	exit(-1);
    }

    printf("lnx64_sigsegv: no match in pagemap for va=%p\n", va);

    int r = thread_pagefault(cur_thread, va, reqflags);
    if (r != 0 && r != -E_RESTART) {
	printf("error handling page fault for %"PRIu64" (%s): %s\n", 
	       cur_thread->th_ko.ko_id, cur_thread->th_ko.ko_name, e2s(r));
	thread_halt(cur_thread);
    }

    if (!cur_thread || !SAFE_EQUAL(cur_thread->th_status, thread_runnable))
	schedule();
    printf("lnx64_sigsegv: returning back to %"PRIu64" (%s)\n",
	   cur_thread->th_ko.ko_id, cur_thread->th_ko.ko_name);
    recursive--;
    thread_run(cur_thread);
}

void
lnxpmap_init(void)
{
    for (void *va = (void *) LNX64_USER_BASE;
	 va < (void *) LNX64_USER_LIM; va += PGSIZE)
    {
	int r = mprotect(va, PGSIZE, PROT_NONE);
	if (r == 0 || errno != ENOMEM) {
	    printf("lnxpmap_init(): %p: %d, %s\n", va, r, strerror(errno));
	    exit(-1);
	}
    }

    struct sigaction sa;
    sa.sa_sigaction = &lnx64_sigsegv;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    if (sigaction(SIGSEGV, &sa, 0) < 0)
	perror("sigaction");
}

int
check_user_access(const void *base, uint64_t nbytes, uint32_t reqflags)
{
    assert(cur_thread && cur_as);

    if (base < (void *) LNX64_USER_BASE || base >= (void *) LNX64_USER_LIM)
	return -E_INVAL;

    uint64_t pte_flags = PTE_P | PTE_U;
    if (reqflags & SEGMAP_WRITE)
	pte_flags |= PTE_W;

    if (nbytes > 0) {
	for (void *va = (void *) ROUNDDOWN(base, PGSIZE);
	     va < ROUNDUP(base + nbytes, PGSIZE); va += PGSIZE)
	{
	    if (va >= (void *) LNX64_USER_LIM)
		return -E_INVAL;

	    int va_ok = 0;
	    for (int i = 0; i < NPME; i++) {
		struct Pagemapent *pme = &cur_as->as_pgmap->pme[i];
		if (pme->va == va && (pme->pte & pte_flags) == pte_flags)
		    va_ok = 1;
	    }

	    if (!va_ok) {
		int r = as_pagefault(cur_as, va, reqflags);
		if (r < 0)
		    return r;
	    }
	}
    }

    as_switch(cur_as);
    return 0;
}

int
page_map_alloc(struct Pagemap **pm_store)
{
    struct Pagemap *pm = malloc(sizeof(*pm));
    memset(pm, 0, sizeof(pm));
    *pm_store = pm;
    return 0;
}

void
page_map_free(struct Pagemap *pgmap)
{
    free(pgmap);
}

int
page_map_traverse(struct Pagemap *pgmap, const void *first, const void *last,
		  int create, page_map_traverse_cb cb, const void *arg)
{
    assert(!create);
    for (int i = 0; i < NPME; i++)
	if (pgmap->pme[i].pte & PTE_P && pgmap->pme[i].va >= first && pgmap->pme[i].va < last)
	    cb(arg, &pgmap->pme[i].pte, pgmap->pme[i].va);
    return 0;
}

int
pgdir_walk(struct Pagemap *pgmap, const void *va,
	   int create, uint64_t **pte_store)
{
    int freeslot = -1;

    for (int i = 0; i < NPME; i++) {
	if (pgmap->pme[i].pte & PTE_P) {
	    if (pgmap->pme[i].va == va) {
		*pte_store = &pgmap->pme[i].pte;
		return 0;
	    }
	} else {
	    if (freeslot == -1)
		freeslot = i;
	}
    }

    if (freeslot == -1)
	return -E_NO_MEM;

    pgmap->pme[freeslot].va = (void *) va;
    pgmap->pme[freeslot].pte = 0;
    *pte_store = &pgmap->pme[freeslot].pte;
    return 0;
}

void
pmap_tlb_invlpg(const void *va)
{
    assert(va >= (void *) LNX64_USER_BASE);
    assert(va < (void *) LNX64_USER_LIM);
    munmap(ROUNDDOWN((void *) va, PGSIZE), PGSIZE);
}

void
pmap_set_current(struct Pagemap *pm, int flush_tlb)
{
    cur_pm = pm;
    if (flush_tlb)
	munmap((void *) LNX64_USER_BASE, LNX64_USER_LIM - LNX64_USER_BASE);
}
