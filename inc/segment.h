#ifndef JOS_INC_SEGMENT_H
#define JOS_INC_SEGMENT_H

#include <inc/types.h>
#include <inc/container.h>

/*
 * Must define at least one of these for the entry to be valid.
 * These match with the ELF flags (inc/elf64.h).
 */
#define SEGMAP_EXEC		0x01
#define SEGMAP_WRITE		0x02
#define SEGMAP_READ		0x04

/*
 * Kernel-interpreted flags.
 */
#define SEGMAP_REVERSE_PAGES	0x08

/*
 * User-interpreted flags
 */
#define SEGMAP_CLOEXEC		0x0100
#define SEGMAP_DELAYED_UNMAP	0x0200
#define SEGMAP_ANON_MMAP	0x0400
#define SEGMAP_VECTOR_PF	0x0800
#define SEGMAP_STACK		0x1000

struct u_segment_mapping {
    struct cobj_ref segment;
    uint64_t start_page;
    uint64_t num_pages;
    uint32_t kslot;
    uint32_t flags;
    void *va;
};

struct u_address_space {
    uint64_t size;
    uint64_t nent;
    struct u_segment_mapping *ents;
    void *trap_handler;
    void *trap_stack_base;
    void *trap_stack_top;
};

#endif
