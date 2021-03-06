/* See COPYRIGHT for copyright information. */

#ifndef JOS_INC_ASSERT_H
#define JOS_INC_ASSERT_H

#include <inc/stdio.h>

void _panic(const char*, int, const char*, ...)
	__attribute__((noreturn))
	__attribute__((__format__ (__printf__, 3, 4)));

#define panic(...) _panic(__FILE__, __LINE__, __VA_ARGS__)

#define assert(x)		\
	do { if (!(x)) panic("assertion failed: %s", #x); } while (0)

/*
 * static_assert(x) will generate a compile-time error if 'x' is false.
 */
#define static_assert(x) do { switch (x) { default: case 0: case (x): break; } } while (0)

#endif /* !JOS_INC_ASSERT_H */
