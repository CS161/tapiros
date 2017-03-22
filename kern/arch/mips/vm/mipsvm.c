/*
 * Machine dependent VM functions.
 */

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

vaddr_t alloc_kpages(unsigned npages) {
	(void) npages;

	panic("Can't alloc_kpages yet!\n");
	return NULL;
}

void free_kpages(vaddr_t addr) {
	(void)addr;

	panic("Can't free_kpages yet!\n");
}

void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void)ts;

	panic("Can't vm_tlbshootdown yet!\n");
	return 0;
}