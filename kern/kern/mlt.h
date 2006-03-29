#ifndef JOS_KERN_MLT_H
#define JOS_KERN_MLT_H

#include <machine/types.h>
#include <kern/kobjhdr.h>
#include <kern/label.h>
#include <inc/mlt.h>

struct Mlt {
    struct kobject_hdr mt_ko;
};

struct mlt_entry {
    uint64_t me_lb_id;
    uint8_t me_inuse;
    uint8_t me_buf[MLT_BUF_SIZE];
};

int  mlt_alloc(const struct Label *l, struct Mlt **mtp)
    __attribute__ ((warn_unused_result));
int  mlt_gc(struct Mlt *mlt)
    __attribute__ ((warn_unused_result));

int  mlt_put(const struct Mlt *mlt, const struct Label *l, uint8_t *buf)
    __attribute__ ((warn_unused_result));
int  mlt_get(const struct Mlt *mlt, uint64_t idx, const struct Label **l, uint8_t *buf)
    __attribute__ ((warn_unused_result));

#endif
