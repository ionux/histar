#ifndef JOS_INC_FD_H
#define JOS_INC_FD_H

#include <inc/types.h>

// pre-declare for forward references
struct Fd;
struct Dev;

struct Dev
{
	int dev_id;
	char *dev_name;
	int (*dev_read)(struct Fd *fd, void *buf, size_t len, off_t offset);
	int (*dev_write)(struct Fd *fd, const void *buf, size_t len, off_t offset);
	int (*dev_close)(struct Fd *fd);
	int (*dev_seek)(struct Fd *fd, off_t pos);
	int (*dev_trunc)(struct Fd *fd, off_t length);
};

struct Fd
{
	int fd_dev_id;
	off_t fd_offset;
	int fd_omode;
	union {
		struct {
			int s;
		} sock;
	} fd_data;
};

char*	fd2data(struct Fd *fd);
int	fd2num(struct Fd *fd);
int	fd_alloc(struct Fd **fd_store);
int	fd_close(struct Fd *fd, bool must_exist);
int	fd_lookup(int fdnum, struct Fd **fd_store);
int	dev_lookup(int devid, struct Dev **dev_store);

extern struct Dev devcons;
extern struct Dev devsock;

#define O_RDONLY	0x0000
#define O_WRONLY	0x0001
#define O_RDWR		0x0002
#define O_ACCMODE	(O_RDONLY | O_RDWR | O_WRONLY)

#endif
