#include <inc/lib.h>
#include <inc/stdio.h>
#include <inc/syscall.h>
#include <inc/assert.h>
#include <inc/memlayout.h>
#include <inc/setjmp.h>
#include <inc/utrap.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static struct jos_jmp_buf jb;

static void __attribute__((noreturn))
trap_handler(struct UTrapframe *utf)
{
    cprintf("utf %p size %zd, src %d num %d arg 0x%"PRIx64"\n",
            utf, sizeof(*utf),
            utf->utf_trap_src, utf->utf_trap_num,
            utf->utf_trap_arg);

    cprintf("rip 0x%zx\n", utf->utf_pc);
    jos_longjmp(&jb, 1);
}

int
main(int ac, char **av)
{
    if (jos_setjmp(&jb) != 0) {
	printf("utrap handler caught something\n");
	exit(-1);
    }

    utrap_set_handler(&trap_handler);

    uint64_t ct = start_env->shared_container;

    struct cobj_ref seg;
    void *va = 0;
    assert(0 == segment_alloc(ct, PGSIZE, &seg, &va, 0, "fault testing"));

    struct ulabel ul = {
	.ul_size = 32,
	.ul_ent = va,
    };

    printf("Trying to get label..\n");
    assert(0 == thread_get_label(&ul));

    void *va2 = 0;
    assert(0 == segment_map(seg, 0, SEGMAP_READ, &va2, 0, 0));
    ul.ul_ent = va2;

    //printf("Trying to get label into RO page\n");
    //assert(0 == thread_get_label(&ul));

    printf("Trying to write into RO page\n");
    //memcpy(va2, "hello world", 10);

    printf("utrap_set_mask 1\n");
    utrap_set_mask(1);

    printf("utrap_set_mask 0\n");
    utrap_set_mask(0);

    printf("Trying to execute funny instruction\n");
    __asm__("ud2");

    printf("done\n");
}
