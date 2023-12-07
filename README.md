# Simple File System
## Design
### Overview

This is a very simple file system, with no file permissions, users, groups, or multiple directories. It only has a root
directory, which points to 'leaf' files.

On a basic level, the file system can be broken down into 4 sections:

| Super Block | &nbsp;&nbsp;&nbsp;&nbsp; Inode Table &nbsp;&nbsp;&nbsp;&nbsp; | &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Data Blocks &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; | Free Bitmap |
|-------------|---------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------|-------------|

The block size is 1024B (bytes).

### Components of the File System

#### Super Block

The super block needs only 1 block, and has the following format:

| Super Block                                                 |
|:------------------------------------------------------------|
| Magic                                                       |
| Block Size                                                  |
| File System Size                                            |
| Inode Table Size                                            |
| Root Directory inode number                                 |
| ... <br/> *the rest of the block is unused space* <br/> ... |

Each field is 4B, so only 5 * 4 = 20B of the 1024B allocated for the super block will be used.

#### Inodes & Inode Table

Inodes have the following format:

| Inode                           |
|---------------------------------|
| Size                            |
| Direct data block pointer 1     |
| Direct data block pointer 2     |
| ...                             |
| Direct data block pointer 12    |
| Single-level indirect pointer 1 |

Each field is 4B, so the total inode size is (1 + 12 + 1) * 4 = 56B.

Unlike the super block, multiple inodes can occupy the same block consecutively, and even be split over 2 blocks, so
the only space wasted is in the last block of the inode table, i.e. the leftover space in the block containing the
last inode.

With 12 direct pointers and 1 single-level indirect pointer, an inode can point to up to 12 + 1 * 1024 = 1036 data
blocks, for a maximum file size of 1036 * 1024 = 1060864B = 1036KB = ~1.01MB.

##### Root Directory

The root directory metadata is stored like any other file metadata, in an inode, found within the inode table. The
data block pointers in it's inode point to data blocks storing directory entries, which have the following format:

| used | &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; filename &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; | &nbsp;&nbsp; inode &nbsp;&nbsp; |
|:----:|:----------------------------------------------------------------------------------------------------------------------------------------------:|:-------------------------------:|
|  1   |                                                                       21                                                                       |                2                |

In total, each directory has a size of 1 + 21 + 2 = 24b = 3B. This means that for a file system with 1000 files, the number of data
blocks needed for their directory entries is (1000 * 3) / 1024 = ~2.92 = 3 blocks.

#### Data Blocks

These are the blocks that store data for either the files, or the root directory as explained in the previous section.
Each block can store 8192b (bits) = 1024B (bytes) = 1KB (Kilobyte).

#### Free Bitmap

The free bitmap for this file system uses bits (not bytes) to track the availability/occupancy of every data block. 
This means that 8 * 1024 = 8192 data blocks can be tracked in a 1-block free bitmap, and another (1024 - n) blocks for
every additional block allocated to the bitmap, where the last n bytes of each block of the bitmap (except the last)
are reserved for pointing to the next block of the bitmap.

Since this is a simple and consequently, small, file system, it is designed to support no more than 8192 data blocks,
so allocating a single block for the free bitmap is enough.

### Allocation of Disk Space

Q = 1 + M + N + L

