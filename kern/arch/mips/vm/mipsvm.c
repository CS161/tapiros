/*
 * Machine dependent VM functions.
 */

#include <vm.h>
#include <spl.h>
#include <cpu.h>
#include <addrspace.h>
#include <wchan.h>
#include <vfs.h>
#include <vnode.h>
#include <kern/stat.h>
#include <uio.h>
#include <clock.h>


static void mat_daemon(void *a, unsigned long b) {
	(void) a;
	(void) b;
	unsigned long i, n, nmax, s, t;
	struct addrspace *as;
	while(true) {
		s = 1;

		if(nfree > 0 && ncmes / nfree < 8)  { 		// more than 1/8 of memory is free
			s = 8 - (ncmes / nfree);				// sleep less if less memory is free
			goto bed;
		}

		if(nswap / ncmes > 2) {			// if there's a lot more in swap than RAM,
			s = 2 * nswap / ncmes;		// writing back with the daemon will just waste time,
			goto bed;					// so go to sleep for a while
		}

		nmax = (ndirty * nswap) / ncmes;
		i = clock;
		n = 0, t = 0;

		spinlock_acquire(&core_map_splk);
		while(n < nmax && t < ncmes) {	// t just in case nmax is too big (from busy or tlb dirty pages)
			if(i == ncmes)
				i = 0;
			if(!core_map[i].md.kernel && !core_map[i].md.busy && !core_map[i].md.tlb) {

				core_map[i].md.busy = 1;
				as = core_map[i].as;

				spinlock_release(&core_map_splk);
				spinlock_acquire(&as->addr_splk);
				spinlock_acquire(&core_map_splk);

				swap_copy_out(as, i);
				n++;

				core_map[i].md.busy = 0;
				wchan_wakeall(as->addr_wchan, &as->addr_splk);
				spinlock_release(&as->addr_splk);
			}
			i++;
			t++;
		}
		spinlock_release(&core_map_splk);

		bed:
			clocksleep(s);
	}
}


void swap_bootstrap(void) {
	int err = vfs_swapon("lhd0:", &swap_vnode);
	if(err != 0) {
		panic("vfs_swapon failed with error: %s\n", strerror(err));
	}

	struct stat stats; 
	err = VOP_STAT(swap_vnode, &stats);
	if(err != 0) {
		panic("vop_stat on swap_vnode failed with error: %s\n", strerror(err));
	}

	swap_size = stats.st_size / PAGE_SIZE;
	if(swap_size >= (2 << 20))		// swap address is stored in 20 bits (minus one for 0)
		swap_size = (2 << 20) - 1;

	swap_bitmap = bitmap_create(swap_size);
	if(swap_bitmap == NULL) {
		panic("bitmap_create of swap_bitmap failed\n");
	}
	bitmap_mark(swap_bitmap, 0); // fill the 0th index, since we don't want bitmap_alloc to return it later

	swap_lk = lock_create("swap_lk");
	if(swap_lk == NULL) {
		panic("lock_create of swap_lk failed\n");
	}

	clock = 0;


	// TLB shootdown setup

	ts_count = -1;

	spinlock_init(&ts_splk);

	ts_wchan = wchan_create("ts_wchan");
	if(ts_wchan == NULL) {
		panic("wchan_create of ts_wchan failed\n");
	}


	// Initialize write-back daemon

	int result = thread_fork("MAT Daemon", NULL, mat_daemon, NULL, 0);
	if (result) {
		panic("mat daemon thread_fork failed: %s\n", strerror(result));
	}
}


// *** Assumes the core map spinlock is held (and probably address space too)
// Returns -1 if there are no pages that can be swapped out (kernel or busy)
static long choose_page_to_swap(void) {
	unsigned long nchecked = 0;
	while(nchecked < ncmes) {
		if(clock == ncmes)	// make the clock circular
			clock = 0;
		if(core_map[clock].md.recent == 1)
			core_map[clock].md.recent = 0;
		else if(!core_map[clock].md.kernel && !core_map[clock].md.busy && !core_map[clock].md.tlb) {
			clock++;
			return clock - 1;
		}
		clock++;
		nchecked++;
	}
	unsigned long twice = 2 * ncmes;	// 2 * ncmes shouldn't be calculated each iteration
	while(nchecked < twice) {
		if(clock == ncmes)
			clock = 0;
		if(!core_map[clock].md.kernel && !core_map[clock].md.busy && !core_map[clock].md.tlb) {	// no more recent entries
			clock++;
			return clock - 1;
		}
		clock++;
		nchecked++;
	}
	unsigned long thrice = 3 * ncmes;	// 2 * ncmes shouldn't be calculated each iteration
	while(nchecked < thrice) {			// if there's nothing on the third loop, give up
		if(clock == ncmes)
			clock = 0;
		if(!core_map[clock].md.kernel && !core_map[clock].md.busy) {	// accept entries in TLB
			clock++;
			return clock - 1;
		}
		clock++;
		nchecked++;
	}
	return -1;
}


// *** Assumes that the address space and core map spinlocks are held
// *** Assumes that the CME has been marked busy by us and doesn't change its status
// Put a copy of data tracked in the core map at 'cmi' into swap 
// (either at an existing index or a new one) and update the CME accordingly.
void swap_copy_out(struct addrspace *as, unsigned long cmi) {

	struct core_map_entry *cme = &core_map[cmi];

	KASSERT(cme->as == as);
	KASSERT(cme->md.busy == 1);
	KASSERT(cme->md.kernel == 0);

	unsigned int swapi; 
	
	if(!cme->md.s_pres) {
		int err = bitmap_alloc(swap_bitmap, &swapi);
		if(err != 0)
			panic("Out of swap space :(\n");

		nswap++;

		cme->md.s_pres = 1;
		cme->md.swap = swapi;
	}
	else
		swapi = cme->md.swap;

	spinlock_release(&core_map_splk);
	spinlock_release(&as->addr_splk);

	lock_acquire(swap_lk);

	struct iovec iov;
	struct uio uio;
	uio_kinit(&iov, &uio, (void *) PADDR_TO_KVADDR(CMI_TO_PADDR(cmi)), PAGE_SIZE, swapi * PAGE_SIZE, UIO_WRITE);

	int err = VOP_WRITE(swap_vnode, &uio);
	if(err != 0)
		panic("Write to swap failed\n");

	lock_release(swap_lk);

	spinlock_acquire(&as->addr_splk);
	spinlock_acquire(&core_map_splk);

	cme->md.dirty = 0;	// swap_copy_out is only called on ptes not in the TLB
	ndirty--;			// (including ones that were just shot down)

	KASSERT(cme->md.busy == 1);
}


// *** Assumes that a different addrspace spinlock and the core map spinlock are held
// Move the data at 'cme' to swap and clear the CME / update the PTE.
void swap_out(unsigned long cmi, struct addrspace *other_as) {

	struct core_map_entry *cme = &core_map[cmi];
	struct addrspace *as = cme->as;

	KASSERT(cme->md.busy == 0);
	KASSERT(cme->md.kernel == 0);
	KASSERT(as != NULL);

	cme->md.busy = 1;
	spinlock_release(&core_map_splk);
	spinlock_release(&other_as->addr_splk);

	spinlock_acquire(&as->addr_splk);
	spinlock_acquire(&core_map_splk);

	union page_table_entry *pte = VADDR_TO_PTE(cme->as->ptd, cme->va);

	while(pte->b) {
		spinlock_release(&core_map_splk);

		wchan_sleep(as->addr_wchan, &as->addr_splk);

		spinlock_acquire(&core_map_splk);
	}

	KASSERT(pte->p == 1);

	pte->b = 1;

	if(cme->md.tlb) {
		spinlock_release(&core_map_splk);
		spinlock_release(&as->addr_splk);

		const struct tlbshootdown ts = {TLBHI_VPAGE & cme->va, as};

		ipi_broadcast_tlbshootdown(&ts);
		cme->md.tlb = 0;

		spinlock_acquire(&as->addr_splk);
		spinlock_acquire(&core_map_splk);
	}

	if(cme->md.dirty || !cme->md.s_pres) {
		swap_copy_out(as, cmi);
	}

	KASSERT(cme->va != 0);
	KASSERT(cme->as != 0);
	KASSERT(cme->md.s_pres);

	pte->p = 0;
	pte->b = 0;
	pte->addr = cme->md.swap;

	KASSERT(cme->md.busy == 1);

	cme->va = 0;
	cme->as = 0;
	cme->md.all = 0;
	cme->md.busy = 1;

	wchan_wakeall(as->addr_wchan, &as->addr_splk);
	// now that the cme doesn't belong to that as,
	// they'll be able to wake up

	spinlock_release(&core_map_splk);
	spinlock_release(&as->addr_splk);

	spinlock_acquire(&other_as->addr_splk);
	spinlock_acquire(&core_map_splk);

	KASSERT(cme->md.busy == 1);

	cme->md.busy = 0;
	// no one can be waiting on this because it
	// doesn't have an address space
}


// *** Assumes that the address space and core map spinlocks are held
// Copy the data tracked by 'pte' in swap into the page referenced by 'cme'
void swap_copy_in(struct addrspace *as, vaddr_t vaddr, unsigned long cmi) {

	union page_table_entry *pte = VADDR_TO_PTE(as->ptd, vaddr);
	struct core_map_entry *cme = &core_map[cmi];

	KASSERT(cme->md.kernel == 0);
	KASSERT(cme->md.busy == 0);
	KASSERT(cme->va == 0);
	KASSERT(pte->b == 0);

	KASSERT(pte->p == 0);
	KASSERT(pte->addr != 0);

	cme->md.busy = 1;
	pte->b = 1;

	spinlock_release(&core_map_splk);
	spinlock_release(&as->addr_splk);

	lock_acquire(swap_lk);

	struct iovec iov;
	struct uio uio;
	uio_kinit(&iov, &uio, (void *) PADDR_TO_KVADDR(CMI_TO_PADDR(cmi)), PAGE_SIZE, pte->addr * PAGE_SIZE, UIO_READ);

	int err = VOP_READ(swap_vnode, &uio);
	if(err != 0)
		panic("Read from swap failed\n");

	lock_release(swap_lk);

	spinlock_acquire(&as->addr_splk);
	spinlock_acquire(&core_map_splk);

	KASSERT(cme->md.busy == 1);
	KASSERT(pte->b == 1);

	cme->va = vaddr;
	cme->as = as;
	cme->md.all = 0;	// also sets busy to 0
	cme->md.swap = pte->addr;
	cme->md.s_pres = 1;

	KASSERT(cme->md.kernel == 0);

	pte->addr = ADDR_TO_FRAME(CMI_TO_PADDR(cmi));
	pte->p = 1;
	pte->b = 0;

	wchan_wakeall(as->addr_wchan, &as->addr_splk);
}


// *** Assumes that the address space and core map spinlocks are held
// Move the data tracked by 'pte' in swap into memory.
// (Swap a page out first if there are no free core map entries.)
void swap_in(struct addrspace *as, vaddr_t vaddr) {

	long cmi = choose_page_to_swap();
	if(cmi == -1) {
		panic("Out of pages to swap :(\n");
	}

	KASSERT(core_map[cmi].md.kernel == 0);
	KASSERT(core_map[cmi].md.busy == 0);

	if(core_map[cmi].va != 0)
		swap_out(cmi, as);

	KASSERT(core_map[cmi].md.busy == 0);
	KASSERT(core_map[cmi].va == 0);
	KASSERT(core_map[cmi].md.kernel == 0);
	KASSERT(core_map[cmi].as == NULL);

	swap_copy_in(as, vaddr, cmi);
}


// *** Assumes that you hold the spinlock of the addrspace 'as' belongs to
// Gets the PTE for a virtual address, or creates one if it doesn't yet exist.
// If the existence of the PTE is an invariant, use VADDR_TO_PTE() instead.
static union page_table_entry* get_pte(struct addrspace *as, vaddr_t vaddr, bool as_splk) {

	struct page_table_directory *ptd = as->ptd;

	vaddr_t l1 = L1INDEX(vaddr);
	if(ptd->pts[l1] == 0) {
		if(as_splk)
			spinlock_release(&as->addr_splk);
		// no other address space will be allocating new PTEs, 
		// so we can let go of the spinlock without causing problems

		ptd->pts[l1] = kmalloc(sizeof(struct page_table));
		if(ptd->pts[l1] == NULL) {
			panic("kmalloc failed\n");
		}

		if(as_splk)
			spinlock_acquire(&as->addr_splk);

		bzero(ptd->pts[l1], sizeof(struct page_table));
	}

	vaddr_t l2 = L2INDEX(vaddr);

	return &ptd->pts[l1]->ptes[l2];
}


// *** Assumes the address space and core map spinlocks are held
static long find_cmi(struct addrspace *as) {
	long i;
	for(i = 0; i < (long) ncmes; i++) {
		if(!core_map[i].md.busy && core_map[i].va == 0) {
			nfree--;
			return i;
		}
	}

	i = choose_page_to_swap();
	if(i < 0) {
		panic("Out of swappable pages :(\n");
	}

	swap_out(i, as);

	KASSERT(core_map[i].md.busy == 0);
	KASSERT(core_map[i].va == 0);
	KASSERT(core_map[i].as == NULL);
	KASSERT(core_map[i].md.kernel == 0);

	return i;
}


// *** 'as_splk' marks whether the address space spinlock is held when calling the function
// *** If not 'as_splk', the CME has its busy bit set at the end - remember to unset and wake
// 'perms' is nonzero when calling from as_define_region()
// If no flags are set, appropriate default values for stack/heap are used 
// (but vaddr must be a valid stack/heap address).
// Returns 0 upon success.
int alloc_upage(struct addrspace *as, vaddr_t vaddr, uint8_t perms, bool as_splk) {
	KASSERT(vaddr < USERSPACETOP);

	union page_table_entry new_pte;
	new_pte.all = 0;

	if(perms == 0) {
		if(vaddr < as->heap_bottom || (vaddr >= as->heap_top && vaddr < USERSTACKBOTTOM) || vaddr >= USERSTACK)
			return EINVAL;
	}

	if(!as_splk)
		spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = get_pte(as, vaddr, true);

	KASSERT(pte->addr == 0);

	spinlock_acquire(&core_map_splk);

	long i = find_cmi(as);

	KASSERT(core_map[i].md.busy == 0);
	KASSERT(core_map[i].md.kernel == 0);
	KASSERT(core_map[i].va == 0);
	KASSERT(core_map[i].as == 0);

	core_map[i].va = vaddr;
	core_map[i].as = as;
	core_map[i].md.all = 0;
	new_pte.p = 1;
	new_pte.addr = ADDR_TO_FRAME(CMI_TO_PADDR(i));
	bzero((void *) PADDR_TO_KVADDR(CMI_TO_PADDR(i)), PAGE_SIZE);
	*pte = new_pte;

	KASSERT(pte->addr != 0);

	if(!as_splk)
		core_map[i].md.busy = 1;

	spinlock_release(&core_map_splk);

	if(!as_splk)
		spinlock_release(&as->addr_splk);
	return 0;
}


// *** Assumes no spinlocks are held except optionally the address space spinlock
// 'as_splk' marks whether the address space spinlock is held when calling the function
void free_upage(struct addrspace *as, vaddr_t vaddr, bool as_splk) {
	KASSERT(vaddr < USERSPACETOP);

	if(!as_splk)
		spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = VADDR_TO_PTE(as->ptd, vaddr);
	KASSERT(pte->addr != 0);

	while(pte->b)
		wchan_sleep(as->addr_wchan, &as->addr_splk);

	if(pte->p) {

		unsigned long i = PTE_TO_CMI(pte);

		spinlock_acquire(&core_map_splk);

		while(core_map[i].md.busy) {	// wait until the physical page isn't busy
			spinlock_release(&core_map_splk);

			wchan_sleep(as->addr_wchan, &as->addr_splk);

			if(!pte->p)
				goto swapped;

			spinlock_acquire(&core_map_splk);
		}

		KASSERT(core_map[i].va != 0);
		KASSERT(core_map[i].as == as);
		KASSERT(core_map[i].md.kernel == 0);
		KASSERT(core_map[i].md.busy == 0);
		KASSERT(pte->b == 0);

		if(core_map[i].md.tlb == 1) {
			int result = tlb_probe(TLBHI_VPAGE & vaddr, 0);
			if(result >= 0)
				tlb_write(TLBHI_INVALID(result), TLBLO_INVALID(), result);
		}

		core_map[i].va = 0;
		core_map[i].as = NULL;
		core_map[i].md.all = 0;
		nfree--;

		if(core_map[i].md.s_pres) {
			spinlock_release(&core_map_splk);
			spinlock_release(&as->addr_splk);	
			lock_acquire(swap_lk);

			bitmap_unmark(swap_bitmap, pte->addr);
			nswap--;

			lock_release(swap_lk);
			spinlock_acquire(&as->addr_splk);
		}
		else {
			spinlock_release(&core_map_splk);
		}
	}
	else {

		swapped:

		spinlock_release(&as->addr_splk);	
		// no other thread will access a pte that's only in swap, 
		// so we don't need to set the busy bit

		lock_acquire(swap_lk);

		bitmap_unmark(swap_bitmap, pte->addr);
		nswap--;

		lock_release(swap_lk);

		spinlock_acquire(&as->addr_splk);
	}

	pte->all = 0;

	if(!as_splk) 
		spinlock_release(&as->addr_splk);
}


// *** Assumes no spinlocks are held
// calls alloc_upage() multiple times with error handling
int alloc_upages(struct addrspace *as, vaddr_t vaddr, unsigned npages, uint8_t perms) {
	spinlock_acquire(&as->addr_splk);

	for(unsigned i = 0; i < npages; i++) {
		int err = alloc_upage(as, vaddr + i * PAGE_SIZE, perms, true);
		if(err != 0) {
			for(unsigned j = 0; j < i; j++) {
				free_upage(as, vaddr + i * PAGE_SIZE, true);
			}
			spinlock_release(&as->addr_splk);
			return err;
		}
	}
	spinlock_release(&as->addr_splk);
	return 0;
}

// *** Assumes no spinlocks are held
// calls free_upage() on pages that have contents
void free_upages(struct addrspace *as, vaddr_t vaddr, unsigned npages) {
	spinlock_acquire(&as->addr_splk);

	struct page_table_directory *ptd = as->ptd;

	unsigned long l1_start = L1INDEX(vaddr);
	unsigned long l1_max = l1_start + ROUND_UP(npages, NUM_PTES);
	for(unsigned long i = l1_start; i < l1_max; i++) {

		if(ptd->pts[i] != 0) {
			struct page_table *pt = ptd->pts[i];

			unsigned long l2_start = (i == l1_start) ? L2INDEX(vaddr) : 0;
			unsigned long l2_max = (i == l1_max - 1) ? L2INDEX(vaddr + npages * PAGE_SIZE) : NUM_PTES;
			if(l2_max == 0)	// previous line doesn't work for ptd-aligned vaddrs
				l2_max = NUM_PTES;

			for(unsigned long j = l2_start; j < l2_max; j++) {
				if(pt->ptes[j].addr != 0)
					free_upage(as, L12_TO_VADDR(i, j), true);	
			}
			if(l2_start == 0 && l2_max == NUM_PTES) {
				kfree(ptd->pts[i]);
				ptd->pts[i] = 0;
			}
		}

	}

	spinlock_release(&as->addr_splk);
}


// *** Assumes no spinlocks are held
void pth_copy(struct addrspace *old, struct addrspace *new) {

	spinlock_acquire(&old->addr_splk);
	// We don't need to acquire new's spinlock because once we put a copied entry into the
	// core map (so swap functions might now try to access it), we never touch its PTE again

	struct page_table_directory *old_ptd = old->ptd;

	unsigned long max = L1INDEX(USERSPACETOP);	// no page tables address MIPS_KSEG0 or up
	for(unsigned long i = 0; i < max; i++) {
		if(old_ptd->pts[i] != 0) {
			struct page_table *old_pt = old_ptd->pts[i];
			for(unsigned long j = 0; j < NUM_PTES; j++) {
				if(old_pt->ptes[j].addr != 0) {
					union page_table_entry *old_pte = &old_pt->ptes[j];

					spinlock_release(&old->addr_splk);
					union page_table_entry *new_pte = get_pte(new, L12_TO_VADDR(i,j), false);
					spinlock_acquire(&old->addr_splk);

					new_pte->addr = 0;
					new_pte->p = 1;

					spinlock_release(&old->addr_splk);

					int err = alloc_upage(new, L12_TO_VADDR(i,j), 1, false);
					// use '1' for perms so that any region is valid, not just stack/heap

					spinlock_acquire(&old->addr_splk);
					spinlock_acquire(&new->addr_splk);	// nothing will acquire new then old, so this won't deadlock
					spinlock_acquire(&core_map_splk);

					core_map[PTE_TO_CMI(new_pte)].md.busy = 0;
					wchan_wakeall(new->addr_wchan, &new->addr_splk);

					spinlock_release(&new->addr_splk);

					KASSERT(err == 0);

					if(!old_pte->p) {	// read from swap into the new page
					
						struct core_map_entry *cme = &core_map[PTE_TO_CMI(new_pte)];

						cme->md.busy = 1;
						old_pte->b = 1;

						spinlock_release(&core_map_splk);
						spinlock_release(&old->addr_splk);

						lock_acquire(swap_lk);

						struct iovec iov;
						struct uio uio;
						uio_kinit(&iov, &uio, (void *) PADDR_TO_KVADDR(CMI_TO_PADDR(PTE_TO_CMI(new_pte))), PAGE_SIZE, old_pte->addr * PAGE_SIZE, UIO_READ);

						int err = VOP_READ(swap_vnode, &uio);
						if(err != 0)
							panic("Read from swap failed\n");

						lock_release(swap_lk);

						spinlock_acquire(&old->addr_splk);
						spinlock_acquire(&new->addr_splk);
						spinlock_acquire(&core_map_splk);

						KASSERT(cme->md.busy == 1);
						KASSERT(old_pte->b == 1);

						cme->md.busy = 0;
						old_pte->b = 0;
						
						wchan_wakeall(old->addr_wchan, &old->addr_splk);
						wchan_wakeall(new->addr_wchan, &new->addr_splk);

						spinlock_release(&new->addr_splk);
					}
					else {
						KASSERT(core_map[PTE_TO_CMI(new_pte)].md.busy == 0);
						KASSERT(core_map[PTE_TO_CMI(new_pte)].as == new);

						memcpy((void *) PADDR_TO_KVADDR(FRAME_TO_ADDR(new_pte->addr)), 
								(void *) PADDR_TO_KVADDR(FRAME_TO_ADDR(old_pte->addr)), 
								PAGE_SIZE);
					}
					spinlock_release(&core_map_splk);
				}
			}
		}
	}

	spinlock_release(&old->addr_splk);
}


int perms_fault(struct addrspace *as, vaddr_t faultaddress) {
	spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = VADDR_TO_PTE(as->ptd, faultaddress);

	// just in case the page is swapped out after the permissions fault is triggered
	// but before it's handled
	while(pte->b) 
		wchan_sleep(as->addr_wchan, &as->addr_splk);

	if(!pte->p) {
		spinlock_release(&as->addr_splk);
		return 0;	// succeed so that the user program will fault again with a TLB miss
	}

	unsigned long i = PTE_TO_CMI(pte);

	spinlock_acquire(&core_map_splk);

	while(core_map[i].md.busy) {
		spinlock_release(&core_map_splk);

		wchan_sleep(as->addr_wchan, &as->addr_splk);

		spinlock_acquire(&core_map_splk);

		if(!pte->p)
			break;
	}

	if(!pte->p) {
		spinlock_release(&core_map_splk);
		spinlock_release(&as->addr_splk);
		return 0;	// succeed so that the user program will fault again with a TLB miss
	}

	core_map[i].md.dirty = 1;
	ndirty++;

	// spinlock turns off interrupts, so TLB won't get messed up

	uint32_t entryhi, entrylo;

	entryhi = faultaddress & TLBHI_VPAGE;

	uint32_t j = tlb_probe(entryhi, 0);
	tlb_read(&entryhi, &entrylo, j);

	entrylo |= TLBLO_DIRTY;
	tlb_write(entryhi, entrylo, j);

	spinlock_release(&core_map_splk);
	spinlock_release(&as->addr_splk);

	return 0;
}


// *** Assumes address space and core map spinlocks are held
// Returns the index of an entry in the TLB to be replaced.
static unsigned long choose_tlb_entry() {
	uint32_t oldentryhi = 0, oldentrylo = 0;
	unsigned long old_cmi;
	unsigned long tlbi;

	do {

		tlbi = random() % NUM_TLB;
		tlb_read(&oldentryhi, &oldentrylo, tlbi);
		if((oldentrylo & TLBLO_PPAGE) == 0) {
			old_cmi = 0;
			break;
		}

		old_cmi = PADDR_TO_CMI(oldentrylo & TLBLO_PPAGE);

	} while (core_map[old_cmi].md.busy);	// it's a pain to replace TLB entries in the middle of swap,
											// and because there are max 32 cpus, max 32 TLB entries can be busy

	if(old_cmi != 0) {
		core_map[old_cmi].md.tlb = 0;
		core_map[old_cmi].md.recent = 1;
	}

	return tlbi;
}


int tlb_miss(struct addrspace *as, vaddr_t faultaddress) {
	spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = get_pte(as, faultaddress, true);

	if(pte->addr == 0) {
		int err = alloc_upage(as, faultaddress, 0, true);
		if(err != 0) {
			spinlock_release(&as->addr_splk);
			return err;
		}
	}
	
	while(pte->b)
		wchan_sleep(as->addr_wchan, &as->addr_splk);

	spinlock_acquire(&core_map_splk);

	if(!pte->p)
		swap_in(as, faultaddress);

	unsigned long cmi = PTE_TO_CMI(pte);
	core_map[cmi].md.tlb = 1;

	uint32_t newentryhi = 0, newentrylo = 0;

	newentryhi = faultaddress & TLBHI_VPAGE;
	newentrylo = (FRAME_TO_ADDR(pte->addr) & TLBLO_PPAGE) | TLBLO_VALID;
	// write permissions aren't set so we can track the dirty bit

	unsigned long tlbi = choose_tlb_entry();

	tlb_write(newentryhi, newentrylo, tlbi);

	spinlock_release(&core_map_splk);
	spinlock_release(&as->addr_splk);

	return 0;
}

void invalidate_tlb(void) {
	for(unsigned i = 0; i < NUM_TLB; i++)
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
}


void vm_tlbshootdown(const struct tlbshootdown *ts) {
	int result = tlb_probe(ts->oldentryhi, 0);
	if(result >= 0)
		tlb_write(TLBHI_INVALID(result), TLBLO_INVALID(), result);

	spinlock_acquire(&ts_splk);

	ts_count--;
	if(ts_count == 0) {
		wchan_wakeall(ts_wchan, &ts_splk);
	}

	spinlock_release(&ts_splk);
}