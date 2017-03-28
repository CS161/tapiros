/*
 * Machine dependent VM functions.
 */

#include <vm.h>
#include <spl.h>
#include <cpu.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <wchan.h>


// ***Assumes that you hold the spinlock of the addrspace 'ptd' belongs to
static union page_table_entry* get_pte(struct page_table_directory *ptd, vaddr_t vaddr) {
	
	vaddr_t l1 = L1INDEX(vaddr);
	if(ptd->pts[l1] == 0) {
		ptd->pts[l1] = kmalloc(sizeof(struct page_table));
		memset(ptd->pts[l1], 0, sizeof(struct page_table));
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
	new_pte.all = 0;

	if(perms == 0) {
		if(!((vaddr > as->heap_bottom && vaddr < as->heap_top) || (vaddr > USERSTACKBOTTOM && vaddr < USERSTACK)))
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
			memset((void *) PADDR_TO_KVADDR(CMI_TO_PADDR(i)), 0, PAGE_SIZE);
			*pte = new_pte;
			break;
		}
	}
	if(i == ncmes) {
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
	
	union page_table_entry *pte = get_pte(as->ptd, vaddr);
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


// ***Assumes no spinlocks are held
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
void pth_free(struct addrspace *as) {
	spinlock_acquire(&as->addr_splk);

	struct page_table_directory *ptd = as->ptd;

	unsigned long max = L1INDEX(USERSPACETOP);	// no page tables address MIPS_KSEG0 or up
	for(unsigned long i = 0; i < max; i++) {
		if(ptd->pts[i] != 0) {
			struct page_table *pt = ptd->pts[i];
			for(unsigned long j = 0; j < NUM_PTES; j++) {
				if(pt->ptes[j].addr != 0) {
					free_upage(as, L12_TO_VADDR(i, j), true);	
					// this finds the PTE again inside - might want to refactor to avoid redundancy
				}
			}
			kfree(ptd->pts[i]);
		}
	}
	kfree(ptd);

	spinlock_release(&as->addr_splk);
}


int perms_fault(struct addrspace *as, vaddr_t faultaddress) {
	spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = get_pte(as->ptd, faultaddress);

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

	uint32_t oldentryhi = 0, oldentrylo = 0;
	uint32_t newentryhi = 0, newentrylo = 0;

	newentryhi = faultaddress & TLBHI_VPAGE;
	newentrylo = (pte->addr << 12 & TLBLO_PPAGE) | TLBLO_VALID;
	// write permissions aren't set so we can track the dirty bit

	unsigned long i;
	unsigned long cmj;
	do {

		i = random() % NUM_TLB;
		tlb_read(&oldentryhi, &oldentrylo, i);
		if((oldentrylo & TLBLO_PPAGE) == 0) {
			cmj = 0;
			break;
		}

		cmj = PADDR_TO_CMI(oldentrylo & TLBLO_PPAGE);

	} while (core_map[cmj].md.busy);	// it's a pain to replace TLB entries in the middle of swap,
										// and because there are max 32 cpus, max 32 TLB entries can be busy
	if(cmj != 0) {
		core_map[cmj].md.tlb = 0;
		core_map[cmj].md.recent = 1;
	}

	tlb_write(newentryhi, newentrylo, i);

	spinlock_release(&core_map_splk);
	spinlock_release(&as->addr_splk);

	return 0;
}

void invalidate_tlb(void) {
	for(unsigned i = 0; i < NUM_TLB; i++)
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
}


void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void)ts;

	panic("Can't vm_tlbshootdown yet!\n");
}