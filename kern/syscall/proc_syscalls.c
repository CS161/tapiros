/*
 * Process-related system calls.
 *
 * Includes getpid(), fork(), execv(), waitpid(), and _exit().
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <syscall.h>


pid_t sys_getpid(void) {
	// do stuff
	return 0;
}

pid_t sys_fork(void) {
	// do stuff
	return 0;
}

int sys_execv(const userptr_t program, userptr_t argv) {
	(void)program;
	(void)argv;
	// do stuff
	return 0;
}

pid_t sys_waitpid(pid_t pid, userptr_t status, int options) {
	(void)pid;
	(void)status;
	(void)options;
	// do stuff
	return 0;
}

void sys__exit(int exitcode) {
	(void) exitcode;
	// do stuff
	return;
}