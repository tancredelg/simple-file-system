# Simple File System
## Design
### Overview

This is a very simple file system, with no file permissions, users, groups, or multiple directories. It only has a root
directory, which points to 'leaf' files.

On a basic level, the file system is organized into the following layout:

| Super Block | &nbsp;&nbsp;&nbsp;&nbsp; Inode Table &nbsp;&nbsp;&nbsp;&nbsp; | &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; Data Blocks &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; | Free Bitmap |
|:-----------:|:-------------------------------------------------------------:|:-------------------------------------------------------------------------------------------------------------------------------------:|:-----------:|
|      1      |                               M                               |                                                                   N                                                                   |      L      |

The block size is 1024B (bytes), or 1KB (Kilobyte).

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

In total, each directory has a size of `1 + 21 + 2 = 24b = 3B`. This means that for a file system with 1000 files, the
number of data blocks needed for their directory entries is `(1000 * 3) / 1024 = ~2.92 = 3` blocks.

#### Data Blocks

These are the blocks that store data for either the files, or the root directory as explained in the previous section.
In this file system, all blocks - including data blocks - have a size of 8192b (bits) = 1024B (bytes) = 1KB (Kilobyte).

#### Free Bitmap

The free bitmap for this file system uses bits (not bytes) to track the availability/occupancy of every data block. 
This means that `1024 * 8 = 8192` data blocks can be tracked in a 1-block free bitmap, and another `(1024 - n) * 8` blocks for
every additional block allocated to the bitmap, where the last n bytes of each block of the bitmap (except the last)
are reserved for pointing to the next block of the bitmap.

### Allocation of Disk Space

The size of each part of the file system (defined in the **Overview** section) is calculated in proportion to each
other, based on the total number of blocks allocated to the file system.

Let:

- Q = Total number of blocks for the file system
- 1 = Number of blocks for the super block
- M = Number of blocks for the inode table
- N = Number of blocks for data
- L = Number of blocks for the free bitmap
- S = Average file size in KB/blocks (1 block = 1KB)

The requirement is that `Q = 1 + M + N + L`.

M, N and L need to be calculated from known values of Q and S. In other words, each part is allocated a different
proportion of the total number of blocks available, Q, in order to get an optimal maximum number of files with an
average file size of S. 

For this file system, the assumption is made that the average file size will be **4KB**. So `S = 4`.

#### Calculation of N from L

The number of data blocks (N) - and consequently the number of inodes & inode blocks (M) - depend not only on Q, but
also on the size of the free bitmap.

As explained previously, when `L = 1` the free bitmap can track a maximum of `1024 * 8` data blocks, and when `L > 1`
the free bitmap can track additional `(1024 - n) * 8` for each additional bitmap block after the first.

Using a 4B pointer to link blocks of the free bitmap (`n = 4`), the maximum number of data blocks (N) is
calculated as follows:

```c
N = (1024 * 8) + ((L - 1) * (1024 - 4) * 8)

N = 32 + (8160 * L)
```

#### Calculation of M from N and L

As discussed above, `S = 4` - each file requires 4 data blocks, on average. So, the aim is for each inode to point to 4
data blocks. For simplicity, the root directory is treated like any other file, contributing to the average file size.
Using this and an inode size of 56B (as defined in the inode section earlier), the optimal number of inode
table blocks (M) is calculated as follows:

```c
M = ((N / S) * 56) / 1024
  = (7 * N) / (128 * S)
// Substitute S = 4
M = (7 * N) / 512
```

This can then be written in terms of L using the equation for N derived in the previous section:

```c
M = (7 * (32 + (8160 * L))) / 512
// Substitute S = 4
M = (7 + (1785 * L)) / 16
```

_Notice how these equations derived for M depend on S, the assumed average file size. So, if the average file size
ends up differing to the assumption, the maximum number of files possible will also be different, and the number of
inodes (files) that the file system can support may be too little (not enough blocks for M) or too much (too many
blocks for M)._

#### Calculation of L from Q

L is calculated from Q. Since L determines the size of the bitmap, N depends on L, and since M is determined by the
value of N, it also depends on L. 

Since N and M can be expressed in terms of L, we can simplify the main equation to be in terms Q and L:

```c
Q = 1 + M + N + L
  = 1 + ((7 + (1785 * L)) / 16) + (32 + (L * 8160)) + L

Q = 33 + (8161 * L) + (7 + (1785 * L)) / 16
```

and since Q is supplied, solve for L, and - since L the minimum whole number
of blocks - round the result up with ceil():

```c
L = ceil(((16 * Q) - 535) / 132361)
```

#### Calculation of M and N from Q

With a value for L using the equation above, M and N could be calculated using the equations derived earlier. However,
this has 2 problems:
1. The equation for N in terms of L from earlier calculates the maximum N for a given L, without respect to Q. This 
could result in overallocation of blocks to N. 
2. Since both M and N need to be whole numbers, they need to be rounded. This could cause situations where the total 
does not sum to Q. 

Instead, N is calculated in terms of L and Q by substituting the equation for M in terms of N into the main equation
and solving for N (avoids the first problem), and then M is calculated as the blocks left over after N and L are
calculated (avoids the second problem)

For N, the result should still be rounded in order to get a whole number of blocks, and is calculated as follows:

```c
Q = 1 + M + N + L
  = 1 + ((7 * N) / 512) + N + L
// Solve for N
N = Q - 1 - M - L
  = Q - 1 - ((7 * N) / 512) - L
  = (512 * (Q - 1 - L)) / 519
// Round up
N = ceil((512 * (Q - 1 - L)) / 519)
```

Now, with Q, L and N known, calculating M is trivial:
```c
M = Q - 1 - N - L
```

#### Restriction on Q
The minimum value of Q that would work with the equation used to calculate N can be found by substituting the minimum
values for M and L - which are both 1 - and the equation for N in terms of Q, into the main equation:
```c
Q >= 1 + 1 + N + 1
  = 1 + 1 + ((512 * (Q - 1 - 1)) / 519) + 1
 
Q >= ceil(76.143)
Q >= 77
```