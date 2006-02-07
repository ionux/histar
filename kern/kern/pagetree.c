#include <machine/pmap.h>
#include <kern/pagetree.h>
#include <kern/lib.h>
#include <inc/error.h>

struct pagetree_page *pt_pages;

static struct pagetree_page *
page_to_ptp(void *p)
{
    ppn_t pn = pa2ppn(kva2pa(p));
    return &pt_pages[pn];
}

static void pagetree_decref(void *p);

static void
pagetree_free_page(void *p)
{
    struct pagetree_page *ptp = page_to_ptp(p);
    assert(ptp->pg_ref == 0);
    assert(ptp->pg_pin == 0);

    if (ptp->pg_indir) {
	struct pagetree_indirect_page *pip = p;
	for (uint32_t i = 0; i < PAGETREE_ENTRIES_PER_PAGE; i++) {
	    if (pip->pt_entry[i].page) {
		pagetree_decref(pip->pt_entry[i].page);
		pip->pt_entry[i].page = 0;
	    }
	}
    }

    memset(ptp, 0, sizeof(*ptp));
    page_free(p);
}

static void
pagetree_decref(void *p)
{
    struct pagetree_page *ptp = page_to_ptp(p);
    if (--ptp->pg_ref == 0)
	pagetree_free_page(p);
}

static void
pagetree_incref(void *p)
{
    struct pagetree_page *ptp = page_to_ptp(p);
    ptp->pg_ref++;
}

static void
pagetree_indir_copy(void *src, void *dst)
{
    struct pagetree_indirect_page *pdst = dst;

    for (uint32_t i = 0; i < PAGETREE_ENTRIES_PER_PAGE; i++)
	if (pdst->pt_entry[i].page)
	    pagetree_incref(pdst->pt_entry[i].page);

    assert(page_to_ptp(src)->pg_indir);
    page_to_ptp(dst)->pg_indir = 1;
}

static int
pagetree_cow(pagetree_entry *ent, void (*copy_cb) (void *, void *))
{
    if (!ent->page)
	return 0;

    struct pagetree_page *ptp = page_to_ptp(ent->page);
    assert(ptp->pg_ref > 0);

    if (ptp->pg_ref > 1) {
	void *copy;

	int r = page_alloc(&copy);
	if (r < 0)
	    return r;

	memcpy(copy, ent->page, PGSIZE);
	pagetree_incref(copy);

	if (copy_cb)
	    copy_cb(ent->page, copy);

	pagetree_decref(ent->page);
	ent->page = copy;
    }

    return 0;
}

void
pagetree_init(struct pagetree *pt)
{
    memset(pt, 0, sizeof(*pt));
}

void
pagetree_copy(struct pagetree *src, struct pagetree *dst)
{
    memcpy(dst, src, sizeof(*dst));

    // XXX need to deal with pinned pages...

    for (int i = 0; i < PAGETREE_DIRECT_PAGES; i++)
	if (dst->pt_direct[i].page)
	    pagetree_incref(dst->pt_direct[i].page);

    for (int i = 0; i < PAGETREE_INDIRECTS; i++)
	if (dst->pt_indirect[i].page)
	    pagetree_incref(dst->pt_indirect[i].page);
}

static void
pagetree_free_ent(pagetree_entry *ent)
{
    if (ent->page) {
	pagetree_decref(ent->page);
	ent->page = 0;
    }
}

void
pagetree_free(struct pagetree *pt)
{
    for (int i = 0; i < PAGETREE_DIRECT_PAGES; i++)
	pagetree_free_ent(&pt->pt_direct[i]);

    for (int i = 0; i < PAGETREE_INDIRECTS; i++)
	pagetree_free_ent(&pt->pt_indirect[i]);

    pagetree_init(pt);
}

static int
pagetree_get_entp_indirect(pagetree_entry *indir, uint64_t npage,
			   pagetree_entry **outp, page_rw_mode rw,
			   int level)
{
    if (rw == page_rw)
	pagetree_cow(indir, &pagetree_indir_copy);

    if (indir->page == 0) {
	if (rw == page_ro) {
	    *outp = 0;
	    return 0;
	}

	int r = page_alloc(&indir->page);
	if (r < 0)
	    return r;

	memset(indir->page, 0, PGSIZE);
	pagetree_incref(indir->page);
	page_to_ptp(indir->page)->pg_indir = 1;
    }

    struct pagetree_indirect_page *pip = indir->page;
    if (level == 0) {
	*outp = &pip->pt_entry[npage];
	return 0;
    }

    uint64_t n_pages_per_pip_entry = 1;
    for (int i = 0; i < level; i++)
	n_pages_per_pip_entry *= PAGETREE_ENTRIES_PER_PAGE;

    uint32_t next_slot = npage / n_pages_per_pip_entry;
    uint32_t next_page = npage % n_pages_per_pip_entry;

    assert(next_slot < PAGETREE_ENTRIES_PER_PAGE);
    return pagetree_get_entp_indirect(&pip->pt_entry[next_slot],
				      next_page, outp, rw, level - 1);
}

static int
pagetree_get_entp(struct pagetree *pt, uint64_t npage,
		  pagetree_entry **entp, page_rw_mode rw)
{
    if (npage < PAGETREE_DIRECT_PAGES) {
	*entp = &pt->pt_direct[npage];
	return 0;
    }
    npage -= PAGETREE_DIRECT_PAGES;

    uint64_t num_indirect_pages = 1;
    for (int i = 0; i < PAGETREE_INDIRECTS; i++) {
	num_indirect_pages *= PAGETREE_ENTRIES_PER_PAGE;
	if (npage < num_indirect_pages)
	    return pagetree_get_entp_indirect(&pt->pt_indirect[i],
					      npage, entp, rw, i);
	npage -= num_indirect_pages;
    }

    cprintf("pagetree_get_entp: %ld leftover!\n", npage);
    return -E_NO_SPACE;
}

int
pagetree_get_page(struct pagetree *pt, uint64_t npage,
		  void **pagep, page_rw_mode rw)
{
    pagetree_entry *ent;
    int r = pagetree_get_entp(pt, npage, &ent, rw);
    if (r < 0)
	return r;

    void *page = ent ? ent->page : 0;
    if (rw == page_ro || page == 0) {
	*pagep = page;
	return 0;
    }

    pagetree_cow(ent, 0);
    *pagep = ent->page;
    return 0;
}

int
pagetree_put_page(struct pagetree *pt, uint64_t npage, void *page)
{
    pagetree_entry *ent;
    int r = pagetree_get_entp(pt, npage, &ent, page_rw);
    if (r < 0)
	return r;

    assert(ent != 0);
    if (ent->page)
	pagetree_decref(ent->page);

    ent->page = page;
    pagetree_incref(page);

    return 0;
}

uint64_t
pagetree_maxpages()
{
    uint64_t npages = PAGETREE_DIRECT_PAGES;

    uint64_t indirs = 1;
    for (int i = 0; i < PAGETREE_INDIRECTS; i++) {
	indirs *= PAGETREE_ENTRIES_PER_PAGE;
	npages += indirs;
    }

    return npages;
}
