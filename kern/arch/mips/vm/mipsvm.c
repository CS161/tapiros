/*
 * Machine dependent VM functions.
 */

#include <vm.h>


void vm_bootstrap(void) {
	size_t ramsize = ram_getsize();
	unsigned long i;

	ncmes = ramsize / PAGE_SIZE;
	unsigned long npages = ((ncmes * sizeof(struct core_map_entry) - 1) / PAGE_SIZE) + 1;
	core_map = (struct core_map_entry *) PADDR_TO_KVADDR(ram_stealmem(npages));

	for(i = 0; i < npages; i++) {
		core_map[i].va = ((vaddr_t) core_map) + i * PAGE_SIZE;
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