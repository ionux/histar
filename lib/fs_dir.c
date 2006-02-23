#include <inc/fs.h>
#include <inc/syscall.h>
#include <inc/lib.h>
#include <inc/string.h>
#include <inc/error.h>
#include <inc/memlayout.h>
#include <inc/mlt.h>
#include <inc/pthread.h>

static int fs_debug = 0;
static int fs_label_debug = 0;

struct fs_dirslot {
    uint64_t id;
    uint8_t inuse;
    char name[KOBJ_NAME_LEN];
};

struct fs_directory {
    pthread_mutex_t lock;
    struct fs_dirslot slots[0];
};

struct fs_opendir {
    struct fs_inode ino;
    int opened;
    struct fs_directory *dir;
    uint64_t dirsize;
};

/*
int
fs_dir_open(struct fs_opendir *s, struct fs_inode dir)
{
    
}
*/

static struct ulabel *
fs_get_label(void)
{
    struct ulabel *l = label_get_current();
    if (l)
	label_change_star(l, l->ul_default);
    return l;
}

static int
fs_get_dent_ct(struct fs_inode d, uint64_t n, struct fs_dent *e)
{
    int64_t slot_id = sys_container_get_slot_id(d.obj.object, n);
    if (slot_id < 0) {
	if (slot_id == -E_INVAL)
	    return -E_RANGE;
	if (slot_id == -E_NOT_FOUND)
	    return -E_NOT_FOUND;
	return slot_id;
    }

    e->de_inode.obj = COBJ(d.obj.object, slot_id);

    int r = sys_obj_get_name(e->de_inode.obj, &e->de_name[0]);
    if (r < 0)
	return r;

    return 0;
}

static int
fs_get_dent_mlt(struct fs_inode d, uint64_t n, struct fs_dent *e)
{
    char buf[MLT_BUF_SIZE];
    uint64_t ct;
    int r = sys_mlt_get(d.obj, n, 0, &buf[0], &ct);
    if (r < 0) {
	if (r == -E_NOT_FOUND)
	    return -E_RANGE;
	return r;
    }

    e->de_inode.obj = COBJ(ct, ct);
    snprintf(&e->de_name[0], sizeof(e->de_name), "%lu", ct);
    return 0;
}

int
fs_get_dent(struct fs_inode d, uint64_t n, struct fs_dent *e)
{
    int type = sys_obj_get_type(d.obj);
    if (type < 0)
	return type;

    // For the debugging cprintf further down
    e->de_name[0] = '\0';

    int r = -E_INVAL;
    if (type == kobj_container)
	r = fs_get_dent_ct(d, n, e);
    else if (type == kobj_mlt)
	r = fs_get_dent_mlt(d, n, e);

    if (fs_debug)
	cprintf("fs_get_dent: dir %ld r %d obj %ld name %s\n",
		d.obj.object, r, e->de_inode.obj.object,
		&e->de_name[0]);

    return r;
}

int
fs_lookup_one(struct fs_inode dir, const char *fn, struct fs_inode *o)
{
    if (fs_debug)
	cprintf("fs_lookup_one: dir %ld fn %s\n",
		dir.obj.object, fn);

    for (int i = 0; i < FS_NMOUNT; i++) {
	struct fs_mtab_ent *mnt = &start_env->fs_mtab.mtab_ent[i];
	if (mnt->mnt_dir.obj.object == dir.obj.object &&
	    !strcmp(&mnt->mnt_name[0], fn))
	{
	    *o = mnt->mnt_root;
	    return 0;
	}
    }

    // Simple MLT support
    if (!strcmp(fn, "@mlt")) {
	char blob[MLT_BUF_SIZE];
	uint64_t ct;
	int retry_count = 0;
	struct ulabel *lcur, *lslot;

retry:
	lcur = label_get_current();
	if (lcur == 0)
	    return -E_NO_MEM;

	lslot = label_alloc();
	if (lslot == 0)
	    return -E_NO_MEM;

	int r = -1;

	for (uint64_t slot = 0; r < 0; slot++) {
	    r = sys_mlt_get(dir.obj, slot, lslot, &blob[0], &ct);
	    if (r < 0) {
		if (r == -E_NOT_FOUND)
		    break;
		return r;
	    }

	    r = label_compare(lcur, lslot, label_leq_starlo);
	    if (r == 0)
		r = label_compare(lslot, lcur, label_leq_starhi);
	}

	if (r < 0) {
	    if (retry_count++ > 0)
		return -E_INVAL;

	    label_change_star(lcur, lcur->ul_default);

	    if (fs_label_debug)
		cprintf("Creating MLT branch with label %s\n",
			label_to_string(lcur));

	    r = sys_mlt_put(dir.obj, lcur, &blob[0]);
	    if (r < 0)
		return r;

	    label_free(lslot);
	    label_free(lcur);
	    goto retry;
	}

	label_free(lslot);
	label_free(lcur);

	o->obj = COBJ(ct, ct);
	return 0;
    }

    struct fs_dent de;
    int n = 0;

    for (;;) {
	int r = fs_get_dent(dir, n++, &de);
	if (r < 0) {
	    if (r == -E_NOT_FOUND)
		continue;
	    if (r == -E_RANGE)
		return -E_NOT_FOUND;
	    return r;
	}

	if (!strcmp(fn, de.de_name)) {
	    *o = de.de_inode;
	    return 0;
	}
    }
}

int
fs_lookup_path(struct fs_inode dir, const char *pn, struct fs_inode *o)
{
    *o = dir;

    while (pn[0] != '\0') {
	const char *name_end = strchr(pn, '/');
	const char *next_pn = name_end ? name_end + 1 : "";
	size_t namelen = name_end ? (size_t) (name_end - pn) : strlen(pn);

	char fn[KOBJ_NAME_LEN];
	if (namelen >= sizeof(fn))
	    return -E_RANGE;

	strncpy(&fn[0], pn, namelen + 1);
	fn[namelen] = '\0';

	if (fn[0] != '\0') {
	    int r = fs_lookup_one(*o, &fn[0], o);
	    if (r < 0)
		return r;
	}

	pn = next_pn;
    }

    return 0;
}

int
fs_namei(const char *pn, struct fs_inode *o)
{
    struct fs_inode d = pn[0] == '/' ? start_env->fs_root : start_env->fs_cwd;
    return fs_lookup_path(d, pn, o);
}

int
fs_mkdir(struct fs_inode dir, const char *fn, struct fs_inode *o)
{
    struct ulabel *l = fs_get_label();
    if (l == 0)
	return -E_NO_MEM;

    int64_t r = sys_container_alloc(dir.obj.object, l, fn);
    label_free(l);
    if (r < 0)
	return r;

    o->obj = COBJ(dir.obj.object, r);
    return 0;
}

int
fs_create(struct fs_inode dir, const char *fn, struct fs_inode *f)
{
    // XXX not atomic
    int r = fs_lookup_one(dir, fn, f);
    if (r >= 0)
	return -E_EXISTS;

    struct ulabel *l = fs_get_label();
    if (l == 0)
	return -E_NO_MEM;

    int64_t id = sys_segment_create(dir.obj.object, 0, l, fn);

    if (fs_label_debug)
	cprintf("Creating file with label %s\n", label_to_string(l));

    label_free(l);
    if (id < 0)
	return id;

    f->obj = COBJ(dir.obj.object, id);
    return 0;
}

int
fs_remove(struct fs_inode f)
{
    return sys_obj_unref(f.obj);
}

void
fs_dirbase(char *pn, const char **dirname, const char **basename)
{
    char *slash = strrchr(pn, '/');
    if (slash == 0) {
	*dirname = "";
	*basename = pn;
    } else {
	*slash = '\0';
	*dirname = pn;
	*basename = slash + 1;
    }
}
