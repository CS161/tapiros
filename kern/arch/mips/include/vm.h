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

#ifndef _MIPS_VM_H_
#define _MIPS_VM_H_


#include <types.h>
#include <mips/tlb.h>

/*
 * Machine-dependent VM system definitions.
 */

#define PAGE_SIZE  			4096         	// size of VM page
#define PAGE_FRAME 			0xfffff000   	// mask for getting page number from addr
#define NUM_PTES			1024			// number of PTES per page table

/*
 * MIPS-I hardwired memory layout:
 *    0xc0000000 - 0xffffffff   kseg2 (kernel, tlb-mapped)
 *    0xa0000000 - 0xbfffffff   kseg1 (kernel, unmapped, uncached)
 *    0x80000000 - 0x9fffffff   kseg0 (kernel, unmapped, cached)
 *    0x00000000 - 0x7fffffff   kuseg (user, tlb-mapped)
 *
 * (mips32 is a little different)
 */

#define MIPS_KUSEG  0x00000000
#define MIPS_KSEG0  0x80000000
#define MIPS_KSEG1  0xa0000000
#define MIPS_KSEG2  0xc0000000

/*
 * The first 512 megs of physical space can be addressed in both kseg0 and
 * kseg1. We use kseg0 for the kernel. This macro returns the kernel virtual
 * address of a given physical address within that range. (We assume we're
 * not using systems with more physical space than that anyway.)
 *
 * N.B. If you, say, call a function that returns a paddr or 0 on error,
 * check the paddr for being 0 *before* you use this macro. While paddr 0
 * is not legal for memory allocation or memory management (it holds
 * exception handler code) when converted to a vaddr it's *not* NULL, *is*
 * a valid address, and will make a *huge* mess if you scribble on it.
 */
#define PADDR_TO_KVADDR(paddr) 		((paddr) + MIPS_KSEG0)

#define L1INDEX(vaddr)			((vaddr) >> 22)
#define L2INDEX(vaddr)			(((vaddr) << 10) >> 22)
#define L12_TO_VADDR(l1, l2)	(((l1) << 22) | ((l2) << 12))

#define PTE_TO_CMI(pte) 			(((pte->addr << 12) - ((vaddr_t)core_map - MIPS_KSEG0)) / PAGE_SIZE)
#define PADDR_TO_CMI(paddr) 		(((paddr) - ((vaddr_t)core_map - MIPS_KSEG0)) / PAGE_SIZE)
#define CMI_TO_PADDR(cmi)			(((vaddr_t)core_map + (cmi) * PAGE_SIZE) - MIPS_KSEG0)
#define VADDR_TO_PTE(ptd, vaddr)	(&ptd->pts[L1INDEX(vaddr)]->ptes[L2INDEX(vaddr)])
#define ADDR_TO_FRAME(addr)			((addr) >> 12)
#define FRAME_TO_ADDR(frame)		((frame) << 12)

#define ROUND_UP(num, denom)			((((num) - 1) / (denom)) + 1)

/*
 * The top of user space. (Actually, the address immediately above the
 * last valid user address.)
 */
#define USERSPACETOP  MIPS_KSEG0

/*
 * The starting value for the stack pointer at user level.  Because
 * the stack is subtract-then-store, this can start as the next
 * address after the stack area.
 *
 * We put the stack at the very top of user virtual memory because it
 * grows downwards.
 */
#define USERSTACK     	(USERSPACETOP)
#define USERSTACKBOTTOM	(USERSPACETOP - (1024 * PAGE_SIZE))	// 1024 stack pages allowed
#define USERHEAPSIZE	(2048 * PAGE_SIZE)	// 8 MiB

union page_table_entry {
	struct {
		unsigned int addr : 20, : 7;	// address in memory or swap
		unsigned int x : 1;				// executable (unused)
		unsigned int r : 1;				// readable (unused)
		unsigned int w : 1;				// writeable (unused)
		unsigned int p : 1;				// present
		unsigned int b : 1;				// busy
	};
	uint32_t all;	// for zeroing the PTE in one instruction
};

struct page_table {
	union page_table_entry ptes[NUM_PTES];
};

struct page_table_directory {
	struct page_table* pts[NUM_PTES];
};

/*
 * Interface to the low-level module that looks after the amount of
 * physical memory we have.
 *
 * ram_getsize returns one past the highest valid physical
 * address. (This value is page-aligned.)  The extant RAM ranges from
 * physical address 0 up to but not including this address.
 *
 * ram_getfirstfree returns the lowest valid physical address. (It is
 * also page-aligned.) Memory at this address and above is available
 * for use during operation, and excludes the space the kernel is
 * loaded into and memory that is grabbed in the very early stages of
 * bootup. Memory below this address is already in use and should be
 * reserved or otherwise not managed by the VM system. It should be
 * called exactly once when the VM system initializes to take over
 * management of physical memory.
 *
 * ram_stealmem can be used before ram_getsize is called to allocate
 * memory that cannot be freed later. This is intended for use early
 * in bootup before VM initialization is complete.
 */

void ram_bootstrap(void);
paddr_t ram_stealmem(unsigned long npages);
paddr_t ram_getsize(void);
paddr_t ram_getfirstfree(void);

/*
 * TLB shootdown bits.
 *
 * We'll take up to 16 invalidations before just flushing the whole TLB.
 */

struct tlbshootdown {
	uint32_t oldentryhi;
	struct addrspace *as;
};

unsigned ts_count;
struct spinlock ts_splk;
struct wchan *ts_wchan;

#define TLBSHOOTDOWN_MAX 16


#endif /* _MIPS_VM_H_ */
