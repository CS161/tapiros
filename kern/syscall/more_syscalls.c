/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009, 2014
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * More file-related system call implementations.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/seek.h>
#include <kern/stat.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <vfs.h>
#include <vnode.h>
#include <syscall.h>

/*
 * Note: if you are receiving this code as a patch to integrate with
 * your own system call code, you'll need to adapt the bottom four
 * functions to interact with your open file and file table code; the
 * code here works with the solution set file tables.
 *
 * The interface this code uses is as follows:
 *    - struct openfile (type for open file object)
 *    - uio_uinit()
 *    - filetable_get()
 *    - filetable_put()
 *
 * struct openfile: (from openfile.h)
 *    - object for an open file that goes in the file table
 *    - contains a vnode (->of_vnode);
 *    - contains the access mode from open (->of_accmode); this is the
 *      O_ACCMODE bits (only) from the open flags, namely one of
 *      O_RDONLY, O_WRONLY, or O_RDWR;
 *    - contains a seek position of type off_t (->of_offset);
 *    - contains a lock to protect the seek position (->of_offsetlock).
 *
 * uio_uinit: (in uio.h)
 *    - is like uio_kinit but initializes a uio with a userspace
 *      pointer.
 *
 * filetable_get: (in filetable.h)
 *    - operates on the current process's file table object
 *      (which is passed in, but can equally validly be implicit);
 *    - takes a file descriptor (open file number);
 *    - does all validity checks on the file descriptor;
 *    - either fails with an error code or returns the open file object
 *      associated with the file descriptor.
 *
 * filetable_put: (in filetable.h)
 *    - does any cleanup necessary after using filetable_get; this may
 *      be nothing.
 *
 * Your open file structure is probably called something else.
 * However, it probably has equivalent members under different names,
 * so adapting this code to use yours probably requires only search
 * and replace, or at most minor edits.
 *
 * While you may not have a direct equivalent of uio_uinit, you have
 * equivalent code in your read and write system calls and should be
 * able to reuse it.
 *
 * And while the operations on your file table probably aren't the
 * same as filetable_get and filetable_put, they may be close; and if
 * not, providing filetable_get and filetable_put in terms of your
 * operations (or even just writing them out) won't be difficult.
 *
 * Also note that if you want to get going on other stuff before
 * dealing with some or all of the above you can #if 0 the material
 * that doesn't compile and return ENOSYS. Use e.g. "(void)fd;" to
 * shut the compiler up if it complains about unused arguments.
 */


/*
 * sync - call vfs_sync
 */
int
sys_sync(void)
{
	int err;

	err = vfs_sync();
	if (err==EIO) {
		/* This is the only likely failure case */
		kprintf("Warning: I/O error during sync\n");
	}
	else if (err) {
		kprintf("Warning: sync: %s\n", strerror(err));
	}
	/* always succeed */
	return 0;
}

/*
 * mkdir - call vfs_mkdir
 */
int
sys_mkdir(userptr_t path, mode_t mode)
{
	char *pathbuf;
	int err;

	pathbuf = kmalloc(PATH_MAX);
	if (pathbuf == NULL) {
		return ENOMEM;
	}

	err = copyinstr(path, pathbuf, PATH_MAX, NULL);
	if (err) {
		kfree(pathbuf);
		return err;
	}

	err = vfs_mkdir(pathbuf, mode);
	kfree(pathbuf);
	return err;
}

/*
 * rmdir - call vfs_rmdir
 */
int
sys_rmdir(userptr_t path)
{
	char *pathbuf;
	int err;

	pathbuf = kmalloc(PATH_MAX);
	if (pathbuf == NULL) {
		return ENOMEM;
	}

	err = copyinstr(path, pathbuf, PATH_MAX, NULL);
	if (err) {
		kfree(pathbuf);
		return err;
	}

	err = vfs_rmdir(pathbuf);
	kfree(pathbuf);
	return err;
}

/*
 * remove - call vfs_remove
 */
int
sys_remove(userptr_t path)
{
	char *pathbuf;
	int err;

	pathbuf = kmalloc(PATH_MAX);
	if (pathbuf == NULL) {
		return ENOMEM;
	}

	err = copyinstr(path, pathbuf, PATH_MAX, NULL);
	if (err) {
		kfree(pathbuf);
		return err;
	}

	err = vfs_remove(pathbuf);
	kfree(pathbuf);
	return err;
}

/*
 * link - call vfs_link
 */
int
sys_link(userptr_t oldpath, userptr_t newpath)
{
	char *oldbuf;
	char *newbuf;
	int err;

	oldbuf = kmalloc(PATH_MAX);
	if (oldbuf == NULL) {
		return ENOMEM;
	}

	newbuf = kmalloc(PATH_MAX);
	if (newbuf == NULL) {
		kfree(oldbuf);
		return ENOMEM;
	}

	err = copyinstr(oldpath, oldbuf, PATH_MAX, NULL);
	if (err) {
		goto fail;
	}

	err = copyinstr(newpath, newbuf, PATH_MAX, NULL);
	if (err) {
		goto fail;
	}

	err = vfs_link(oldbuf, newbuf);
 fail:
	kfree(newbuf);
	kfree(oldbuf);
	return err;
}

/*
 * rename - call vfs_rename
 */
int
sys_rename(userptr_t oldpath, userptr_t newpath)
{
	char *oldbuf;
	char *newbuf;
	int err;

	oldbuf = kmalloc(PATH_MAX);
	if (oldbuf == NULL) {
		return ENOMEM;
	}

	newbuf = kmalloc(PATH_MAX);
	if (newbuf == NULL) {
		kfree(oldbuf);
		return ENOMEM;
	}

	err = copyinstr(oldpath, oldbuf, PATH_MAX, NULL);
	if (err) {
		goto fail;
	}

	err = copyinstr(newpath, newbuf, PATH_MAX, NULL);
	if (err) {
		goto fail;
	}

	err = vfs_rename(oldbuf, newbuf);
 fail:
	kfree(newbuf);
	kfree(oldbuf);
	return err;
}

/*
 * getdirentry - call VOP_GETDIRENTRY
 */
int
sys_getdirentry(int fd, userptr_t buf, size_t buflen, int *retval)
{
	if(fd < 0 || fd >= OPEN_MAX || CUR_FDS(fd) < 0)		// invalid fd
		return EBADF;

	struct iovec iov;
	struct uio useruio;
	int err;

	/* better be a valid file descriptor */

	struct vfile *file = VFILES(CUR_FDS(fd));
	if(file == NULL) {
		return EBADF;
	}

	/* all directories should be seekable */
	KASSERT(VOP_ISSEEKABLE(file->vf_vnode));

	spinlock_acquire(&file->vf_lock);

	/* Dirs shouldn't be openable for write at all, but be safe... */
	if ((file->vf_flags & O_ACCMODE) == O_WRONLY) {
		spinlock_release(&file->vf_lock);
		return EBADF;
	}

	/* set up a uio with the buffer, its size, and the current offset */
	uio_uinit(&iov, &useruio, buf, buflen, file->vf_offset, UIO_READ);

	/* do the read */
	err = VOP_GETDIRENTRY(file->vf_vnode, &useruio);
	if (err) {
		spinlock_release(&file->vf_lock);
		return err;
	}

	/* set the offset to the updated offset in the uio */
	file->vf_offset = useruio.uio_offset;

	spinlock_release(&file->vf_lock);

	/*
	 * the amount read is the size of the buffer originally, minus
	 * how much is left in it. Note: it is not correct to use
	 * uio_offset for this!
	 */
	*retval = buflen - useruio.uio_resid;

	return 0;
}

/*
 * fstat - call VOP_FSTAT
 */
int
sys_fstat(int fd, userptr_t statptr)
{
	if(fd < 0 || fd >= OPEN_MAX || CUR_FDS(fd) < 0)		// invalid fd
		return EBADF;

	struct stat kbuf;
	int err;

	struct vfile *file = VFILES(CUR_FDS(fd));
	if(file == NULL) {
		return EBADF;
	}

	/*
	 * No need to lock the openfile - it cannot disappear under us,
	 * and we're not using any of its non-constant fields.
	 */

	err = VOP_STAT(file->vf_vnode, &kbuf);
	if (err) {
		return err;
	}

	return copyout(&kbuf, statptr, sizeof(struct stat));
}

/*
 * fsync - call VOP_FSYNC
 */
int
sys_fsync(int fd)
{
	if(fd < 0 || fd >= OPEN_MAX || CUR_FDS(fd) < 0)		// invalid fd
		return EBADF;

	int err;

	struct vfile *file = VFILES(CUR_FDS(fd));
	if(file == NULL) {
		return EBADF;
	}

	/*
	 * No need to lock the openfile - it cannot disappear under us,
	 * and we're not using any of its non-constant fields.
	 */

	err = VOP_FSYNC(file->vf_vnode);
	return err;
}

/*
 * ftruncate - call VOP_TRUNCATE
 */
int
sys_ftruncate(int fd, off_t len)
{
	if(fd < 0 || fd >= OPEN_MAX || CUR_FDS(fd) < 0)		// invalid fd
		return EBADF;
	
	int err;

	if (len < 0) {
		return EINVAL;
	}

	struct vfile *file = VFILES(CUR_FDS(fd));
	if(file == NULL) {
		return EBADF;
	}

	if ((file->vf_flags & O_ACCMODE) == O_RDONLY) {
		return EBADF;
	}

	/*
	 * No need to lock the openfile - it cannot disappear under us,
	 * and we're not using any of its non-constant fields.
	 */

	err = VOP_TRUNCATE(file->vf_vnode, len);
	return err;
}
