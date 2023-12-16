#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "sfs_api.h"
#include "disk_emu.h"


// -- MACROS --
#define BYTE_OFFSET(b) ((b) / 8)
#define BIT_OFFSET(b)  ((b) % 8)


// -- CONSTANTS --
#define B 1024 // Block size - MUST SET THE SAME VALUE FOR `BLOCK_SIZE` (in disk_emu.c)
#define Q 8306 // Total number of blocks for the file system - MUST SET THE SAME VALUE FOR `MAX_BLOCK` (in disk_emu.c)
#define M 112 // Number of inode table blocks - calculated by running 'python calc_disk_alloc.py <Q>'
#define N 8192 // Number of data blocks - calculated by running 'python calc_disk_alloc.py <Q>'
#define L 1 // Number of free bitmap blocks - calculated by running 'python calc_disk_alloc.py <Q>'
#define DIR_SIZE 2048  // Max directory size (number of files) - calculated by running 'python calc_disk_alloc.py <Q>'
#define MAX_FILE_SIZE (268 * B)
#define FDT_SIZE 10
char DISKNAME[] = "SFS_DISK";


// -- STRUCTS/TYPES --
typedef char Byte; // Alias for char to improve comprehensibility

typedef struct Inode {
    int size; // Size of the inode's data in bytes
    int blockPointers[13]; // 0-11 = direct pointers, 12 = single-indirect pointer
} Inode;

typedef struct SuperBlock {
    int magic;
    int blockSize; // Size of each block, in bytes
    int sfsSize; // Size of the entire file system, in blocks (Q)
    int inodeTableSize; // Size of the inode table, in blocks (M)
    int dataBlocksCount; // Number of data blocks (N)
    int fbmSize; // Size of the free bitmap, in blocks (L)
    Inode rootDir; // The inode for the root directory
} SuperBlock;

typedef struct DirEntry {
    Byte used;
    char filename[MAXFILENAME + 1];
    short inodeNum;
} DirEntry;

typedef struct File {
    short inodeNum;
    int rwHeadPos;
} File;


// -- STATIC MEMBERS --
SuperBlock superBlock;
Inode inodeTable[DIR_SIZE];
DirEntry rootDirEntries[DIR_SIZE];
int currentFileIndex; // Used in sfs_getnextfilename() to track the index of the current file
Byte fbm[L * B]; // Free bitmap
File FDT[FDT_SIZE]; // File descriptor table


// -- HELPER FUNCTIONS --

// Helper for single bit set (used by free bitmap)
void setBit(Byte *bytes, int n) {
    bytes[BYTE_OFFSET(n)] |= (1 << BIT_OFFSET(n));
}

// Helper for single bit clear (used by free bitmap)
void clearBit(Byte *bytes, int n) {
    bytes[BYTE_OFFSET(n)] &= ~(1 << BIT_OFFSET(n));
}

// Helper for single bit get (used by free bitmap)
int getBit(const Byte *bytes, int n) {
    Byte bit = bytes[BYTE_OFFSET(n)] & (1 << BIT_OFFSET(n));
    return bit != 0;
}

// Returns the total number of data blocks that have not been allocated
int sfs_countFreeDataBlocks(void) {
    int count = 0;
    for (int i = 0; i < N; ++i) {
        if (getBit(&fbm[BYTE_OFFSET(i)], i) == 0)
            ++count;
    }
    return count;
}

// Returns the first free data block as per the free bitmap, updating the free bitmap on successful allocation
int sfs_allocateFreeDataBlock(void) {
    for (int i = 0; i < N; ++i) {
        if (getBit(fbm, i) == 0) {
            setBit(fbm, i);
            return i + 1 + superBlock.inodeTableSize; // Offset from absolute address of the data block at `i`
        }
    }
    return -1;
}

// Deallocates a block by clearing its 'tracker bit' in the free bitmap
int sfs_freeDataBlock(int block) {
    int n = block - 1 - superBlock.inodeTableSize; // Offset from absolute address of the data block
    if (n < 0 || n > N) {
        fprintf(stderr, "Failed to free block: block address is outside of free bitmap bounds.\n");
        return -1;
    }

    clearBit(fbm, n);
    return 0;
}

// Finds the first FDT slot not in use 
int sfs_getNextFreeFDTPos(int startPos) {
    for (int i = 0; i < FDT_SIZE; ++i) {
        if (FDT[(startPos + i) % FDT_SIZE].inodeNum < 0)
            return (startPos + i) % FDT_SIZE;
    }
    return -1; // FDT is full
}

// Finds up the first directory entry not in use
int sfs_getNextFreeDirEntry(int startPos) {
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (rootDirEntries[(startPos + i) % DIR_SIZE].used == 0) {
            return (startPos + i) % DIR_SIZE;
        }
    }
    return -1; // Root directory is full
}

// Gets the data block pointers (excluding the indirect pointer block) of an inode (up to `blocksToGet`)
int getInodeBlockPointers(Inode inode, int pointers[], int blocksToGet) {
    int i = 0;
    for (; i < 12 && i < blocksToGet; ++i) {
        pointers[i] = inode.blockPointers[i];
    }
    if (blocksToGet > 12) {
        int indirectBlockPointers[B / sizeof(int)];
        read_blocks(inode.blockPointers[12], 1, indirectBlockPointers);

        for (; i < blocksToGet; ++i) {
            pointers[i] = indirectBlockPointers[i - 12];
        }
    }
    return i;
}


// -- SFS API FUNCTIONS --

void mksfs(int fresh) {
    if (fresh) { // New file system
        init_fresh_disk(DISKNAME, B, Q);

        // Init super block
        superBlock.blockSize = B;
        superBlock.sfsSize = Q;
        superBlock.inodeTableSize = M;
        superBlock.dataBlocksCount = N;
        superBlock.fbmSize = L;
        superBlock.rootDir.size = DIR_SIZE * sizeof(DirEntry);

        // Init inode table and root directory
        for (short i = 0; i < DIR_SIZE; ++i) {
            inodeTable[i].size = -1;
            rootDirEntries[i].inodeNum = i;
        }

        // Write inodeTable to disk
        write_blocks(1, superBlock.inodeTableSize, inodeTable);

        // Allocate blocks for root dir and then write the root dir to disk
        int dirSizeInBlocks = ceil((double)superBlock.rootDir.size / B);
        if (dirSizeInBlocks > 12) // Add a block for the indirect pointers
            ++dirSizeInBlocks;
        
        if (N <= dirSizeInBlocks) {
            fprintf(stderr, "Failed to make new sfs: sfs size is too small for the size of the root directory.\n");
            return;
        }

        int dirBlockPointers[dirSizeInBlocks];
        for (int i = 0; i < dirSizeInBlocks; ++i) {
            dirBlockPointers[i] = sfs_allocateFreeDataBlock();
        }

        int i = 0;
        for (; i < 12 && i < dirSizeInBlocks; ++i) {
            superBlock.rootDir.blockPointers[i] = dirBlockPointers[i];
        }
        if (dirSizeInBlocks > 12) {
            int indirectBlockPointers[B / sizeof(int)];
            for (; i < dirSizeInBlocks; ++i) {
                indirectBlockPointers[i - 12] = sfs_allocateFreeDataBlock();
            }
            // Write the indirect pointers block first
            write_blocks(dirBlockPointers[dirSizeInBlocks - 1], 1, indirectBlockPointers);
            superBlock.rootDir.blockPointers[12] = dirBlockPointers[dirSizeInBlocks - 1];
        }
        // Write directory entries to disk (we assume that the data blocks allocated are contiguous and in order)
        write_blocks(dirBlockPointers[0], dirSizeInBlocks - 1, rootDirEntries);
        
        // Write the super block to disk too
        write_blocks(0, 1, &superBlock);
    } else { // Existing file system
        init_disk(DISKNAME, B, Q);

        // Load super block
        read_blocks(0, 1, &superBlock);

        // Load inode table
        read_blocks(1, superBlock.inodeTableSize, inodeTable);

        // Load root directory
        // Load directory entries from data blocks pointed to by the root dir inode
        int dirSizeInBlocks = ceil((double)superBlock.rootDir.size / B);
        int dirBlockPointers[dirSizeInBlocks * sizeof(int)];
        getInodeBlockPointers(superBlock.rootDir, dirBlockPointers, dirSizeInBlocks);
        
        Byte dirBlocksData[dirSizeInBlocks * B];
        for (int i = 0; i < dirSizeInBlocks; ++i) {
            // Read root directory data block i
            read_blocks(dirBlockPointers[i], 1, dirBlocksData + (i * B));
        }   
        
        for (int i = 0; i < DIR_SIZE; ++i) {
            rootDirEntries[i] = ((DirEntry *) dirBlocksData)[i];
        }
        
        // Load free bitmap
        read_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
    }

    // Init FDT
    for (int i = 0; i < FDT_SIZE; ++i) {
        FDT[i].inodeNum = -1;
    }
    
    // Init `currentFileIndex`
    currentFileIndex = 0;
}

int sfs_getnextfilename(char *filename) {
    // Look up the next used directory entry (= next file)
    for (int i = currentFileIndex; i < DIR_SIZE; ++i) {
        if (rootDirEntries[i].used == 1) {
            if (strncpy(filename, rootDirEntries[i].filename, MAXFILENAME) == NULL) {
                fprintf(stderr, "Failed to get filename: strcpy failed.\n");
                return -1;
            }
            currentFileIndex = i;
            break;
        }
    }
    if (currentFileIndex == DIR_SIZE) // Reset to start of directory
        currentFileIndex = 0;

    return currentFileIndex;
}

int sfs_getfilesize(const char *filename) {
    if (strlen(filename) > MAXFILENAME) {
        fprintf(stderr, "Failed to get file size: File name is too long.\n");
        return -1;
    }

    // Look up the file in the directory
    int dir_pos = -1;
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (rootDirEntries[i].used != 0 && strcmp(rootDirEntries[i].filename, filename) == 0) {
            dir_pos = i;
            break;
        }
    }
    
    if (dir_pos < 0) {
        fprintf(stderr, "Failed to get file size: '%s' does not exist.\n", filename);
        return -1;
    } 
    
    // File exists, return file size
    return inodeTable[rootDirEntries[dir_pos].inodeNum].size;
}

int sfs_fopen(char *filename) {
    if (strlen(filename) > MAXFILENAME) {
        fprintf(stderr, "Failed to open file: File name is too long.\n");
        return -1;
    }

    // Look up the file in the directory
    int dir_pos = -1;
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (rootDirEntries[i].used != 0 && strcmp(rootDirEntries[i].filename, filename) == 0) {
            dir_pos = i;
            break;
        }
    }

    // Get next free FDT slot index - if the file is not in the FDT, we know in advance if and where there is space
    int fdt_pos = sfs_getNextFreeFDTPos(0);

    if (dir_pos < 0) { // File does not exist, need to 'create' a new directory entry
        if ((dir_pos = sfs_getNextFreeDirEntry(0)) < 0 ) { // Directory is full
            fprintf(stderr, "Failed to create file: The directory is full.\n");
            return -1;
        }

        if (fdt_pos < 0) { // FDT is full
            fprintf(stderr, "Failed to create file: The FDT is full.\n");
            return -1;
        }

        // Update entry at the newly found free position in the directory for the file
        if (strcpy(rootDirEntries[dir_pos].filename, filename) == NULL) {
            fprintf(stderr, "Failed to create file: Failed to copy filename to rootDirEntries[%d].filename", dir_pos);
            return -1;
        }
        rootDirEntries[dir_pos].used = 1;
        rootDirEntries[dir_pos].inodeNum = (short)dir_pos;
        inodeTable[rootDirEntries[dir_pos].inodeNum].size = 0;

        // Successfully created the entry, now update the root directory on the disk
        // Write updated inodeTable to disk
        write_blocks(1, superBlock.inodeTableSize, inodeTable);
        // Write directory entries to disk (we assume that the data blocks allocated are contiguous and in order)
        int dirSizeInBlocks = ceil((double)superBlock.rootDir.size / B);
        write_blocks(superBlock.rootDir.blockPointers[0], dirSizeInBlocks - 1, rootDirEntries);
    } else { // File exists, need to check if it's already in the FDT
        for (int i = 0; i < FDT_SIZE; ++i) {
            if (FDT[i].inodeNum == rootDirEntries[dir_pos].inodeNum) { // File already in the FDT, at pos i
                return i;
            }
        }

        if (fdt_pos < 0) { // File not in FDT, and FDT is full
            fprintf(stderr, "Failed to open file: The FDT is full.\n");
            return -1;
        }
    }

    // File not in FDT, but there is space for it, so open the file (add it) in append mode (read/write head at EOF)
    FDT[fdt_pos].inodeNum = rootDirEntries[dir_pos].inodeNum;
    FDT[fdt_pos].rwHeadPos = inodeTable[rootDirEntries[dir_pos].inodeNum].size;
    return fdt_pos;
}

int sfs_fclose(int fd) {
    if (fd < 0 || fd >= FDT_SIZE) {
        fprintf(stderr, "Failed to close file: the file descriptor is outside the bounds of the FDT.\n");
        return -1;
    }

    if (FDT[fd].inodeNum < 0) {
        fprintf(stderr, "Failed to close file: the file descriptor has no file associated.\n");
        return -1;
    }

    // FDT[fd] points to valid open file, close it
    FDT[fd].inodeNum = -1;
    return 0;
}

int sfs_fwrite(int fd, const char *buf, int length) {
    if (length < 1) {
        return 0;
    }
    
    if (fd < 0 || fd >= FDT_SIZE) {
        fprintf(stderr, "Failed to write to file: the file descriptor is outside the bounds of the FDT.\n");
        return -1;
    }

    if (FDT[fd].inodeNum < 0) {
        fprintf(stderr, "Failed to write to file: the file descriptor has no file associated.\n");
        return -1;
    }
    
    // `FDT[fd]` points to a valid open file
    Inode inode = inodeTable[FDT[fd].inodeNum];
    
    if (FDT[fd].rwHeadPos > inode.size) {
        fprintf(stderr, "Failed to write to file: the read/write head is beyond the end of the file.\n");
        return -1;
    }
    
    if (FDT[fd].rwHeadPos + length > MAX_FILE_SIZE) {
        fprintf(stderr, "Failed to write to file: the file will exceed the max file size.\n");
        return -1;
    }
    
    // Get start and end bytes/blocks
    int startPos = FDT[fd].rwHeadPos;
    int endPos = startPos + length;

    int startBlock = startPos / B;
    int endBlock = endPos / B;
    
    int startBlockStartPos = startPos % B;
    
    int totalBlocksOld = ceil((double)inode.size / B);
    int totalBlocksNew = endBlock + 1;
    
    int blocksToAdd = totalBlocksNew - totalBlocksOld;
    int blocksToChange = totalBlocksOld - startBlock;
    if (totalBlocksOld <= 12 && totalBlocksNew > 12) // Need to include the indirect pointer block itself
        ++blocksToAdd;
    
    if (sfs_countFreeDataBlocks() < blocksToAdd) {
        fprintf(stderr, "Failed to write to file: there are not enough free data blocks available.\n");
        return -1;
    }

    // Gather pointers to relevant blocks (existing blocks to change + new blocks to add)
    int blocksToWrite = blocksToChange + blocksToAdd; // Existing blocks + blocksToAdd
    int blocksToWriteDataTo = blocksToWrite;
    if (totalBlocksOld <= 12 && totalBlocksNew > 12)
        --blocksToWriteDataTo;
    int blocksToWritePointers[blocksToWrite];
    
    if (blocksToChange == 0) { // All blocks in `blocksToWriteToPointers` need to be added
        // Get new blocks
        for (int i = 0; i < blocksToWrite; ++i) {
            int newBlock = sfs_allocateFreeDataBlock();
            if (newBlock < 0) {
                fprintf(stderr, "Failed to write to file: failed to get free data blocks.\n");
                for (int j = i - 1; j > blocksToChange; --j) {
                    clearBit(fbm, blocksToWritePointers[j]);
                }
                return -1;
            }
            blocksToWritePointers[i] = newBlock;
        }
    } else { // Need to write in existing blocks
        int *existingBlocksPointers = (int *) malloc(totalBlocksOld * sizeof(int));
        if (existingBlocksPointers == NULL) {
            fprintf(stderr, "Failed to write to file: ran out of memory while trying to get existing blocks.\n");
            return -1;
        }

        getInodeBlockPointers(inode, existingBlocksPointers, totalBlocksOld);
        int i = 0;
        // Get required existing blocks
        for (; i < blocksToChange; ++i) {
            blocksToWritePointers[i] = existingBlocksPointers[startBlock + i];
        }
        // Get new blocks
        for (; i < blocksToWrite; ++i) {
            int newBlock = sfs_allocateFreeDataBlock();
            if (newBlock < 0) {
                fprintf(stderr, "Failed to write to file: failed to get free data blocks.\n");
                for (int j = i - 1; j > blocksToChange; --j) {
                    clearBit(fbm, blocksToWritePointers[j]);
                }
                return -1;
            }
            blocksToWritePointers[i] = newBlock;
        }
        free(existingBlocksPointers);
    }

    // Create a new buffer that includes potential existing data in the start and end blocks
    Byte newBuf[blocksToWriteDataTo * B];
    if (blocksToChange > 0) { // Copy data from `startBlock` to the front of `newBuf`
        read_blocks(blocksToWritePointers[0], 1, newBuf);
    }
    if (blocksToChange == blocksToWrite && startBlock != endBlock) { // Copy data from `endBlock` to the end of `newBuf`
        read_blocks(blocksToWritePointers[blocksToWrite - 1], 1, newBuf + (blocksToWrite - 1) * B);
    }

    // Copy `buf` into `newBuf`, in between existing data from `startBlock` and `endBlock`
    for (int i = 0; i < length; ++i) {
        newBuf[startBlockStartPos + i] = buf[i];
    }

    // Write the buffer to disk
    // The block needed to write the indirect pointers is at the last index of `blocksToWrite` (if it's needed)
    for (int i = 0; i < blocksToWriteDataTo; ++i) {
        write_blocks(blocksToWritePointers[i], 1, newBuf + (i * B));
    }

    // Update the inode data
    // and update the availability of the newly allocated data blocks on the free bitmap
    if (blocksToAdd > 0) { // New blocks were added, need to update inode pointer data
        int i = 0;
        // Update direct pointers
        for (; i < blocksToWriteDataTo && startBlock + i < 12; ++i) {
            inode.blockPointers[startBlock + i] = blocksToWritePointers[i];
        }

        // Update indirect pointer block & the pointer to it (if necessary)
        if (endBlock > 11) {
            int indirectBlockPointers[B / sizeof(int)];
            // Load the indirect pointer block if it already exists
            if (totalBlocksOld > 12)
                read_blocks(inode.blockPointers[12], 1, indirectBlockPointers);

            // Update indirect pointers
            for (; i < blocksToWriteDataTo; ++i) {
                indirectBlockPointers[startBlock + i - 12] = blocksToWritePointers[i];
            }

            if (totalBlocksOld <= 12) { // Pointer to indirect block needs to be updated
                inode.blockPointers[12] = blocksToWritePointers[i];
            }
            // Write new/updated indirect pointer block to disk
            write_blocks(inode.blockPointers[12], 1, indirectBlockPointers);
        }
    }

    // Update the read/write head position and inode size (only if the write caused the file to increase in size)
    FDT[fd].rwHeadPos += length;
    if (FDT[fd].rwHeadPos > inode.size)
        inode.size = FDT[fd].rwHeadPos;
    
    // Update the inode in `inodeTable` and write the updated inodeTable back to disk
    inodeTable[FDT[fd].inodeNum] = inode;
    write_blocks(1, superBlock.inodeTableSize, inodeTable);
    
    // Write the updated free bitmap back to disk
    write_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
    
    return length;
}

int sfs_fread(int fd, char *buf, int length) {
    if (fd < 0 || fd >= FDT_SIZE) {
        fprintf(stderr, "Failed to read file: the file descriptor is outside the bounds of the FDT.\n");
        return -1;
    }

    if (FDT[fd].inodeNum < 0) {
        fprintf(stderr, "Failed to read file: the file descriptor has no file associated.\n");
        return -1;
    }
    
    // FDT[fd] points to valid open file
    Inode inode = inodeTable[FDT[fd].inodeNum];
    
    // Reduce length of read if EOF is closer than FDT[fd].rwHeadPos + length
    if (FDT[fd].rwHeadPos + length > inode.size) {
        length = inode.size - FDT[fd].rwHeadPos;
        if (length <= 0) {
            return 0;
        }
    }
    
    // Get start and end blocks
    int startBlock = FDT[fd].rwHeadPos / B;
    int endBlock = (FDT[fd].rwHeadPos + length) / B;

    int existingBlocksPointers[endBlock + 1];
    getInodeBlockPointers(inode, existingBlocksPointers, endBlock + 1);
    
    // Load all the blocks from startBlock to endBlock
    Byte loadedBlocksData[(endBlock - startBlock + 1) * B];
    Byte *currentBlockData = (Byte *) malloc(B);
    for (int i = startBlock; i <= endBlock; ++i) {
        read_blocks(existingBlocksPointers[i], 1, currentBlockData);
        for (int j = 0; j < B; ++j) {
            loadedBlocksData[(i - startBlock) * B + j] = currentBlockData[j];
        }
    }
    free(currentBlockData);

    int startBlockStartPos = FDT[fd].rwHeadPos % B;
    for (int j = 0; j < length; ++j) {
        buf[j] = loadedBlocksData[startBlockStartPos + j];
    }
    
    FDT[fd].rwHeadPos += length;
    return length;
}

int sfs_fseek(int fd, int loc) {
    if (fd < 0 || fd >= FDT_SIZE) {
        fprintf(stderr, "Failed to seek in file: the file descriptor is outside the bounds of the FDT.\n");
        return -1;
    }

    if (FDT[fd].inodeNum < 0) {
        fprintf(stderr, "Failed to seek in file: the file descriptor has no file associated.\n");
        return -1;
    }

    // FDT[fd] points to valid open file
    if (loc < 0 || loc > inodeTable[FDT[fd].inodeNum].size) {
        fprintf(stderr, "Failed to seek in file: the location to seek to is not valid for this file.\n");
        return -1;
    }
    FDT[fd].rwHeadPos = loc;
    return 0;
}

int sfs_remove(char *filename) {
    if (strlen(filename) > MAXFILENAME) {
        fprintf(stderr, "Failed to remove file: File name is too long.\n");
        return -1;
    }

    // Look up the file in the directory
    int dir_pos = -1;
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (strcmp(rootDirEntries[i].filename, filename) == 0) {
            dir_pos = i;
            rootDirEntries[i].used = 0;
            break;
        }
    }

    if (dir_pos < 0) {
        fprintf(stderr, "Failed to remove file: File does not exist.\n");
        return -1;
    }

    // File exists, remove it
    // Get Inode
    Inode inode = inodeTable[rootDirEntries[dir_pos].inodeNum];
    
    // Release data blocks
    int totalBlocks = ceil((double)inode.size / B);
    int existingBlocksPointers[totalBlocks];
    getInodeBlockPointers(inode, existingBlocksPointers, totalBlocks);
    
    for (int i = 0; i < totalBlocks; ++i) {
        if (sfs_freeDataBlock(existingBlocksPointers[i]) != 0) {
            fprintf(stderr, "Failed to remove file: inode data block %d could not be freed.\n", i);
            return -1;
        }
    }
    if (totalBlocks > 12 && sfs_freeDataBlock(existingBlocksPointers[12]) != 0) { // Need to free the indirect pointer block as well
        fprintf(stderr, "Failed to remove file: inode data block %d could not be freed.\n", 12);
        return -1;
    }
    
    // Release inode
    inode.size = -1;
    
    // Write changes to disk
    int dirSizeInBlocks = ceil((double)superBlock.rootDir.size / B);
    write_blocks(superBlock.rootDir.blockPointers[0], dirSizeInBlocks - 1, rootDirEntries);
    write_blocks(1, superBlock.inodeTableSize, inodeTable);
    write_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
    return 0;
}
