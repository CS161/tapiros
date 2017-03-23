/*
 * Machine dependent VM functions.
 */

#include <vm.h>


void vm_bootstrap(void) {
	size_t ramsize = ram_getsize();

	// core map length should round up to nearest page
	ncmes = ramsize / PAGE_SIZE;
	unsigned long npages = (ncmes * sizeof(struct core_map_entry) - 1) / PAGE_SIZE + 1;
	core_map = (struct core_map_entry *) ram_stealmem(npages);

	memset(core_map, 0, npages * PAGE_SIZE); // it would be pretty bad if the core map had garbage in it
	for(unsigned long i = 0; i < npages; i++) {
		core_map[i].va = (vaddr_t) -1;	// mark core map pages differently from other kernel pages
		core_map[i].md.kernel = 1;
	}
	spinlock_init(&core_map_splk);
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