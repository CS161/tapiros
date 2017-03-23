/*
 * Machine dependent VM functions.
 */

#include <types.h>
#include <lib.h>
#include <vm.h>


void vm_bootstrap(void) {
	panic("Can't vm_boostrap yet! (which means this message will never actually be printed, but whatever)\n");
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
	(void) faulttype;
	(void) faultaddress;

	panic("Can't vm_fault yet!\n");
	return 0;
}

void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void)ts;

	panic("Can't vm_tlbshootdown yet!\n");
}