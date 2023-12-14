#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <malloc.h>
#include <memory.h>
#include <math.h>
#include <string.h>

#include "sfs_api.h"
#include "disk_emu.h"

#define B 1024
#define Q 8306 // MUST BE SAME VALUE AS MAX_BLOCK (in disk_eum.c)
#define M 112 // Calculate by running 'python calc_disk_alloc.py <Q>'
#define N 8192 // Calculate by running 'python calc_disk_alloc.py <Q>'
#define L 1 // Calculate by running 'python calc_disk_alloc.py <Q>'
#define DIR_SIZE 2048  // Calculate by running 'python calc_disk_alloc.py <Q>'

#define FDT_SIZE 10
#define MAX_FILE_SIZE (268 * B)

char DISKNAME[] = "SFS_DISK";

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
    char filename[MAX_FILENAME_LENGTH];
    short inodeNum;
} DirEntry;

typedef struct File {
    short inodeNum;
    int rwHeadPos;
} File;

SuperBlock superBlock;
Inode inodeTable[DIR_SIZE];
DirEntry rootDirEntries[DIR_SIZE];
Byte fbm[L * B];
File FDT[FDT_SIZE];

void printByte(Byte *byte) {
    for (int i = 0; i < 8; ++i) {
        printf("%d", getBit(byte, i));
    }
    printf(" '%c'\t", *byte);
}

void setBit(Byte *bytes, int n) {
    bytes[BYTE_OFFSET(n)] |= (1 << BIT_OFFSET(n));
}

void clearBit(Byte *bytes, int n) {
    bytes[BYTE_OFFSET(n)] &= ~(1 << BIT_OFFSET(n));
}

int getBit(const Byte *bytes, int n) {
    Byte bit = bytes[BYTE_OFFSET(n)] & (1 << BIT_OFFSET(n));
    return bit != 0;
}

int sfs_countFreeDataBlocks(void) {
    int count = 0;
    for (int i = 0; i < N; ++i) {
        if (getBit(&fbm[BYTE_OFFSET(i)], i) == 0)
            ++count;
    }
    return count;
}

int sfs_getFreeDataBlock(void) {
    for (int i = 0; i < N; ++i) {
        if (getBit(&fbm[BYTE_OFFSET(i)], i) == 0)
            return i;
    }
    return -1;
}

int sfs_getNextFreeFDTPos(int startPos) {
    for (int i = 0; i < FDT_SIZE; ++i) {
        if (FDT[(startPos + i) % FDT_SIZE].inodeNum < 0)
            return (startPos + i) % FDT_SIZE;
    }
    return -1; // FDT is full
}

int sfs_getNextFreeDirEntry(int startPos) {
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (rootDirEntries[(startPos + i) % DIR_SIZE].used == 0)
            return (startPos + i) % DIR_SIZE;
    }
    return -1; // Root directory is full
}

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
        printf("  superBlock.blockSize = %d\n", superBlock.blockSize);
        printf("  superBlock.sfsSize = %d\n", superBlock.sfsSize);
        printf("  superBlock.inodeTableSize = %d\n", superBlock.inodeTableSize);
        printf("  superBlock.dataBlocksCount = %d\n", superBlock.dataBlocksCount);
        printf("  superBlock.fbmSize = %d\n", superBlock.fbmSize);
        printf("  superBlock.rootDir.size = %d\n", superBlock.rootDir.size);

        // Init inode table and root directory
        printf("mksfs: init inode table and root directory...\n");
        for (int i = 0; i < DIR_SIZE; ++i) {
            inodeTable[i].size = -1;
            rootDirEntries[i].inodeNum = i;
            if (i == 0 || i == DIR_SIZE - 1) {
                printf("  inodeTable[%d].size = %d\n", i, inodeTable[i].size);
                printf("  rootDirEntries[%d] = { .used = %d; .filename = '%s'; .inodeNum = %d; }\n",
                       i, rootDirEntries[i].used, rootDirEntries[i].filename, rootDirEntries[i].inodeNum);
            }
        }
    } else { // Existing file system
        printf("mksfs: init old disk\n");
        init_disk(DISKNAME, B, Q);

        // Load super block
        printf("mksfs: load super block...\n");
        read_blocks(0, 1, &superBlock);
        printf("  superBlock.blockSize = %d\n", superBlock.blockSize);
        printf("  superBlock.sfsSize = %d\n", superBlock.sfsSize);
        printf("  superBlock.inodeTableSize = %d\n", superBlock.inodeTableSize);
        printf("  superBlock.dataBlocksCount = %d\n", superBlock.dataBlocksCount);
        printf("  superBlock.fbmSize = %d\n", superBlock.fbmSize);
        printf("  superBlock.rootDir.size = %d\n", superBlock.rootDir.size);

        // Load inode table
        printf("mksfs: load inode table...\n");
        read_blocks(1, superBlock.inodeTableSize, &inodeTable);
        for (int i = 0; i < DIR_SIZE; ++i) {
            if (i < 2 || i > DIR_SIZE - 3) {
                printf("  inodeTable[%d].size = %d\n", i, inodeTable[i].size);
            }
        }

        // Load root directory
        printf("mksfs: load root directory");
        Byte *dataBlocksBuffer = (Byte *) malloc(superBlock.rootDir.size * B);
        Byte *currentBlock = (Byte *) malloc(B);
        // Load directory entries from data blocks pointed to by the root dir inode
        for (int i = 0; i < superBlock.rootDir.size - 1; ++i) { // i = block num
            // Read block i
            read_blocks(1 + superBlock.inodeTableSize + superBlock.rootDir.blockPointers[i], 1, currentBlock);
            if (i < 12) { // Block i is data (direct)
                memcpy((Byte *) dataBlocksBuffer + (i * B), currentBlock, B);
            } else { // Block i is indirectBlockPointers to data (single-indirect)
                int indirectBlockPointers[B / 4];
                memcpy(indirectBlockPointers, (int *) currentBlock, B);
                for (int j = 0; j < B / 4; ++j) {
                    read_blocks(1 + superBlock.inodeTableSize + indirectBlockPointers[j], 1, currentBlock);
                    memcpy((Byte *) dataBlocksBuffer + ((i + j) * B), currentBlock, B);
                }
            }
        }
        free(currentBlock);
        printf("mksfs: data blocks merged into single buffer, copy to root directory...");
        /* SINGLE LOOP INITIALIZATION
        DirEntry *entry = (DirEntry *) dataBlocksBuffer;
        for (int i = 0; i < DIR_SIZE; ++i) { 
            if (entry == NULL) { // Set used flag for remaining entries to 0
                rootDirEntries[i].used = 0;
                continue;
            }
            rootDirEntries[i] = *entry;
            printf("  rootDirEntries[%d] = { .used = %d; .filename = '%s'; .inodeNum = %d; }\n",
                   i, rootDirEntries[i].used, rootDirEntries[i].filename, rootDirEntries[i].inodeNum);
            entry += sizeof(DirEntry);
        }
        */

        int i = 0;
        DirEntry *entry = (DirEntry *) dataBlocksBuffer;
        while (entry != NULL) { // Add existing directory entries
            rootDirEntries[i] = *entry;
            printf("  rootDirEntries[%d] = { .used = %d; .filename = '%s'; .inodeNum = %d; }\n",
                   i, rootDirEntries[i].used, rootDirEntries[i].filename, rootDirEntries[i].inodeNum);
            entry += sizeof(DirEntry);
            ++i;
        }
        for (; i < DIR_SIZE; ++i) { // Set used flag for remaining entries to 0
            rootDirEntries[i].used = 0;
        }
        
        free(dataBlocksBuffer);
        printf("mksfs: root directory fully loaded (%d entries).", i + 1);
        
        // Load free bitmap
        printf("mksfs: load free bitmap\n");
        read_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
        for (i = 0; i < superBlock.fbmSize * B; ++i) {
            if (i % 128 == 0) {
                printf("  fbm[%d] = ", i);
                printByte(&fbm[i]);
                printf("\n");
            }
        }
    }
    
    // Init FDT
    printf("mksfs: init FDT\n");
    for (int i = 0; i < FDT_SIZE; ++i) {
        FDT[i].inodeNum = -1;
    }

    printf("mksfs: initialization complete\n\n");
}

int sfs_fopen(char *filename) {
    printf("sfs_open: opening '%s'\n", filename);
    if (strlen(filename) > MAX_FILENAME_LENGTH) {
        fprintf(stderr, "Failed to open file: File name is too long.\n");
        return -1;
    }

    // Look up the file in the directory
    int dir_pos = -1;
    for (int i = 0; i < DIR_SIZE; ++i) {
        if (strcmp(rootDirEntries[i].filename, filename) == 0) {
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

        // Create entry for the file at the newly found free position
        if (strcpy(rootDirEntries[dir_pos].filename, filename) == NULL) {
            fprintf(stderr, "Failed to create file: Failed to copy filename to DirEntry::filename");
            return -1;
        }
        rootDirEntries[dir_pos].used = 1;
        inodeTable[rootDirEntries[dir_pos].inodeNum].size = 0;
    } else { // File exists, need to check if it's already in the FDT
        for (int i = 0; i < FDT_SIZE; ++i) {
            if (FDT[i].inodeNum == rootDirEntries[dir_pos].inodeNum) // File already in the FDT, at pos i
                return i;
        }

        if (fdt_pos < 0) { // File not in FDT, and FDT is full
            fprintf(stderr, "Failed to open file: The FDT is full.\n");
            return -1;
        }
    }

    // File not in FDT, but there is space for it, so add it
    FDT[fdt_pos].inodeNum = rootDirEntries[dir_pos].inodeNum;
    printf("sfs_open: '%s' opened in the FDT at index %d\n\n", filename, fdt_pos);
    return fdt_pos;
}

int sfs_fclose(int fd) {
    return 0;
}

int sfs_fwrite(int fd, const char *buf, int length) {
    printf("sfs_write: attempting to write '%s' to the file at FDT[%d]\n", buf, fd);
    if (fd < 0 || fd >= FDT_SIZE) {
        fprintf(stderr, "Failed to write to file: the file descriptor is outside the bounds of the FDT.\n");
        return -1;
    }

    if (FDT[fd].inodeNum < 0) {
        fprintf(stderr, "Failed to write to file: the file descriptor has no file associated.\n");
        return -1;
    }
    
    // FDT[fd] points to valid open file
    Inode inode = inodeTable[FDT[fd].inodeNum];
    printf("sfs_write: loaded inode for file at FDT[%d]\n", fd);
    int newSize = inode.size + length;

    if (newSize > MAX_FILE_SIZE) {
        fprintf(stderr, "Failed to write to file: there is not enough space in the file.\n");
        return -1;
    }
    
    printf("sfs_write: preparing for write...\n");

    // Get start and end blocks
    int startBlockNum = FDT[fd].rwHeadPos / B;
    int endBlockNum = (FDT[fd].rwHeadPos + length) / B;
    
    int lastBlockOffset = inode.size % B;
    int lastBlockSpaceLeft = B - lastBlockOffset;
    if (inode.size == 0)
        lastBlockSpaceLeft = 0;
    
    int blocksUsedBefore = inode.size / B + (inode.size % B != 0 ? 1 : 0);
    int extraBlocksNeeded = (length - lastBlockSpaceLeft) / B + ((length - lastBlockSpaceLeft) % B != 0 ? 1 : 0);
    int blocksUsedAfter = blocksUsedBefore + extraBlocksNeeded;
    if (blocksUsedBefore <= 12 && blocksUsedAfter > 12) // Need to include the indirect pointer block itself
        ++extraBlocksNeeded;

    printf("  startBlockNum = %d\n", startBlockNum);
    printf("  endBlockNum = %d\n", endBlockNum);
    printf("  lastBlockOffset = %d\n", lastBlockOffset);
    printf("  lastBlockSpaceLeft = %d\n", lastBlockSpaceLeft);
    printf("  blocksUsedBefore = %d\n", blocksUsedBefore);
    printf("  extraBlocksNeeded = %d\n", extraBlocksNeeded);
    printf("  blocksUsedAfter = %d\n", blocksUsedAfter);
    
    if (sfs_countFreeDataBlocks() < extraBlocksNeeded) {
        fprintf(stderr, "Failed to write to file: there are not enough free data blocks available.");
        return -1;
    }
    
    // Gather pointers to relevant blocks (extra blocks + current last block if it has space)
    int blocksToWrite = extraBlocksNeeded + (lastBlockSpaceLeft == 0 ? 0 : 1);
    int blocksToWritePointers[blocksToWrite];

    // If the current last block has space, include it in the set of blocks to write to
    if (lastBlockSpaceLeft > 0) {
        if (inode.size > 12 * B) { // Current last block is an indirect block
            int indirectBlockPointers[B / 4];
            read_blocks(1 + superBlock.inodeTableSize + inode.blockPointers[12], 1, indirectBlockPointers);
            blocksToWritePointers[0] = indirectBlockPointers[blocksUsedBefore - 13];
        } else { // Current last block is a direct block
            blocksToWritePointers[0] = inode.blockPointers[blocksUsedBefore - 1];
        }
    }
    
    // Get & add extra block pointers
    for (int i = 0; i < extraBlocksNeeded; ++i) {
        int newBlock = sfs_getFreeDataBlock();
        if (newBlock < 0) {
            fprintf(stderr, "Failed to write to file: failed to get free data blocks.");
            return -1;
        }
        blocksToWritePointers[(lastBlockSpaceLeft == 0 ? 0 : 1) + i] = newBlock;
    }

    printf("sfs_write: fetched data blocks: [ %d", blocksToWritePointers[0]);
    for (int i = 1; i < blocksToWrite; ++i) {
        printf(", %d", blocksToWritePointers[i]);
    }
    printf(" ]\n");

    printf("sfs_write: updating buffer...\n");
    // Update the buffer to (potentially) include the data on the current last block in the buffer
    char *newBuf = (char *) malloc(lastBlockOffset + length);
    if (lastBlockSpaceLeft > 0) {
        if (inode.size > 12 * B) { // Current last block is an indirect block
            int indirectBlockPointers[B / 4];
            read_blocks(1 + superBlock.inodeTableSize + inode.blockPointers[12], 1, indirectBlockPointers);
            read_blocks(1 + superBlock.inodeTableSize + indirectBlockPointers[blocksUsedBefore - 13], 1, newBuf);
        } else { // Current last block is a direct block
            read_blocks(1 + superBlock.inodeTableSize + inode.blockPointers[blocksUsedBefore - 1], 1, newBuf);
        }
    }

    // Copy each byte of `buf` into `newBuf`, after the data from the current last block (if any)
    for (int i = 0; i < length; ++i) {
        newBuf[lastBlockOffset + i] = buf[i];
    }
    printf("  newBuf = '%s'\n", newBuf);

    printf("sfs_write: writing buffer to disk...\n");
    // Write the buffer to disk
    // If a block is needed to write the indirect pointers to, reserve the block at the last index of `blocksToWrite`
    int blocksToWriteBuffer = blocksToWrite - (blocksUsedBefore <= 12 && blocksUsedAfter > 12 ? 1 : 0);
    for (int i = 0; i < blocksToWriteBuffer; ++i) {
        write_blocks(1 + superBlock.inodeTableSize + blocksToWritePointers[i], 1, newBuf + (i * B));
        printf("  wrote 1 block at location %d: '%s'\n", 1 + superBlock.inodeTableSize + blocksToWritePointers[i], newBuf + (i * B));
    }
    free(newBuf);

    printf("sfs_write: updating inode data and free bitmap...\n");
    // Update the inode data
    // and update the availability of the newly allocated data blocks on the free bitmap
    int blockNum = blocksUsedBefore - (lastBlockSpaceLeft == 0 ? 0 : 1);
    int i = 0;
    for (; i < blocksToWrite && blockNum < 12; ++i, ++blockNum) { // Update direct pointers
        inode.blockPointers[blockNum] = blocksToWritePointers[i];
        setBit(fbm, blocksToWritePointers[i]);
        printf("  inode.blockPointers[%d] = %d\n", blockNum, inode.blockPointers[blockNum]);
    }
    
    if (blocksUsedAfter > 12) { // Need to add block pointers in the indirect block
        int indirectBlockPointers[B / 4];
        if (blocksUsedBefore > 12) // Load the indirect pointer block if it already exists
            read_blocks(1 + superBlock.inodeTableSize + inode.blockPointers[12], 1, indirectBlockPointers);

        for (; i < blocksToWrite; ++i, ++blockNum) { // Update indirect pointers
            indirectBlockPointers[blockNum - 13] = blocksToWritePointers[i];
            setBit(fbm, blocksToWritePointers[i]);
            printf("  indirectBlockPointers[%d] = %d\n", blockNum - 13, indirectBlockPointers[blockNum - 13]);
        }
        
        // Write new/updated indirect pointer block to disk
        write_blocks(1 + superBlock.inodeTableSize + blocksToWritePointers[i], 1, indirectBlockPointers);
        setBit(fbm, blocksToWritePointers[i]);
        inode.blockPointers[12] = blocksToWritePointers[i];
        printf("  inode.blockPointers[12] = %d\n", inode.blockPointers[12]);
    }
    
    inode.size = newSize;
    printf("  inode.size = %d\n", inode.size);

    printf("sfs_write: writing updated inode data and updated free bitmap to disk...\n");
    inodeTable[FDT[fd].inodeNum] = inode;    
    
    // Write the updated inode back to disk
    write_blocks(1 + FDT[fd].inodeNum, 1, &inode);
    
    // Write the updated free bitmap back to disk
    write_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
    
    FDT[fd].rwHeadPos = inode.size;
    printf("sfs_write: wrote '%s' in file at FDT[%d] (FDT[%d].rwHeadPos = %d))\n\n", buf, fd, fd, FDT[fd].rwHeadPos);
    return 0;
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
    printf("inode.size = %d\n", inode.size);
    if (FDT[fd].rwHeadPos + length >= inode.size) {
        length = inode.size - FDT[fd].rwHeadPos;
        if (length < 0)
            length = 0;
        printf("sfs_read: reading %d bytes (up to EOF)\n", length);
    }
    
    // Get start and end blocks
    int startBlockNum = FDT[fd].rwHeadPos / B;
    int endBlockNum = (FDT[fd].rwHeadPos + length) / B;
    
    // Load all the blocks from startBlockNum to endBlockNum
    printf("sfs_read: loading %d block(s) to read\n", endBlockNum - startBlockNum + 1);
    Byte loadedData[(endBlockNum - startBlockNum + 1) * B];
    Byte *currentBlockData = (Byte *) malloc(B);
    int i = startBlockNum, blocksRead = 0;
    for (; i <= endBlockNum && i < 12; ++i, ++blocksRead) { // Read direct blocks
        read_blocks(1 + superBlock.inodeTableSize + inode.blockPointers[i], 1, currentBlockData);
        for (int j = 0; j < B; ++j) { // Copy currentBlockData to loadedData
            loadedData[blocksRead + j] = currentBlockData[j];
        }
        printf("  read 1 block at location %d\n", 1 + superBlock.inodeTableSize + inode.blockPointers[i]);
    }

    if (endBlockNum > 12) { // Need to read blocks from indirect block
        int indirectBlockPointers[B / 4];
        read_blocks(1 + superBlock.inodeTableSize + inode.blockPointers[12], 1, indirectBlockPointers);
        for (; i <= endBlockNum; ++i, ++blocksRead) { // Read indirect blocks
            read_blocks(1 + superBlock.inodeTableSize + indirectBlockPointers[i - 13], 1, currentBlockData);
            for (int j = 0; j < B; ++j) { // Copy currentBlockData to loadedData
                loadedData[blocksRead + j] = currentBlockData[j];
            }
            printf("  read 1 block at location %d\n", 1 + superBlock.inodeTableSize + indirectBlockPointers[i - 13]);
        }
    }
    free(currentBlockData);

    printf("sfs_read: copying read data to buffer\n");
    int startBlockOffset = FDT[fd].rwHeadPos % B;
    for (int j = 0; j < length; ++j) {
        buf[j] = loadedData[j + startBlockOffset];
        if (j < 4 || j > length - 5) {
            printf("  buf[%d] = '%c'\n", j, buf[j]);
        }
    }
    
    FDT[fd].rwHeadPos += length;
    printf("sfs_read: read %d bytes from file at FDT[%d]\n\n", length, fd);
    return 0;
}

int sfs_fseek(int fd, int loc) {
    printf("sfs_seek: attempting to seek to byte %d of FDT[%d]\n", loc, fd);
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
    printf("sfs_seek: seek complete, FDT[%d].rwHeadPos = %d\n\n", fd, FDT[fd].rwHeadPos);
    return 0;
}
