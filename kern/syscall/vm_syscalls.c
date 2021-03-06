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
#include <current.h>
#include <proc.h>


int sys_sbrk(intptr_t amount, int *retval) {
	if(amount % PAGE_SIZE != 0)	// 'amount' must be page-aligned
		return EINVAL;
	
	struct addrspace *as = curproc->p_addrspace;

	// We don't need synchronization for heap_bottom and heap_top
	// because there can only be one thread per address space

	// We don't need to worry about signed/unsigned having different ranges
	// because the top half (last bit) of the positive range in vaddr_t 
	// is only used for kernel address space
	if(amount >= 0) {
		// allow four times physical memory of heap space
		unsigned long min = 4 * ncmes * PAGE_SIZE < USERHEAPSIZE ? 4 * ncmes * PAGE_SIZE : USERHEAPSIZE;
		if(as->heap_bottom + min < as->heap_top + amount)
			return ENOMEM;

		*retval = as->heap_top;
		as->heap_top += amount;
	}
	else {
		if((unsigned) (-1 * amount) > as->heap_top - as->heap_bottom) // check overflow too
			return EINVAL;
		
		*retval = as->heap_top;
		as->heap_top += amount;

		free_upages(as, as->heap_top, -(amount / PAGE_SIZE));
	}

	return 0;
}