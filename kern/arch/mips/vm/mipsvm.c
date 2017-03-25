/*
 * Machine dependent VM functions.
 */

#include <vm.h>


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