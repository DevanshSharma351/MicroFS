/*
 * microfs_core.c — Format, mount/unmount, superblock and bitmap I/O
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "microfs.h"

/* =============================================
 * Checksum utility
 * ============================================= */
uint32_t mfs_checksum(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t csum = 0xDEADBEEF;
    for (size_t i = 0; i < len; i++) {
        csum ^= ((uint32_t)p[i] << (8 * (i % 4)));
        csum = (csum << 3) | (csum >> 29);  /* rotate left 3 */
    }
    return csum;
}

/* =============================================
 * Error handling
 * ============================================= */
const char *mfs_strerror(int err) {
    switch (err) {
        case MFS_OK:           return "Success";
        case MFS_ERR_FULL:     return "No space left on device";
        case MFS_ERR_EXISTS:   return "File exists";
        case MFS_ERR_NOTFOUND: return "No such file or directory";
        case MFS_ERR_NOTDIR:   return "Not a directory";
        case MFS_ERR_NOTFILE:  return "Is a directory";
        case MFS_ERR_PERM:     return "Permission denied";
        case MFS_ERR_IO:       return "I/O error";
        case MFS_ERR_CORRUPT:  return "Filesystem corrupted";
        case MFS_ERR_NOTEMPTY: return "Directory not empty";
        case MFS_ERR_INVALID:  return "Invalid argument";
        case MFS_ERR_SYMLOOP:  return "Too many symbolic links";
        default:               return "Unknown error";
    }
}

void mfs_perror(int err) {
    fprintf(stderr, "mfs error: %s\n", mfs_strerror(err));
}

/* =============================================
 * Format — write a fresh filesystem to disk
 * ============================================= */
int mfs_format(const char *disk_path) {
    int fd = open(disk_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return MFS_ERR_IO; }

    /* Allocate and zero the entire 4MB disk */
    uint8_t zero_block[BLOCK_SIZE];
    memset(zero_block, 0, BLOCK_SIZE);
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        if (write(fd, zero_block, BLOCK_SIZE) != BLOCK_SIZE) {
            perror("write"); close(fd); return MFS_ERR_IO;
        }
    }

    /* --- Write superblock --- */
    Superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic            = MICROFS_MAGIC;
    sb.version          = SUPERBLOCK_VERSION;
    sb.total_blocks     = TOTAL_BLOCKS;
    sb.total_inodes     = MAX_INODES;
    sb.free_blocks      = DATA_BLOCKS - 2;  /* -2: block 0 (reserved) + block 1 (root dir) */
    sb.free_inodes      = MAX_INODES - 1;   /* -1 for root inode */
    sb.block_size       = BLOCK_SIZE;
    sb.inode_table_start = INODE_TABLE_START;
    sb.data_start       = DATA_START;
    sb.root_inode       = 1;                /* inode 0 reserved, root = 1 */
    sb.journal_start    = JOURNAL_START;
    sb.journal_size     = JOURNAL_BLOCKS;
    sb.created_at       = time(NULL);
    sb.last_mounted     = time(NULL);
    sb.mount_count      = 0;
    sb.checksum         = 0;
    sb.checksum         = mfs_checksum(&sb, sizeof(sb) - sizeof(uint8_t[436]) - sizeof(uint32_t));

    lseek(fd, (off_t)SUPERBLOCK_BLOCK * BLOCK_SIZE, SEEK_SET);
    write(fd, &sb, sizeof(sb));

    /* --- Inode bitmap: mark inode 0 (reserved) and 1 (root) as used --- */
    uint8_t inode_bmap[BLOCK_SIZE];
    memset(inode_bmap, 0, BLOCK_SIZE);
    inode_bmap[0] |= 0x03;  /* bits 0 and 1 = inodes 0 and 1 */
    lseek(fd, (off_t)INODE_BITMAP_BLOCK * BLOCK_SIZE, SEEK_SET);
    write(fd, inode_bmap, BLOCK_SIZE);

    /* --- Block bitmap: mark blocks 0 AND 1 as used.
     * Block 0 is permanently reserved (sentinel: direct_blocks=0 means "no block").
     * Block 1 is root directory's data block. --- */
    uint8_t block_bmap[BLOCK_BITMAP_BLOCKS * BLOCK_SIZE];
    memset(block_bmap, 0, sizeof(block_bmap));
    block_bmap[0] |= 0x03;  /* bits 0 and 1 = blocks 0 (reserved) and 1 (root dir) */
    lseek(fd, (off_t)BLOCK_BITMAP_START * BLOCK_SIZE, SEEK_SET);
    write(fd, block_bmap, sizeof(block_bmap));

    /* --- Root inode (inode 1) --- */
    Inode root;
    memset(&root, 0, sizeof(root));
    root.magic       = INODE_MAGIC;
    root.type        = INODE_DIR;
    root.permissions = PERM_DEFAULT_DIR;
    root.uid         = 0;
    root.gid         = 0;
    root.hard_links  = 2;       /* "." and ".." */
    root.size        = 2 * sizeof(DirEntry);
    root.created_at  = time(NULL);
    root.modified_at = time(NULL);
    root.accessed_at = time(NULL);
    root.direct_blocks[0] = 1;  /* data block 1 (0 = reserved/unallocated sentinel) */
    root.checksum    = mfs_checksum(&root, sizeof(root) - sizeof(uint32_t) - sizeof(uint8_t[30]));

    /* inode 1 lives at: inode_table_start*BLOCK_SIZE + 1*sizeof(Inode) */
    off_t root_inode_off = (off_t)INODE_TABLE_START * BLOCK_SIZE + 1 * sizeof(Inode);
    lseek(fd, root_inode_off, SEEK_SET);
    write(fd, &root, sizeof(root));

    /* --- Root directory block (data block 1) --- */
    uint8_t dir_block[BLOCK_SIZE];
    memset(dir_block, 0, BLOCK_SIZE);
    DirEntry *entries = (DirEntry *)dir_block;

    /* "." entry */
    entries[0].inode_num = 1;
    strncpy(entries[0].name, ".", MAX_FILENAME - 1);

    /* ".." entry */
    entries[1].inode_num = 1;  /* root's parent is itself */
    strncpy(entries[1].name, "..", MAX_FILENAME - 1);

    lseek(fd, (off_t)(DATA_START + 1) * BLOCK_SIZE, SEEK_SET);
    write(fd, dir_block, BLOCK_SIZE);

    close(fd);
    printf("MicroFS: formatted disk '%s' (%d blocks, %d KB)\n",
           disk_path, TOTAL_BLOCKS, (TOTAL_BLOCKS * BLOCK_SIZE) / 1024);
    return MFS_OK;
}

/* =============================================
 * Mount / Unmount
 * ============================================= */
int mfs_mount(MicroFS *fs, const char *disk_path) {
    memset(fs, 0, sizeof(MicroFS));

    fs->disk_fd = open(disk_path, O_RDWR);
    if (fs->disk_fd < 0) { perror("open"); return MFS_ERR_IO; }

    int ret;
    if ((ret = mfs_read_superblock(fs)) != MFS_OK) {
        close(fs->disk_fd); return ret;
    }
    if ((ret = mfs_read_bitmaps(fs)) != MFS_OK) {
        close(fs->disk_fd); return ret;
    }

    /* Attempt journal recovery if needed */
    mfs_journal_recover(fs);

    fs->cwd_inode = fs->sb.root_inode;
    strncpy(fs->cwd_path, "/", sizeof(fs->cwd_path) - 1);
    fs->journal_txn_id = 1;

    /* Update mount stats */
    fs->sb.last_mounted = time(NULL);
    fs->sb.mount_count++;
    mfs_write_superblock(fs);

    return MFS_OK;
}

void mfs_unmount(MicroFS *fs) {
    if (fs->disk_fd >= 0) {
        mfs_write_superblock(fs);
        mfs_write_bitmaps(fs);
        close(fs->disk_fd);
        fs->disk_fd = -1;
    }
}

/* =============================================
 * Superblock I/O
 * ============================================= */
int mfs_read_superblock(MicroFS *fs) {
    lseek(fs->disk_fd, (off_t)SUPERBLOCK_BLOCK * BLOCK_SIZE, SEEK_SET);
    if (read(fs->disk_fd, &fs->sb, sizeof(fs->sb)) != sizeof(fs->sb))
        return MFS_ERR_IO;

    if (fs->sb.magic != MICROFS_MAGIC) {
        fprintf(stderr, "mfs: bad magic (0x%08X), not a MicroFS disk\n", fs->sb.magic);
        return MFS_ERR_CORRUPT;
    }

    uint32_t stored = fs->sb.checksum;
    fs->sb.checksum = 0;
    uint32_t computed = mfs_checksum(&fs->sb, offsetof(Superblock, checksum));
    fs->sb.checksum = stored;
    if (computed != stored) {
        fprintf(stderr, "mfs: superblock checksum mismatch (stored=0x%08X computed=0x%08X)\n",
                stored, computed);
        return MFS_ERR_CORRUPT;
    }
    return MFS_OK;
}

int mfs_write_superblock(MicroFS *fs) {
    fs->sb.checksum = 0;
    fs->sb.checksum = mfs_checksum(&fs->sb,
        sizeof(fs->sb) - sizeof(uint8_t[436]) - sizeof(uint32_t));

    lseek(fs->disk_fd, (off_t)SUPERBLOCK_BLOCK * BLOCK_SIZE, SEEK_SET);
    if (write(fs->disk_fd, &fs->sb, sizeof(fs->sb)) != sizeof(fs->sb))
        return MFS_ERR_IO;
    return MFS_OK;
}

/* =============================================
 * Bitmap I/O
 * ============================================= */
int mfs_read_bitmaps(MicroFS *fs) {
    /* Inode bitmap */
    lseek(fs->disk_fd, (off_t)INODE_BITMAP_BLOCK * BLOCK_SIZE, SEEK_SET);
    read(fs->disk_fd, fs->inode_bitmap, sizeof(fs->inode_bitmap));

    /* Block bitmap */
    lseek(fs->disk_fd, (off_t)BLOCK_BITMAP_START * BLOCK_SIZE, SEEK_SET);
    read(fs->disk_fd, fs->block_bitmap, sizeof(fs->block_bitmap));
    return MFS_OK;
}

int mfs_write_bitmaps(MicroFS *fs) {
    lseek(fs->disk_fd, (off_t)INODE_BITMAP_BLOCK * BLOCK_SIZE, SEEK_SET);
    write(fs->disk_fd, fs->inode_bitmap, sizeof(fs->inode_bitmap));

    lseek(fs->disk_fd, (off_t)BLOCK_BITMAP_START * BLOCK_SIZE, SEEK_SET);
    write(fs->disk_fd, fs->block_bitmap, sizeof(fs->block_bitmap));
    return MFS_OK;
}

/* =============================================
 * Block-level I/O
 * ============================================= */
int mfs_read_block(MicroFS *fs, uint32_t block_num, void *buf) {
    if (block_num >= TOTAL_BLOCKS) return MFS_ERR_INVALID;
    off_t off = (off_t)block_num * BLOCK_SIZE;
    lseek(fs->disk_fd, off, SEEK_SET);
    if (read(fs->disk_fd, buf, BLOCK_SIZE) != BLOCK_SIZE) return MFS_ERR_IO;
    return MFS_OK;
}

int mfs_write_block(MicroFS *fs, uint32_t block_num, const void *buf) {
    if (block_num >= TOTAL_BLOCKS) return MFS_ERR_INVALID;
    off_t off = (off_t)block_num * BLOCK_SIZE;
    lseek(fs->disk_fd, off, SEEK_SET);
    if (write(fs->disk_fd, buf, BLOCK_SIZE) != BLOCK_SIZE) return MFS_ERR_IO;
    return MFS_OK;
}

/* =============================================
 * Inode I/O
 * ============================================= */
int mfs_read_inode(MicroFS *fs, uint32_t inum, Inode *out) 
{
    if (inum == 0 || inum >= MAX_INODES)
    {
        return MFS_ERR_INVALID;
    }
    off_t off = (off_t)INODE_TABLE_START * BLOCK_SIZE + (off_t)inum * sizeof(Inode);
    lseek(fs->disk_fd, off, SEEK_SET);
    if (read(fs->disk_fd, out, sizeof(Inode)) != sizeof(Inode))
    {
        return MFS_ERR_IO;
    }
    if (out->magic != INODE_MAGIC)
    {
        return MFS_ERR_CORRUPT;
    }
    return MFS_OK;
}

int mfs_write_inode(MicroFS *fs, uint32_t inum, const Inode *in) {
    if (inum == 0 || inum >= MAX_INODES)
    {
        return MFS_ERR_INVALID;
    }
    off_t off = (off_t)INODE_TABLE_START * BLOCK_SIZE + (off_t)inum * sizeof(Inode);
    lseek(fs->disk_fd, off, SEEK_SET);
    if (write(fs->disk_fd, in, sizeof(Inode)) != sizeof(Inode))
    {
        return MFS_ERR_IO;
    }
    return MFS_OK;
}

/* =============================================
 * Inode allocation
 * ============================================= */
int mfs_alloc_inode(MicroFS *fs, uint8_t type, uint16_t perms) {
    if (fs->sb.free_inodes == 0) return MFS_ERR_FULL;

    /* Scan inode bitmap for first free bit (skip inode 0) */
    for (int i = 1; i < MAX_INODES; i++) {
        int byte = i / 8, bit = i % 8;
        if (!(fs->inode_bitmap[byte] & (1 << bit))) {
            fs->inode_bitmap[byte] |= (1 << bit);
            fs->sb.free_inodes--;

            Inode inode;
            memset(&inode, 0, sizeof(inode));
            inode.magic       = INODE_MAGIC;
            inode.type        = type;
            inode.permissions = perms;
            inode.hard_links  = 0;
            inode.created_at  = time(NULL);
            inode.modified_at = time(NULL);
            inode.accessed_at = time(NULL);
            inode.checksum    = mfs_checksum(&inode,
                sizeof(inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));

            mfs_write_inode(fs, i, &inode);
            mfs_write_bitmaps(fs);
            mfs_write_superblock(fs);
            return i;
        }
    }
    return MFS_ERR_FULL;
}

int mfs_free_inode(MicroFS *fs, uint32_t inum) {
    if (inum == 0 || inum >= MAX_INODES) return MFS_ERR_INVALID;
    int byte = inum / 8, bit = inum % 8;
    if (!(fs->inode_bitmap[byte] & (1 << bit))) return MFS_ERR_INVALID;

    fs->inode_bitmap[byte] &= ~(1 << bit);
    fs->sb.free_inodes++;

    /* Zero out the inode on disk */
    uint8_t zero[sizeof(Inode)];
    memset(zero, 0, sizeof(zero));
    off_t off = (off_t)INODE_TABLE_START * BLOCK_SIZE + (off_t)inum * sizeof(Inode);
    lseek(fs->disk_fd, off, SEEK_SET);
    write(fs->disk_fd, zero, sizeof(Inode));

    mfs_write_bitmaps(fs);
    mfs_write_superblock(fs);
    return MFS_OK;
}

/* =============================================
 * Block allocation
 * ============================================= */
int mfs_alloc_block(MicroFS *fs) {
    if (fs->sb.free_blocks == 0) return MFS_ERR_FULL;

    for (int i = 0; i < (int)DATA_BLOCKS; i++) {
        int byte = i / 8, bit = i % 8;
        if (!(fs->block_bitmap[byte] & (1 << bit))) {
            fs->block_bitmap[byte] |= (1 << bit);
            fs->sb.free_blocks--;

            /* Zero the block */
            uint8_t zero[BLOCK_SIZE];
            memset(zero, 0, BLOCK_SIZE);
            mfs_write_block(fs, DATA_START + i, zero);

            mfs_write_bitmaps(fs);
            mfs_write_superblock(fs);
            return i;  /* returns offset from DATA_START */
        }
    }
    return MFS_ERR_FULL;
}

int mfs_free_block(MicroFS *fs, uint32_t block_idx) {
    if (block_idx >= DATA_BLOCKS) return MFS_ERR_INVALID;
    int byte = block_idx / 8, bit = block_idx % 8;
    fs->block_bitmap[byte] &= ~(1 << bit);
    fs->sb.free_blocks++;
    mfs_write_bitmaps(fs);
    mfs_write_superblock(fs);
    return MFS_OK;
}

/* =============================================
 * Get or allocate a block for inode at block_index
 * ============================================= */
int mfs_inode_get_block(MicroFS *fs, Inode *inode, uint32_t block_index, int allocate) {
    if (block_index >= MAX_DIRECT_BLOCKS) return MFS_ERR_FULL;

    if (inode->direct_blocks[block_index] == 0 && !allocate)
        return MFS_ERR_NOTFOUND;

    if (inode->direct_blocks[block_index] == 0) {
        int new_blk = mfs_alloc_block(fs);
        if (new_blk < 0) return new_blk;
        inode->direct_blocks[block_index] = (uint32_t)new_blk;
    }

    return (int)inode->direct_blocks[block_index];
}

/* =============================================
 * Utility: format permissions as "drwxr-xr-x"
 * ============================================= */
void mfs_perm_string(uint16_t perms, uint8_t type, char *out) {
    /* type character */
    switch (type) {
        case INODE_DIR:     out[0] = 'd'; break;
        case INODE_SYMLINK: out[0] = 'l'; break;
        default:            out[0] = '-'; break;
    }
    out[1] = (perms & PERM_OWNER_R) ? 'r' : '-';
    out[2] = (perms & PERM_OWNER_W) ? 'w' : '-';
    out[3] = (perms & PERM_OWNER_X) ? 'x' : '-';
    out[4] = (perms & PERM_GROUP_R) ? 'r' : '-';
    out[5] = (perms & PERM_GROUP_W) ? 'w' : '-';
    out[6] = (perms & PERM_GROUP_X) ? 'x' : '-';
    out[7] = (perms & PERM_OTHER_R) ? 'r' : '-';
    out[8] = (perms & PERM_OTHER_W) ? 'w' : '-';
    out[9] = (perms & PERM_OTHER_X) ? 'x' : '-';
    out[10] = '\0';
}

void mfs_format_size(uint32_t bytes, char *out) {
    if (bytes < 1024)
        snprintf(out, 16, "%uB", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, 16, "%uK", bytes / 1024);
    else
        snprintf(out, 16, "%uM", bytes / (1024 * 1024));
}
