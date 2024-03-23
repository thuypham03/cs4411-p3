/* Author: Robbert van Renesse, August 2015
 *
 * This block store module mirrors the underlying block store but contains
 * a write-through cache.  The caching strategy is CLOCK, approximating LRU.
 * The interface is as follows:
 *
 *		block_if clockdisk_init(block_if below,
 *									block_t *blocks, block_no nblocks)
 *			'below' is the underlying block store.  'blocks' points to
 *			a chunk of memory wth 'nblocks' blocks for caching.
 *
 *		void clockdisk_dump_stats(block_if bi)
 *			Prints the cache statistics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <egos/block_store.h>

enum block_status {
	EMPTY,	// block not in use
	OLD,	// block not used for a while (ready to be replaced)
	NEW		// block recently used
};

/* Metadata for a single cache slot, including address of underlying
 * storage (if in use), empty otherwise
 */
typedef struct block_info {
	enum block_status status;
	unsigned int dirty;
	unsigned int ino;
	block_no offset;
} block_info_t;

/* State contains the pointer to the block module below as well as caching
 * information and caching statistics.
 */
struct clockdisk_state {
	block_if below;				// block store below
	block_t *blocks;			// memory for caching blocks
	block_info_t *block_infos;  // memory for caching blocks' metadata
	block_no nblocks;			// size of cache (not size of block store!)
	block_no clock_hand;

	/* Stats.
	 */
	unsigned int read_hit, read_miss, write_hit, write_miss, nops;
};

void clockdisk_dump_stats_if_needed(block_if bi);

static int clockdisk_getninodes(block_store_t *this_bs){
	struct clockdisk_state *cs = this_bs->state;
	++cs->nops;
	clockdisk_dump_stats_if_needed(this_bs);
	return (*cs->below->getninodes)(cs->below);
}

static int clockdisk_getsize(block_if bi, unsigned int ino){
	struct clockdisk_state *cs = bi->state;
	++cs->nops;
	clockdisk_dump_stats_if_needed(bi);
	return (*cs->below->getsize)(cs->below, ino);
}

static int clockdisk_setsize(block_if bi, unsigned int ino, block_no nblocks){
	struct clockdisk_state *cs = bi->state;
	++cs->nops;

	// Clear cache's entries of about-to-be-deleted blocks
	for (block_no i = 0; i < cs->nblocks; ++i) {
		if (cs->block_infos[i].status != EMPTY && 
			cs->block_infos[i].ino == ino && cs->block_infos[i].offset >= nblocks) {
				cs->block_infos[i].status = EMPTY;
			}
	}

	clockdisk_dump_stats_if_needed(bi);
	return (*cs->below->setsize)(cs->below, ino, nblocks);
}

static void cache_update(struct clockdisk_state *cs, unsigned int ino, block_no offset, block_t *block, unsigned int dirty) {
	while (1) {
		block_no i = cs->clock_hand;
		if (cs->block_infos[i].status != NEW) {
			// Write-back if the evicted slot is dirty
			if (cs->block_infos[i].status != EMPTY && cs->block_infos[i].dirty) {
				(*cs->below->write)(cs->below, cs->block_infos[i].ino, cs->block_infos[i].offset, &cs->blocks[i]);
			}

			// Write new block in
			cs->block_infos[i].status = NEW;
			cs->block_infos[i].dirty = dirty;
			cs->block_infos[i].ino = ino;
			cs->block_infos[i].offset = offset;
			memcpy(&cs->blocks[i], block, sizeof(block_t));

			cs->clock_hand = (i + 1) % cs->nblocks;
			return;
		}  
		
		cs->block_infos[i].status = OLD;
		cs->clock_hand = (i + 1) % cs->nblocks;
	}
}

static int clockdisk_read(block_if bi, unsigned int ino, block_no offset, block_t *block){
	struct clockdisk_state *cs = bi->state;
	++cs->nops;

	for (block_no i = 0; i < cs->nblocks; ++i) {
		if (cs->block_infos[i].status != EMPTY && 
			cs->block_infos[i].ino == ino && cs->block_infos[i].offset == offset) {
				// Cache hit
				memcpy(block, &cs->blocks[i], sizeof(block_t));
				cs->block_infos[i].status = NEW;
				cs->read_hit += 1;

				clockdisk_dump_stats_if_needed(bi);
				return 0;
			}
	}

	// Cache miss
	cs->read_miss += 1;

	int r = (*cs->below->read)(cs->below, ino, offset, block);
	if (r == -1) return r;
	cache_update(cs, ino, offset, block, 0);

	clockdisk_dump_stats_if_needed(bi);
	return 0;
}

static int clockdisk_write(block_if bi, unsigned int ino, block_no offset, block_t *block){
	struct clockdisk_state *cs = bi->state;
	++cs->nops;

	for (block_no i = 0; i < cs->nblocks; ++i) {
		if (cs->block_infos[i].status != EMPTY && 
			cs->block_infos[i].ino == ino && cs->block_infos[i].offset == offset) {
				// Cache hit
				memcpy(&cs->blocks[i], block, sizeof(block_t));
				cs->block_infos[i].status = NEW;
				cs->block_infos[i].dirty = 1;
				cs->write_hit += 1;

				clockdisk_dump_stats_if_needed(bi);
				return 0;
			}
	}

	// Cache miss
	cs->write_miss += 1;
	cache_update(cs, ino, offset, block, 1);

	clockdisk_dump_stats_if_needed(bi);
	return 0;
}

static int clockdisk_sync(block_if bi, unsigned int ino){
	struct clockdisk_state *cs = bi->state;
	++cs->nops;

	for (block_no i = 0; i < cs->nblocks; ++i) {
		if (cs->block_infos[i].status != EMPTY && cs->block_infos[i].dirty && 
			(cs->block_infos[i].ino == ino || ino == (unsigned int) -1)) {
				(*cs->below->write)(cs->below, cs->block_infos[i].ino, cs->block_infos[i].offset, &cs->blocks[i]);
				cs->block_infos[i].dirty = 0;
			}
	}

	clockdisk_dump_stats_if_needed(bi);
	return (*cs->below->sync)(cs->below, ino);
}

static void clockdisk_release(block_if bi){
	struct clockdisk_state *cs = bi->state;
	free(cs->block_infos);
	free(cs);
	free(bi);
}

void clockdisk_dump_stats_if_needed(block_if bi) {
	struct clockdisk_state *cs = bi->state;
	if (cs->nops % 20 == 0) clockdisk_dump_stats(bi);
}

void clockdisk_dump_stats(block_if bi){
	struct clockdisk_state *cs = bi->state;

	printf("!$CLOCK: #read hits:    %u\n", cs->read_hit);
	printf("!$CLOCK: #read misses:  %u\n", cs->read_miss);
	printf("!$CLOCK: #write hits:   %u\n", cs->write_hit);
	printf("!$CLOCK: #write misses: %u\n", cs->write_miss);
}

/* Create a new block store module on top of the specified module below.
 * blocks points to a chunk of memory of nblocks blocks that can be used
 * for caching.
 */
block_if clockdisk_init(block_if below, block_t *blocks, block_no nblocks){
	/* Create the block store state structure.
	 */
	struct clockdisk_state *cs = new_alloc(struct clockdisk_state);
	cs->below = below;
	cs->blocks = blocks;
	cs->nblocks = nblocks;
	cs->clock_hand = 0;
	cs->block_infos = calloc(nblocks, sizeof(block_info_t));

	cs->read_hit = 0;
	cs->read_miss = 0;
	cs->write_hit = 0;
	cs->write_miss = 0;
	cs->nops = 0;

	/* Return a block interface to this inode.
	 */
	block_if bi = new_alloc(block_store_t);
	bi->state = cs;
	bi->getninodes = clockdisk_getninodes;
	bi->getsize = clockdisk_getsize;
	bi->setsize = clockdisk_setsize;
	bi->read = clockdisk_read;
	bi->write = clockdisk_write;
	bi->release = clockdisk_release;
	bi->sync = clockdisk_sync;
	return bi;
}
