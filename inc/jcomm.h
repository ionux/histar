#ifndef JOS_INC_JCOMM_H
#define JOS_INC_JCOMM_H

#include <inc/container.h>
#include <inc/multisync.h>
#include <inc/label.h>

enum { jlink_bufsz = 4000 };

struct jlink {
    char buf[jlink_bufsz];
    char reader_waiting;
    char writer_waiting;
    uint64_t open;
    uint32_t read_ptr;  /* read at this offset */
    uint64_t bytes; /* # bytes in circular buffer */
    jthread_mutex_t mu;
};

struct jcomm {
    uint64_t segment;
    uint16_t mode;
    char a;
};

struct jcomm_ref {
    uint64_t container;
    struct jcomm jc;
};

#define JCOMM(container, comm) \
        ((struct jcomm_ref) { (container), (comm) })

#define JCOMM_NONBLOCK 0x0001
#define JCOMM_PACKET   0x0002

#define JCOMM_SHUT_RD  0x0001
#define JCOMM_SHUT_WR  0x0002

int jcomm_alloc(uint64_t ct, struct ulabel *l, int16_t mode,
		struct jcomm_ref *a, struct jcomm_ref *b);
int jcomm_probe(struct jcomm_ref jr, dev_probe_t probe);
int jcomm_shut(struct jcomm_ref jr, uint16_t how);
int jcomm_multisync(struct jcomm_ref jr, dev_probe_t probe, struct wait_stat *wstat);

int jcomm_addref(struct jcomm_ref jr, uint64_t ct);
int jcomm_unref(struct jcomm_ref jr);

int64_t jcomm_read(struct jcomm_ref jr, void *buf, uint64_t cnt);
int64_t jcomm_write(struct jcomm_ref jr, const void *buf, uint64_t cnt);

#endif
