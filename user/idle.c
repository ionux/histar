#include <inc/syscall.h>

int
main(int ac, char **av)
{
    for (;;)
	sys_thread_yield();
}
