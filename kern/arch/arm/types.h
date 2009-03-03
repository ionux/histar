#ifndef JOS_MACHINE_TYPES_H
#define JOS_MACHINE_TYPES_H

#ifndef NULL
#define NULL (0)
#endif

#ifndef inline
#define inline __inline__
#endif

// Represents true-or-false values
typedef int bool_t;

// Explicitly-sized versions of integer types
typedef __signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
#if __LONG_MAX__==9223372036854775807L
typedef long int64_t;
typedef unsigned long uint64_t;
#elif __LONG_LONG_MAX__==9223372036854775807LL
typedef long long int64_t;
typedef unsigned long long uint64_t;
#else
#error Missing 64-bit type
#endif
typedef uint64_t __uint64_t;

// Pointers and addresses are 32 bits long.
// We use pointer types to represent virtual addresses,
// uintptr_t to represent the numerical values of virtual addresses,
// and physaddr_t to represent physical addresses.
typedef int32_t intptr_t;
typedef uint32_t uintptr_t;
typedef uint32_t physaddr_t;

// Page numbers are 32 bits long.
typedef uint32_t ppn_t;

#include <stddef.h>	// gcc header file

#define PRIu64 "lld"
#define PRIx64 "llx"

// Machine-dependent bits for the page_info structure;
struct md_page_info {
	uint8_t **mpi_pmap_pvp;		// pgmap virtual page descriptors
	void     *mpi_pmap_free_list;	// Pmap 1k subpage free list pointer
};

#endif /* !JOS_MACHINE_TYPES_H */
