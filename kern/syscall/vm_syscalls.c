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

	// We don't need to worry about signed/unsigned having different ranges
	// because the top half (last bit) of the positive range in vaddr_t 
	// is only used for kernel address space
	if(amount >= 0) {
		if(USERHEAPTOP - amount > as->heap_top)
			return ENOMEM;

		*retval = as->heap_top;
		as->heap_top += amount;
	}
	else {
		if(as->heap_top + amount < as->heap_bottom)
			return EINVAL;
		
		*retval = as->heap_top;
		as->heap_top += amount;

		for(vaddr_t i = as->heap_top; i < (vaddr_t) *retval; i += PAGE_SIZE) {
			free_upage(as, i);
		}
	}

	return 0;
}