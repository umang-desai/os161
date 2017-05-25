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
#include <coremap.h>
#include <syscall.h>
/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 *
 * NOTE: it's been found over the years that students often begin on
 * the VM assignment by copying dumbvm.c and trying to improve it.
 * This is not recommended. dumbvm is (more or less intentionally) not
 * a good design reference. The first recommendation would be: do not
 * look at dumbvm at all. The second recommendation would be: if you
 * do, be sure to review it from the perspective of comparing it to
 * what a VM system is supposed to do, and understanding what corners
 * it's cutting (there are many) and why, and more importantly, how.
 */

/* under dumbvm, always have 72k of user stack */
/* (this must be > 64K so argument blocks of size ARG_MAX will fit) */
#define DUMBVM_STACKPAGES    18

/*
 * Wrap ram_stealmem in a spinlock.
 */

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

//static struct coremap_entry **coremap;
//static int coremap_sz;

void
init_coremap(void){
	uint32_t firstpaddr = 0; // address of first free physical page 
	uint32_t lastpaddr = 0; // one past end of last free physical page
	int i;
	ram_getsize(&firstpaddr, &lastpaddr);
	DEBUG(DB_VM,"ram_getsize: %d %d\n", firstpaddr, lastpaddr);

	coremap_sz = (lastpaddr-firstpaddr)/PAGE_SIZE;
	coremap = kmalloc(sizeof(struct coremap_entry*) * coremap_sz);
	
	for (i = 0; i < coremap_sz; i++) {
		struct coremap_entry *entry = kmalloc(sizeof(struct coremap_entry));
		entry->paddr = firstpaddr + (i * PAGE_SIZE);
		entry->valid = 0;
		entry->block_len = -1;
		coremap[i] = entry;
	}
	
	coremap_ready = 1;

	ram_getsize(&firstpaddr, &lastpaddr);
	
	for(i = 0; coremap[i]->paddr < firstpaddr; i++) {
		coremap[i]->valid = 1;
	}

}

void
vm_bootstrap(void){
//	init_coremap(); /* Do nothing. */
//	(void) stealmem_lock;
}

paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);

/*	if(coremap_ready == 0)
		return ram_stealmem(npages);

	int i, j;
	unsigned int count = 0;
	for (i = 0; i < coremap_sz; i++) {
		if (coremap[i]->valid) {
			count = 0;
		} else {
			count++;
		}
		if (count == npages) {
			coremap[i - npages + 1]->block_len = npages;
			for (j = i - npages + 1; j <= i; j++) {
				coremap[j]->valid = 1;
			}
			addr = coremap[i - npages + 1]->paddr;
			// DEBUG(DB_VM, "getppages: coremap_ready-addr 0x%x\n", addr);
 			return addr;
		}
	}*/
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages)
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
	(void) addr;
	/* nothing - leak the memory. */
/*	paddr_t paddr = KVADDR_TO_PADDR(addr);
	int i, j;
	for (i = 0; coremap[i]->paddr != paddr; i++);
	
	KASSERT(coremap[i]->block_len != -1);
	
	for (j = 0; j < coremap[i]->block_len; j++) {
		coremap[j]->valid = 0;
	}
	
	coremap[i]->block_len = -1;
*/
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
	unsigned int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
	bool textseg = false;	

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	as = proc_getas();

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		if(as->isloaded){
			sys__exit(-1);	
		}	
//		panic("dumbvm: got VM_FAULT_READONLY\n");
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

	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	//TLB miss error is found. How to fix it?
	//Try going through the ptable and check if the fault address we got has a page in the page table or not. we should have one, check if its valid or not.
/*	struct page *p;
	for(i = 0; i < array_num(as->ptable); i++){
		p = array_get(as->ptable, i);
		if(p->vaddr == faultaddress)
		{
			//Check if page is valid
			if(p->valid == 0){
				//We have got ourselves a page fault
				paddr = getppages(1);
				if(!paddr){
					return ENOMEM;
				}
				p->paddr = paddr;
				p->valid = 1;

				if (as->as_pbase1 == 0 && faultaddress >= as->as_vbase1 && faultaddress < as->as_vbase1 + as->as_npages1 * PAGE_SIZE)
				{
					as->as_pbase1 = paddr;
				}
				if (as->as_pbase2 == 0 && faultaddress >= as->as_vbase2 && faultaddress < as->as_vbase2 + as->as_npages2 * PAGE_SIZE)
				{
					as->as_pbase2 = paddr;
				}
				if (as->as_stackpbase == 0 && faultaddress >= USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE && faultaddress < USERSTACK)
				{
					as->as_stackpbase = paddr;
				}
			}else{
				paddr = p->paddr;
			}
			break;
		}
	} 
	
	if(p->perms & 0x2)
		textseg = true;
*/
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
		paddr = (faultaddress - vbase1) + as->as_pbase1;
		textseg = true;
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

//	DEBUG(DB_VM, "dumbvm: For loop will now begin. 0x%x -> 0x%x\n", faultaddress, paddr);
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		if(textseg && as->isloaded)
        	{
               // 	kprintf("Abuot to set dirty bits to 0");
                	elo = (paddr & ~TLBLO_DIRTY) | TLBLO_VALID;
        	}else{
                	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;        
        	}	
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
	
	ehi = faultaddress;
	
	if(textseg && as->isloaded)
	{
	//	kprintf("Abuot to set dirty bits to 0");
		elo = (paddr & ~TLBLO_DIRTY) | TLBLO_VALID;
	}else{
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;	
	}

	tlb_random(ehi, elo);
	splx(spl);
	return 0;
	
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

/*
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

	return as;
}

void
as_destroy(struct addrspace *as)
{
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

//	 Disable interrupts on this CPU while frobbing the TLB. /
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
//	 nothing 
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages;

//	 Align the region. First, the base... 
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	// ...and now the length. //
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	// We don't use these - all pages are read-write 
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

	
	//  Support for more than two regions is not available.
	 
	kprintf("dumbvm: Warning: too many regions\n");
	return ENOSYS;
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

	// (Mis)use as_prepare_load to allocate some physical memory. /
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
}*/
