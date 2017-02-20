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
#include <limits.h>
#include <copyinout.h>
#include <kern/seek.h>
#include <stat.h>

/*
 * Initialize the vfiles array, including stdin, stdout, and stderr.
 */
void vfiles_init(void) {
	vfiles = vfilearray_create();
	if(vfiles == NULL) {
		panic("vfilearray_create for vfiles failed\n");
	}

	spinlock_init(&gf_lock);

	char* console;

	// open standard in
	console = kstrdup("con:");		// vfs methods eat the pathname,
	if(console == NULL)				// so it can't be a const char *
		panic("console string couldn't be allocated\n");
	if(sys_open(console, O_RDONLY, NULL))
		panic("stdin open failed\n");
	kfree(console);

	// open standard out
	console = kstrdup("con:");
	if(console == NULL)
		panic("console string couldn't be allocated\n");
	if(sys_open(console, O_WRONLY, NULL))
		panic("stdout open failed\n");
	kfree(console);

	// open standard error
	console = kstrdup("con:");
	if(console == NULL)
		panic("console string couldn't be allocated\n");
	if(sys_open(console, O_WRONLY, NULL))
		panic("stderr open failed\n");
	kfree(console);
}

/*
 * Find the index of a free vfile slot in vfiles.
 */
static int add_vfile(struct vfile *vfile, int fd) {
	int err = 0;

	spinlock_acquire(&gf_lock);

	int max = vfilearray_num(vfiles);
	int slot = -1;
	for(int i = 0; i < max; i++) {	// iterate through vfiles until we find
		if(VFILES(i) == NULL) {		// an empty spot or reach the end
			slot = i;
			break;
		}
	}

	if(slot < 0) {		// add to the end of vfiles
		err = vfilearray_add(vfiles, vfile, NULL);
		if(err == 0)
			CUR_FDS(fd) = max;
	}
	else {		// use an empty slot in the middle of vfiles
		vfilearray_set(vfiles, slot, vfile);
		CUR_FDS(fd) = slot;
	}

	spinlock_release(&gf_lock);

	return err;
}

int sys_open(char* pathname, int flags, int *retval) {
	int err = 0;

	int fd = -1;
	for(int i = 0; i < MAX_FDS; i++) {
		if(CUR_FDS(i) == -1) {
			fd = i;
			break;
		}
	}
	if(fd == -1) {
		err = EMFILE;
		goto err1;
	}

	struct vfile *vf = kmalloc(sizeof(struct vfile *));
	if(vf == NULL) {
		err = ENOMEM;
		goto err1;
	}

	vf->vf_name = kstrdup(pathname);
	if(vf->vf_name == NULL) {
		err = ENOMEM;
		goto err2;
	}

	err = vfs_open(pathname, flags, 0666, &vf->vf_vnode);	// 0666 for read/write
	if(err != 0) {											// vf_flags will enforce perms
		goto err3;
	}

	spinlock_init(&vf->vf_lock);
	vf->vf_flags = flags;
	vf->vf_offset = 0;
	vf->vf_refcount = 1;

	if(add_vfile(vf, fd) != 0)	{	// add the appropriate entries to the per-process
		goto err4;						// and global file descriptor tables
	}
	
	if(retval != NULL)
		*retval = fd;
	return 0;

	// error cleanup

	err4:
		vfs_close(vf->vf_vnode);
		spinlock_cleanup(&vf->vf_lock);
	err3:
		kfree(vf->vf_name);
	err2:
		kfree(vf);
	err1:
		return err;
}

int sys_read(int fd, userptr_t buf, size_t buflen, int *retval) {
	if(fd < 0 || fd >= MAX_FDS || CUR_FDS(fd) < 0)	// invalid fd
		return EBADF;
	if(VFILES(CUR_FDS(fd))->vf_flags == O_WRONLY)	// reads not permitted
		return EBADF;

	struct iovec iov;
	struct uio uio;


	spinlock_acquire(&VFILES(CUR_FDS(fd))->vf_lock); // protect access to vf_offset

	off_t off = VFILES(CUR_FDS(fd))->vf_offset;
	uio_uinit(&iov, &uio, buf, buflen, off, UIO_READ);

	spinlock_release(&VFILES(CUR_FDS(fd))->vf_lock);


	int err = VOP_READ(VFILES(CUR_FDS(fd))->vf_vnode, &uio);
	if(err != 0)
		return err;

	if(retval != NULL)
		*retval = uio.uio_offset - off;


	spinlock_acquire(&VFILES(CUR_FDS(fd))->vf_lock); 

	VFILES(CUR_FDS(fd))->vf_offset = uio.uio_offset;

	spinlock_release(&VFILES(CUR_FDS(fd))->vf_lock);

	return 0;
}

int sys_write(int fd, const userptr_t buf, size_t buflen, int *retval) {
	if(fd < 0 || fd >= MAX_FDS || CUR_FDS(fd) < 0)	// invalid fd
		return EBADF;
	if(VFILES(CUR_FDS(fd))->vf_flags == O_RDONLY)	// writes not permitted
		return EBADF;

	struct iovec iov;
	struct uio uio;


	spinlock_acquire(&VFILES(CUR_FDS(fd))->vf_lock); // protect access to vf_offset

	off_t off = VFILES(CUR_FDS(fd))->vf_offset;
	uio_uinit(&iov, &uio, buf, buflen, off, UIO_WRITE);

	spinlock_release(&VFILES(CUR_FDS(fd))->vf_lock);


	int err = VOP_WRITE(VFILES(CUR_FDS(fd))->vf_vnode, &uio);
	if(err != 0)
		return err;

	if(retval != NULL)
		*retval = uio.uio_offset - off;


	spinlock_acquire(&VFILES(CUR_FDS(fd))->vf_lock); 

	VFILES(CUR_FDS(fd))->vf_offset = uio.uio_offset;

	spinlock_release(&VFILES(CUR_FDS(fd))->vf_lock);

	return 0;
}
int sys_lseek(int fd, off_t pos, int whence, int *retval, int *retval2) {
	if(fd < 0 || fd >= MAX_FDS || CUR_FDS(fd) < 0)	// nefarious user errors
		return EBADF;

	struct vfile *vf = VFILES(CUR_FDS(fd));

	if(!VOP_ISSEEKABLE(vf->vf_vnode))
		return ESPIPE;

	switch(whence) {
		case SEEK_SET: {
			if(pos < 0)
				return EINVAL;
			spinlock_acquire(&vf->vf_lock);

			vf->vf_offset = pos;

			spinlock_release(&vf->vf_lock);
			break;
		}
		case SEEK_CUR: {
			if(pos + vf->vf_offset < 0)
				return EINVAL;
			spinlock_acquire(&vf->vf_lock);

			vf->vf_offset = pos + vf->vf_offset;

			spinlock_release(&vf->vf_lock);
			break;
		}
		case SEEK_END: {
			struct stat stats;
			VOP_STAT(vf->vf_vnode, &stats);
			if(pos + stats.st_size < 0)
				return EINVAL;
			spinlock_acquire(&vf->vf_lock);

			vf->vf_offset = pos + stats.st_size;

			spinlock_release(&vf->vf_lock);
			break;
		}
		default:
			return EINVAL;
	}

	if(retval != NULL)
		*retval = vf->vf_offset >> 32;

	if(retval2 != NULL)
		*retval2 = vf->vf_offset;

	return 0;
}

int sys_close(int fd) {
	if(fd < 0 || fd >= MAX_FDS || CUR_FDS(fd) < 0)	// nefarious user errors
		return EBADF;

	KASSERT((size_t)CUR_FDS(fd) < procarray_num(procs));	// these conditions shouldn't be possible
	KASSERT(VFILES(CUR_FDS(fd)) != NULL);				// without errors in kernel code elsewhere

	struct vfile *vf = VFILES(CUR_FDS(fd));
	CUR_FDS(fd) = -1;	// mark per-process fd slot as available

	spinlock_acquire(&vf->vf_lock);		// multiple processes might close the same file simultaneously

	KASSERT(vf->vf_refcount > 0);
	vf->vf_refcount--;

	spinlock_release(&vf->vf_lock);

	// at this point, we only need to touch vf if no one else can,
	// which means we don't need to worry about synchronization
	if(vf->vf_refcount == 0) {
		kfree(vf->vf_name);
		vfs_close(vf->vf_vnode);
		spinlock_cleanup(&vf->vf_lock);
		kfree(vf);
	}

	return 0;
}

int sys_dup2(int oldfd, int newfd, int *retval) {
	if(oldfd < 0 || oldfd >= MAX_FDS || CUR_FDS(oldfd) < 0 || 
		newfd < 0 || newfd >= MAX_FDS)
		return EBADF;

	if(oldfd == newfd) {	// if both fds are the same, do nothing
		if(retval != NULL)
			*retval = newfd;
		return 0;
	}

	if(CUR_FDS(newfd) != -1) {	// close newfd if it already describes a file
		sys_close(newfd);
	}
	CUR_FDS(newfd) = CUR_FDS(oldfd);

	spinlock_acquire(&VFILES(CUR_FDS(newfd))->vf_lock);

	VFILES(CUR_FDS(newfd))->vf_refcount++;	// keep track of references

	spinlock_release(&VFILES(CUR_FDS(newfd))->vf_lock);

	if(retval != NULL)
		*retval = newfd;
	return 0;
}

int sys_chdir(const userptr_t pathname) {
	int err = 0;

	size_t len = 0;
	char *kbuf = kmalloc(sizeof(char) * PATH_MAX);
	if(kbuf == NULL)
		return ENOMEM;
			
	err = copyinstr(pathname, kbuf, PATH_MAX, &len);
	if(err == 0)
		err = vfs_chdir(kbuf);

	kfree(kbuf);

	return err;		// 0 upon success
}

int sys___getcwd(userptr_t buf, size_t buflen, int *retval) {
	struct iovec iov;
	struct uio uio;

	uio_uinit(&iov, &uio, buf, buflen, 0, UIO_READ);

	int err = vfs_getcwd(&uio);
	if(err != 0)
		return err;

	if(retval != NULL)
		*retval = uio.uio_offset;
	return 0;
}