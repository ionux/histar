#include <machine/pmap.h>
#include <machine/trap.h>
#include <kern/label.h>
#include <kern/kobj.h>
#include <inc/error.h>
#include <inc/safeint.h>

////////////////////////////////
// Level comparison functions
////////////////////////////////

typedef int (level_comparator_fn) (level_t, level_t);
struct level_comparator_buf {
    level_comparator_fn *gen;
    int inited;
    int8_t cmp[LB_LEVEL_STAR + 1][LB_LEVEL_STAR + 1];
    int8_t max[LB_LEVEL_STAR + 1][LB_LEVEL_STAR + 1];
};

static int
label_leq_starlo_fn(level_t a, level_t b)
{
    if (a == LB_LEVEL_STAR)
	return 0;
    if (b == LB_LEVEL_STAR)
	return -E_LABEL;
    return (a <= b) ? 0 : -E_LABEL;
}

static int
label_leq_starhi_fn(level_t a, level_t b)
{
    if (b == LB_LEVEL_STAR)
	return 0;
    if (a == LB_LEVEL_STAR)
	return -E_LABEL;
    return (a <= b) ? 0 : -E_LABEL;
}

static int
label_eq_fn(level_t a, level_t b)
{
    return (a == b) ? 0 : -E_LABEL;
}

#define LEVEL_COMPARATOR(x)						\
    static struct level_comparator_buf x##_buf = { .gen = &x##_fn };	\
    level_comparator x = &x##_buf

LEVEL_COMPARATOR(label_leq_starlo);
LEVEL_COMPARATOR(label_leq_starhi);
LEVEL_COMPARATOR(label_eq);

static void
level_comparator_init(level_comparator c)
{
    if (c->inited)
	return;

    for (int a = 0; a <= LB_LEVEL_STAR; a++) {
	for (int b = 0; b <= LB_LEVEL_STAR; b++) {
	    c->cmp[a][b] = c->gen(a, b);
	    c->max[a][b] = (c->cmp[a][b] == 0) ? b : a;
	}
    }
    c->inited = 1;
}

////////////////////
// Label handling
////////////////////

static uint32_t
label_nslots(const struct Label *l)
{
    return NUM_LB_ENT_INLINE + kobject_npages(&l->lb_ko) * NUM_LB_ENT_PER_PAGE;
}

static int
label_get_slotp_read(const struct Label *l, uint32_t slotnum,
		     const uint64_t **slotp)
{
    if (slotnum <= NUM_LB_ENT_INLINE) {
	*slotp = &l->lb_ent[slotnum];
	return 0;
    }

    slotnum -= NUM_LB_ENT_INLINE;
    const struct Label_page *lpg;
    int r = kobject_get_page(&l->lb_ko, slotnum / NUM_LB_ENT_PER_PAGE,
			     (void **) &lpg, page_shared_ro);
    if (r < 0)
	return r;

    *slotp = &lpg->lp_ent[slotnum % NUM_LB_ENT_PER_PAGE];
    return 0;
}

static int
label_get_slotp_write(struct Label *l, uint32_t slotnum, uint64_t **slotp)
{
    if (slotnum <= NUM_LB_ENT_INLINE) {
	*slotp = &l->lb_ent[slotnum];
	return 0;
    }

    slotnum -= NUM_LB_ENT_INLINE;
    struct Label_page *lpg;
    int r = kobject_get_page(&l->lb_ko, slotnum / NUM_LB_ENT_PER_PAGE,
			     (void **) &lpg, page_excl_dirty);
    if (r < 0)
	return r;

    *slotp = &lpg->lp_ent[slotnum % NUM_LB_ENT_PER_PAGE];
    return 0;
}

static int
label_get_level(const struct Label *l, uint64_t handle)
{
    for (uint32_t i = 0; i < label_nslots(l); i++) {
	const uint64_t *entp;
	assert(0 == label_get_slotp_read(l, i, &entp));
	if (*entp && LB_HANDLE(*entp) == handle)
	    return LB_LEVEL(*entp);
    }

    return l->lb_def_level;
}

int
label_alloc(struct Label **lp, level_t def)
{
    struct kobject *ko;
    int r = kobject_alloc(kobj_label, 0, &ko);
    if (r < 0)
	return r;

    struct Label *l = &ko->lb;
    l->lb_def_level = def;
    for (int i = 0; i < NUM_LB_ENT_INLINE; i++)
	l->lb_ent[i] = 0;

    *lp = l;
    return 0;
}

int
label_copy(const struct Label *src, struct Label **dstp)
{
    struct Label *dst;
    int r = label_alloc(&dst, LB_LEVEL_UNDEF);
    if (r < 0)
	return r;

    r = kobject_set_nbytes(&dst->lb_ko, src->lb_ko.ko_nbytes);
    if (r < 0)
	return r;

    dst->lb_def_level = src->lb_def_level;
    memcpy(&dst->lb_ent[0], &src->lb_ent[0],
	   NUM_LB_ENT_INLINE * sizeof(dst->lb_ent[0]));

    for (uint32_t i = 0; i < kobject_npages(&src->lb_ko); i++) {
	void *src_pg, *dst_pg;

	r = kobject_get_page(&src->lb_ko, i, &src_pg, page_shared_ro);
	if (r < 0)
	    return r;

	r = kobject_get_page(&dst->lb_ko, i, &dst_pg, page_excl_dirty);
	if (r < 0)
	    return r;

	memcpy(dst_pg, src_pg, PGSIZE);
    }

    *dstp = dst;
    return 0;
}

int
label_set(struct Label *l, uint64_t handle, level_t level)
{
    int slot_idx = -1;

    for (uint32_t i = 0; i < label_nslots(l); i++) {
	const uint64_t *entp;
	assert(0 == label_get_slotp_read(l, i, &entp));
	if (!*entp && slot_idx < 0) {
	    slot_idx = i;
	    continue;
	}

	if (*entp && LB_HANDLE(*entp) == handle) {
	    slot_idx = i;
	    break;
	}
    }

    if (slot_idx < 0) {
	slot_idx = label_nslots(l);
	int r = kobject_set_nbytes(&l->lb_ko, l->lb_ko.ko_nbytes + PGSIZE);
	if (r < 0)
	    return r;
    }

    uint64_t *entp;
    int r = label_get_slotp_write(l, slot_idx, &entp);
    if (r < 0)
	return r;

    *entp = (level == l->lb_def_level) ? 0 : LB_CODE(handle, level);
    return 0;
}

int
label_to_ulabel(const struct Label *l, struct ulabel *ul)
{
    int r = check_user_access(ul, sizeof(*ul), SEGMAP_WRITE);
    if (r < 0)
	return r;

    ul->ul_default = l->lb_def_level;
    ul->ul_nent = 0;
    uint32_t ul_size = ul->ul_size;
    uint64_t *ul_ent = ul->ul_ent;

    int mul_of = 0;
    r = check_user_access(ul_ent,
			  safe_mul(&mul_of, ul_size, sizeof(*ul_ent)),
			  SEGMAP_WRITE);
    if (r < 0)
	return r;

    if (mul_of)
	return -E_INVAL;

    uint32_t slot = 0;
    uint32_t overflow = 0;
    for (uint32_t i = 0; i < label_nslots(l); i++) {
	const uint64_t *entp;
	r = label_get_slotp_read(l, i, &entp);
	if (r < 0)
	    return r;

	if (!*entp)
	    continue;

	if (slot < ul_size) {
	    ul_ent[slot] = *entp;
	    slot++;
	} else {
	    overflow++;
	}
    }

    ul->ul_nent = slot;
    ul->ul_needed = overflow;

    return overflow ? -E_NO_SPACE : 0;
}

int
ulabel_to_label(struct ulabel *ul, struct Label *l)
{
    int r = check_user_access(ul, sizeof(*ul), 0);
    if (r < 0)
	return r;

    l->lb_def_level = ul->ul_default;
    uint32_t ul_nent = ul->ul_nent;
    uint64_t *ul_ent = ul->ul_ent;

    int mul_of = 0;
    r = check_user_access(ul_ent,
			  safe_mul(&mul_of, ul_nent, sizeof(*ul_ent)), 0);
    if (r < 0)
	return r;

    if (mul_of)
	return -E_INVAL;

    // XXX minor annoyance if ul_nent is huge
    for (uint32_t i = 0; i < ul_nent; i++) {
	uint64_t ul_val = ul_ent[i];

	int level = LB_LEVEL(ul_val);
	if (level < 0 && level > LB_LEVEL_STAR)
	    return -E_INVAL;

	r = label_set(l, LB_HANDLE(ul_val), level);
	if (r < 0)
	    return r;
    }

    return 0;
}

struct compare_cache_ent {
    kobject_id_t lhs;
    kobject_id_t rhs;
    level_comparator cmp;
};

enum { compare_cache_size = 16 };
static struct compare_cache_ent compare_cache[compare_cache_size];
static int compare_cache_next;

int
label_compare_id(kobject_id_t l1_id, kobject_id_t l2_id, level_comparator cmp)
{
    for (int i = 0; i < compare_cache_size; i++)
	if (compare_cache[i].lhs == l1_id &&
	    compare_cache[i].rhs == l2_id &&
	    compare_cache[i].cmp == cmp)
	    return 0;

    const struct kobject *l1_ko, *l2_ko;
    int r;

    r = kobject_get(l1_id, &l1_ko, kobj_label, iflow_none);
    if (r < 0)
	return r;

    r = kobject_get(l2_id, &l2_ko, kobj_label, iflow_none);
    if (r < 0)
	return r;

    return label_compare(&l1_ko->lb, &l2_ko->lb, cmp);
}

int
label_compare(const struct Label *l1,
	      const struct Label *l2, level_comparator cmp)
{
    assert(l1);
    assert(l2);

    kobject_id_t lhs_id = l1->lb_ko.ko_id;
    kobject_id_t rhs_id = l2->lb_ko.ko_id;

    for (int i = 0; i < compare_cache_size; i++)
	if (compare_cache[i].lhs == lhs_id &&
	    compare_cache[i].rhs == rhs_id &&
	    compare_cache[i].cmp == cmp)
	    return 0;

    level_comparator_init(cmp);

    const uint64_t *entp;
    int r;

    for (uint32_t i = 0; i < label_nslots(l1); i++) {
	r = label_get_slotp_read(l1, i, &entp);
	if (r < 0)
	    return r;

	if (!*entp)
	    continue;

	uint64_t h = LB_HANDLE(*entp);
	level_t lv1 = LB_LEVEL(*entp);
	r = cmp->cmp[lv1][label_get_level(l2, h)];
	if (r < 0)
	    return r;
    }

    for (uint32_t i = 0; i < label_nslots(l2); i++) {
	r = label_get_slotp_read(l2, i, &entp);
	if (r < 0)
	    return r;

	if (!*entp)
	    continue;

	uint64_t h = LB_HANDLE(*entp);
	level_t lv2 = LB_LEVEL(*entp);
	r = cmp->cmp[label_get_level(l1, h)][lv2];
	if (r < 0)
	    return r;
    }

    r = cmp->cmp[l1->lb_def_level][l2->lb_def_level];
    if (r < 0)
	return r;

    int cache_slot = (compare_cache_next++) % compare_cache_size;
    compare_cache[cache_slot].lhs = lhs_id;
    compare_cache[cache_slot].rhs = rhs_id;
    compare_cache[cache_slot].cmp = cmp;

    return 0;
}

int
label_max(const struct Label *a, const struct Label *b,
	  struct Label *dst, level_comparator leq)
{
    level_comparator_init(leq);
    dst->lb_def_level = leq->max[a->lb_def_level][b->lb_def_level];

    const uint64_t *entp;
    int r;

    for (uint32_t i = 0; i < label_nslots(a); i++) {
	r = label_get_slotp_read(a, i, &entp);
	if (r < 0)
	    return r;

	if (!*entp)
	    continue;

	uint64_t h = LB_HANDLE(*entp);
	level_t alv = LB_LEVEL(*entp);
	r = label_set(dst, h, leq->max[alv][label_get_level(b, h)]);
	if (r < 0)
	    return r;
    }

    for (uint32_t i = 0; i < label_nslots(b); i++) {
	r = label_get_slotp_read(b, i, &entp);
	if (r < 0)
	    return r;

	if (!*entp)
	    continue;

	uint64_t h = LB_HANDLE(*entp);
	level_t blv = LB_LEVEL(*entp);
	r = label_set(dst, h, leq->max[label_get_level(a, h)][blv]);
	if (r < 0)
	    return r;
    }

    return 0;
}

///////////////////////////
// Label pretty-printing
///////////////////////////

void
label_cprint(const struct Label *l)
{
    cprintf("Label %p: {", l);
    for (uint32_t i = 0; i < label_nslots(l); i++) {
	const uint64_t *entp;
	assert(0 == label_get_slotp_read(l, i, &entp));

	uint64_t ent = *entp;
	if (ent) {
	    level_t level = LB_LEVEL(ent);
	    char lchar[2];
	    if (level == LB_LEVEL_STAR)
		lchar[0] = '*';
	    else
		snprintf(&lchar[0], 2, "%d", level);
	    cprintf(" %lu:%c,", LB_HANDLE(ent), lchar[0]);
	}
    }
    cprintf(" %d }\n", l->lb_def_level);
}
