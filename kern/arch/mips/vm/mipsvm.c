/*
 * Machine dependent VM functions.
 */

#include <vm.h>
#include <spl.h>
#include <cpu.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <wchan.h>
#include <vfs.h>
#include <vnode.h>
#include <kern/stat.h>


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

	swap_bitmap = bitmap_create(stats.st_size / PAGE_SIZE);
	if(swap_bitmap == NULL) {
		panic("bitmap_create of swap_bitmap failed\n");
	}
	bitmap_mark(swap_bitmap, 0); // fill the 0th index, since we don't want bitmap_alloc to return it later

	swap_lk = lock_create("swap_lk");
	if(swap_lk == NULL) {
		panic("lock_create of swap_lk failed\n");
	}

	clock = 0;
}


// *** Assumes the core map spinlock is held
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
	while(nchecked < twice) {			// if there's nothing on the second loop, give up
		if(clock == ncmes)
			clock = 0;
		else if(!core_map[clock].md.kernel && !core_map[clock].md.busy) {	// accept entries in tlb
			clock++;
			return clock - 1;
		}
		clock++;
		nchecked++;
	}
	return -1;
}


// *** Assumes that [] are held
// Move the data tracked by 'pte' in swap into memory.
// (Swap a page out first if there are no free core map entries.)
void swap_in(struct addrspace *as, union page_table_entry *pte) {
	(void)as;
	(void)pte;
	long cmi = choose_page_to_swap();
	if(cmi == -1) {
		panic("Out of pages to swap :(\n");
	}

	return;
}


// *** Assumes that no spinlocks are held
// Make a copy of data at 'cmi' in swap, and update the PTE/CME appropriately.
void swap_out(struct core_map_entry *cme) {
	(void)cme;
	return;
}


// *** Assumes that you hold the spinlock of the addrspace 'ptd' belongs to
// Gets the PTE for a virtual address, or creates one if it doesn't yet exist.
// If the existence of the PTE is an invariant, use VADDR_TO_PTE() instead.
static union page_table_entry* get_pte(struct page_table_directory *ptd, vaddr_t vaddr) {
	
	vaddr_t l1 = L1INDEX(vaddr);
	if(ptd->pts[l1] == 0) {
		ptd->pts[l1] = kmalloc(sizeof(struct page_table));
		bzero(ptd->pts[l1], sizeof(struct page_table));
	}

	vaddr_t l2 = L2INDEX(vaddr);

	KASSERT((unsigned long)(ptd->pts[l1] + l2) % PAGE_SIZE == 0);

	return &ptd->pts[l1]->ptes[l2];
}


// 'perms' is nonzero when calling from as_define_region()
// 'as_splk' marks whether the address space spinlock is held when calling the function
// If no flags are set, appropriate default values for stack/heap are used 
// (but vaddr must be a valid stack/heap address).
// Returns 0 upon success.
int alloc_upage(struct addrspace *as, vaddr_t vaddr, uint8_t perms, bool as_splk) {
	KASSERT(vaddr < USERSPACETOP);

	union page_table_entry new_pte;
	new_pte.all = 0; //Clear entry

	if(perms == 0) {
		if(vaddr < as->heap_bottom || (vaddr >= as->heap_top && vaddr < USERSTACKBOTTOM) || vaddr >= USERSTACK)
			return EINVAL;
	}

	if(!as_splk)
		spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = get_pte(as->ptd, vaddr);
	KASSERT(pte->addr == 0);

	spinlock_acquire(&core_map_splk);

	unsigned long i;
	for(i = 0; i < ncmes; i++) {
		if(!core_map[i].md.busy && core_map[i].va == 0) {
			KASSERT(core_map[i].md.kernel == 0);
			core_map[i].va = vaddr;
			core_map[i].as = as;
			core_map[i].md.recent = 1;
			new_pte.p = 1;
			new_pte.addr = CMI_TO_PADDR(i) >> 12;
			bzero((void *) PADDR_TO_KVADDR(CMI_TO_PADDR(i)), PAGE_SIZE);
			*pte = new_pte;
			break;
		}
	}
	if(i == ncmes) {
		panic("Out of memory :(\n");
		// ***handle swap out then allocation
	}

	KASSERT(pte->addr != 0);

	spinlock_release(&core_map_splk);

	if(!as_splk)
		spinlock_release(&as->addr_splk);
	return 0;
}


// 'as_splk' marks whether the address space spinlock is held when calling the function
void free_upage(struct addrspace *as, vaddr_t vaddr, bool as_splk) {
	KASSERT(vaddr < USERSPACETOP);

	if(!as_splk)
		spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = VADDR_TO_PTE(as->ptd, vaddr);
	KASSERT(pte->addr != 0);

	unsigned long i = PTE_TO_CMI(pte);

	spinlock_acquire(&core_map_splk);

	while(core_map[i].md.busy) {	// wait until the physical page isn't busy
		spinlock_release(&core_map_splk);

		wchan_sleep(as->addr_wchan, &as->addr_splk);

		spinlock_acquire(&core_map_splk);
	}

	KASSERT(core_map[i].va != 0);
	KASSERT(core_map[i].as == as);
	KASSERT(core_map[i].md.kernel == 0);
	KASSERT(core_map[i].md.busy == 0);
	KASSERT(pte->b == 0);
	// pte->b should be 1 a subset of the time core_map[i].md.busy is 1

	// ***will need to handle tlb shootdowns and pages in swap
	core_map[i].va = 0;
	core_map[i].as = NULL;
	core_map[i].md.all = 0;

	pte->all = 0;

	spinlock_release(&core_map_splk);

	if(!as_splk)
		spinlock_release(&as->addr_splk);
}


// *** Assumes no spinlocks are held
// calls alloc_upage() multiple times with error handling
int alloc_upages(struct addrspace *as, vaddr_t vaddr, unsigned npages, uint8_t perms) {
	for(unsigned i = 0; i < npages; i++) {
		int err = alloc_upage(as, vaddr + i * PAGE_SIZE, perms, false);
		if(err != 0) {
			for(unsigned j = 0; j < i; j++) {
				free_upage(as, vaddr + i * PAGE_SIZE, false);
			}
			return err;
		}
	}
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
	struct page_table_directory *new_ptd = new->ptd;

	unsigned long max = L1INDEX(USERSPACETOP);	// no page tables address MIPS_KSEG0 or up
	for(unsigned long i = 0; i < max; i++) {
		if(old_ptd->pts[i] != 0) {
			struct page_table *old_pt = old_ptd->pts[i];
			for(unsigned long j = 0; j < NUM_PTES; j++) {
				if(old_pt->ptes[j].addr != 0) {
					union page_table_entry *old_pte = &old_pt->ptes[j];
					union page_table_entry *new_pte = get_pte(new_ptd, L12_TO_VADDR(i,j));
					new_pte->addr = 0;
					new_pte->p = 1;

					int err = alloc_upage(new, L12_TO_VADDR(i,j), 1, true);
						// use '1' for perms so that any region is valid, not just stack/heap
						// pretend we hold the spinlock already because we know it's not needed
					KASSERT(err == 0);

					if(!old_pte->p) {
						// read from swap into the new page
					}
					else {
						memcpy((void *) PADDR_TO_KVADDR(new_pte->addr << 12), 
								(void *) PADDR_TO_KVADDR(old_pte->addr << 12), 
								PAGE_SIZE);
					}
				}
			}
		}
	}

	spinlock_release(&old->addr_splk);
}


int perms_fault(struct addrspace *as, vaddr_t faultaddress) {
	spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = VADDR_TO_PTE(as->ptd, faultaddress);

	unsigned long i = PTE_TO_CMI(pte);

	spinlock_acquire(&core_map_splk);

	// *** need to think about the possibility of swap happening after this fault starts

	while(core_map[i].md.busy) {	// wait until the physical page isn't busy
		spinlock_release(&core_map_splk);

		wchan_sleep(as->addr_wchan, &as->addr_splk);

		spinlock_acquire(&core_map_splk);
	}

	core_map[i].md.dirty = 1;

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

	union page_table_entry *pte = get_pte(as->ptd, faultaddress);

	if(pte->addr == 0) {
		int err = alloc_upage(as, faultaddress, 0, true);
		if(err != 0) {
			spinlock_release(&as->addr_splk);
			return err;
		}
	}
	
	while(pte->b)
		wchan_sleep(as->addr_wchan, &as->addr_splk);

	if(!pte->p) {
		// swap stuff goes here
		panic("tlb_miss: !pte_p shouldn't be possible without swap\n");
	}

	spinlock_acquire(&core_map_splk);

	unsigned long cmi = PTE_TO_CMI(pte);
	core_map[cmi].md.tlb = 1;

	uint32_t newentryhi = 0, newentrylo = 0;

	newentryhi = faultaddress & TLBHI_VPAGE;
	newentrylo = (pte->addr << 12 & TLBLO_PPAGE) | TLBLO_VALID;
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

void vm_tlbshootdown(const struct tlbshootdown *ts){
	int res = tlb_probe(ts->oldentryhi, 0);
	if (res > -1 ){
		tlb_write(TLBHI_INVALID(res), TLBLO_INVALID(),res);
	}
	spinlock_acquire(ts->sd_lock);
	shootdown_count--;
	if(shootdown_count == 0){ //Last CPU gets to wake all!
		wchan_wakeall(ts->targetaddress->addr_wchan, &(ts->targetaddress->addr_splk));
	}
	spinlock_release(ts->sd_lock);

}
