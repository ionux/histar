#include <stdlib.h>
#include <stdio.h>

#include <kern/arch.h>
#include <kern/lib.h>
#include <assert.h>

static const char *panicstr;

void
_panic(const char *file, int line, const char *fmt, ...)
{
    if (panicstr)
	assert(0);
    panicstr = fmt;

    va_list ap;
    va_start(ap, fmt);
    printf("[%"PRIu64"] kpanic: %s:%d: ",
	    cur_thread ? cur_thread->th_ko.ko_id : 0,
	    file, line);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);

    assert(0);
}