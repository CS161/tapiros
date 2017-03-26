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

	spinlock_acquire(&as->addr_splk);

	struct page_table_entry *pte = get_pte(as->ptd, faultaddress);
	(void) pte;

	switch(faulttype) {
		case VM_FAULT_READONLY:
			break;
		case VM_FAULT_READ:
		case VM_FAULT_WRITE:
			break;
		default:
			spinlock_release(&as->addr_splk);
			return EINVAL;
	}

	spinlock_release(&as->addr_splk);

	return 0;
}

void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void)ts;

	panic("Can't vm_tlbshootdown yet!\n");
}