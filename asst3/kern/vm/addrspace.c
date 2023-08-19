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
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

// initialize virtual address space with no regions
struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL)
	{
		return NULL;
	}

	as->regions = NULL;

	return as;
}

int as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas == NULL){
		return ENOMEM;
	}

	newas -> regions = NULL;
	struct region *old_region = old->regions;
	
	// Copy the regions
	while (old_region != NULL){

		struct region *temp = kmalloc(sizeof(struct region));
		if (temp == NULL){
			as_destroy(newas);
			return ENOMEM;
		}

		// copy all contents from old region to new region
		temp->base = old_region->base;
		temp->permission = old_region->permission;
		temp->size = old_region->size;
		temp->next = NULL;
		
		// insert new region at the head of linklist
		if (newas->regions == NULL){
			newas->regions = temp;
		} else {
			temp->next = newas->regions;
			newas->regions = temp;
		}

		old_region = old_region->next;
	}
	// copy each entry of old as
	int check_copy = 0;
	check_copy = copy_HPT((uint32_t)old, (uint32_t)newas);

	if (check_copy != 0){
		return ENOMEM;
	}

	*ret = newas;
	return 0;
}

void as_destroy(struct addrspace *as)
{
	struct region *temp = as->regions;
	// recursively free linklist (i.e all regions)
	while (temp != NULL)
	{
		struct region *next = temp->next;
		kfree(temp);
		temp = next;
	}

	remove_HPT((uint32_t)as);

	kfree(as);
}

void as_activate(void)
{
	struct addrspace *as;
	as = proc_getas();
	if (as == NULL){
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	int spl = splhigh();
	for (int i = 0; i < NUM_TLB; i++){
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	splx(spl);
}

void as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
	int spl = splhigh();

	for (int i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
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
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
										 int readable, int writeable, int executable)
{	
	/* Align the region. First, the base... */
	memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

		/* ...and now the length. */
	memsize = (memsize + PAGE_SIZE -1) & PAGE_FRAME;

	// check if the end address of this region is overlapped with kernel region
	if (as == NULL || (vaddr + memsize) > MIPS_KSEG0) {
		return EFAULT;
	}

	struct region *temp = as->regions;

	// Check if the region is overlapping with any other region (i.e data and code)
	while (temp != NULL) {
		if (vaddr < (temp->base + temp->size) && (vaddr + memsize) > temp->base) {
			return EFAULT;
		}
		temp = temp->next;
	}

	// Create a new region
	struct region *new_region = kmalloc(sizeof(struct region));
	if (new_region == NULL) {
		return ENOMEM;
	}
	new_region->base = vaddr;
	new_region->size = memsize;
	new_region->permission = 0;

	if (readable) {
		new_region->permission |= READABLE;
	}
	if (writeable) {
		new_region->permission |= WRITEABLE;
	}
	if (executable) {
		new_region->permission |= EXECUTABLE;
	}

	// Insert the new region at the start of linked list
	new_region->next = as->regions;
	as->regions = new_region;

	return 0;
}

int as_prepare_load(struct addrspace *as)
{
	if (as == NULL) {
		return EFAULT;
	}
	// make readalbe only region to be writeable
	struct region *temp = as->regions;
	while (temp != NULL) {
		if (temp->permission & READABLE) {
			temp->permission |= WRITEABLE;
			temp->permission |= LOADING;
		}
		temp = temp->next;
	}

	return 0;
}

int as_complete_load(struct addrspace *as)
{
	if (as == NULL) {
		return EFAULT;
	}

	// make readalbe only region to be writeable
	struct region *temp = as->regions;
	while (temp != NULL) {
		if (temp->permission & LOADING)
		{
			temp->permission &= ~WRITEABLE;
			temp->permission &= ~LOADING;
		}
		temp = temp->next;
	}

	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	// set size of stack, 16 pages * 4096 bytes
	size_t stack_size = STACK_PAGE * PAGE_SIZE;

	// base address of stack
	vaddr_t stack_address = USERSTACK - stack_size;

	// define region for stack
	int ret = as_define_region(as, stack_address, stack_size, 1, 1, 0);
	if (ret) {
		return ret;
	}
	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;
	
	return 0;
}
