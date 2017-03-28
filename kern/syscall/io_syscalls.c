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
 * Find the index of a free vfile slot in vfiles.
 */
static int add_vfile(struct vfile *vfile, int fd) {
	int err = 0;

	spinlock_acquire(&gf_lock);	// protect additions in the global file array

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

int sys_open(char* pathname, int flags, int *retval) {
	int err = 0;

	int fd = -1;
	for(int i = 0; i < OPEN_MAX; i++) {		// find available fd in per-process table
		if(CUR_FDS(i) == -1) {
			fd = i;
			break;
		}
	}
	if(fd == -1) {		// process has too many fds
		err = EMFILE;
		goto err1;
	}

	struct vfile *vf = kmalloc(sizeof(struct vfile));
	if(vf == NULL) {	// not enough memory to kmalloc
		err = ENOMEM;
		goto err1;
	}

	vf->vf_name = kstrdup(pathname);	// parameter will be destroyed by vfs_open
	if(vf->vf_name == NULL) {
		err = ENOMEM;
		goto err2;
	}

	err = vfs_open(pathname, flags, 0666, &vf->vf_vnode);	// 0666 for read/write
	if(err != 0) {											// vf_flags will enforce perms
		goto err3;
	}

	spinlock_init(&vf->vf_lock);

	vf->io_lock = lock_create(vf->vf_name);
	if(vf->io_lock == NULL) {
		goto err4;
	}

	vf->vf_flags = flags;
	vf->vf_offset = 0;
	vf->vf_refcount = 1;

	if(add_vfile(vf, fd) != 0)	{		// add the appropriate entries to the per-process
		goto err5;						// and global file descriptor tables
	}

	if(retval != NULL)		// allow kernel to ignore return value for convenience
		*retval = fd;
	
	curthread->io_priority = true;		// for scheduling

	return 0;

	// error cleanup

	err5:
		lock_destroy(vf->io_lock);
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
	if(fd < 0 || fd >= OPEN_MAX || CUR_FDS(fd) < 0)	// invalid fd
		return EBADF;
	if((VFILES(CUR_FDS(fd))->vf_flags & O_ACCMODE) == O_WRONLY)	// reads not permitted
		return EBADF;

	struct iovec iov;
	struct uio uio;


	spinlock_acquire(&VFILES(CUR_FDS(fd))->vf_lock); // protect access to vf_offset

	off_t off = VFILES(CUR_FDS(fd))->vf_offset;
	uio_uinit(&iov, &uio, buf, buflen, off, UIO_READ);

	spinlock_release(&VFILES(CUR_FDS(fd))->vf_lock);


	lock_acquire(VFILES(CUR_FDS(fd))->io_lock);
	int err = VOP_READ(VFILES(CUR_FDS(fd))->vf_vnode, &uio);
	lock_release(VFILES(CUR_FDS(fd))->io_lock);

	if(err != 0)
		return err;

	if(retval != NULL)
		*retval = uio.uio_offset - off;		// difference in offsets is amount read


	if(VOP_ISSEEKABLE(VFILES(CUR_FDS(fd))->vf_vnode)) {		// only increase offset if offsets are meaningful
		spinlock_acquire(&VFILES(CUR_FDS(fd))->vf_lock); 

		VFILES(CUR_FDS(fd))->vf_offset = uio.uio_offset;

		spinlock_release(&VFILES(CUR_FDS(fd))->vf_lock);
	}

	curthread->io_priority = true;		// for scheduling

	return 0;
}

int sys_write(int fd, const userptr_t buf, size_t buflen, int *retval) {
	if(fd < 0 || fd >= OPEN_MAX || CUR_FDS(fd) < 0)		// invalid fd
		return EBADF;
	if((VFILES(CUR_FDS(fd))->vf_flags & O_ACCMODE) == O_RDONLY)		// writes not permitted
		return EBADF;

	struct iovec iov;
	struct uio uio;


	spinlock_acquire(&VFILES(CUR_FDS(fd))->vf_lock); 	// protect access to vf_offset

	off_t off = VFILES(CUR_FDS(fd))->vf_offset;
	uio_uinit(&iov, &uio, buf, buflen, off, UIO_WRITE);

	spinlock_release(&VFILES(CUR_FDS(fd))->vf_lock);


	lock_acquire(VFILES(CUR_FDS(fd))->io_lock);
	int err = VOP_WRITE(VFILES(CUR_FDS(fd))->vf_vnode, &uio);
	lock_release(VFILES(CUR_FDS(fd))->io_lock);

	if(err != 0)
		return err;

	if(retval != NULL)
		*retval = uio.uio_offset - off;		// difference in offsets is amount written

	if(VOP_ISSEEKABLE(VFILES(CUR_FDS(fd))->vf_vnode)) {
		spinlock_acquire(&VFILES(CUR_FDS(fd))->vf_lock); 

		VFILES(CUR_FDS(fd))->vf_offset = uio.uio_offset;

		spinlock_release(&VFILES(CUR_FDS(fd))->vf_lock);
	}

	curthread->io_priority = true;		// for scheduling

	return 0;
}
int sys_lseek(int fd, off_t pos, int whence, int *retval, int *retval2) {
	if(fd < 0 || fd >= OPEN_MAX || CUR_FDS(fd) < 0)	// nefarious user errors
		return EBADF;

	struct vfile *vf = VFILES(CUR_FDS(fd));

	if(!VOP_ISSEEKABLE(vf->vf_vnode))	// file not seekable
		return ESPIPE;

	switch(whence) {
		case SEEK_SET: {						// pos is absolute position
			if(pos < 0)
				return EINVAL;
			spinlock_acquire(&vf->vf_lock);

			vf->vf_offset = pos;

			spinlock_release(&vf->vf_lock);
			break;
		}
		case SEEK_CUR: {						// pos is relative to current position
			spinlock_acquire(&vf->vf_lock);

			if(pos + vf->vf_offset < 0) {
				spinlock_release(&vf->vf_lock);
				return EINVAL;
			}

			vf->vf_offset = pos + vf->vf_offset;

			spinlock_release(&vf->vf_lock);
			break;
		}
		case SEEK_END: {						// pos is relative to end of file
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
		*retval2 = vf->vf_offset;	// combine these two in syscall.c

	curthread->io_priority = true;		// for scheduling

	return 0;
}

int sys_close(int fd) {
	if(fd < 0 || fd >= OPEN_MAX || CUR_FDS(fd) < 0)		// nefarious user errors
		return EBADF;

	KASSERT((size_t)CUR_FDS(fd) < vfilearray_num(vfiles));		// these conditions shouldn't be possible
	KASSERT(VFILES(CUR_FDS(fd)) != NULL);						// without errors in kernel code elsewhere

	int index = CUR_FDS(fd);
	struct vfile *vf = VFILES(index);
	CUR_FDS(fd) = -1;							// mark per-process fd slot as available

	spinlock_acquire(&vf->vf_lock);				// multiple processes might close the same file simultaneously

	KASSERT(vf->vf_refcount > 0);
	vf->vf_refcount--;
	int refcount = vf->vf_refcount;

	spinlock_release(&vf->vf_lock);

	if(refcount == 0) {
		kfree(vf->vf_name);
		vfs_close(vf->vf_vnode);
		spinlock_cleanup(&vf->vf_lock);
		lock_destroy(vf->io_lock);
		kfree(vf);

		spinlock_acquire(&gf_lock);
		vfilearray_set(vfiles, index, NULL);
		while((unsigned) index == vfilearray_num(vfiles) - 1) {		// last element in array
			if(VFILES(index) != NULL)					
				break;
			vfilearray_remove(vfiles, index);			// purge NULL entries from end of array
			index--;
		}
		spinlock_release(&gf_lock);

	}

	return 0;
}

int sys_dup2(int oldfd, int newfd, int *retval) {
	if(oldfd < 0 || oldfd >= OPEN_MAX || CUR_FDS(oldfd) < 0 || 
		newfd < 0 || newfd >= OPEN_MAX)	// naughty user
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

	curthread->io_priority = true;		// for scheduling

	return 0;
}

int sys_chdir(const userptr_t pathname) {
	int err = 0;

	size_t len = 0;
	char *kbuf = kmalloc(sizeof(char) * PATH_MAX);	// move parameter into kernel space
	if(kbuf == NULL)
		return ENOMEM;
			
	err = copyinstr(pathname, kbuf, PATH_MAX, &len);
	if(err == 0)
		err = vfs_chdir(kbuf);	// change directory

	kfree(kbuf);

	curthread->io_priority = true;		// for scheduling

	return err;		// 0 upon success
}

int sys___getcwd(userptr_t buf, size_t buflen, int *retval) {
	struct iovec iov;
	struct uio uio;

	uio_uinit(&iov, &uio, buf, buflen, 0, UIO_READ);

	int err = vfs_getcwd(&uio);	// read working directory to user space
	if(err != 0)
		return err;

	if(retval != NULL)
		*retval = uio.uio_offset;	// offset is size of path name in bytes
	return 0;
}