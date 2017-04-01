/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _VM_H_
#define _VM_H_

/*
 * VM system-related definitions.
 *
 * You'll probably want to add stuff here.
 */


#include <machine/vm.h>
#include <lib.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <kern/errno.h>
#include <bitmap.h>
#include <thread.h>
#include <mips/tlb.h>

/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ        0    /* A read was attempted */
#define VM_FAULT_WRITE       1    /* A write was attempted */
#define VM_FAULT_READONLY    2    /* A write to a readonly page was attempted*/

union metadata {
	struct {
		unsigned int swap : 20, : 5;	// address in swap
		unsigned int recent : 1;		// recently evicted from TLB
		unsigned int tlb : 1;			// currently in TLB
		unsigned int dirty : 1;			// dirty page
		unsigned int contig : 1;		// end of kernel allocation
		unsigned int kernel : 1;		// belongs to kernel
		unsigned int s_pres : 1;		// present in swap
		unsigned int busy : 1;			// busy
	};
	uint32_t all;	// for zeroing metadata in one instruction
};

struct core_map_entry {
	vaddr_t va;				// virtual address of the page
	struct addrspace *as;	// address space for the virtual address
	uint32_t reserved;		// reserved to make nicely aligned 16 byte entries
							// could put other stuff here, like a refcount if you
							// implemented copy on write
	union metadata md;		// 4 bytes of metadata
};

struct core_map_entry *core_map;
unsigned long ncmes;				// number of core map entries
unsigned long clock;				// pointer to clock hand for page eviction algorithm
struct spinlock core_map_splk;

// stat tracking
unsigned long nfree;	// number of free physical pages
unsigned long ndirty;	// number of dirty physical pages
unsigned long nswap;	// number of pages in swap

struct vnode *swap_vnode;
struct bitmap *swap_bitmap;
struct lock *swap_lk;			// protects access to swap_bitmap and swap_vnode
unsigned long swap_size;

/* Initialization function */
void vm_bootstrap(void);
void swap_bootstrap(void);
int print_core_map(int nargs, char **args);	// accessed with 'cm' from the kernel menu

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);
int perms_fault(struct addrspace *as, vaddr_t faultaddress);
int tlb_miss(struct addrspace *as, vaddr_t faultaddress);

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(unsigned npages);
void free_kpages(vaddr_t addr);

/* Allocate/free user pages */
// See mipsvm.c for which spinlocks must/must not be held when calling these
int alloc_upage(struct addrspace *as, vaddr_t vaddr, uint8_t perms, bool as_splk); // 'perms' is nonzero from as_define_region
int alloc_upages(struct addrspace *as, vaddr_t vaddr, unsigned npages, uint8_t perms);
void free_upage(struct addrspace *as, vaddr_t vaddr, bool as_splk);
void free_upages(struct addrspace *as, vaddr_t vaddr, unsigned npages);

// deep copy all pages in the page table hierarchy in 'old' to 'new'
void pth_copy(struct addrspace *old, struct addrspace *new);

/* Swap in/out pages */
// See mipsvm.c for which spinlocks must/must not be held when calling these
void swap_in(struct addrspace *as, vaddr_t vaddr);
void swap_copy_in(struct addrspace *as, vaddr_t vaddr, unsigned long cmi);
void swap_out(unsigned long cmi, struct addrspace *other_as);
void swap_copy_out(struct addrspace *as, unsigned long cmi);

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown(const struct tlbshootdown *ts);

/* Invalidate the entire TLB; used in as_activate() */
void invalidate_tlb(void);


#endif /* _VM_H_ */
