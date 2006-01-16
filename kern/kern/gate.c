#include <kern/gate.h>
#include <kern/lib.h>

int
gate_alloc(struct Label *l, struct Gate **gp)
{
    struct Gate *g;
    int r = kobject_alloc(kobj_gate, l, (struct kobject **)&g);
    if (r < 0)
	return r;

    static_assert(sizeof(*g) <= sizeof(struct kobject_buf));

    memset(&g->gt_target_label, 0, sizeof(g->gt_target_label));
    memset(&g->gt_te, 0, sizeof(g->gt_te));

    *gp = g;
    return 0;
}
