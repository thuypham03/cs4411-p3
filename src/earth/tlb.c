#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <earth/earth.h>
#include <earth/intf.h>

#ifdef MACOSX
#include <libkern/OSCacheControl.h>
#endif

/* An entry in the TLB.  If phys == 0, the entry is unused.
 */
struct tlb_entry {
	page_no virt;					// virtual page number
	void *phys;						// physical address of frame
	unsigned int prot;				// protection bits
};

/* Global but private data.
 */
struct tlb {
	address_t virt_start;			// start of virtual address space
	page_no virt_pages;				// size of virtual address space
	unsigned int nentries;			// size of TLB
	struct tlb_entry *entries;		// array of tlb_entries
};

static struct tlb tlb;

/* Convert P_READ etc. to PROT_READ etc.
 */
unsigned int prot_cvt(unsigned int prot){
	unsigned int result = 0;

	if (prot & P_READ) {
		result |= PROT_READ;
	}
	if (prot & P_WRITE) {
		result |= PROT_WRITE;
	}
	if (prot & P_EXEC) {
		result |= PROT_EXEC;
	}
	return result;
}

/* Find a TLB mapping by virtual page number.
 */
static int tlb_get_entry(page_no virt){
	unsigned int i;
	struct tlb_entry *te;

	for (i = 0, te = tlb.entries; i < tlb.nentries; i++, te++) {
		if (te->virt == virt && te->phys != 0) {
			return i;
		}
	}
	return -1;
}

/* Sync the given entry in the tlb.
 */
static void tlb_sync_entry(struct tlb_entry *te){
	/* Find its virtual address.
	 */
	address_t addr = (address_t) te->virt * PAGESIZE;

	/* If the page is writable, it may have been updated.  Save it.
	 */
	if (te->prot & P_WRITE) {
		if (!(te->prot & P_READ)) {
            if (0 && te->virt == (VIRT_BASE / PAGESIZE)) {
                printf("XXX tlb_sync_entry: made readable\n\r");
            }
			if (mprotect((void *) addr, PAGESIZE, PROT_READ) != 0) {
                perror("tlb_sync_entry");
                exit(1);
            }
		}

		/* TODO.  If stack page, we do not have to save below stack pointer.
		 */
		memcpy(te->phys, (void *) addr, PAGESIZE);
	}
}

/* Flush the given entry from the TLB.
 */
static void tlb_flush_entry(struct tlb_entry *te){
	/* If not mapped, we're done.
	 */
	if (te->phys == 0) {
		return;
	}

	// printf("flush %x\n\r", te->virt);

	/* Write page back to frame.
	 */
	tlb_sync_entry(te);

	/* Mark page as inaccessible.
	 */
	address_t addr = (address_t) te->virt * PAGESIZE;
	if (mprotect((void *) addr, PAGESIZE, PROT_NONE) != 0) {
        perror("tlb_flush_entry");
        exit(1);
    }
    if (0 && te->virt == (VIRT_BASE / PAGESIZE)) {
        printf("XXX tlb_flush_entry: made inaccessible\n\r");
    }

	/* Release the entry.
	 */
	te->virt = 0;
	te->phys = 0;
	te->prot = 0;
}

/* Unmap TLB entries corresponding to the given virtual page.
 */
static void tlb_unmap(page_no virt){
	unsigned int i;
	struct tlb_entry *te;

	for (i = 0, te = tlb.entries; i < tlb.nentries; i++, te++) {
		if (te->virt == virt) {
			tlb_flush_entry(te);
		}
	}
}

/* Get the content of a TLB entry.
 */
static int tlb_get(unsigned int tlb_index, page_no *virt,
							void **phys, unsigned int *prot){
	struct tlb_entry *te;

	if (tlb_index >= tlb.nentries) {
		fprintf(stderr, "tlb_get: non-existing entry %u %u\n", tlb_index, tlb.nentries);
        exit(1);
		return 0;
	}
	te = &tlb.entries[tlb_index];

	if (virt != 0) {
		*virt = te->virt;
	}
	if (phys != 0) {
		*phys = te->phys;
	}
	if (prot != 0) {
		*prot = te->prot;
	}
	return 1;
}

/* In the TLB, map the given virtual page to the given physical address.
 */
static int tlb_map(unsigned int tlb_index, page_no virt,
									void *phys, unsigned int prot){
	struct tlb_entry *te;

	if (tlb_index >= tlb.nentries) {
		fprintf(stderr, "tlb_map: non-existing entry %u %u\n", tlb_index, tlb.nentries);
        exit(1);
		return 0;
	}
	te = &tlb.entries[tlb_index];

	/* Figure out the base address.
	 */
	address_t addr = (address_t) virt * PAGESIZE;

	// printf("tlb_map %"PRIaddr" to %"PRIaddr"\n\r", addr, (uint64_t) phys);

	/* Check to see if we're just changing the protection.
	 *
	 * TODO.  Optimize this case.
	 */
	if (te->virt == virt && te->phys == phys) {
		// printf("tlb_map: changing protection bits\n\r");
	}

	/* See if the entry is currently mapped.  If so, flush it.
	 */
	if (te->phys != 0) {
		tlb_flush_entry(te);
	}

	/* Fill the TLB entry.
	 */
	te->virt = virt;
	te->phys = phys;
	te->prot = prot;

	/* Temporarily set write access so we can copy the frame into the
	 * right position.
	 */
	if (!(prot & P_WRITE)) {
        if (0 && te->virt == (VIRT_BASE / PAGESIZE)) {
            printf("XXX tlb_map: made writable\n\r");
        }
		if (mprotect((void *) addr, PAGESIZE, PROT_WRITE) != 0) {
			perror("mprotect 1");
		}
	}
	else {
        if (0 && te->virt == (VIRT_BASE / PAGESIZE)) {
            printf("XXX tlb_map: made accessible %x\n\r", prot);
        }
		if (mprotect((void *) addr, PAGESIZE, prot_cvt(prot)) != 0) {
			perror("mprotect 2");
		}
	}

	// printf("restore %x %"PRIaddr" to %"PRIaddr" %x\n\r",
	//  			virt, (uint64_t) phys, (uint64_t) addr, prot);
	/* TODO.  If stack do not restore below stack pointer.
	 */
	memcpy((void *) addr, phys, PAGESIZE);

	/* Now set the permissions correctly and we're done.
	 */
	if (!(prot & P_WRITE)) {
        if (0 && te->virt == (VIRT_BASE / PAGESIZE)) {
            printf("XXX tlb_map: make accessible %x\n\r", prot);
        }
		if (mprotect((void *) addr, PAGESIZE, prot_cvt(prot)) != 0) {
			perror("mprotect 3");
		}
#ifdef MACOSX
        sys_icache_invalidate((void *) addr, PAGESIZE);
#endif
        // printf("VALUE %d\n", * (int *) addr);
        // printf("STACK %d\n", * (int *) 0x9003ffff08);
	}

	return 1;
}

/* Flush the TLB.
 */
static void tlb_flush(void){
	unsigned int i;

	for (i = 0; i < tlb.nentries; i++) {
		tlb_flush_entry(&tlb.entries[i]);
	}
}

/* Sync the TLB: write all modified pages back to the frames.
 */
static void tlb_sync(void){
	unsigned int i;

	for (i = 0; i < tlb.nentries; i++) {
		if (tlb.entries[i].phys != 0) {
			tlb_sync_entry(&tlb.entries[i]);
		}
	}
}

/* Set up everything.
 */
static void tlb_initialize(unsigned int nentries){
	printf("earth: tlb_initialize: %u entries\n\r", nentries);
	unsigned int pagesize = getpagesize();

	/* Sanity checks.
	 */
	if (sizeof(address_t) != sizeof(void *)) {
		fprintf(stderr, "address_t needs to be size of pointer\n");
		exit(1);
	}
	if (PAGESIZE % pagesize != 0) {
		fprintf(stderr, "tlb_initialize: PAGESIZE not a multiple of actual page size %u %u\n", PAGESIZE, pagesize);
		exit(1);
	}
	if (VIRT_BASE % pagesize != 0) {
		fprintf(stderr, "tlb_initialize: VIRT_BASE not a multiple of actual page size\n");
		exit(1);
	}

	tlb.nentries = nentries;
	tlb.entries = (struct tlb_entry *) calloc(nentries, sizeof(*tlb.entries));

	/* Try to map the virtual address range.
	 */
	void *addr = mmap((void *) VIRT_BASE, VIRT_PAGES * PAGESIZE,
			PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
	if (addr != (void *) VIRT_BASE) {
		fprintf(stderr, "Fatal error: can't map virtual address space at %"PRIaddr"\n", VIRT_BASE);
		exit(1);
	}
}

void tlb_setup(struct tlb_intf *ti){
	ti->initialize = tlb_initialize;
	ti->flush = tlb_flush;
	ti->sync = tlb_sync;
	ti->map = tlb_map;
	ti->get = tlb_get;
	ti->unmap = tlb_unmap;
	ti->get_entry = tlb_get_entry;
}
