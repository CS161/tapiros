/*
 * IO-related system calls.
 *
 * Includes open(), read(), write(), close(), lseek(),
 * dup2(), chdir(), and __getcwd().
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <uio.h>
#include <vnode.h>
#include <vfs.h>
#include <addrspace.h>
#include <syscall.h>
#include <kern/fcntl.h>
#include <current.h>
#include <proc.h>


void vfiles_init(void) {
	vfiles = vfilearray_create();
	if(vfiles == NULL) {
		panic("vfilearray_create for vfiles failed\n");
	}

	spinlock_init(&gf_lock);

	char* console = kstrdup("con:");
	if(console == NULL) {
		panic("console string coudln't be allocated\n");
	}

	if(sys_open(console, O_RDONLY, NULL))
		panic("stdin open failed\n");
	kfree(console);

	console = kstrdup("con:");
	if(sys_open(console, O_WRONLY, NULL))
		panic("stdout open failed\n");
	kfree(console);

	console = kstrdup("con:");
	if(sys_open(console, O_WRONLY, NULL))
		panic("stderr open failed\n");
	kfree(console);
}

/*
 * Find the index of a free or matching vfile slot in vfiles.
 */
void set_vfile(struct vfile *vfile, int fd) {
	spinlock_acquire(&gf_lock);

	int max = vfilearray_num(vfiles);
	int slot = -1;
	for(int i = 0; i < max; i++) {
		if(VFILES(i) == NULL) {
			slot = i;
			break;
		}
	}

	if(slot < 0) {
		vfilearray_add(vfiles, vfile, NULL);
		CUR_FDS(fd) = max;
	}
	else {
		vfilearray_set(vfiles, slot, vfile);
		CUR_FDS(fd) = slot;
	}

	spinlock_release(&gf_lock);
}

int sys_open(char* pathname, int flags, int *retval) {
	int ret = 0;
	int fd = -1;
	for(int i = 0; i < MAX_FDS; i++) {
		if(CUR_FDS(i) == -1) {
			fd = i;
			break;
		}
	}
	if(fd == -1) {
		ret = EMFILE;
		goto err1;
	}

	struct vfile *vf = kmalloc(sizeof(struct vfile *));
	if(vf == NULL) {
		ret = ENOMEM;
		goto err1;
	}

	vf->vf_name = kstrdup(pathname);
	if(vf->vf_name == NULL) {
		ret = ENOMEM;
		goto err2;
	}

	//ret = vfs_open(pathname, flags, 0, &vf->vf_vnode);
	if(ret != 0) {
		goto err3;
	}

	spinlock_init(&vf->vf_lock);
	vf->vf_flags = flags;
	vf->vf_offset = 0;
	vf->vf_refcount = 1;

	set_vfile(vf, fd);

	if(retval != NULL)
		*retval = fd;
	return 0;

	err3:
		kfree(vf->vf_name);
	err2:
		kfree(vf);
	err1:
		return ret;
}

int sys_read(int fd, userptr_t buf, size_t buflen, int *retval) {
	(void)fd;
	(void)buf;
	(void)buflen;
	(void)retval;
	// do stuff
	return 0;
}

int sys_write(int fd, const userptr_t buf, size_t buflen, int *retval) {
	(void)fd;
	(void)buf;
	(void)buflen;
	(void)retval;
	// do stuff
	return 0;
}
int sys_lseek(int fd, off_t pos, int whence, int *retval) {
	(void)fd;
	(void)pos;
	(void)whence;
	(void)retval;
	// do stuff
	return 0;
}

int sys_close(int fd) {
	(void)fd;
	// do stuff
	return 0;
}

int sys_dup2(int oldfd, int newfd) {
	(void)oldfd;
	(void)newfd;
	// do stuff
	return 0;
}

int sys_chdir(const userptr_t pathname) {
	(void)pathname;
	// do stuff
	return 0;
}

int sys___getcwd(userptr_t buf, size_t buflen) {
	(void)buf;
	(void)buflen;
	// do stuff
	return 0;
}