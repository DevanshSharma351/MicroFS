/*
 * microfs_file.c — File operations: create, read, write, unlink, stat, links
 *
 * NOTE: dir_add_entry and dir_remove_entry are declared here as extern
 *       and defined as static in microfs_dir.c — to avoid this coupling
 *       we expose them via internal linkage through a shared internal header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "microfs.h"
#include "microfs_internal.h"

/* File handle table */
#define MAX_OPEN_FILES 16
FileHandle fh_table[MAX_OPEN_FILES];
static int fh_initialized = 0;

static void init_fh_table(void) {
    if (!fh_initialized) {
        memset(fh_table, 0, sizeof(fh_table));
        fh_initialized = 1;
    }
}

/* =============================================
 * mfs_create — create a new regular file
 * ============================================= */
int mfs_create(MicroFS *fs, const char *path, uint16_t perms) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);

    if (ret == MFS_OK) return MFS_ERR_EXISTS;
    if (ret != MFS_ERR_NOTFOUND) return ret;
    if (res.basename[0] == '\0') return MFS_ERR_INVALID;

    int new_inum = mfs_alloc_inode(fs, INODE_FILE, perms);
    if (new_inum < 0) return new_inum;

    Inode inode;
    mfs_read_inode(fs, new_inum, &inode);
    inode.type        = INODE_FILE;
    inode.permissions = perms;
    inode.hard_links  = 1;
    inode.size        = 0;
    inode.checksum    = mfs_checksum(&inode,
        sizeof(inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
    mfs_write_inode(fs, new_inum, &inode);

    ret = dir_add_entry(fs, res.parent_inode, new_inum, res.basename);
    if (ret != MFS_OK) { mfs_free_inode(fs, new_inum); return ret; }
    return MFS_OK;
}

/* =============================================
 * mfs_open — open a file, return handle index
 * ============================================= */
int mfs_open(MicroFS *fs, const char *path, int flags) {
    init_fh_table();
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) return ret;
    if (res.inode.type == INODE_DIR) return MFS_ERR_NOTFILE;

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fh_table[i].valid) {
            fh_table[i].inode_num = res.inode_num;
            fh_table[i].offset    = 0;
            fh_table[i].flags     = flags;
            fh_table[i].valid     = 1;
            res.inode.accessed_at = time(NULL);
            mfs_write_inode(fs, res.inode_num, &res.inode);
            return i;
        }
    }
    return MFS_ERR_FULL;
}

/* =============================================
 * mfs_read — read bytes from open file
 * ============================================= */
int mfs_read(MicroFS *fs, FileHandle *fh, void *buf, uint32_t len) {
    if (!fh || !fh->valid) return MFS_ERR_INVALID;
    Inode inode;
    if (mfs_read_inode(fs, fh->inode_num, &inode) != MFS_OK) return MFS_ERR_IO;
    if (fh->offset >= inode.size) return 0;
    if (fh->offset + len > inode.size) len = inode.size - fh->offset;

    uint8_t *out = (uint8_t *)buf;
    uint32_t bytes_read = 0;

    while (bytes_read < len) {
        uint32_t block_index  = (fh->offset + bytes_read) / BLOCK_SIZE;
        uint32_t block_offset = (fh->offset + bytes_read) % BLOCK_SIZE;
        uint32_t can_read     = BLOCK_SIZE - block_offset;
        if (can_read > len - bytes_read) can_read = len - bytes_read;

        int blk = mfs_inode_get_block(fs, &inode, block_index, 0);
        if (blk < 0) break;

        uint8_t block[BLOCK_SIZE];
        mfs_read_block(fs, DATA_START + blk, block);
        memcpy(out + bytes_read, block + block_offset, can_read);
        bytes_read += can_read;
    }

    fh->offset += bytes_read;
    return (int)bytes_read;
}

/* =============================================
 * mfs_write — write bytes to open file
 * ============================================= */
int mfs_write(MicroFS *fs, FileHandle *fh, const void *buf, uint32_t len) {
    if (!fh || !fh->valid) return MFS_ERR_INVALID;
    Inode inode;
    if (mfs_read_inode(fs, fh->inode_num, &inode) != MFS_OK) return MFS_ERR_IO;

    const uint8_t *in = (const uint8_t *)buf;
    uint32_t bytes_written = 0;

    while (bytes_written < len) {
        uint32_t block_index  = (fh->offset + bytes_written) / BLOCK_SIZE;
        uint32_t block_offset = (fh->offset + bytes_written) % BLOCK_SIZE;
        uint32_t can_write    = BLOCK_SIZE - block_offset;
        if (can_write > len - bytes_written) can_write = len - bytes_written;

        if (block_index >= MAX_DIRECT_BLOCKS) break;

        int blk = mfs_inode_get_block(fs, &inode, block_index, 1);
        if (blk < 0) break;

        uint8_t block[BLOCK_SIZE];
        mfs_read_block(fs, DATA_START + blk, block);
        memcpy(block + block_offset, in + bytes_written, can_write);
        mfs_write_block(fs, DATA_START + blk, block);
        bytes_written += can_write;
    }

    fh->offset += bytes_written;
    if (fh->offset > inode.size) inode.size = fh->offset;
    inode.modified_at = time(NULL);
    inode.checksum = mfs_checksum(&inode,
        sizeof(inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
    mfs_write_inode(fs, fh->inode_num, &inode);
    return (int)bytes_written;
}

/* =============================================
 * mfs_stat, mfs_truncate, mfs_unlink, mfs_link, mfs_symlink, mfs_readlink
 * ============================================= */
int mfs_stat(MicroFS *fs, const char *path, Inode *out) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) return ret;
    *out = res.inode;
    return MFS_OK;
}

int mfs_truncate(MicroFS *fs, const char *path) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) return ret;
    if (res.inode.type != INODE_FILE) return MFS_ERR_NOTFILE;
    for (int b = 0; b < MAX_DIRECT_BLOCKS; b++) {
        if (res.inode.direct_blocks[b]) {
            mfs_free_block(fs, res.inode.direct_blocks[b]);
            res.inode.direct_blocks[b] = 0;
        }
    }
    res.inode.size = 0;
    res.inode.modified_at = time(NULL);
    res.inode.checksum = mfs_checksum(&res.inode,
        sizeof(res.inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
    mfs_write_inode(fs, res.inode_num, &res.inode);
    return MFS_OK;
}

int mfs_unlink(MicroFS *fs, const char *path) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) return ret;
    if (res.inode.type == INODE_DIR) return MFS_ERR_NOTFILE;

    dir_remove_entry(fs, res.parent_inode, res.basename);

    res.inode.hard_links--;
    if (res.inode.hard_links == 0) {
        for (int b = 0; b < MAX_DIRECT_BLOCKS; b++) {
            if (res.inode.direct_blocks[b])
                mfs_free_block(fs, res.inode.direct_blocks[b]);
        }
        mfs_free_inode(fs, res.inode_num);
    } else {
        res.inode.checksum = mfs_checksum(&res.inode,
            sizeof(res.inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
        mfs_write_inode(fs, res.inode_num, &res.inode);
    }
    return MFS_OK;
}

int mfs_link(MicroFS *fs, const char *src, const char *dst) {
    PathResult src_res;
    int ret = mfs_resolve_path(fs, src, &src_res);
    if (ret != MFS_OK) return ret;
    if (src_res.inode.type == INODE_DIR) return MFS_ERR_PERM;

    PathResult dst_res;
    ret = mfs_resolve_path(fs, dst, &dst_res);
    if (ret == MFS_OK) return MFS_ERR_EXISTS;
    if (ret != MFS_ERR_NOTFOUND) return ret;

    dir_add_entry(fs, dst_res.parent_inode, src_res.inode_num, dst_res.basename);
    src_res.inode.hard_links++;
    src_res.inode.checksum = mfs_checksum(&src_res.inode,
        sizeof(src_res.inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
    mfs_write_inode(fs, src_res.inode_num, &src_res.inode);
    return MFS_OK;
}

int mfs_symlink(MicroFS *fs, const char *target, const char *linkpath) {
    PathResult res;
    int ret = mfs_resolve_path(fs, linkpath, &res);
    if (ret == MFS_OK) return MFS_ERR_EXISTS;
    if (ret != MFS_ERR_NOTFOUND) return ret;
    if (res.basename[0] == '\0' || strlen(target) >= 60) return MFS_ERR_INVALID;

    int new_inum = mfs_alloc_inode(fs, INODE_SYMLINK, 0777);
    if (new_inum < 0) return new_inum;

    Inode inode;
    mfs_read_inode(fs, new_inum, &inode);
    inode.type       = INODE_SYMLINK;
    inode.hard_links = 1;
    inode.size       = strlen(target);
    strncpy(inode.symlink_target, target, sizeof(inode.symlink_target) - 1);
    inode.checksum = mfs_checksum(&inode,
        sizeof(inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
    mfs_write_inode(fs, new_inum, &inode);

    ret = dir_add_entry(fs, res.parent_inode, new_inum, res.basename);
    if (ret != MFS_OK) { mfs_free_inode(fs, new_inum); return ret; }
    return MFS_OK;
}

int mfs_readlink(MicroFS *fs, const char *path, char *buf, uint32_t bufsz) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) return ret;
    if (res.inode.type != INODE_SYMLINK) return MFS_ERR_INVALID;
    strncpy(buf, res.inode.symlink_target, bufsz - 1);
    buf[bufsz - 1] = '\0';
    return MFS_OK;
}

void mfs_close(FileHandle *fh) { if (fh) fh->valid = 0; }

FileHandle *mfs_get_handle(int fd) {
    if (!fh_initialized) init_fh_table();
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fh_table[fd].valid) return NULL;
    return &fh_table[fd];
}
