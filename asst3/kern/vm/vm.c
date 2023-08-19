#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <spl.h>

/* Place your page table functions here */

static struct spinlock HPT_lock = SPINLOCK_INITIALIZER;

// another lock for copy
static struct spinlock HPT_copy_lock = SPINLOCK_INITIALIZER;

uint32_t hpt_size = 0;

struct HPT *HP_table;

static paddr_t lookup_HPT(vaddr_t virtual_page_number, struct addrspace *as, uint32_t *dirty) {
	// get index of entry we want to check
	uint32_t index = (((uint32_t )as) ^ (virtual_page_number >> 12)) % hpt_size;
	
	// process id is assigned by the value of address space
	uint32_t pid = (uint32_t)as;

	// use lock to warp HPT to prevent race condition
	spinlock_acquire(&HPT_lock);

	paddr_t ret = 0;

	// check whether is a valid translation on HPT
	if ((HP_table[index].entry_lo & TLBLO_VALID) == 0) {
		// not a valid translation
		spinlock_release(&HPT_lock);
		return ret;
	}

	// find the corresponding virtual page number
	if (HP_table[index].pid == pid && HP_table[index].vpn == virtual_page_number){
		ret = HP_table[index].entry_lo;
		
		if ((HP_table[index].entry_lo & WRITEABLE) == 0){
		// read only region
			*dirty = 0;
		} else {
			// read-write region
			*dirty = TLBLO_DIRTY;
		}
		
		spinlock_release(&HPT_lock);
		return ret;
	}

	// recursively check all entries 
	while ((HP_table[index].entry_lo & TLBLO_VALID) != 0) {
		if (HP_table[index].pid == pid && HP_table[index].vpn == virtual_page_number){
			ret = HP_table[index].entry_lo;
			
			if ((HP_table[index].entry_lo & WRITEABLE) == 0){
				// read only region
				*dirty = 0;
			} else {
				// read-write region
				*dirty = TLBLO_DIRTY;
			}

			break;
		}
		index = HP_table[index].next;
	}
	
	spinlock_release(&HPT_lock);
	return ret;
}   

static int check_valid_translation(vaddr_t faultaddress, struct addrspace *as,uint32_t *dirty){
	// scan all regions in virtual address space
	struct region *temp = as -> regions;
	while (temp != NULL) {
		vaddr_t base_address = temp -> base;
		vaddr_t end_address = (temp -> base) + (temp -> size);

		// find a corresponding region
		if (faultaddress >= base_address && faultaddress < end_address ) {
			if ((temp -> permission & WRITEABLE) == 0){
				// read only region
				*dirty = 0;
			} else {
				// read-write region
				*dirty = TLBLO_DIRTY;
			}

			return 0;
		}

		// go check next region
		temp = temp -> next;
	}

	return EFAULT;
}

// scan HPT to insert entry
static paddr_t insert_HPT(struct addrspace *as, vaddr_t virtual_page_number, paddr_t frame_number,uint32_t dirty){
	// get index of entry we want to insert
	uint32_t index = (((uint32_t )as) ^ (virtual_page_number >> 12)) % hpt_size;
	
	uint32_t next_entry = 0;

	// use lock to warp HPT to prevent race condition
	spinlock_acquire(&HPT_lock);

	// if the first entry is free, there is no collision
	if ((HP_table[index].entry_lo & TLBLO_VALID) == 0) {
		next_entry = index;
		HP_table[next_entry].pid = (uint32_t)as;
		HP_table[next_entry].vpn = (virtual_page_number & PAGE_FRAME);
		HP_table[next_entry].entry_lo = (frame_number & PAGE_FRAME) | dirty | TLBLO_VALID;
		HP_table[next_entry].next = -1;
		
		spinlock_release(&HPT_lock);
		return HP_table[next_entry].entry_lo;
	}

	// get the last child of linklist and the index where we want to insert an new entry
	while (HP_table[index].next != -1) {
		index = HP_table[index].next;
	}

	// find an available entry to insert
	next_entry = (index + hpt_size/2) % hpt_size;
	while ((HP_table[next_entry].entry_lo & TLBLO_VALID) != 0){
		next_entry = (2 * next_entry) % hpt_size;
	}

	// link two entries
	HP_table[index].next = next_entry;

	// initialize new entry
	HP_table[next_entry].pid = (uint32_t)as;
	HP_table[next_entry].vpn = (virtual_page_number & PAGE_FRAME);
	HP_table[next_entry].entry_lo = (frame_number & PAGE_FRAME) | dirty | TLBLO_VALID;
	HP_table[next_entry].next = -1;

	spinlock_release(&HPT_lock);

	return HP_table[next_entry].entry_lo;
}

int copy_HPT(uint32_t old, uint32_t new) {
	spinlock_acquire(&HPT_copy_lock);
	for (uint32_t i = 0; i < hpt_size; i++){
		// find an entry with old process id
		if (HP_table[i].pid == old) {
			vaddr_t old_vpn = HP_table[i].vpn;
			paddr_t old_entryLo = HP_table[i].entry_lo;
			uint32_t dirty = old_entryLo & TLBLO_DIRTY;
			
			// allocate a new frame for new process
			vaddr_t base = alloc_kpages(1);

			// allocate frame failed
			if (base == 0){
				as_destroy((struct addrspace*)new);
				return EFAULT;
			}

			// zero fill fresh pages before mapping
			bzero((void*)base, PAGE_SIZE);

			// convert kernel virtual address to physical address
			paddr_t frame_number = KVADDR_TO_PADDR(base);

			paddr_t new_entryLo = insert_HPT((struct addrspace *)new ,old_vpn,frame_number,dirty);

			paddr_t new_frame = new_entryLo & PAGE_FRAME;
			paddr_t old_frame = old_entryLo & PAGE_FRAME;

			memcpy((void *)PADDR_TO_KVADDR(new_frame), (const void*)PADDR_TO_KVADDR(old_frame), PAGE_SIZE);
		}
	}

	spinlock_release(&HPT_copy_lock);
	return 0;
}

void remove_HPT(uint32_t pid) {
	int32_t next_entry = 0;
	spinlock_acquire(&HPT_lock);
	for (uint32_t i = 0; i < hpt_size; i++){
		if (HP_table[i].pid == pid){
			paddr_t frame_number = HP_table[i].entry_lo & PAGE_FRAME;
			free_kpages(PADDR_TO_KVADDR(frame_number));

			if (HP_table[i].next != -1) {
				// current entry is in the middle of collision chain, remove this 
				// entry then connect its adjacent nodes
				next_entry = HP_table[i].next;

				HP_table[i].pid = HP_table[next_entry].pid;
				HP_table[i].vpn = HP_table[next_entry].vpn;
				HP_table[i].entry_lo = HP_table[next_entry].entry_lo;
				HP_table[i].next = HP_table[next_entry].next;

				// delete current node
				HP_table[next_entry].pid = 0;
				HP_table[next_entry].vpn = 0;
				HP_table[next_entry].entry_lo = 0;
				HP_table[next_entry].next = -1;
			} else {
				// current node is at the end of link list
				HP_table[i].pid = 0;
				HP_table[i].vpn = 0;
				HP_table[i].entry_lo = 0;
				HP_table[i].next = 1;
			}
		}
	}

	spinlock_release(&HPT_lock);
}

void vm_bootstrap(void)
{
	/* Initialise any global components of your VM sub-system here.
		*
		* You may or may not need to add anything here depending what's
		* provided or required by the assignment spec.
		*/
	// get size of ram of compute number of entries for HPT
	paddr_t ram_size = ram_getsize();

	// get number of entries in HPT
	hpt_size = 2 * (ram_size / PAGE_SIZE);

	// initialize HPT
	HP_table = kmalloc(hpt_size * sizeof(struct HPT));

	// initialize each entry
	for (uint32_t i = 0; i < hpt_size; i++){
		HP_table[i].pid = 0;
		HP_table[i].vpn = 0;
		HP_table[i].entry_lo = 0;
		HP_table[i].next = -1;
	}
}

int vm_fault(int faulttype, vaddr_t faultaddress)
{   
	// check read only fault
	if (faulttype == VM_FAULT_READONLY) {
		return EFAULT;
	}

	if (curproc == NULL) {
		return EFAULT;
	}

	// get virtual address space, would be used later if we didn't find an entry in HPT
	struct addrspace *as = proc_getas();
	if (as == NULL) {
		return EFAULT;
	}

	// get virtual page number
	faultaddress &= PAGE_FRAME;

	// set up dirty bit, will be reset later when we lookup HPT and regions
	uint32_t dirty = 0;

	// look up HPT, we use a single lock for HPT to make sure each time there is only
	// one process accessing critical region, which is HPT
	paddr_t frame_number = lookup_HPT(faultaddress, as, &dirty);

	// not found in HPT, search virtual address
	if (frame_number == 0){
		// check whether this is an valid region in virtual address space
		int ret = check_valid_translation(faultaddress, as, &dirty);

		// not an valid region
		if (ret){
			return ret;
		}

		// find an valid region and update it into HPT
		// allocate an new frame and convert to physical address
		vaddr_t base = alloc_kpages(1);

		// allocate frame failed
		if (base == 0){
			return EFAULT;
		}

		// zero fill fresh pages before mapping
		bzero((void*)base, PAGE_SIZE);

		// convert kernel virtual address to physical address
		frame_number = KVADDR_TO_PADDR(base);

		paddr_t new_entry = insert_HPT(as, faultaddress, frame_number,dirty);
		// insert failed
		if (new_entry == 0){
			return EFAULT;
		}
	}

	// for debug
	// panic("vm_fault hasn't been written yet\n");
	uint32_t entryHi = faultaddress & TLBHI_VPAGE;
	uint32_t entryLo = frame_number | TLBLO_VALID;
	if (dirty == 0) {
		entryLo |= TLBLO_DIRTY;
	} else {
		entryLo |= dirty;
	}

	// disable interrutps to write an new entry into TLB
	int spl = splhigh();
	tlb_random(entryHi,entryLo);
	splx(spl);

	return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

