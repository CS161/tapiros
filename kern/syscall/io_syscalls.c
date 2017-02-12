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
#include <addrspace.h>
#include <syscall.h>


int sys_open(const userptr_t pathname, int flags) {
	(void)pathname;
	(void)flags;
	// do stuff
	return 0;
}

ssize_t sys_read(int fd, userptr_t buf, size_t buflen) {
	(void)fd;
	(void)buf;
	(void)buflen;
	// do stuff
	return 0;
}

ssize_t sys_write(int fd, const userptr_t buf, size_t buflen) {
	(void)fd;
	(void)buf;
	(void)buflen;
	// do stuff
	return 0;
}
off_t sys_lseek(int fd, off_t pos, int whence) {
	(void)fd;
	(void)pos;
	(void)whence;
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