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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <wchan.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		goto err1;
	}

	as->ptd = kmalloc(sizeof(struct page_table_directory));
	if(as->ptd == NULL) {
		goto err2;
	}
	bzero(as->ptd, sizeof(struct page_table_directory));

	as->addr_wchan = wchan_create("addrspace wchan");
	if(as->addr_wchan == NULL) {
		goto err3;
	}

	spinlock_init(&as->addr_splk);

	as->heap_bottom = 0;
	as->heap_top = 0;

	return as;

	err3:
		kfree(as->ptd);
	err2:
		kfree(as);
	err1:
		return NULL;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new == NULL) {
		return ENOMEM;
	}

	pth_copy(old, new);
	new->heap_bottom = old->heap_bottom;
	new->heap_top = old->heap_top;

	*ret = new;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	free_upages(as, 0, USERSPACETOP / PAGE_SIZE);
	kfree(as->ptd);

	spinlock_cleanup(&as->addr_splk);
	wchan_destroy(as->addr_wchan);
	kfree(as);
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	invalidate_tlb();
}

void
as_deactivate(void)
{
	/*
	 * Do nothing.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	(void) readable;
	(void) writeable;
	(void) executable;

	vaddr &= PAGE_FRAME;
	uint8_t perms = 1;

	unsigned npages = (memsize + PAGE_SIZE - 1) / PAGE_SIZE; // round up memsize to next page

	if(vaddr + npages * PAGE_SIZE > USERHEAPTOP)
		return EINVAL;

	int err = alloc_upages(as, vaddr, npages, perms);
	if(err != 0)
		return err;

	as->heap_bottom = vaddr + npages * PAGE_SIZE;
	as->heap_top = as->heap_bottom;

	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	/*
	 * Do nothing.
	 */

	(void) as;
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	if(as->heap_bottom == 0)	// if an executable has no code region
		return EINVAL;

	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	(void)as;

	// We use on-demand paging for the stack, so this function
	// doesn't need to do much.

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}

