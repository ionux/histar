#include <inc/lib.h>
#include <inc/stdio.h>
#include <inc/syscall.h>
#include <inc/assert.h>
#include <inc/memlayout.h>

int
main(int ac, char **av)
{
    uint64_t ct = start_env->container;

    struct cobj_ref seg;
    void *va = 0;
    assert(0 == segment_alloc(ct, PGSIZE, &seg, &va));

    struct ulabel ul = {
	.ul_size = 32,
	.ul_ent = va,
    };

    printf("Trying to get label..\n");
    assert(0 == thread_get_label(ct, &ul));

    void *va2 = 0;
    assert(0 == segment_map(seg, SEGMAP_READ, &va2, 0));
    ul.ul_ent = va2;

    printf("Trying to get label into RO page\n");
    assert(0 == thread_get_label(ct, &ul));
}
