#ifndef JOS_KERN_KOBJ_H
#define JOS_KERN_KOBJ_H

#include <kern/kobjhdr.h>

#include <machine/memlayout.h>
#include <kern/container.h>
#include <kern/segment.h>
#include <kern/gate.h>
#include <kern/label.h>
#include <kern/pagetree.h>
#include <kern/thread.h>
#include <kern/as.h>
#include <kern/netdev.h>

#define KOBJ_DISK_SIZE	512
#define KOBJ_MEM_SIZE	1024

struct kobject_persistent {
    union {
	struct kobject_hdr hdr;
	char disk_buf[KOBJ_DISK_SIZE];

	struct Container ct;
	struct Thread th;
	struct Gate gt;
	struct Address_space as;
	struct Segment sg;
	struct Label lb;
	struct Netdev nd;
    };
};

struct kobject_mem {
    struct kobject_persistent;
    struct pagetree ko_pt;

    LIST_ENTRY(kobject) ko_link;
    LIST_ENTRY(kobject) ko_hash;
    LIST_ENTRY(kobject) ko_gc_link;

    union {
	struct Thread_ephemeral ko_th_e;
    };
};

struct kobject {
    union {
	char mem_buf[KOBJ_MEM_SIZE];
	struct kobject_mem;
    };
};

struct kobject_pair {
    struct kobject active;
    struct kobject snapshot;
};

LIST_HEAD(kobject_list, kobject);
extern struct kobject_list ko_list;

struct kobject_quota_resv {
    struct kobject_hdr *qr_ko;
    uint64_t qr_nbytes;
};

void kobject_init(void);

int  kobject_get(kobject_id_t id, const struct kobject **kpp,
		 uint8_t type, info_flow_type iflow)
    __attribute__ ((warn_unused_result));
int  kobject_alloc(uint8_t type, const struct Label *contaminate,
		   const struct Label *clearance,
		   struct kobject **kpp)
    __attribute__ ((warn_unused_result));
int  kobject_alloc_real(uint8_t type, const struct Label *contaminate,
			const struct Label *clearance,
			struct kobject **kpp)
    __attribute__ ((warn_unused_result));
int  kobject_incore(kobject_id_t id)
    __attribute__ ((warn_unused_result));

int  kobject_set_nbytes(struct kobject_hdr *kp, uint64_t nbytes)
    __attribute__ ((warn_unused_result));
uint64_t
     kobject_npages(const struct kobject_hdr *kp);
int  kobject_get_page(const struct kobject_hdr *kp, uint64_t page_num,
		      void **pp, page_sharing_mode rw)
    __attribute__ ((warn_unused_result));

int  kobject_get_label(const struct kobject_hdr *kp, int idx,
		       const struct Label **lpp)
    __attribute__ ((warn_unused_result));
int  kobject_set_label(struct kobject_hdr *kp, int idx,
		       const struct Label *lp)
    __attribute__ ((warn_unused_result));
void kobject_set_label_prepared(struct kobject_hdr *kp, int idx,
				const struct Label *old_label,
				const struct Label *new_label,
				struct kobject_quota_resv *qr);

// Mark the kobject as dirty and return the same kobject
struct kobject *
     kobject_dirty(const struct kobject_hdr *kh);

// Do not mark the kobject as dirty, for ephemeral fields
struct kobject *
     kobject_ephemeral_dirty(const struct kobject_hdr *kh);

// Evaluate KOBJ_DIRTY_LATER as KOBJ_DIRTY or not
void kobject_dirty_eval(struct kobject *ko);

// The object has been brought in from disk.
void kobject_swapin(struct kobject *kp);

// Called when the object has been written out to disk and the
// in-memory copy should be discarded.
void kobject_swapout(struct kobject *kp);

// Perform a garbage-collection pass.
void kobject_gc_scan(void);

// Deallocate physical memory used for clean objects.
void kobject_reclaim(void);

void kobject_snapshot(struct kobject_hdr *kp);
void kobject_snapshot_release(struct kobject_hdr *kp);
struct kobject *
     kobject_get_snapshot(struct kobject_hdr *kp);
struct kobject *
     kobject_h2k(struct kobject_hdr *kh);
const struct kobject *
     kobject_ch2ck(const struct kobject_hdr *kh);

int  kobject_incref(const struct kobject_hdr *kp, struct kobject_hdr *refholder)
    __attribute__ ((warn_unused_result));
void kobject_incref_resv(const struct kobject_hdr *kp, struct kobject_quota_resv *resv);
void kobject_decref(const struct kobject_hdr *kp, struct kobject_hdr *refholder);

void kobject_pin_hdr(const struct kobject_hdr *kp);
void kobject_unpin_hdr(const struct kobject_hdr *kp);

void kobject_pin_page(const struct kobject_hdr *kp);
void kobject_unpin_page(const struct kobject_hdr *kp);

void kobject_negative_insert(kobject_id_t id);
void kobject_negative_remove(kobject_id_t id);
bool_t kobject_negative_contains(kobject_id_t id);

// object has to stay in-core or be brought in at startup
bool_t kobject_initial(const struct kobject *ko);

// copy pages from one kobject into another
int  kobject_copy_pages(const struct kobject_hdr *src,
			struct kobject_hdr *dst);

// Quota reservation for allocating all quota upfront
void kobject_qres_init(struct kobject_quota_resv *qr, struct kobject_hdr *ko);
int  kobject_qres_reserve(struct kobject_quota_resv *qr, const struct kobject_hdr *ko);
void kobject_qres_take(struct kobject_quota_resv *qr, const struct kobject_hdr *ko);
void kobject_qres_release(struct kobject_quota_resv *qr);

#endif
