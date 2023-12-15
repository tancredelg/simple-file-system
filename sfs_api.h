#ifndef SFS_API_H
#define SFS_API_H

#include <sys/types.h>

#define MAX_FILENAME_LENGTH 31

#define BYTE_OFFSET(b) ((b) / 8)
#define BIT_OFFSET(b)  ((b) % 8)

typedef u_char Byte;

void printByte(Byte *byte);

void setBit(Byte *bytes, int n);

void clearBit(Byte *bytes, int n);

int getBit(const Byte *bytes, int n);

int sfs_countFreeDataBlocks(void);

int sfs_getFreeDataBlockAddress(void);

int sfs_getNextFreeFDTPos(int);

int sfs_getNextFreeDirEntry(int);

void mksfs(int);

int sfs_getnextfilename(char*);

int sfs_getfilesize(const char*);

int sfs_fopen(char*);

int sfs_fclose(int);

int sfs_fwrite(int, const char*, int);

int sfs_fread(int, char*, int);

int sfs_fseek(int, int);

int sfs_remove(char*);

#endif
