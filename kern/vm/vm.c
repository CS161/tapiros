/*
 * Machine independent VM functions.
 */

#include <vm.h>
#include <addrspace.h>
#include <wchan.h>
#include <bitmap.h>


// It's relevant to note that our core map starts at the core map's page;
// the kernel's code is not referenced in the array. This should reduce
// the risk of overwriting kernel code and means we can start at an index of 0
// for all memory we ever intend to write to.
void vm_bootstrap(void) {
	size_t ramsize = ram_getsize();
	size_t start = ram_stealmem(0);	// get the address of the first writeable page
	unsigned long i;

	ncmes = (ramsize - start) / PAGE_SIZE;
	unsigned long npages = ROUND_UP(ncmes * sizeof(struct core_map_entry), PAGE_SIZE);
	//unsigned long npages = ((ncmes * sizeof(struct core_map_entry) - 1) / PAGE_SIZE) + 1;
	core_map = (struct core_map_entry *) PADDR_TO_KVADDR(ram_stealmem(npages));

	for(i = 0; i < npages; i++) {
		core_map[i].va = ((vaddr_t) core_map) + i * PAGE_SIZE;
		core_map[i].md.kernel = 1;
	}
	spinlock_init(&core_map_splk);

	nfree = ncmes - npages;
	ndirty = 0;
	nswap = 0;
}

// 'cm' in the kernel menu
// handy to check for memory leaks, but now that they're fixed it's not so interesting
int print_core_map(int nargs, char **args) {
	(void) nargs;
	(void) args;
	unsigned long nkernel = 0;
	unsigned long nuser = 0;
	for(unsigned long i = 0; i < ncmes; i++) {
		struct core_map_entry cme = core_map[i];
		if(cme.md.kernel)
			nkernel++;
		else if(cme.va)
			nuser++;
		kprintf("%lu: vaddr: %p, as: %p, c:%d, b:%d\n", i, (void *) cme.va, cme.as, cme.md.contig, cme.md.busy);
	}
	kprintf("\nKernel Pages: %lu\nUser Pages: %lu\nTotal Pages: %lu\n\n", nkernel, nuser, nkernel + nuser);

	unsigned int i;
	for(i = 1; i < swap_size; i++) {
		if(bitmap_isset(swap_bitmap, i)) {
			kprintf("Swap isn't properly zeroed.\n");
			break;
		}
	}
	if(i == swap_size)
		kprintf("Swap is properly zeroed.\n");

	return 0;
}


// Delegate responses to vm_faults
int vm_fault(int faulttype, vaddr_t faultaddress) {

	if(faultaddress < PAGE_SIZE || faultaddress >= USERSPACETOP) {	// reject NULL page pointer or kernel addresses
		return EFAULT;
	}

	if(curproc == NULL) {
		return EFAULT;
	}

	struct addrspace *as = proc_getas();

	if(as == NULL) {
		return EFAULT;
	}

	switch(faulttype) {
		case VM_FAULT_READONLY:
			return perms_fault(as, faultaddress);

		case VM_FAULT_WRITE:
		case VM_FAULT_READ:
			return tlb_miss(as, faultaddress);

		default:
			return EINVAL;
	}
}


// alloc_kpages is more readable with this, and macros don't incur overhead from function calls 
// (though that might be compiled out with inlining)

#define TERMINATE_CHAIN(n)				\
	if(candidates[n] > lengths[n]) {	\
		starts[n] = i - candidates[n];	\
		lengths[n] = candidates[n];		\
	}									\
	candidates[n] = 0;					\


// *** Does not guarantee zero-filled pages
// *** Assumes no spinlocks are held
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
						TERMINATE_CHAIN(1);
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

	for(i = 0; i < 3; i++) {
		if(lengths[i] >= npages) {
			break;
		}
	}
	if(i == 3) {
		return 0;
	}

	vaddr_t ret = ((vaddr_t) core_map) + (starts[i] * PAGE_SIZE);
	unsigned long j;

	for(j = starts[i]; j < starts[i] + lengths[i]; j++) {
		KASSERT(core_map[j].md.busy == 0);
		core_map[j].md.busy = 1;	// protect the pages we've chosen so another thread
									// won't swoop in and take them from under us
									// (especially bad if it's another alloc_kpages, because then
									// our contiguous chain has an unswappable block in the middle)
	}

	for(j = starts[i]; j < starts[i] + lengths[i]; j++) {
		if(core_map[j].va != 0) {
			spinlock_release(&core_map_splk);

			struct addrspace *other_as = core_map[j].as;

			spinlock_acquire(&other_as->addr_splk);		// synchronization dance
			spinlock_acquire(&core_map_splk);

			KASSERT(core_map[j].md.busy == 1);

			core_map[j].md.busy = 0;
			wchan_wakeall(core_map[j].as->addr_wchan, &core_map[j].as->addr_splk);

			swap_out(j, other_as);
			core_map[j].md.busy = 1;

			spinlock_release(&other_as->addr_splk);
		}
		KASSERT(core_map[j].va == 0);
		KASSERT(core_map[j].md.kernel == 0);
		KASSERT(core_map[j].as == NULL);
		KASSERT(core_map[j].md.busy == 1);

		core_map[j].va = ((vaddr_t) core_map) + j * PAGE_SIZE;
		core_map[j].md.kernel = 1;
		core_map[j].md.busy = 0;
		nfree--;

		KASSERT(core_map[j].md.contig == 0);
	}
	core_map[j-1].md.contig = 1;	// mark only the final page in a chain

	spinlock_release(&core_map_splk);

	KASSERT(ret > (vaddr_t)core_map);
	KASSERT(ret % PAGE_SIZE == 0);
	KASSERT(ret < (vaddr_t)core_map + ncmes * PAGE_SIZE);

	return ret;
}


void free_kpages(vaddr_t addr) {

	KASSERT(addr > (vaddr_t) core_map && addr < (vaddr_t) MIPS_KSEG1);

	unsigned long i = (addr - (vaddr_t)core_map) / PAGE_SIZE;	// index in core_map
	
	spinlock_acquire(&core_map_splk);

	while(core_map[i].md.contig == 0) {		// free non-final pages
		KASSERT(core_map[i].va != 0);
		KASSERT(core_map[i].md.kernel == 1);

		core_map[i].va = 0;
		core_map[i].md.kernel = 0;
		i++;
		nfree++;
	}
	KASSERT(core_map[i].va != 0);
	KASSERT(core_map[i].md.kernel == 1);
	KASSERT(core_map[i].md.contig == 1);
	core_map[i].va = 0;						// free the final page
	core_map[i].md.kernel = 0;
	core_map[i].md.contig = 0;
	nfree++;

	spinlock_release(&core_map_splk);
}