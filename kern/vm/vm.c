/*
 * Machine independent VM functions.
 */

#include <types.h>
#include <lib.h>
#include <vm.h>


vaddr_t alloc_kpages(unsigned npages) {
	(void) npages;

	panic("Can't alloc_kpages yet!\n");
	return 0;
}

void free_kpages(vaddr_t addr) {
	(void)addr;

	panic("Can't free_kpages yet!\n");
}

int alloc_upage(struct addrspace *as, vaddr_t vaddr) {
	(void) as;
	(void) vaddr;

	panic("Can't alloc_upage yet!\n");
	return 0;
}

void free_upage(struct addrspace *as, vaddr_t vaddr) {
	(void) as;
	(void) vaddr;

	panic("Can't free_upage yet!\n");
}