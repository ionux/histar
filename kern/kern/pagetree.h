#ifndef JOS_KERN_PAGETREE_H
#define JOS_KERN_PAGETREE_H

#include <machine/types.h>
#include <machine/memlayout.h>
#include <machine/mmu.h>
#include <inc/safetype.h>
#include <inc/queue.h>

typedef struct pagetree_entry {
    /* actual page of memory referenced by this entry */
    void *page;

    /* either pagetree_indirect_page (low bit set) or kobject (low clear) */
    void *parent;

    /* all pagetree_entry's that reference this->page */
    LIST_ENTRY(pagetree_entry) link;
} pagetree_entry;

LIST_HEAD(pagetree_entry_list, pagetree_entry);

#define PAGETREE_DIRECT_PAGES		4
#define PAGETREE_INDIRECTS		4
#define PAGETREE_ENTRIES_PER_PAGE	(PGSIZE / sizeof(pagetree_entry))

/*
 * The page tree can store slightly over
 * PAGETREE_ENTRIES_PER_PAGE^PAGETREE_INDIRECTS pages.
 */

struct pagetree {
    pagetree_entry pt_direct[PAGETREE_DIRECT_PAGES];
    pagetree_entry pt_indirect[PAGETREE_INDIRECTS];
};

struct pagetree_indirect_page {
    pagetree_entry pt_entry[PAGETREE_ENTRIES_PER_PAGE];
};

typedef SAFE_TYPE(int) page_sharing_mode;
#define page_shared_ro		SAFE_WRAP(page_sharing_mode, 1)
#define page_excl_dirty		SAFE_WRAP(page_sharing_mode, 2)
#define page_excl_dirty_later	SAFE_WRAP(page_sharing_mode, 3)

// Fancy API for zeroing out & setting parent pointers
void pagetree_init(struct pagetree *pt, void *kobjptr);

// Copy (with COW) src into dst
int  pagetree_copy(const struct pagetree *src, struct pagetree *dst,
		   void *kobjptr, int *share_write_pinned)
    __attribute__ ((warn_unused_result));

// Free the pagetree, including all of the pages (that aren't shared)
void pagetree_free(struct pagetree *pt, int was_share_write_pinned);

// Get a page currently stored in the page tree
// Returns 1 if copy-on-write happened, 0 if not, negative for error
int  pagetree_get_page(struct pagetree *pt, uint64_t npage, void **pagep,
		       page_sharing_mode rw)
    __attribute__ ((warn_unused_result));

// Put a page into the page tree
int  pagetree_put_page(struct pagetree *pt, uint64_t npage, void *page)
    __attribute__ ((warn_unused_result));

// Max number of pages in a pagetree
uint64_t pagetree_maxpages(void);

// Pin/unpin pages in a pagetree
void pagetree_incpin_write(void *p);
void pagetree_decpin_write(void *p);
void pagetree_incpin_read(void *p);
void pagetree_decpin_read(void *p);

#endif
