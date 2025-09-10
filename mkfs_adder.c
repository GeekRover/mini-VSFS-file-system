#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum;          // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t  type;
    char     name[58];
    uint8_t  checksum; // XOR of bytes 0..62
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
static void superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, sizeof(superblock_t) - 4);
    sb->checksum = s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER INODE ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; 
    memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER DIRENT ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

int get_bit(uint8_t *bitmap, int bit_num) {
    int byte_idx = bit_num / 8;
    int bit_idx = bit_num % 8;
    return (bitmap[byte_idx] >> bit_idx) & 1;
}

void set_bit(uint8_t *bitmap, int bit_num) {
    int byte_idx = bit_num / 8;
    int bit_idx = bit_num % 8;
    bitmap[byte_idx] |= (1 << bit_idx);
}

// Find the first free inode (0-indexed)
int find_free_inode(uint8_t *inode_bitmap, int max_inodes) {
    for (int i = 1; i < max_inodes; i++) { // Start from 1, as 0 (inode #1) is root
        if (!get_bit(inode_bitmap, i)) {
            return i;
        }
    }
    return -1;
}

void print_usage() {
    printf("Usage: mkfs_adder --input <input_image> --output <output_image> --file <filename>\n");
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    char *input_name = NULL;
    char *output_name = NULL;
    char *file_name = NULL;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_name = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_name = argv[++i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file_name = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage();
            return 1;
        }
    }
    
    // Validate arguments
    if (!input_name || !output_name || !file_name) {
        fprintf(stderr, "Error: Missing required arguments\n");
        print_usage();
        return 1;
    }
    
    // Check if file to add exists and is a regular file
    struct stat file_stat;
    if (stat(file_name, &file_stat) != 0) {
        fprintf(stderr, "Error: File '%s' not found: %s\n", file_name, strerror(errno));
        return 1;
    }
    
    if (!S_ISREG(file_stat.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a regular file\n", file_name);
        return 1;
    }
    
    // Open input image
    FILE *input_fp = fopen(input_name, "rb");
    if (!input_fp) {
        fprintf(stderr, "Error: Cannot open input image '%s': %s\n", input_name, strerror(errno));
        return 1;
    }
    
    // Read superblock
    superblock_t sb;
    fseek(input_fp, 0, SEEK_SET);
    if (fread(&sb, sizeof(sb), 1, input_fp) != 1) {
        fprintf(stderr, "Error reading superblock\n");
        fclose(input_fp);
        return 1;
    }
    
    // Validate magic number
    if (sb.magic != 0x4D565346) {
        fprintf(stderr, "Error: Invalid file system magic number\n");
        fclose(input_fp);
        return 1;
    }
    
    // Read inode bitmap
    uint8_t inode_bitmap[BS];
    fseek(input_fp, sb.inode_bitmap_start * BS, SEEK_SET);
    if (fread(inode_bitmap, BS, 1, input_fp) != 1) {
        fprintf(stderr, "Error reading inode bitmap\n");
        fclose(input_fp);
        return 1;
    }
    
    // Read data bitmap
    uint8_t data_bitmap[BS];
    fseek(input_fp, sb.data_bitmap_start * BS, SEEK_SET);
    if (fread(data_bitmap, BS, 1, input_fp) != 1) {
        fprintf(stderr, "Error reading data bitmap\n");
        fclose(input_fp);
        return 1;
    }
    
    // Find free inode (0-indexed)
    int free_inode_idx = find_free_inode(inode_bitmap, sb.inode_count);
    if (free_inode_idx == -1) {
        fprintf(stderr, "Error: No free inodes available\n");
        fclose(input_fp);
        return 1;
    }
    
    // Calculate blocks needed for the file
    uint64_t file_size = file_stat.st_size;
    uint32_t blocks_needed = (file_size + BS - 1) / BS;
    
    if (blocks_needed > DIRECT_MAX) {
        fprintf(stderr, "Error: File too large (exceeds %d direct blocks)\n", DIRECT_MAX);
        fclose(input_fp);
        return 1;
    }
    
    // Find free data blocks
    uint32_t data_blocks_indices[DIRECT_MAX];
    int blocks_found = 0;
    for (uint32_t i = 0; i < sb.data_region_blocks && blocks_found < blocks_needed; i++) {
        if (!get_bit(data_bitmap, i)) {
            data_blocks_indices[blocks_found++] = i;
        }
    }
    
    if (blocks_found < blocks_needed) {
        fprintf(stderr, "Error: Not enough free data blocks (%d needed, %d available)\n", 
                blocks_needed, blocks_found);
        fclose(input_fp);
        return 1;
    }
    
    // Read entire inode table into memory
    uint8_t *inode_table = malloc(sb.inode_table_blocks * BS);
    if (!inode_table) {
        fprintf(stderr, "Error: Memory allocation for inode table failed\n");
        fclose(input_fp);
        return 1;
    }
    fseek(input_fp, sb.inode_table_start * BS, SEEK_SET);
    if (fread(inode_table, sb.inode_table_blocks * BS, 1, input_fp) != 1) {
        fprintf(stderr, "Error reading inode table\n");
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    
    // Read entire data region into memory
    uint8_t *data_region = malloc(sb.data_region_blocks * BS);
    if (!data_region) {
        fprintf(stderr, "Error: Memory allocation for data region failed\n");
        free(inode_table);
        fclose(input_fp);
        return 1;
    }
    fseek(input_fp, sb.data_region_start * BS, SEEK_SET);
    if (fread(data_region, sb.data_region_blocks * BS, 1, input_fp) != 1) {
        fprintf(stderr, "Error reading data region\n");
        free(inode_table);
        free(data_region);
        fclose(input_fp);
        return 1;
    }
    
    fclose(input_fp); // Done with the input file
    
    // --- Start modifying the file system in memory ---

    // 1. Mark inode and data blocks as used in bitmaps
    set_bit(inode_bitmap, free_inode_idx);
    for (int i = 0; i < blocks_needed; i++) {
        set_bit(data_bitmap, data_blocks_indices[i]);
    }

    // 2. Create and add new inode to inode table
    time_t now = time(NULL);
    inode_t new_inode = {0};
    new_inode.mode = 0100000; // Regular file mode (octal)
    new_inode.links = 1;
    new_inode.uid = 0;
    new_inode.gid = 0;
    new_inode.size_bytes = file_size;
    new_inode.atime = new_inode.mtime = new_inode.ctime = (uint64_t)now;

    for (int i = 0; i < blocks_needed; i++) {
        new_inode.direct[i] = sb.data_region_start + data_blocks_indices[i];
    }
    inode_crc_finalize(&new_inode);
    memcpy(inode_table + (free_inode_idx * INODE_SIZE), &new_inode, INODE_SIZE);

    // 3. Copy file content into the data region
    FILE *file_fp = fopen(file_name, "rb");
    if (!file_fp) {
        fprintf(stderr, "Error: Cannot open file '%s': %s\n", file_name, strerror(errno));
        free(inode_table);
        free(data_region);
        return 1;
    }
    for (int i = 0; i < blocks_needed; i++) {
        uint8_t *dest = data_region + data_blocks_indices[i] * BS;
        fread(dest, 1, BS, file_fp);
    }
    fclose(file_fp);

    // 4. Update root directory
    inode_t *root_inode = (inode_t*)(inode_table + (ROOT_INO - 1) * INODE_SIZE);
    uint32_t root_dir_block_idx = root_inode->direct[0] - sb.data_region_start;
    uint8_t *root_dir_data = data_region + root_dir_block_idx * BS;
    
    // Find an empty entry or append
    int entry_idx = -1;
    for (int i = 0; i < (root_inode->size_bytes / sizeof(dirent64_t)); i++) {
        dirent64_t *entry = (dirent64_t*)(root_dir_data + i * sizeof(dirent64_t));
        if (entry->inode_no == 0) {
            entry_idx = i;
            break;
        }
    }
    if (entry_idx == -1) {
        entry_idx = root_inode->size_bytes / sizeof(dirent64_t);
        root_inode->size_bytes += sizeof(dirent64_t);
    }

    // Create and write the new directory entry
    dirent64_t new_entry = {0};
    new_entry.inode_no = free_inode_idx + 1; // Inodes are 1-indexed
    new_entry.type = 1; // 1 for file
    
    char *base_name = strrchr(file_name, '/');
    base_name = base_name ? base_name + 1 : file_name;
    
    if (strlen(base_name) >= sizeof(new_entry.name)) {
        fprintf(stderr, "Error: Filename too long (max 57 characters)\n");
        free(inode_table);
        free(data_region);
        return 1;
    }
    strcpy(new_entry.name, base_name);
    dirent_checksum_finalize(&new_entry);
    memcpy(root_dir_data + entry_idx * sizeof(dirent64_t), &new_entry, sizeof(dirent64_t));
    
    // 5. Update root inode metadata
    root_inode->links++;
    root_inode->mtime = root_inode->ctime = (uint64_t)now;
    inode_crc_finalize(root_inode);

    // 6. Finalize superblock
    sb.mtime_epoch = (uint64_t)now;
    superblock_crc_finalize(&sb);

    // --- Write everything to the output file ---
    FILE *output_fp = fopen(output_name, "wb");
    if (!output_fp) {
        fprintf(stderr, "Error: Cannot create output image '%s': %s\n", output_name, strerror(errno));
        free(inode_table);
        free(data_region);
        return 1;
    }

    // Write superblock, bitmaps, inode table, and data region
    fwrite(&sb, sizeof(sb), 1, output_fp);
    // Pad the rest of the superblock block with zeros
    uint8_t sb_pad[BS - sizeof(sb)] = {0};
    fwrite(sb_pad, 1, sizeof(sb_pad), output_fp);
    
    fwrite(inode_bitmap, BS, 1, output_fp);
    fwrite(data_bitmap, BS, 1, output_fp);
    fwrite(inode_table, sb.inode_table_blocks * BS, 1, output_fp);
    fwrite(data_region, sb.data_region_blocks * BS, 1, output_fp);

    fclose(output_fp);
    free(inode_table);
    free(data_region);
    
    printf("File '%s' added to file system successfully.\n", base_name);
    printf("Output image written to '%s'.\n", output_name);
    
    return 0;
}

