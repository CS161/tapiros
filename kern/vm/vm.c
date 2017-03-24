/*
 * Machine independent VM functions.
 */

#include <vm.h>


vaddr_t alloc_kpages(unsigned npages) {

	(void) npages;
	// Currently allocates one page, no matter the request

	spinlock_acquire(&core_map_splk);
	vaddr_t ret = 0;
	for(unsigned long i = 0; i < ncmes; i++) {
		if(core_map[i].va == 0) {
			ret = ((vaddr_t) core_map) + i * PAGE_SIZE;
			core_map[i].va = ret;
			core_map[i].md.kernel = 1;
			break;
		}
	}
	spinlock_release(&core_map_splk);

	if(ret == 0)
		panic("Out of memory! :( \n");

	return ret;
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