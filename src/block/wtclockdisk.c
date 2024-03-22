/* Author: Robbert van Renesse, August 2015
 *
 * This is the write-through version of clockdisk.
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
	unsigned int ino;
	block_no offset;
} block_info_t;

/* State contains the pointer to the block module below as well as caching
 * information and caching statistics.
 */
struct wtclockdisk_state {
	block_if below;				// block store below
	block_t *blocks;			// memory for caching blocks
	block_info_t *block_infos;  // memory for caching blocks' metadata
	block_no nblocks;			// size of cache (not size of block store!)
	block_no clock_hand;

	/* Stats.
	 */
	unsigned int read_hit, read_miss, write_hit, write_miss;
};

static int wtclockdisk_getninodes(block_store_t *this_bs){
	struct wtclockdisk_state *cs = this_bs->state;
	return (*cs->below->getninodes)(cs->below);
}

static int wtclockdisk_getsize(block_if bi, unsigned int ino){
	struct wtclockdisk_state *cs = bi->state;
	return (*cs->below->getsize)(cs->below, ino);
}

static int wtclockdisk_setsize(block_if bi, unsigned int ino, block_no nblocks){
	struct wtclockdisk_state *cs = bi->state;
	
	// Clear cache's entries of about-to-be-deleted blocks
	for (block_no i = 0; i < cs->nblocks; ++i) {
		if (cs->block_infos[i].status != EMPTY && 
			cs->block_infos[i].ino == ino && cs->block_infos[i].offset >= nblocks) {
				cs->block_infos[i].status = EMPTY;
			}
	}

	return (*cs->below->setsize)(cs->below, ino, nblocks);
}

static void cache_update(struct wtclockdisk_state *cs, unsigned int ino, block_no offset, block_t *block) {
	while (1) {
		block_no id = cs->clock_hand;
		if (cs->block_infos[id].status != NEW) {
			// Evict this cache slot
			cs->block_infos[id].status = NEW;
			cs->block_infos[id].ino = ino;
			cs->block_infos[id].offset = offset;
			memcpy(&cs->blocks[id], block, sizeof(block_t));

			cs->clock_hand = (id + 1) % cs->nblocks;
			return;
		}
		
		cs->block_infos[id].status = OLD;
		cs->clock_hand = (id + 1) % cs->nblocks;
	}
}

static int wtclockdisk_read(block_if bi, unsigned int ino, block_no offset, block_t *block){
	struct wtclockdisk_state *cs = bi->state;

	for (block_no i = 0; i < cs->nblocks; ++i) {
		if (cs->block_infos[i].status != EMPTY && 
			cs->block_infos[i].ino == ino && cs->block_infos[i].offset == offset) {
				// Cache hit
				memcpy(block, &cs->blocks[i], sizeof(block_t));
				cs->block_infos[i].status = NEW;
				cs->read_hit += 1;
				return 0;
			}
	}

	// Cache miss
	cs->read_miss += 1;

	int r = (*cs->below->read)(cs->below, ino, offset, block);
	if (r == -1) return r;

	cache_update(cs, ino, offset, block);
	return 0;
}

static int wtclockdisk_write(block_if bi, unsigned int ino, block_no offset, block_t *block){
	struct wtclockdisk_state *cs = bi->state;

	for (block_no id = 0; id < cs->nblocks; ++id) {
		if (cs->block_infos[id].status != EMPTY && 
			cs->block_infos[id].ino == ino && cs->block_infos[id].offset == offset) {
				// Cache hit
				memcpy(&cs->blocks[id], block, sizeof(block_t));
				cs->block_infos[id].status = NEW;
				cs->write_hit += 1;
				return (*cs->below->write)(cs->below, ino, offset, block);
			}
	}

	// Cache miss
	cs->write_miss += 1;

	int w = (*cs->below->write)(cs->below, ino, offset, block);
	if (w == -1) return w;

	cache_update(cs, ino, offset, block);
	return 0;
}

static void wtclockdisk_release(block_if bi){
	struct wtclockdisk_state *cs = bi->state;
	free(cs->block_infos);
	free(cs);
	free(bi);
}

static int wtclockdisk_sync(block_if bi, unsigned int ino){
	struct wtclockdisk_state *cs = bi->state;
	return (*cs->below->sync)(cs->below, ino);
}

void wtclockdisk_dump_stats(block_if bi){
	struct wtclockdisk_state *cs = bi->state;

	printf("!$WTCLOCK: #read hits:    %u\n", cs->read_hit);
	printf("!$WTCLOCK: #read misses:  %u\n", cs->read_miss);
	printf("!$WTCLOCK: #write hits:   %u\n", cs->write_hit);
	printf("!$WTCLOCK: #write misses: %u\n", cs->write_miss);
}

/* Create a new block store module on top of the specified module below.
 * blocks points to a chunk of memory of nblocks blocks that can be used
 * for caching.
 */
block_if wtclockdisk_init(block_if below, block_t *blocks, block_no nblocks){
	/* Create the block store state structure.
	 */
	struct wtclockdisk_state *cs = new_alloc(struct wtclockdisk_state);
	cs->below = below;
	cs->blocks = blocks;
	cs->nblocks = nblocks;
	cs->clock_hand = 0;
	cs->block_infos = calloc(nblocks, sizeof(*cs->block_infos));

	cs->read_hit = 0;
	cs->read_miss = 0;
	cs->write_hit = 0;
	cs->write_miss = 0;

	/* Return a block interface to this inode.
	 */
	block_if bi = new_alloc(block_store_t);
	bi->state = cs;
	bi->getninodes = wtclockdisk_getninodes;
	bi->getsize = wtclockdisk_getsize;
	bi->setsize = wtclockdisk_setsize;
	bi->read = wtclockdisk_read;
	bi->write = wtclockdisk_write;
	bi->release = wtclockdisk_release;
	bi->sync = wtclockdisk_sync;
	return bi;
}
