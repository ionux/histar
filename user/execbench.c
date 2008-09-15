#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/wait.h>

int
main(int ac, char **av)
{
    if (ac != 2) {
	printf("Usage: %s count\n", av[0]);
	exit(-1);
    }

    uint32_t count = atoi(av[1]);
    if (count == 0) {
	printf("Bad count\n");
	exit(-1);
    }

    struct timeval start;
    gettimeofday(&start, 0);

    uint32_t i;
    for (i = 0; i < count; i++) {
	pid_t pid = fork();
	if (pid == 0) {
	    execl("/bin/true", "true", 0);
	    perror("exec");
	    exit(-1);
	}

	int status;
	waitpid(pid, &status, 0);
    }

    struct timeval end;
    gettimeofday(&end, 0);

    uint64_t diff_usec =
	(end.tv_sec - start.tv_sec) * 1000000 +
	end.tv_usec - start.tv_usec;
    printf("Total time: %"PRIu64" usec\n", diff_usec);
    printf("usec per rtt: %"PRIu64"\n", diff_usec / count);
}
