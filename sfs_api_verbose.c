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
#define B 1024 // Block size - MUST SET THE SAME VALUE FOR `BLOCK_SIZE` (in disk_eum.c)
#define Q 8306 // Total number of blocks for the file system - MUST SET THE SAME VALUE FOR `MAX_BLOCK` (in disk_eum.c)
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
    int size; // Number of data blocks in use
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
//    n -= 1 + superBlock.inodeTableSize; // Offset from absolute address of the data block at `n`
    bytes[BYTE_OFFSET(n)] |= (1 << BIT_OFFSET(n));
}

// Helper for single bit clear (used by free bitmap)
void clearBit(Byte *bytes, int n) {
//    n -= 1 + superBlock.inodeTableSize; // Offset from absolute address of the data block at `n`
    bytes[BYTE_OFFSET(n)] &= ~(1 << BIT_OFFSET(n));
}

// Helper for single bit get (used by free bitmap)
int getBit(const Byte *bytes, int n) {
//    n -= 1 + superBlock.inodeTableSize; // Offset from absolute address of the data block at `n` 
    Byte bit = bytes[BYTE_OFFSET(n)] & (1 << BIT_OFFSET(n));
    return bit != 0;
}

// Prints a specified byte as a string of 8 bits
void printByte(Byte *byte) {
    for (int i = 0; i < 8; ++i) {
        printf("%d", getBit(byte, i));
    }
    printf(" (%d)", *byte);
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
    printf("sfs_getNextFreeDirEntry: searching for next free entry...\n");
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (rootDirEntries[(startPos + i) % DIR_SIZE].used == 0) {
            printf("  rootDirEntries[%d].used == %d <-- FREE\n", (startPos + i) % DIR_SIZE, rootDirEntries[(startPos + i) % DIR_SIZE].used);
            return (startPos + i) % DIR_SIZE;
        }
        printf("  rootDirEntries[%d].used == %d\n", (startPos + i) % DIR_SIZE, rootDirEntries[(startPos + i) % DIR_SIZE].used);
    }
    return -1; // Root directory is full
}

// Helper to visualize the directory entries THAT ARE IN USE
void printDirectory(void) {
    printf("\n---- ROOT DIRECTORY ----\n");
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (rootDirEntries[i].used == 0)
            continue;
        
        printf("[%d]  '%s'  (inode %d)\n", i, rootDirEntries[i].filename, rootDirEntries[i].inodeNum);
    }
    printf("\n");
}

// Helper to visualize the file descriptor table
void printFDT(void) {
    printf("\n---- FDT ----\n");
    for (int i = 0; i < FDT_SIZE; ++i) {
        printf("[%d]  Inode %d  rwHeadPos = %d\n", i, FDT[i].inodeNum, FDT[i].rwHeadPos);
    }
    printf("\n");
}

// Helper to visualize free bitmap
void printFreeBitmap(void) {
    printf("\n---- FREE BITMAP ----\n");
    for (int i = 0; i < L * B * 8; ++i) {
        printf("%d", getBit(fbm, i));
        if (i > 1 && (i + 1) % (B / 8) == 0)
            printf(" bits %d-%d\n", i + 1 - (B / 8), i);
    }
    printf("\n");
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
        printf("mksfs: init fresh disk\n");
        init_fresh_disk(DISKNAME, B, Q);

        // Init super block
        printf("mksfs: init super block...\n");
        superBlock.blockSize = B;
        superBlock.sfsSize = Q;
        superBlock.inodeTableSize = M;
        superBlock.dataBlocksCount = N;
        superBlock.fbmSize = L;
        superBlock.rootDir.size = DIR_SIZE * sizeof(DirEntry);
        printf("  superBlock.blockSize = %d\n", superBlock.blockSize);
        printf("  superBlock.sfsSize = %d\n", superBlock.sfsSize);
        printf("  superBlock.inodeTableSize = %d\n", superBlock.inodeTableSize);
        printf("  superBlock.dataBlocksCount = %d\n", superBlock.dataBlocksCount);
        printf("  superBlock.fbmSize = %d\n", superBlock.fbmSize);
        printf("  superBlock.rootDir.size = %d\n", superBlock.rootDir.size);

        // Init inode table and root directory
        printf("mksfs: init inode table and root directory...\n");
        for (short i = 0; i < DIR_SIZE; ++i) {
            inodeTable[i].size = -1;
            rootDirEntries[i].inodeNum = i;
        }
        printDirectory();

        // Write inodeTable to disk
        printf("mksfs: writing inode table to disk\n");
        write_blocks(1, superBlock.inodeTableSize, inodeTable);

        // Allocate blocks for root dir and then write the root dir to disk
        printf("mksfs: writing root directory to disk\n");
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
        printf("  root dir is stored in blocks %d to %d\n", superBlock.rootDir.blockPointers[0], superBlock.rootDir.blockPointers[0] + dirSizeInBlocks - 1);
        
        // Write the super block to disk too
        printf("mksfs: writing super block to disk\n");
        write_blocks(0, 1, &superBlock);
    } else { // Existing file system
        printf("mksfs: init old disk\n");
        init_disk(DISKNAME, B, Q);

        // Load super block
        printf("mksfs: loading super block...\n");
        read_blocks(0, 1, &superBlock);
        printf("  superBlock.blockSize = %d\n", superBlock.blockSize);
        printf("  superBlock.sfsSize = %d\n", superBlock.sfsSize);
        printf("  superBlock.inodeTableSize = %d\n", superBlock.inodeTableSize);
        printf("  superBlock.dataBlocksCount = %d\n", superBlock.dataBlocksCount);
        printf("  superBlock.fbmSize = %d\n", superBlock.fbmSize);
        printf("  superBlock.rootDir.size = %d\n", superBlock.rootDir.size);

        // Load inode table
        printf("mksfs: loading inode table...\n");
        read_blocks(1, superBlock.inodeTableSize, inodeTable);
        for (int i = 0; i < DIR_SIZE; ++i) {
            if (i < 2 || i > DIR_SIZE - 3) {
                printf("  inodeTable[%d].size = %d\n", i, inodeTable[i].size);
            }
        }

        // Load root directory
        printf("mksfs: loading root directory...\n");
        // Load directory entries from data blocks pointed to by the root dir inode
        int dirSizeInBlocks = ceil((double)superBlock.rootDir.size / B);
        int dirBlockPointers[dirSizeInBlocks * sizeof(int)];
        getInodeBlockPointers(superBlock.rootDir, dirBlockPointers, dirSizeInBlocks);
        
        Byte dirBlocksData[dirSizeInBlocks * B];
        for (int i = 0; i < dirSizeInBlocks; ++i) {
            // Read root directory data block i
            read_blocks(dirBlockPointers[i], 1, dirBlocksData + (i * B));
        }   
        printf("  %d data blocks merged into single buffer, copying to root directory...\n", dirSizeInBlocks);
        
        for (int i = 0; i < DIR_SIZE; ++i) {
            rootDirEntries[i] = ((DirEntry *) dirBlocksData)[i];
        }
        
        printf("mksfs: root directory fully loaded (%d entries):\n", DIR_SIZE);
        printDirectory();
        
        // Load free bitmap
        printf("mksfs: loading free bitmap...\n");
        read_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
    }

    printFreeBitmap();
    
    // Init FDT
    printf("mksfs: init FDT\n");
    for (int i = 0; i < FDT_SIZE; ++i) {
        FDT[i].inodeNum = -1;
    }
    
    // Init `currentFileIndex`
    currentFileIndex = 0;

    printf("mksfs: initialization complete\n\n");
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
    printf("sfs_getfilesize: attempting get file size for '%s'\n", filename);
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
    printf("sfs_getfilesize: file size for '%s': %d bytes\n\n", filename, inodeTable[rootDirEntries[dir_pos].inodeNum].size);
    return inodeTable[rootDirEntries[dir_pos].inodeNum].size;
}

int sfs_fopen(char *filename) {
    printf("sfs_fopen: attempting to open '%s'\n", filename);
    if (strlen(filename) > MAXFILENAME) {
        fprintf(stderr, "Failed to open file: File name is too long.\n");
        return -1;
    }

    // Look up the file in the directory
    int dir_pos = -1;
    printf("sfs_fopen: looking in the directory...\n");
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (rootDirEntries[i].used != 0 && strcmp(rootDirEntries[i].filename, filename) == 0) {
            printf("  rootDirEntries[%d].filename = '%s' <-- FOUND\n", i, rootDirEntries[i].filename);
            dir_pos = i;
            break;
        }
    }

    // Get next free FDT slot index - if the file is not in the FDT, we know in advance if and where there is space
    int fdt_pos = sfs_getNextFreeFDTPos(0);

    if (dir_pos < 0) { // File does not exist, need to 'create' a new directory entry
        printf("sfs_fopen: '%s' does not exist, attempting to create it...\n", filename);
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
        printf("sfs_fopen: '%s' was created at rootDirEntries[%d] (inode %d), updating disk...\n", rootDirEntries[dir_pos].filename, dir_pos, rootDirEntries[dir_pos].inodeNum);
        // Write updated inodeTable to disk
        write_blocks(1, superBlock.inodeTableSize, inodeTable);
        // Write directory entries to disk (we assume that the data blocks allocated are contiguous and in order)
        int dirSizeInBlocks = ceil((double)superBlock.rootDir.size / B);
        write_blocks(superBlock.rootDir.blockPointers[0], dirSizeInBlocks - 1, rootDirEntries);
        printf("  root dir is stored in blocks %d to %d\n", superBlock.rootDir.blockPointers[0], superBlock.rootDir.blockPointers[0] + dirSizeInBlocks - 1);
    } else { // File exists, need to check if it's already in the FDT
        printf("sfs_fopen: '%s' exists (rootDirEntries[%d]), checking if it's already in the FDT...\n", filename, rootDirEntries[dir_pos].inodeNum);
        for (int i = 0; i < FDT_SIZE; ++i) {
            if (FDT[i].inodeNum == rootDirEntries[dir_pos].inodeNum) { // File already in the FDT, at pos i
                printf("sfs_fopen: '%s' is already opened at FDT[%d]\n\n", filename, i);
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
    printf("sfs_fopen: '%s' (inode %d) opened at FDT[%d]:\n", filename, FDT[fdt_pos].inodeNum, fdt_pos);
    printFDT();
    return fdt_pos;
}

int sfs_fclose(int fd) {
    printf("sfs_fclose: attempting to close the file at FDT[%d]\n", fd);
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
    printf("sfs_fclose: file at FDT[%d] closed successfully\n\n", fd);
    return 0;
}

int sfs_fwrite(int fd, const char *buf, int length) {
    printf("sfs_fwrite: attempting to write %d bytes to the file at FDT[%d]\n", length, fd);
    
    if (length < 1) {
    printf("sfs_fwrite: nothing to write (length < 1)\n\n");
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
    printf("sfs_fwrite: loaded inode for file at FDT[%d]\n", fd);
    
    if (FDT[fd].rwHeadPos > inode.size) {
        fprintf(stderr, "Failed to write to file: the read/write head is beyond the end of the file.\n");
        return -1;
    }
    
    if (FDT[fd].rwHeadPos + length > MAX_FILE_SIZE) {
        fprintf(stderr, "Failed to write to file: the file will exceed the max file size.\n");
        return -1;
    }
    
    printf("sfs_fwrite: preparing for write...\n");
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
        printf("sfs_fwrite: getting %d new blocks (%d for data)\n", blocksToWrite - blocksToChange, blocksToWriteDataTo - blocksToChange);
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
        printf("sfs_fwrite: getting %d required existing blocks\n", blocksToChange);
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
        printf("sfs_fwrite: getting %d new blocks (%d for data)\n", blocksToWrite - blocksToChange, blocksToWriteDataTo - blocksToChange);
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

    printf("sfs_fwrite: data blocks to write to: [%d", blocksToWritePointers[0]);
    for (int i = 1; i < blocksToWrite; ++i) {
        printf(", %d", blocksToWritePointers[i]);
    }
    printf("]\n");

    printf("sfs_fwrite: updating buffer...\n");
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
        if (i < 2 || i > length - 3) {
            printf("  newBuf[%d] = ", startBlockStartPos + i);
            printByte(&newBuf[startBlockStartPos + i]);
            printf(" (buf[%d] = %d)\n", i, buf[i]);
            if (i == 1 && length > 1) { 
                printf("  ...\n");
            }
        }
        
    }

    printf("sfs_fwrite: writing buffer to disk...\n");
    // Write the buffer to disk
    // The block needed to write the indirect pointers is at the last index of `blocksToWrite` (if it's needed)
    for (int i = 0; i < blocksToWriteDataTo; ++i) {
        printf("  writing block %d of %d at location %d (byte 0 = %d)\n", i + 1, blocksToWriteDataTo, blocksToWritePointers[i], (newBuf + (i * B))[0]);
        write_blocks(blocksToWritePointers[i], 1, newBuf + (i * B));
    }

    printf("sfs_fwrite: updating inode data and free bitmap...\n");
    // Update the inode data
    // and update the availability of the newly allocated data blocks on the free bitmap
    if (blocksToAdd > 0) { // New blocks were added, need to update inode pointer data
        int i = 0;
        // Update direct pointers
        for (; i < blocksToWriteDataTo && startBlock + i < 12; ++i) {
            inode.blockPointers[startBlock + i] = blocksToWritePointers[i];
            printf("  inode.blockPointers[%d] = %d\n", startBlock + i, inode.blockPointers[startBlock + i]);
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
                printf("  indirectBlockPointers[%d] = %d\n", startBlock + i - 12, indirectBlockPointers[startBlock + i - 12]);
            }

            if (totalBlocksOld <= 12) { // Pointer to indirect block needs to be updated
                inode.blockPointers[12] = blocksToWritePointers[i];
                printf("  inode.blockPointers[12] = %d\n", inode.blockPointers[12]);
            }
            // Write new/updated indirect pointer block to disk
            write_blocks(inode.blockPointers[12], 1, indirectBlockPointers);
        }
    }
        
    FDT[fd].rwHeadPos += length;
    if (FDT[fd].rwHeadPos > inode.size)
        inode.size = FDT[fd].rwHeadPos;
    
    printf("  inode.size = %d\n", inode.size);
    printf("  FDT[%d].rwHeadPos = %d\n", fd, FDT[fd].rwHeadPos);
    
    // Update the inode in `inodeTable` and write the updated inodeTable back to disk
    printf("sfs_fwrite: writing updated inode data and updated free bitmap to disk...\n");
    inodeTable[FDT[fd].inodeNum] = inode;
    write_blocks(1, superBlock.inodeTableSize, inodeTable);
    
    // Write the updated free bitmap back to disk
    write_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
    
    printf("sfs_fwrite: wrote %d bytes in file at FDT[%d] (FDT[%d].rwHeadPos = %d, new file size = %d bytes)\n\n", length, fd, fd, FDT[fd].rwHeadPos, inode.size);
    return length;
}

int sfs_fread(int fd, char *buf, int length) {
    printf("sfs_read: attempting to read %d bytes from file at FDT[%d]\n", length, fd);
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
            printf("sfs_read: read 0 bytes from file at FDT[%d] (FDT[%d].rwHeadPos = %d, file size = %d)", fd, fd, FDT[fd].rwHeadPos, inode.size);
            return 0;
        }
    }
    printf("sfs_read: reading %d bytes, starting at byte %d (file size = %d bytes)\n", length, FDT[fd].rwHeadPos, inode.size);
    
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

    printf("sfs_read: copying read data to buffer\n");
    int startBlockStartPos = FDT[fd].rwHeadPos % B;
    printf("  startBlockStartPos = %d\n", startBlockStartPos);
    for (int j = 0; j < length; ++j) {
        buf[j] = loadedBlocksData[startBlockStartPos + j];
        if (j < 2 || j > length - 3) {
            printf("  buf[%d] = ", j);
            printByte(&buf[j]);
            printf(" (loadedBlocksData[%d])\n", startBlockStartPos + j);
            if (j == 1 && length > 1) {
                printf("  ...\n");
            }
        }
    }
    
    FDT[fd].rwHeadPos += length;
    printf("sfs_read: read %d bytes from file at FDT[%d] (FDT[%d].rwHeadPos = %d)\n\n", length, fd, fd, FDT[fd].rwHeadPos);
    return length;
}

int sfs_fseek(int fd, int loc) {
    printf("sfs_fseek: attempting to seek to byte %d of FDT[%d]\n", loc, fd);
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
    printf("sfs_fseek: seek complete, FDT[%d].rwHeadPos = %d\n\n", fd, FDT[fd].rwHeadPos);
    return 0;
}

int sfs_remove(char *filename) {
    printf("sfs_remove: attempting to remove '%s'\n", filename);
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
    printf("sfs_remove: '%s' found at position %d in the directory, removing...\n", filename, dir_pos);
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
    printf("sfs_remove: '%s' was successfully removed from the file system\n\n", filename);
    return 0;
}
