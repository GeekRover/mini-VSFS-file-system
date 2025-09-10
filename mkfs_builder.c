// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;               // 0x4D565346
    uint32_t version;             // 1
    uint32_t block_size;          // 4096
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;  // 1
    uint64_t inode_bitmap_blocks; // 1
    uint64_t data_bitmap_start;   // 2
    uint64_t data_bitmap_blocks;  // 1
    uint64_t inode_table_start;   // 3
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;          // 1
    uint64_t mtime_epoch;
    uint32_t flags;               // 0
    
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;                 // 0
    uint32_t gid;                 // 0
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];  // 12 direct blocks
    uint32_t reserved_0;          // 0
    uint32_t reserved_1;          // 0
    uint32_t reserved_2;          // 0
    uint32_t proj_id;             // Your group ID
    uint32_t uid16_gid16;         // 0
    uint64_t xattr_ptr;           // 0

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;            // 0 if free
    uint8_t  type;                // 1=file, 2=dir
    char     name[58];
    uint8_t  checksum;            // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; 
    memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

void set_bit(uint8_t *bitmap, int bit_num) {
    int byte_idx = bit_num / 8;
    int bit_idx = bit_num % 8;
    bitmap[byte_idx] |= (1 << bit_idx);
}

void print_usage() {
    printf("Usage: mkfs_builder --image <image_name> --size-kib <180..4096> --inodes <128..512>\n");
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    char *image_name = NULL;
    uint32_t size_kib = 0;
    uint32_t inode_count = 0;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            image_name = argv[++i];
        } else if (strcmp(argv[i], "--size-kib") == 0 && i + 1 < argc) {
            size_kib = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--inodes") == 0 && i + 1 < argc) {
            inode_count = (uint32_t)atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage();
            return 1;
        }
    }
    
    // Validate arguments
    if (!image_name || size_kib == 0 || inode_count == 0) {
        fprintf(stderr, "Error: Missing required arguments\n");
        print_usage();
        return 1;
    }
    
    if (size_kib < 180 || size_kib > 4096 || size_kib % 4 != 0) {
        fprintf(stderr, "Error: size-kib must be between 180-4096 and multiple of 4\n");
        return 1;
    }
    
    if (inode_count < 128 || inode_count > 512) {
        fprintf(stderr, "Error: inodes must be between 128-512\n");
        return 1;
    }
    
    uint64_t total_blocks = (size_kib * 1024) / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS; // Round up
    
    // Check if we have enough blocks
    uint64_t required_blocks = 1 + 1 + 1 + inode_table_blocks + 1; // superblock + inode_bitmap + data_bitmap + inode_table + at least 1 data block
    if (total_blocks < required_blocks) {
        fprintf(stderr, "Error: Not enough blocks for the specified configuration\n");
        return 1;
    }
    
    time_t now = time(NULL);
    
    // Create superblock
    superblock_t sb = {0};
    sb.magic = 0x4D565346;
    sb.version = 1;
    sb.block_size = BS;
    sb.total_blocks = total_blocks;
    sb.inode_count = inode_count;
    sb.inode_bitmap_start = 1;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = 2;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = 3;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_region_start = 3 + inode_table_blocks;
    sb.data_region_blocks = total_blocks - sb.data_region_start;
    sb.root_inode = ROOT_INO;
    sb.mtime_epoch = (uint64_t)now;
    sb.flags = 0;
    
    // Create root inode
    inode_t root_inode = {0};
    root_inode.mode = 040000; // Directory mode (octal)
    root_inode.links = 2;     // . and ..
    root_inode.uid = 0;
    root_inode.gid = 0;
    root_inode.size_bytes = 2 * sizeof(dirent64_t); // . and .. entries
    root_inode.atime = (uint64_t)now;
    root_inode.mtime = (uint64_t)now;
    root_inode.ctime = (uint64_t)now;
    root_inode.direct[0] = (uint32_t)sb.data_region_start; // First data block
    for (int i = 1; i < DIRECT_MAX; i++) {
        root_inode.direct[i] = 0;
    }
    root_inode.reserved_0 = 0;
    root_inode.reserved_1 = 0;
    root_inode.reserved_2 = 0;
    root_inode.proj_id = 0; // Set your group ID here
    root_inode.uid16_gid16 = 0;
    root_inode.xattr_ptr = 0;
    
    // Create root directory entries
    dirent64_t dot_entry = {0};
    dot_entry.inode_no = ROOT_INO;
    dot_entry.type = 2; // directory
    strcpy(dot_entry.name, ".");
    
    dirent64_t dotdot_entry = {0};
    dotdot_entry.inode_no = ROOT_INO;
    dotdot_entry.type = 2; // directory
    strcpy(dotdot_entry.name, "..");
    
    // Finalize checksums
    superblock_crc_finalize(&sb);
    inode_crc_finalize(&root_inode);
    dirent_checksum_finalize(&dot_entry);
    dirent_checksum_finalize(&dotdot_entry);
    
    // Write to file
    FILE *fp = fopen(image_name, "wb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot create image file %s: %s\n", image_name, strerror(errno));
        return 1;
    }
    
    // Write superblock
    uint8_t block[BS] = {0};
    memcpy(block, &sb, sizeof(sb));
    if (fwrite(block, BS, 1, fp) != 1) {
        fprintf(stderr, "Error writing superblock\n");
        fclose(fp);
        return 1;
    }
    
    // Write inode bitmap (mark root inode as used)
    memset(block, 0, BS);
    set_bit(block, 0); // Root inode (1-indexed, so bit 0)
    if (fwrite(block, BS, 1, fp) != 1) {
        fprintf(stderr, "Error writing inode bitmap\n");
        fclose(fp);
        return 1;
    }
    
    // Write data bitmap (mark first data block as used)
    memset(block, 0, BS);
    set_bit(block, 0); // First data block
    if (fwrite(block, BS, 1, fp) != 1) {
        fprintf(stderr, "Error writing data bitmap\n");
        fclose(fp);
        return 1;
    }
    
    // Write inode table
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        memset(block, 0, BS);
        if (i == 0) {
            // First block contains root inode
            memcpy(block, &root_inode, sizeof(root_inode));
        }
        if (fwrite(block, BS, 1, fp) != 1) {
            fprintf(stderr, "Error writing inode table block %lu\n", i);
            fclose(fp);
            return 1;
        }
    }
    
    // Write root directory data block
    memset(block, 0, BS);
    memcpy(block, &dot_entry, sizeof(dot_entry));
    memcpy(block + sizeof(dot_entry), &dotdot_entry, sizeof(dotdot_entry));
    if (fwrite(block, BS, 1, fp) != 1) {
        fprintf(stderr, "Error writing root directory data\n");
        fclose(fp);
        return 1;
    }
    
    // Write remaining data blocks (empty)
    memset(block, 0, BS);
    for (uint64_t i = 1; i < sb.data_region_blocks; i++) {
        if (fwrite(block, BS, 1, fp) != 1) {
            fprintf(stderr, "Error writing data block %lu\n", i);
            fclose(fp);
            return 1;
        }
    }
    
    fclose(fp);
    printf("File system image '%s' created successfully\n", image_name);
    printf("Total blocks: %lu, Inodes: %u\n", total_blocks, inode_count);
    
    return 0;
}
