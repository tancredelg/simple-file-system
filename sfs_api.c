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
    u_int16_t rwHeadPos;
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
    n -= 1 + superBlock.inodeTableSize; // Offset from absolute address of the data block at `n`
    bytes[BYTE_OFFSET(n)] |= (1 << BIT_OFFSET(n));
}

void clearBit(Byte *bytes, int n) {
    n -= 1 + superBlock.inodeTableSize; // Offset from absolute address of the data block at `n`
    bytes[BYTE_OFFSET(n)] &= ~(1 << BIT_OFFSET(n));
}

int getBit(const Byte *bytes, int n) {
    n -= 1 + superBlock.inodeTableSize; // Offset from absolute address of the data block at `n` 
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

int sfs_getFreeDataBlockAddress(void) {
    for (int i = 0; i < N; ++i) {
        int n = i + 1 + superBlock.inodeTableSize; // Offset from absolute address of the data block at `i`
        if (getBit(&fbm[BYTE_OFFSET(n)], n) == 0)
            return n;
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

int sfs_getInodeBlockPointers(Inode inode, int *pointers[], int sizeInBlocks) {
    int i = 0;
    for (; i < 12 && i < sizeInBlocks; ++i) {
        *pointers[i] = inode.blockPointers[i];
    }
    if (sizeInBlocks > 12) {
        int indirectBlockPointers[B / sizeof(int)];
        read_blocks(inode.blockPointers[12], 1, indirectBlockPointers);

        for (i = 12; i < sizeInBlocks; ++i) {
            *pointers[i] = indirectBlockPointers[i - 12];
        }
    }
    return i;
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
            rootDirEntries[i].inodeNum = (short) i;
            if (i < 2 || i == DIR_SIZE - 3) {
                printf("  inodeTable[%d].size = %d\n", i, inodeTable[i].size);
                printf("  rootDirEntries[%d] = { .used = %d; .filename = '%s'; .inodeNum = %d; }\n",
                       i, rootDirEntries[i].used, rootDirEntries[i].filename, rootDirEntries[i].inodeNum);
            }
        }
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
        read_blocks(1, superBlock.inodeTableSize, &inodeTable);
        for (int i = 0; i < DIR_SIZE; ++i) {
            if (i < 2 || i > DIR_SIZE - 3) {
                printf("  inodeTable[%d].size = %d\n", i, inodeTable[i].size);
            }
        }

        // Load root directory
        printf("mksfs: loading root directory...");
        // Load directory entries from data blocks pointed to by the root dir inode
        int dirSizeInBlocks = superBlock.rootDir.size / B + (superBlock.rootDir.size % B != 0 ? 1 : 0);
        int *dirBlockPointers = (int *) malloc(dirSizeInBlocks * sizeof(int));
        sfs_getInodeBlockPointers(superBlock.rootDir, &dirBlockPointers, dirSizeInBlocks);
        
        Byte dirBlocksData[dirSizeInBlocks * B];
        Byte *currentBlockData = (Byte *) malloc(B);
        
        for (int i = 0; i < dirSizeInBlocks; ++i) {
            // Read root directory data block i
            read_blocks(dirBlockPointers[i], 1, currentBlockData);
            for (int j = 0; j < B; ++j) { // Copy into all blocks data buffer
                dirBlocksData[(i * B) + j] = currentBlockData[j];
            }
        }
        free(currentBlockData);        
        printf("  data blocks merged into single buffer, copying to root directory...");
        
        for (int i = 0; i < DIR_SIZE; ++i) {
            rootDirEntries[i] = ((DirEntry *) dirBlocksData)[i];
            printf("  rootDirEntries[%d] = { .used = %d; .filename = '%s'; .inodeNum = %d; }\n",
                   i, rootDirEntries[i].used, rootDirEntries[i].filename, rootDirEntries[i].inodeNum);
        }
        
        printf("  root directory fully loaded (%d entries).", DIR_SIZE);
        
        // Load free bitmap
        printf("mksfs: loading free bitmap...\n");
        read_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
        for (int i = 0; i < superBlock.fbmSize * B; ++i) {
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
    printf("sfs_fclose: attempting close file at FDT[%d]\n", fd);
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
    printf("sfs_write: attempting to write '%s' to the file at FDT[%d]\n", buf, fd);
    
    if (length < 1) {
        printf("sfs_write: nothing to write (length < 1)\n\n");
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
    printf("sfs_write: loaded inode for file at FDT[%d]\n", fd);
    
    if (FDT[fd].rwHeadPos > inode.size) {
        fprintf(stderr, "Failed to write to file: the read/write head is beyond the end of the file.\n");
        return -1;
    }
    
    if (FDT[fd].rwHeadPos + length >= MAX_FILE_SIZE) {
        fprintf(stderr, "Failed to write to file: the file will exceed the max file size.\n");
        return -1;
    }
    
    printf("sfs_write: preparing for write...\n");
    // Get start and end bytes/blocks
    int startPos = FDT[fd].rwHeadPos;
    int endPos = startPos + length;

    int startBlock = startPos / B;
    int endBlock = endPos / B;
    
    int startBlockStartPos = startPos % B;
    int endBlockEndPos = endPos % B;
    int lastBlockSpaceLeft = inode.size == 0 ? 0 : B - endBlockEndPos;
    
    int totalBlocksOld = inode.size / B + (inode.size % B != 0 ? 1 : 0);
    int totalBlocksNew = endBlock + 1;
    int blocksToAdd = totalBlocksNew - totalBlocksOld;
    int blocksToChange = totalBlocksOld - startBlock;
    if (totalBlocksOld <= 12 && totalBlocksNew > 12) // Need to include the indirect pointer block itself
        ++blocksToAdd;

    printf("  startPos = %d\n", startPos);
    printf("  endPos = %d\n", endPos);
    printf("  startBlock = %d\n", startBlock);
    printf("  endBlock = %d\n", endBlock);
    printf("  startBlockStartPos = %d\n", startBlockStartPos);
    printf("  endBlockEndPos = %d\n", endBlockEndPos);
    printf("  lastBlockSpaceLeft = %d\n", lastBlockSpaceLeft);
    printf("  totalBlocksOld = %d\n", totalBlocksOld);
    printf("  totalBlocksNew = %d\n", totalBlocksNew);
    printf("  blocksToAdd = %d\n", blocksToAdd);
    
    if (sfs_countFreeDataBlocks() < blocksToAdd) {
        fprintf(stderr, "Failed to write to file: there are not enough free data blocks available.\n");
        return -1;
    }

    // Gather pointers to relevant blocks (existing blocks to change + new blocks to add)
    int blocksToWrite = blocksToChange + blocksToAdd; // Existing blocks + blocksToAdd
    int blocksToWritePointers[blocksToWrite];
    
    if (blocksToChange == 0) { // All blocks in `blocksToWriteToPointers` need to be added
        // Get new blocks
        for (int i = 0; i < blocksToWrite; ++i) {
            int newBlock = sfs_getFreeDataBlockAddress();
            if (newBlock < 0) {
                fprintf(stderr, "Failed to write to file: failed to get free data blocks.\n");
                return -1;
            }
            blocksToWritePointers[i] = newBlock;
        }
    } else { // Need to write in existing blocks
        int *existingBlocksPointers = (int *) malloc(totalBlocksOld * sizeof(int));
        sfs_getInodeBlockPointers(inode, &existingBlocksPointers, totalBlocksOld);
        int i = 0;
        // Get required existing blocks
        for (; i < blocksToChange; ++i) {
            blocksToWritePointers[i] = existingBlocksPointers[startBlock + i];
        }
        // Get new blocks
        for (; i < blocksToWrite; ++i) {
            int newBlock = sfs_getFreeDataBlockAddress();
            if (newBlock < 0) {
                fprintf(stderr, "Failed to write to file: failed to get free data blocks.\n");
                return -1;
            }
            blocksToWritePointers[i] = newBlock;
        }
    }

    printf("sfs_write: data blocks to write to: [ %d", blocksToWritePointers[0]);
    for (int i = 1; i < blocksToWrite; ++i) {
        printf(", %d", blocksToWritePointers[i]);
    }
    printf(" ]\n");

    printf("sfs_write: updating buffer...\n");
    // Create a new buffer that includes potential existing data in the start and end blocks
    char *newBuf = (char *) malloc(blocksToWrite * B);
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
    printf("  newBuf = '%s'\n", newBuf);

    printf("sfs_write: writing buffer to disk...\n");
    // Write the buffer to disk
    // The block needed to write the indirect pointers is at the last index of `blocksToWrite` (if it's needed)
    int blocksToWriteDataTo = blocksToWrite;
    if (totalBlocksOld <= 12 && totalBlocksNew > 12)
        --blocksToWriteDataTo;
    
    for (int i = 0; i < blocksToWriteDataTo; ++i) {
        write_blocks(blocksToWritePointers[i], 1, newBuf + (i * B));
        printf("  wrote block %d of %d at location %d: '%s'\n", i, blocksToWriteDataTo, blocksToWritePointers[i], newBuf + (i * B));
    }
    free(newBuf);

    printf("sfs_write: updating inode data and free bitmap...\n");
    // Update the inode data
    // and update the availability of the newly allocated data blocks on the free bitmap
    if (blocksToAdd > 0) { // New blocks were added, need to update inode pointer data
        int i = 0;
        // Update direct pointers
        for (; i < blocksToWriteDataTo && startBlock + i < 12; ++i) {
            inode.blockPointers[startBlock + i] = blocksToWritePointers[i];
            setBit(fbm, blocksToWritePointers[i]);
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
                setBit(fbm, blocksToWritePointers[i]);
                printf("  indirectBlockPointers[%d] = %d\n", startBlock + i - 12,
                       indirectBlockPointers[startBlock + i - 12]);
            }

            if (totalBlocksOld <= 12) { // Pointer to indirect block needs to be updated
                setBit(fbm, blocksToWritePointers[i]);
                inode.blockPointers[12] = blocksToWritePointers[i];
                printf("  inode.blockPointers[12] = %d\n", inode.blockPointers[12]);
            }
            // Write new/updated indirect pointer block to disk
            write_blocks(inode.blockPointers[12], 1, indirectBlockPointers);
        }
    }
    
    // Update read/write head to `endPos`
    FDT[fd].rwHeadPos = endPos; // = FDT[fd].rwHeadPos + length
    if (endPos > inode.size)
        inode.size = endPos;
    
    printf("  inode.size = %d\n", inode.size);
    printf("  FDT[%d].rwHeadPos = %d\n", fd, FDT[fd].rwHeadPos);
    
    // Update the inode in `inodeTable` and write the updated inode back to disk
    printf("sfs_write: writing updated inode data and updated free bitmap to disk...\n");
    inodeTable[FDT[fd].inodeNum] = inode;    
    write_blocks(1 + FDT[fd].inodeNum, 1, &inode);
    
    // Write the updated free bitmap back to disk
    write_blocks(superBlock.sfsSize - superBlock.fbmSize - 1, superBlock.fbmSize, fbm);
    
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
    
    // Reduce length of read if EOF is closer than FDT[fd].rwHeadPos + length
    if (FDT[fd].rwHeadPos + length >= inode.size) {
        length = inode.size - FDT[fd].rwHeadPos;
        if (length < 0)
            length = 0;
        printf("sfs_read: reading %d bytes (up to EOF)\n", length);
    }
    
    // Get start and end blocks
    int startBlock = FDT[fd].rwHeadPos / B;
    int endBlock = (FDT[fd].rwHeadPos + length) / B;
    
    // Load all the blocks from startBlock to endBlock
    printf("sfs_read: loading %d block(s) to read\n", endBlock - startBlock + 1);
    Byte loadedData[(endBlock - startBlock + 1) * B];
    Byte *currentBlockData = (Byte *) malloc(B);
    int i = 0;
    for (; startBlock + i <= endBlock && i < 12; ++i) { // Read direct blocks
        read_blocks(inode.blockPointers[startBlock + i], 1, currentBlockData);
        for (int j = 0; j < B; ++j) { // Copy currentBlockData to loadedData
            loadedData[i + j] = currentBlockData[j];
        }
        printf("  read 1 block at location %d\n", inode.blockPointers[startBlock + i]);
    }

    if (endBlock > 11) { // Need to read blocks from indirect block
        int indirectBlockPointers[B / sizeof(int)];
        read_blocks(inode.blockPointers[12], 1, indirectBlockPointers);
        for (; startBlock + i <= endBlock; ++i) { // Read indirect blocks
            read_blocks(indirectBlockPointers[startBlock + i - 12], 1, currentBlockData);
            for (int j = 0; j < B; ++j) { // Copy currentBlockData to loadedData
                loadedData[i + j] = currentBlockData[j];
            }
            printf("  read 1 block at location %d\n", indirectBlockPointers[startBlock + i - 12]);
        }
    }
    free(currentBlockData);

    printf("sfs_read: copying read data to buffer\n");
    int startBlockOffset = FDT[fd].rwHeadPos % B;
    for (int j = 0; j < length; ++j) {
        buf[j] = loadedData[startBlockOffset + j];
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
