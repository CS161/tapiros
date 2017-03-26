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
	
	vaddr_t l1 = vaddr >> 22;
	if(ptd->pts[l1] == 0) {
		ptd->pts[l1] = kmalloc(sizeof(struct page_table));
		memset(ptd->pts[l1], 0, sizeof(struct page_table));
	}

	vaddr_t l2 = (vaddr << 10) >> 22;

	return (union page_table_entry *) (ptd->pts[l1] + l2);
}

// ***Assumes that no spinlocks are held
// 'perms' is used to flag executable, read, and write permissions as follows: 00000xrw
// If no flags are set, appropriate default values for stack/heap are used 
// (but vaddr must be a valid stack/heap address).
// Returns 0 upon success.
int alloc_upage(struct addrspace *as, vaddr_t vaddr, uint8_t perms) {
	KASSERT(vaddr < USERSPACETOP);

	union page_table_entry new_pte;
	new_pte.all = 0;

	// sys161 doesn't support executable perms, but might as well support them
	if(perms != 0) {
		if(perms & 1)
			new_pte.w = 1;
		if(perms & 2)
			new_pte.r = 1;
		if(perms & 4)
			new_pte.x = 1;
	}
	else {
		if((vaddr > as->heap_bottom && vaddr < as->heap_top) || (vaddr > USERSTACKBOTTOM && vaddr < USERSTACK)) {
			new_pte.w = 1;
			new_pte.r = 1;
		}
		else
			return EINVAL;
	}

	spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = get_pte(as->ptd, vaddr);
	KASSERT(pte->addr == 0);

	spinlock_acquire(&core_map_splk);

	unsigned long i;
	for(i = 0; i < ncmes; i++) {
		if(!core_map[i].md.busy && core_map[i].va == 0) {
			core_map[i].va = vaddr;
			core_map[i].as = as;
			new_pte.p = 1;
			new_pte.addr = CMI_TO_PADDR(i);
			*pte = new_pte;
			break;
		}
	}
	if(i == ncmes) {
		// ***handle swap out then allocation
	}

	spinlock_release(&core_map_splk);

	spinlock_release(&as->addr_splk);
	return 0;
}


// ***Assumes that no spinlocks are held
void free_upage(struct addrspace *as, vaddr_t vaddr) {
	KASSERT(vaddr < USERSPACETOP);

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

	spinlock_release(&as->addr_splk);
}


// calls alloc_upage() multiple times with error handling
int alloc_upages(struct addrspace *as, vaddr_t vaddr, unsigned npages, uint8_t perms) {
	for(unsigned i = 0; i < npages; i++) {
		int err = alloc_upage(as, vaddr + i * PAGE_SIZE, perms);
		if(err != 0) {
			for(unsigned j = 0; j < i; j++) {
				free_upage(as, vaddr + i * PAGE_SIZE);
			}
			return err;
		}
	}
	return 0;
}


void pth_free(struct addrspace *as, struct page_table_directory *ptd) {

	// doesn't traverse the whole page table yet

	(void) as;

	kfree(ptd);
}


int perms_fault(struct addrspace *as, vaddr_t faultaddress) {
	spinlock_acquire(&as->addr_splk);

	union page_table_entry *pte = get_pte(as->ptd, faultaddress);

	if(!pte->w) {	// check if the page actually doesn't permit writes
		spinlock_release(&as->addr_splk);
		return EFAULT;
	}

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

	}
	(void) pte;

	spinlock_release(&as->addr_splk);

	return 0;
}


void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void)ts;

	panic("Can't vm_tlbshootdown yet!\n");
}