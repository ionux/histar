#ifndef JOS_INC_CONTAINER_H
#define JOS_INC_CONTAINER_H

#include <inc/types.h>

typedef enum {
    cobj_none,
    cobj_container,
    cobj_thread,
    cobj_gate,
    cobj_segment,

    cobj_any
} container_object_type;

struct cobj_ref {
    uint64_t container;
    uint64_t slot;
};

#define COBJ(container, slot)	((struct cobj_ref) { (container), (slot) } )

#endif
