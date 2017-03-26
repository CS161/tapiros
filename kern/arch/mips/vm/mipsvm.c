/*
 * Machine dependent VM functions.
 */

#include <vm.h>
#include <current.h>
#include <spl.h>
#include <cpu.h>
#include <kern/errno.h>
#include <proc.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <wchan.h>


void pth_free(struct addrspace *as, struct page_table_directory *ptd) {

	// doesn't traverse the whole page table yet

	(void) as;

	kfree(ptd);
}

// assumes that you hold the spinlock of the addrspace 'ptd' belongs to
static struct page_table_entry* get_pte(struct page_table_directory *ptd, vaddr_t addr) {
	
	vaddr_t l1 = addr >> 22;
	if(ptd->pts[l1] == 0)
		ptd->pts[l1] = kmalloc(sizeof(struct page_table));

	vaddr_t l2 = (addr << 10) >> 22;

	return (struct page_table_entry *) (ptd->pts[l1] + l2);
}

static int perms_fault(struct addrspace *as, vaddr_t faultaddress) {
	spinlock_acquire(&as->addr_splk);

	struct page_table_entry *pte = get_pte(as->ptd, faultaddress);
	if(!pte->w) {	// check if the page actually doesn't permit writes
		spinlock_release(&as->addr_splk);
		return EFAULT;
	}

	spinlock_acquire(&core_map_splk);

	unsigned long i = PTE_TO_PADDR(pte) / PAGE_SIZE;

	while(core_map[i].md.busy) {	// wait until the physical page isn't busy
		spinlock_release(&core_map_splk);

		wchan_sleep(as->addr_wchan, &as->addr_splk);

		spinlock_acquire(&core_map_splk);
	}

	core_map[i].md.dirty = 1;

	// update TLB entry with writeable bit here
	// to be implemented

	spinlock_release(&core_map_splk);
	spinlock_release(&as->addr_splk);

	return 0;
}

static int tlb_miss(struct addrspace *as, vaddr_t faultaddress) {
	spinlock_acquire(&as->addr_splk);

	struct page_table_entry *pte = get_pte(as->ptd, faultaddress);
	(void) pte;

	spinlock_release(&as->addr_splk);

	return 0;
}

int vm_fault(int faulttype, vaddr_t faultaddress) {

	if(faultaddress >= USERSPACETOP) {
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

void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void)ts;

	panic("Can't vm_tlbshootdown yet!\n");
}