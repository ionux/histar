#ifndef JOS_KERN_LIB_H
#define JOS_KERN_LIB_H

#include <machine/types.h>
#include <stdarg.h>	// gcc header file

void *memset(void *dest, int c, size_t len);
void *memcpy(void *dest, const void *src, size_t len);
void *memmove(void *dest, const void *src, size_t len);
int   memcmp(const void *s1, const void *s2, size_t n);
char *strchr (const char *p, int ch);
int strcmp (const char *s1, const char *s2);
int strncmp (const char *p, const char *q, size_t n);
size_t strlen (const char *);
char *strncpy(char *dest, const char *src, size_t size);
char *strstr(const char *haystack, const char *needle);

// printfmt.c
int vsnprintf(char *str, size_t size, const char *fmt, va_list)
	__attribute__((__format__ (__printf__, 3, 0)));
void printfmt (void (*putch) (int, void *), void *putdat,
	const char *fmt, ...)
	__attribute__((__format__ (__printf__, 3, 4)));
void vprintfmt (void (*putch) (int, void *), void *putdat,
	const char *fmt, va_list ap)
	__attribute__((__format__ (__printf__, 3, 0)));
int vcprintf (const char *fmt, va_list ap)
	__attribute__((__format__ (__printf__, 1, 0)));
int cprintf (const char *fmt, ...)
	__attribute__((__format__ (__printf__, 1, 2)));
int snprintf(char *str, size_t size, const char *fmt, ...)
	__attribute__((__format__ (__printf__, 3, 4)));
int sprintf(char *str, const char *fmt, ...)
	__attribute__((__format__ (__printf__, 2, 3)));

const char *e2s(int err);
const char *syscall2s(int sys);

void abort (void) __attribute__((__noreturn__));
void _panic (const char *file, int line, const char *fmt, ...)
	__attribute__((__format__ (__printf__, 3, 4)))
	__attribute__((__noreturn__));
#define panic(fmt, varargs...) _panic(__FILE__, __LINE__, fmt, ##varargs)

#define __stringify(s) #s
#define stringify(s) __stringify(s)
#define __FL__ __FILE__ ":" stringify (__LINE__)

#define assert(x)				\
    do {					\
	if (!(x))				\
	    panic("assertion failed:\n%s", #x);	\
    } while (0)

// static_assert(x) will generate a compile-time error if 'x' is false.
#define static_assert(x) do { switch (x) { default: case 0: case (x): break; } } while (0)

/*
 * Page allocation
 */
extern uint64_t global_npages;

extern struct page_stats {
    uint64_t pages_used;
    uint64_t pages_avail;
    uint64_t allocations;
    uint64_t failures;
} page_stats;

void page_alloc_init(void);
int  page_alloc(void **p)
    __attribute__ ((warn_unused_result));
void page_free(void *p);

#endif /* !JOS_KERN_LIB_H */
