# 📖 Summary of MiniVSFS Project Steps

This document provides a **high-level, step-by-step overview** of the work completed for the MiniVSFS project.

---

## ✅ Step 1: Understanding the Goal

The primary goal was to build a simplified, **inode-based file system** from scratch by creating two distinct C programs:

- **`mkfs_builder`** → builds a new, empty file system image.  
- **`mkfs_adder`** → adds a file to an existing image.  

---

## ✅ Step 2: Building the File System Creator (`mkfs_builder.c`)

The first program acts like a **disk formatting tool**. Its core responsibilities are:

1. **Parse arguments** → image size and total number of inodes.  
2. **Calculate on-disk layout** → superblock, bitmaps, inode table, data region.  
3. **Initialize core structures** in memory.  
4. **Create the root directory (`/`)**:  
   - Root inode (#1).  
   - Allocate data block for it.  
   - Create `.` and `..` directory entries.  
5. **Write structures sequentially** into a `.img` binary file, creating a valid, empty file system.

---

## ✅ Step 3: Building the File Adder (`mkfs_adder.c`)

The second program modifies an existing file system image. Its workflow is:

1. **Read an existing `.img` file** into memory.  
2. **Parse arguments** → input image, output image, and file to add.  
3. **Find free space**:  
   - Scan inode bitmap for a free inode.  
   - Scan data bitmap for enough free blocks.  
4. **Update metadata**: mark inode and data bits as allocated.  
5. **Create new file entry**:  
   - New inode with size, timestamps, and block pointers.  
   - Add a directory entry to the root directory, linking filename → inode number.  
6. **Copy file data** into allocated data blocks.  
7. **Finalize updates**:  
   - Update root inode (link count, timestamps).  
   - Update superblock timestamp.  
   - Recalculate checksums.  
8. **Write the modified file system** to a new `.img` output file.

---

## ✅ Step 4: Compiling the Programs

Both source files were compiled on **Linux Mint** using `gcc`.

```bash
# Compile the builder
gcc mkfs_builder.c -o mkfs_builder

# Compile the adder
gcc mkfs_adder.c -o mkfs_adder

# Build
./mkfs_builder --image my_fs.img --size-kib 256 --inodes 128

# Add file
./mkfs_adder --input my_fs.img --output my_fs_final.img --file file_38.txt


#🔍 Inspect 
xxd my_fs_final.img | less
