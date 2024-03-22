#ifndef _EGOS_BLOCK_STORE_H
#define _EGOS_BLOCK_STORE_H
/*
 * (C) 2017, Cornell University
 * All rights reserved.
 */

/* Author: Robbert van Renesse, August 2015
 *
 * This is the include file for all block store modules.  A block store
 * maintains one or more "inodes", each representing an array of blocks.
 * A physical disk would typically just have one inode (inode 0), while
 * a virtualized disk may have many.  Each block store module has an
 * 'init' function that returns a block_store_t *.  The block_store_t * is
 * a pointer to a structure that contains the following seven methods:
 *
 *      int getninodes(block_store_t *this_bs)
 *          returns the number of inodes of the block store
 *
 *      int getsize(block_store_t *this_bs, unsigned int ino)
 *          returns the size of the block store at the given inode number
 *			(inode numbers start at 0)
 *
 *      int setsize(block_store_t *this_bs, unsigned int ino, block_no newsize)
 *          set the size of the block store at the given inode number
 *          returns the old size
 *
 *      int read(block_store_t *this_bs, unsigned int ino, block_no offset, block_t *block)
 *          read the block at the given inode number and offset and return in *block
 *          returns 0
 *
 *      int write(block_store_t *this_bs, unsigned int ino, block_no offset, block_t *block)
 *          write *block to the block at the given inode number and offset
 *          returns 0
 *
 *      int sync(block_store_t *this_bs, unsigned int ino)
 *          sync data in the block at the given inode number for write-back caches
 *          sync all data in the block for write-back caches if ino is (unsigned int) -1
 *          returns 0
 *
 *      void release(block_store_t *this_bs)
 *          clean up the block store interface
 *
 * All these return -1 upon error (typically after printing the
 * reason for the error).
 *
 * A 'block_t' is a block of BLOCK_SIZE bytes.  A block store is an array
 * of blocks.  A 'block_no' holds the index of the block in the block store.
 *
 * A block_store_t * also maintains a void* pointer called 'state' to internal
 * state the block store module needs to keep.
 */

#include <earth/earth.h>
#include <egos/syscall.h>

typedef unsigned int block_no;		// index of a block

typedef struct block {
	char bytes[BLOCK_SIZE];
} block_t;

typedef struct block_store {
	void *state;
    int (*getninodes)(struct block_store *this_bs);
    int (*getsize)(struct block_store *this_bs, unsigned int ino);
    int (*setsize)(struct block_store *this_bs, unsigned int ino, block_no newsize);
    int (*read)(struct block_store *this_bs, unsigned int ino, block_no offset, block_t *block);
    int (*write)(struct block_store *this_bs, unsigned int ino, block_no offset, block_t *block);
    void (*release)(struct block_store *this_bs);
    int (*sync)(struct block_store *this_bs, unsigned int ino);
} block_store_t;

typedef block_store_t *block_if;			// block store interface

/* Each block store module has an 'init' function that returns a
 * 'block_store_t *' type.  Here are the 'init' functions of various
 * available block store types.
 */
block_if checkdisk_init(block_if below, const char *descr);
block_if clockdisk_init(block_if below, block_t *blocks, block_no nblocks);
block_if combinedisk_init(block_if *below, unsigned int nbelow);
block_if debugdisk_init(block_if below, const char *descr);
block_if fatdisk_init(block_if below, unsigned int below_ino);
block_if filedisk_init(const char *file_name, block_no nblocks);
block_if mapdisk_init(block_if below, unsigned int ino);
block_if partdisk_init(block_if below, unsigned int ninodes, block_no partsizes[]);
block_if protdisk_init(gpid_t below, unsigned int ino);
block_if raid0disk_init(block_if *below, unsigned int nbelow);
block_if raid1disk_init(block_if *below, unsigned int nbelow);
block_if raid4disk_init(block_if *below, unsigned int nbelow);
block_if raid5disk_init(block_if *below, unsigned int nbelow);
block_if ramdisk_init(block_t *blocks, block_no nblocks);
block_if statdisk_init(block_if below);
block_if tracedisk_init(block_if below, char *trace);
block_if treedisk_init(block_if below, unsigned int below_ino);
block_if unixdisk_init(block_if below, unsigned int below_ino);
block_if wtclockdisk_init(block_if below, block_t *blocks, block_no nblocks);

/* Some useful functions on some block store types.
 */
int treedisk_create(block_if below, unsigned int below_ino, unsigned int ninodes);
int fatdisk_create(block_if below, unsigned int below_ino, unsigned int ninodes);
int unixdisk_create(block_if below, unsigned int below_ino, unsigned int ninodes);

int treedisk_check(block_if below);
void wtclockdisk_dump_stats(block_if this_bs);
void clockdisk_dump_stats(block_if this_bs);
void statdisk_dump_stats(block_if this_bs);

#ifdef CLOCKDISK_GRADING
#define STATDISK_GETTER(stat) \
    unsigned int statdisk_get##stat(block_store_t *this_bs);
STATDISK_GETTER(ngetsize)
STATDISK_GETTER(nsetsize)
STATDISK_GETTER(nread)
STATDISK_GETTER(nwrite)
STATDISK_GETTER(nsync)
#undef STATDISK_GETTER
#endif

#endif
