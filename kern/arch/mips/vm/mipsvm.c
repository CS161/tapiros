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
static struct page_table_entry* get_pte(struct page_table_directory *ptd, vaddr_t vaddr) {
	
	vaddr_t l1 = vaddr >> 22;
	if(ptd->pts[l1] == 0) {
		ptd->pts[l1] = kmalloc(sizeof(struct page_table));
		memset(ptd->pts[l1], 0, sizeof(struct page_table));
	}

	vaddr_t l2 = (vaddr << 10) >> 22;

	return (struct page_table_entry *) (ptd->pts[l1] + l2);
}

// ***Assumes that no spinlocks are held
// 'perms' is used to flag executable, read, and write permissions as follows: 00000xrw
// If no flags are set, appropriate default values for stack/heap are used.
// Returns 0 upon success.
int alloc_upage(struct addrspace *as, vaddr_t vaddr, uint8_t perms) {
	KASSERT(vaddr < USERSPACETOP);

	spinlock_acquire(&as->addr_splk);

	struct page_table_entry *pte = get_pte(as->ptd, vaddr);
	KASSERT(pte->addr == 0);

	(void) vaddr;
	(void) perms;

	spinlock_release(&as->addr_splk);
	return 0;
}


// ***Assumes that no spinlocks are held
void free_upage(struct addrspace *as, vaddr_t vaddr) {
	KASSERT(vaddr < USERSPACETOP);

	spinlock_acquire(&as->addr_splk);
	
	struct page_table_entry *pte = get_pte(as->ptd, vaddr);
	KASSERT(pte->addr != 0);

	unsigned long i = PTE_TO_PADDR(pte) / PAGE_SIZE;

	spinlock_acquire(&core_map_splk);

	while(core_map[i].md.busy) {	// wait until the physical page isn't busy
		spinlock_release(&core_map_splk);

		wchan_sleep(as->addr_wchan, &as->addr_splk);

		spinlock_acquire(&core_map_splk);
	}

	KASSERT(core_map[i].va != 0);
	KASSERT(core_map[i].md.kernel == 0);
	KASSERT(core_map[i].md.busy == 0);
	KASSERT(pte->b == 0);
	// pte->b should be 1 a subset of the time core_map[i].md.busy is 1

	// will need to handle tlb shootdowns and swap
	core_map[i].va = 0;
	core_map[i].md.swap = 0;
	core_map[i].md.recent = 0;
	core_map[i].md.tlb = 0;
	core_map[i].md.dirty = 0;
	core_map[i].md.contig = 0;
	core_map[i].md.s_pres = 0;

	pte->addr = 0;
	pte->x = 0;
	pte->r = 0;
	pte->w = 0;
	pte->p = 0;

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

	struct page_table_entry *pte = get_pte(as->ptd, faultaddress);

	if(!pte->w) {	// check if the page actually doesn't permit writes
		spinlock_release(&as->addr_splk);
		return EFAULT;
	}

	unsigned long i = PTE_TO_PADDR(pte) / PAGE_SIZE;

	spinlock_acquire(&core_map_splk);

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

	struct page_table_entry *pte = get_pte(as->ptd, faultaddress);

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