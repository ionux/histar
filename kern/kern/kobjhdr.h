#ifndef JOS_KERN_KOBJHDR_H
#define JOS_KERN_KOBJHDR_H

#include <machine/types.h>
#include <inc/kobj.h>
#include <inc/queue.h>
#include <inc/safetype.h>

enum {
    kolabel_contaminate,
    kolabel_clearance,
    kolabel_verify_contaminate,
    kolabel_verify_clearance,
    kolabel_max
};

#define KOBJ_ON_DISK		0x0002	// Might have some version on disk
#define KOBJ_SNAPSHOTING	0x0004	// Being written out to disk
#define KOBJ_DIRTY		0x0008	// Modified since last swapin/out
#define KOBJ_SNAPSHOT_DIRTY	0x0010	// Dirty if swapout fails
#define KOBJ_LABEL_MUTABLE	0x0020	// Label can change after creation
#define KOBJ_FIXED_QUOTA	0x0040	// Cannot modify quota (for hard links)
#define KOBJ_SHARED_MAPPINGS	0x0080	// Shared pages maybe mapped somewhere
#define KOBJ_DIRTY_LATER	0x0100	// Need to collect dirty bits
#define KOBJ_READONLY		0x0200	// Cannot be modified

struct kobject_hdr {
    kobject_id_t ko_id;
    kobject_type_t ko_type;

    uint64_t ko_ref;	// persistent references (via containers or TLS)

    // Bytes reserved for this object.  Counts towards the parent container's
    // ko_quota_used, and represents the space that is taken up (or could be
    // taken up, without information flow) by this object.
    uint64_t ko_quota_total;

    // Number of bytes that this object is actually taking up.
    // Must be <= ko_quota_total, unless ko_quota_total == CT_QUOTA_INF.
    uint64_t ko_quota_used;

    // Parent object that holds a reference on this object.  Used to borrow
    // quota when object grows (multi-homed objects cannot grow quota).  Also
    // used for container ".." (parent directory) tracking.
    uint64_t ko_parent;

    uint64_t ko_flags;
    uint64_t ko_nbytes;
    uint64_t ko_label[kolabel_max];	// id of label object (holds refcount)
    char ko_name[KOBJ_NAME_LEN];
    char ko_meta[KOBJ_META_LEN];

    // For verifying the persistence layer
    uint64_t ko_cksum;

    // When this object's data was written to disk
    uint64_t ko_sync_ts;

    // Ephemeral state (doesn't persist across swapout)
    uint32_t ko_pin_pg;	// pages are pinned (DMA, PTE)
    uint32_t ko_pin;	// header is pinned (linked lists)
} __attribute__((aligned (16)));

typedef SAFE_TYPE(int) info_flow_type;
#define iflow_read  SAFE_WRAP(info_flow_type, 1)
#define iflow_rw    SAFE_WRAP(info_flow_type, 3)
#define iflow_none  SAFE_WRAP(info_flow_type, 4)
#define iflow_alloc SAFE_WRAP(info_flow_type, 5)

struct kobject;

#endif
