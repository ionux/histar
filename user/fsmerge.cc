extern "C" {
#include <inc/fs.h>
#include <inc/lib.h>
#include <inc/error.h>
#include <inc/syscall.h>
#include <string.h>
#include <stdlib.h>
}

#include <inc/error.hh>
#include <inc/scopeguard.hh>

static int merge_debug = 0;

static void
merge_mlt_file(struct fs_inode file, struct fs_inode dst)
{
    int type = sys_obj_get_type(file.obj);
    error_check(type);

    // we don't really know how to merge containers quite yet,
    // need to change cobj_get() to do iflow_none on the container
    // if the object == container itself.
    if (type == kobj_container)
	return;

    if (type != kobj_segment) {
	printf("fs_merge: funny object %lu.%lu type %d\n",
	       file.obj.container, file.obj.object, type);
	return;
    }

    char name[KOBJ_NAME_LEN];
    error_check(sys_obj_get_name(file.obj, &name[0]));
    if (!strcmp(&name[0], "directory segment"))
	return;

    // check if this file has already been linked into dst
    type = sys_obj_get_type(COBJ(dst.obj.object, file.obj.object));
    if (type >= 0)
	return;

    if (merge_debug)
	printf("trying to link %lu.%lu into %lu\n",
	       file.obj.container, file.obj.object,
	       dst.obj.object);

    error_check(fs_link(dst, &name[0], file));
}

static void
merge_mlt_slot(struct fs_inode mct, struct fs_inode dst)
{
    if (mct.obj.object == dst.obj.object)
	return;

    if (merge_debug)
	printf("found non-trivial MLT slot %lu\n", mct.obj.object);

    struct fs_readdir_state s;
    error_check(fs_readdir_init(&s, mct));
    scope_guard<void, fs_readdir_state *> g(fs_readdir_close, &s);

    for (;;) {
	struct fs_dent de;
	int r = fs_readdir_dent(&s, &de, 0);
	if (r < 0)
	    throw error(r, "merge_mlt_slot: fs_readdir_dent");
	if (r == 0)
	    break;

	merge_mlt_file(de.de_inode, dst);
    }
}

static void
merge_mlt(struct fs_inode src, struct fs_inode dst)
{
    struct fs_readdir_state s;
    error_check(fs_readdir_init(&s, src));
    scope_guard<void, fs_readdir_state *> g(fs_readdir_close, &s);

    for (uint64_t i = 0; ; i++) {
	struct fs_dent de;
	int r = fs_readdir_dent(&s, &de, 0);
	if (r < 0)
	    throw error(r, "merge_mlt: fs_readdir_dent");
	if (r == 0)
	    break;

	merge_mlt_slot(de.de_inode, dst);
    }
}

int
main(int ac, char **av)
try
{
    if (ac != 3) {
	printf("Usage: %s mlt declassify-into\n", av[0]);
	exit(-1);
    }

    const char *src_pn = av[1];
    const char *dst_pn = av[2];

    struct fs_inode src;
    error_check(fs_namei(src_pn, &src));

    struct fs_inode dst;
    error_check(fs_namei(dst_pn, &dst));

    printf("fsmerge: merging MLT %s into %s\n", src_pn, dst_pn);

    for (;;) {
	if (merge_debug)
	    printf("fsmerge: trying to declassify MLT..\n");

	merge_mlt(src, dst);
	thread_sleep(1000);
    }
} catch (std::exception &e) {
    printf("fsmerge: %s\n", e.what());
}
