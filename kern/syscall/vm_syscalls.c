/*
 * VM-related system calls.
 *
 * Includes sbrk().
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <vm.h>
#include <addrspace.h>
#include <syscall.h>


int sys_sbrk(intptr_t amount, int *retval) {
	(void) amount;
	(void) retval;
	return 0;
}