/*
 * Machine independent VM functions.
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


// alloc_kpages is more readable with this, and macros don't incur overhead from function calls 
// (though that might be compiled out)

#define TERMINATE_CHAIN(n)				\
	if(candidates[n] > lengths[n]) {	\
		starts[n] = i - candidates[n];	\
		lengths[n] = candidates[n];		\
	}									\
	candidates[n] = 0;					\


vaddr_t alloc_kpages(unsigned npages) {

	/*
	 *	L0: contiguous free non-busy pages
	 *	L1:	contiguous non-kernel non-tlb non-busy pages
	 *	L2: contiguous non-kernel non-busy pages
	 */

	unsigned long starts[3] = {0};			// start index of max chain found so far
	unsigned long lengths[3] = {0};			// max chain length found so far
	unsigned long candidates[3] = {0};		// length of currently tracked candidate

	unsigned long i;

	spinlock_acquire(&core_map_splk);

	// find candidate chains of contiguous memory

	for(i = 0; i < ncmes; i++) {
		if(core_map[i].md.busy) {	// terminate all chains in progress
			TERMINATE_CHAIN(0);
			TERMINATE_CHAIN(1);
			TERMINATE_CHAIN(2);
		}
		else {	// page isn't busy
			if(core_map[i].va == 0) {	// page is free
				candidates[0]++;
				candidates[1]++;
				candidates[2]++;
			}
			else {	// page isn't free
				TERMINATE_CHAIN(0);
				if(core_map[i].md.kernel == 0) {	// page is user-allocated
					if(core_map[i].md.tlb == 0) {	// page isn't in TLB
						candidates[1]++;
					}
					else {	// page is in TLB
						TERMINATE_CHAIN(2);
					}
					candidates[2]++;
				}
				else {	// page is kernel-allocated
					TERMINATE_CHAIN(1);
					TERMINATE_CHAIN(2);
				}
			}
		}
		if(candidates[0] == npages) {	// optimal chain has been found
			i++;
			break;
		}
	}
	TERMINATE_CHAIN(0);
	TERMINATE_CHAIN(1);
	TERMINATE_CHAIN(2);

	for(i = 0; i < 2; i++) {
		if(lengths[i] >= npages) {
			break;
		}
	}
	if(i == 3) {
		panic("alloc_kpages couldn't find enough free memory. SAD!\n");
	}

	vaddr_t ret = ((vaddr_t) core_map) + (starts[i] * PAGE_SIZE);
	unsigned long j;

	for(j = starts[i]; j < starts[i] + lengths[i]; j++) {
		core_map[j].va = ((vaddr_t) core_map) + j * PAGE_SIZE;
		core_map[j].md.kernel = 1;
	}
	core_map[j-1].md.contig = 1;

	spinlock_release(&core_map_splk);

	return ret;
}


void free_kpages(vaddr_t addr) {

	KASSERT(addr > (vaddr_t) core_map && addr < (vaddr_t) MIPS_KSEG1);

	unsigned long i = (addr - (vaddr_t)core_map) / PAGE_SIZE;	// index in core_map

	spinlock_acquire(&core_map_splk);

	while(core_map[i].md.contig == 0) {
		KASSERT(core_map[i].va != 0);
		KASSERT(core_map[i].md.kernel == 1);

		core_map[i].va = 0;
		core_map[i].md.kernel = 0;
		i++;
	}
	core_map[i].va = 0;
	core_map[i].md.kernel = 0;
	core_map[i].md.contig = 0;

	spinlock_release(&core_map_splk);
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