#include <kern/label.h>
#include <machine/pmap.h>
#include <inc/error.h>

int
label_alloc(struct Label **lp)
{
    void *p;
    int r = page_alloc(&p);
    if (r < 0)
	return r;

    struct Label *l = p;
    l->lb_hdr.num_ent = 0;

    *lp = l;
    return 0;
}

int
label_copy(struct Label *src, struct Label **dstp)
{
    int r = label_alloc(dstp);
    if (r < 0)
	return r;

    struct Label *dst = *dstp;
    memcpy(dst, src, PGSIZE);
    return 0;
}

void
label_free(struct Label *l)
{
    page_free(l);
}

static int
label_find_slot(struct Label *l, uint64_t handle)
{
    for (int i = 0; i < l->lb_hdr.num_ent; i++)
	if (LB_HANDLE(l->lb_ent[i]) == handle)
	    return i;

    return -E_NO_MEM;
}

static int
label_get_level(struct Label *l, uint64_t handle)
{
    int i = label_find_slot(l, handle);
    if (i < 0)
	return l->lb_hdr.def_level;
    return LB_LEVEL(l->lb_ent[i]);
}

int
label_set(struct Label *l, uint64_t handle, int level)
{
    int i = label_find_slot(l, handle);
    if (i < 0) {
	if (l->lb_hdr.num_ent == NUM_LB_ENT)
	    return -E_NO_MEM;
	i = l->lb_hdr.num_ent++;
    }

    l->lb_ent[i] = LB_CODE(handle, level);
    return 0;
}

int
label_compare(struct Label *l1, struct Label *l2, level_comparator cmp)
{
    for (int i = 0; i < l1->lb_hdr.num_ent; i++) {
	int l1l = LB_LEVEL(l1->lb_ent[i]);
	int l2l = label_get_level(l2, LB_HANDLE(l1->lb_ent[i]));

	int r = cmp(l1l, l2l);
	if (r < 0)
	    return r;
    }

    for (int i = 0; i < l2->lb_hdr.num_ent; i++) {
	int l1l = label_get_level(l1, LB_HANDLE(l2->lb_ent[i]));
	int l2l = LB_LEVEL(l2->lb_ent[i]);

	int r = cmp(l1l, l2l);
	if (r < 0)
	    return r;
    }

    int l1d = l1->lb_hdr.def_level;
    int l2d = l2->lb_hdr.def_level;

    int r = cmp(l1d, l2d);
    if (r < 0)
	return r;

    return 0;
}

int
label_leq_starlo(int a, int b)
{
    if (a == LB_LEVEL_STAR)
	return 0;
    if (b == LB_LEVEL_STAR)
	return -E_LABEL;
    if (a <= b)
	return 0;
    return -E_LABEL;
}

int
label_leq_starhi(int a, int b)
{
    if (b == LB_LEVEL_STAR)
	return 0;
    if (a == LB_LEVEL_STAR)
	return -E_LABEL;
    if (a <= b)
	return 0;
    return -E_LABEL;
}

int
label_eq(int a, int b)
{
    if (a == b)
	return 0;
    return -E_LABEL;
}
