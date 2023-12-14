#include <malloc.h>
#include <sys/types.h>

typedef u_int8_t Byte;
#define B 1024

#define BYTE_OFFSET(b) ((b) / 8)
#define BIT_OFFSET(b)  ((b) % 8)

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

void printByte(Byte *byte) {
    for (int i = 0; i < 8; ++i) {
        printf("%d", getBit(byte, i));
    }
    printf(" '%c'\t", *byte);
}

int main() {
    Byte *block = (Byte *) malloc(B);
    // Fill 1st half of block with placeholder data (lowercase alphas)
    for (int i = 0; i < B / 2; ++i) {
        block[i] = 97 + i % 26;
    }

    int offset;
    Byte *last_byte = block;
    // Seek to last byte written in block
    for (offset = 0; offset < B && *last_byte != 0; ++offset, ++last_byte) {
        printByte(last_byte);
        printf(" ");
        if (offset % 8 == 7)
            printf("\t%d-%d\n", offset - 7, offset);
    }
    for (int i = 0; i < 16; ++i, ++last_byte) {
        printByte(last_byte);
        printf(" ");
        if (i % 8 == 7)
            printf("\t%d-%d\n", offset + i - 7, offset + i);
    }
    printf("\nrw_head_pos = %d\n", offset);
    
    char *buf = "THIS IS A LONGER MESSAGE";
    int length = 25;

    for (int i = offset; i < offset + length; ++i, ++buf) {
        block[i] = *((Byte *) buf);
    }

    printf("rw_head_pos = %d\n", offset + length);
    for (int i = 0; i < B; ++i) {
        printByte(&block[i]);
        printf(" ");
        if (i % 8 == 7)
            printf("\t%d-%d\n", i - 7, i);
    }

    free(block);
}