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
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>

#include "opt-A3.h" /* required for A3 */

#if OPT_A3
#include <kern/wait.h>
#include <syscall.h>
#include <copyinout.h>
#endif /* OPT_A3 */

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

#if OPT_A3
unsigned long numpages;           				/* total number of pages in core-map */
paddr_t page_start;								/* start address of pages */
paddr_t coremap;           						/* core-map that stores the page segments */
bool core_created = false;                      /* indicate if core-map has been created */
static struct spinlock core_lock = SPINLOCK_INITIALIZER;        /* core-map lock */
#endif /* OPT_A3 */

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;


#if OPT_A3
void
print_coremap(void)
{
	for (unsigned int i = 0; i < numpages; i++) {
		kprintf("%lu ", *(unsigned long *) (PADDR_TO_KVADDR(coremap + i * sizeof(unsigned long))));
	}
	kprintf("\n");
}
#endif /* OPT_A3 */


void
vm_bootstrap(void)
{
#if OPT_A3
	/* calculate the number of pages fit in the memory */
	paddr_t lo, hi;
	ram_getsize(&lo, &hi);
	numpages = (hi - lo) / (PAGE_SIZE + sizeof(unsigned long));

	spinlock_acquire(&core_lock);
	/* now create the core-map */
	coremap = lo;

	/* calculate the start frame that core-map manages */
	page_start = ROUNDUP(coremap + numpages * sizeof(unsigned long), PAGE_SIZE);
	
	/* for now, create an empty core-map with all pages unused */
	for (unsigned long i = 0; i < numpages; i++) {
		*(unsigned long *) (PADDR_TO_KVADDR(coremap + i * sizeof(unsigned long))) = 0;
	}

	/* print the coremap */
	/*kprintf("*****************DEBUG******************\n");
	kprintf("core-map start address: %x.\n", (unsigned int) coremap);
	kprintf("paging start address: %x.\n", (unsigned int) page_start);
	kprintf("core-map page number: %u.\n", (unsigned int) numpages);
	kprintf("****************************************\n");*/

	core_created = true;
	spinlock_release(&core_lock);
#else
	/* Do nothing. */
#endif /* OPT_A3 */
}

static
paddr_t
getppages(unsigned long npages)
{
#if OPT_A3

	paddr_t addr = 0;

	if (core_created) {
		/* core-map has been created, use core-map instead of stealing memory */
		spinlock_acquire(&core_lock);

		unsigned long contigiuous_counter = 0;
		unsigned long start_index = 0;

		/* loop through the coremap, try to allocate a contigiuous block of n pages */
		for (unsigned long i = 0; i < numpages; i++) {
			if (*(unsigned long *) (PADDR_TO_KVADDR(coremap + i * sizeof(unsigned long))) == 0) {
				if (contigiuous_counter == 0) {
					/* set addr to the beginning of this chunk of memory */
					addr = page_start + i * PAGE_SIZE;
					start_index = i;
				}
				contigiuous_counter++;
			} else {
				/* this chunk of memory has been used, reset the counter */
				contigiuous_counter = 0;
			}

			/* if we find a memory chunk that is npages pages long, modify the core-map and return the address */
			if (contigiuous_counter == npages) {
				/* modify core-map */
				for (unsigned long j = start_index; j <= i; j++) {
					*(unsigned long *) (PADDR_TO_KVADDR(coremap + j * sizeof(unsigned long))) = j + 1 - start_index;
				}

				// *************************DEBUG*************************
				// kprintf("Successfully allocated %lu pages, started at index %lu.\n", npages, start_index);
				// print_coremap();

				spinlock_release(&core_lock);
				return addr;
			}
		}

		spinlock_release(&core_lock);
		/* we should not reach there if we find valid memory in core-map
		* if we reach there, we do not have enough memory in the core-map
		* return appropriate error code */

		// *************************DEBUG*************************
		// print_coremap();

		panic("Not enough memory!\n");
		return 0;
	}
	else {
		/* core-map has not been created, for now just steal memory */
		spinlock_acquire(&stealmem_lock);
		addr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
		return addr;
	}

#else

	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);
	return addr;

#endif
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
#if OPT_A3

	spinlock_acquire(&core_lock);
	
	/* calculate the corresponding corresponding page table entry */
	paddr_t pa = KVADDR_TO_PADDR(addr);
	unsigned long index = (unsigned long) ROUNDUP((pa - page_start) / PAGE_SIZE, 1);

	/* start from this index, find all contiguous entries and reset the page table entry */
	unsigned long *tracker = (unsigned long *) (PADDR_TO_KVADDR(coremap + index * sizeof(unsigned long)));
	unsigned long tracker_val = *tracker;
	index += 1;

	// *************************DEBUG*************************
	// kprintf("freed physical address: %x.\n", pa);
	// kprintf("freed virtual address: %x.\n", addr);
	// kprintf("freed address at index %lu, where core-map value is %lu.\n", index - 1, tracker_val);

	while (index < numpages) {
		unsigned long *new_tracker = (unsigned long *) (PADDR_TO_KVADDR(coremap + index * sizeof(unsigned long)));

		if (*new_tracker != tracker_val + 1) {
			/* end of current contiguous page, break */
			*tracker = 0;
			break;
		}
		else {
			/* current page continuous */
			*tracker = 0;
			tracker = new_tracker;
			tracker_val = *tracker;

			index++;
		}
	}

	// *************************DEBUG*************************
	// print_coremap();

	spinlock_release(&core_lock);
	
#else

	/* nothing - leak the memory. */
	(void)addr;

#endif /* OPT_A3 */
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

#if OPT_A3
	bool isCode = false;
#endif /* OPT_A3 */

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:

		#if OPT_A3
		/* terminate current process */
		sys__exit(__WROMWRITE);
		#else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
		#endif /* OPT_A3 */

	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		#if OPT_A3
		// the faultaddress is in code segment, mark as isCode
		isCode = true;
		#endif /* OPT_A3 */
		
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;

		#if OPT_A3
		// if this address space is in code segment and has completed loading, load the TLB entry with TLBLO_DIRTY off
		if (isCode && as->hasLoaded) {
			elo &= ~TLBLO_DIRTY;
		}
		#endif /* OPT_A3 */

		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

#if OPT_A3
	// TLB is full, call tlb_random() to write to a random TLB entry
	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	// if this address space is in code segment and has completed loading, load the TLB entry with TLBLO_DIRTY off
	if (isCode && as->hasLoaded) {
		elo &= ~TLBLO_DIRTY;
	}
	tlb_random(ehi, elo);
	splx(spl);
	return 0;
#else 
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif /* OPT_A3 */
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;

	#if OPT_A3
	as->hasLoaded = false;
	#endif /* OPT_A3 */

	return as;
}

void
as_destroy(struct addrspace *as)
{
	#if OPT_A3

	/* free all memory allocated in code segment */
	kfree((void *) PADDR_TO_KVADDR(as->as_pbase1));

	/* free all memory allocated in data segment */
	kfree((void *) PADDR_TO_KVADDR(as->as_pbase2));

	/* free all memory allocated in stack segment */
	kfree((void *) PADDR_TO_KVADDR(as->as_stackpbase));

	#endif /* OPT_A3 */

	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
	
	*ret = new;
	return 0;
}
