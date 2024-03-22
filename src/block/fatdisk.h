#ifdef HW_FS	

/* FAT File System Layout
 * +-------------+-----------------+---------------+---------------+-----+-------------------+
 * | super block |   inode blocks  |   fat blocks  | data block1   | ... | last data block   |
 * +-------------+-----------------+---------------+---------------+-----+-------------------+
 * |<- 1 block ->|<-n_inodeblocks->|<-n_fatblocks->|<-           data blocks               ->|
 */

typedef unsigned int fatentry_no;       // index of a fat entry


#define INODES_PER_BLOCK    (BLOCK_SIZE / sizeof(struct fatdisk_inode))
#define FAT_PER_BLOCK       (BLOCK_SIZE / sizeof(struct fatdisk_fatentry))

/* Contents of the "superblock".  There is only one of these.
 */
struct fatdisk_superblock {
    block_no n_inodeblocks;     // # blocks containing inodes
    block_no n_fatblocks;       // # blocks containing fat entries
    fatentry_no fat_free_list;  // fat index of the first free fat entry
    char unused[BLOCK_SIZE - sizeof(block_no) * 2 - sizeof(fatentry_no)];
};

/* An inode describes a file (a virtual block store).  "nblocks" contains
 * the number of blocks in the file, while "head" is the first fat
 * entry for the file. Note that initially all files exist but are of
 * size 0. It is intended to keep track of which files are free elsewhere.
 */
struct fatdisk_inode {
    fatentry_no head;     // first fat entry
    block_no nblocks;     // total size (in blocks) of the file
};

struct fatdisk_fatentry {
    fatentry_no next;     // next entry in the file or in the free list
                          // 0 (or -1) for EOF or end of free list
};

/* An inode block is filled with inodes.
 */
struct fatdisk_inodeblock {
    struct fatdisk_inode inodes[INODES_PER_BLOCK];
};

/* An fat block is filled with fatentries
 */
struct fatdisk_fatblock {
    struct fatdisk_fatentry entries[FAT_PER_BLOCK];
};

/* A convenient structure that's the union of all block types.  It should
 * have size BLOCK_SIZE, which may not be true for the elements.
 */
union fatdisk_block {
    struct fatdisk_superblock superblock;
    struct fatdisk_inodeblock inodeblock;
    struct fatdisk_fatblock fatblock;
    block_t datablock;
};

#endif
