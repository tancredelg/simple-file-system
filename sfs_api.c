#include <stdint.h>
#include "sfs_api.h"
#include "disk_emu.c"

char DISKNAME[] = "SFS_DISK";
#define DIR_SIZE 2000 // Set this to ~ MAX_BLOCK / 4  (where 4 = average file size)
#define FDT_SIZE 10

struct Inode {
    int size;
    int db1, db2, db3, db4, db5, db6, db7, db8, db9, db10, db11, db12; // direct data block pointers
    int dbi1; // Single-indirect data block pointer
};

struct SuperBlock {
    int magic;
    int block_size; // Size of each block, in bytes
    int sfs_size; // Size of the entire file system, in blocks
    int inode_table_size; // Size of the inode table, in blocks
    Inode root_dir; // The inode for the root directory
};

struct DirEntry {
    uint8_t used;
    char filename[15];
    short inode_num;
};

struct File {
    Inode inode;
    int rw_head_pos;
};

// Init on process stack:
SuperBlock super_block;
DirEntry root_directory[DIR_SIZE];
File FDT[FDT_SIZE];

void mksfs(int fresh) {
    /* Init caches
     *  - directory cache
     *  - inode table
     * Init FDT
     * Init disk structures:
     *  - super block
     *  - root directory
     *  - inode for root directory
     */
}
