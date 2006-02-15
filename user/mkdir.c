#include <inc/stdio.h>
#include <inc/lib.h>
#include <inc/error.h>
#include <inc/fs.h>

int
main(int ac, char **av)
{
    if (ac != 2) {
	printf("Usage: %s pathname\n", av[0]);
	return -1;
    }

    char *pn = av[1];
    const char *dname, *fn;

    fs_dirbase(pn, &dname, &fn);

    struct fs_inode dir;
    int r = fs_namei(dname, &dir);
    if (r < 0) {
	printf("cannot lookup %s: %s\n", dname, e2s(r));
	return r;
    }

    struct fs_inode ndir;
    r = fs_mkdir(dir, fn, &ndir);
    if (r < 0) {
	printf("cannot mkdir %s: %s\n", fn, e2s(r));
	return r;
    } 
}
