/*
 * microfs_dir.c — Path resolution, directory operations
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "microfs.h"
#include "microfs_internal.h"

/* =============================================
 * Directory entry helpers (internal, but exposed via microfs_internal.h)
 * ============================================= */

int dir_read_entries(MicroFS *fs, Inode *dir_inode, DirEntry *entries, int *count) {
    *count = 0;
    uint8_t block[BLOCK_SIZE];
    int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);

    for (int b = 0; b < MAX_DIRECT_BLOCKS; b++) {
        if (dir_inode->direct_blocks[b] == 0) break;
        uint32_t abs_block = DATA_START + dir_inode->direct_blocks[b];
        if (mfs_read_block(fs, abs_block, block) != MFS_OK) return MFS_ERR_IO;

        DirEntry *blk_entries = (DirEntry *)block;
        for (int i = 0; i < entries_per_block; i++) {
            if (blk_entries[i].inode_num != 0)
                if (*count < MAX_DIR_ENTRIES * MAX_DIRECT_BLOCKS)
                    entries[(*count)++] = blk_entries[i];
        }
    }
    return MFS_OK;
}

int dir_add_entry(MicroFS *fs, uint32_t dir_inum, uint32_t new_inum, const char *name) {
    Inode dir_inode;
    if (mfs_read_inode(fs, dir_inum, &dir_inode) != MFS_OK) return MFS_ERR_IO;

    uint8_t block[BLOCK_SIZE];
    int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);

    for (int b = 0; b < MAX_DIRECT_BLOCKS; b++) {
        if (dir_inode.direct_blocks[b] == 0) {
            int nb = mfs_alloc_block(fs);
            if (nb < 0) return MFS_ERR_FULL;
            dir_inode.direct_blocks[b] = (uint32_t)nb;
            memset(block, 0, BLOCK_SIZE);
            mfs_write_block(fs, DATA_START + nb, block);
        }

        uint32_t abs_block = DATA_START + dir_inode.direct_blocks[b];
        mfs_read_block(fs, abs_block, block);
        DirEntry *entries = (DirEntry *)block;

        for (int i = 0; i < entries_per_block; i++) {
            if (entries[i].inode_num == 0) {
                entries[i].inode_num = new_inum;
                strncpy(entries[i].name, name, MAX_FILENAME - 1);
                entries[i].name[MAX_FILENAME - 1] = '\0';
                mfs_write_block(fs, abs_block, block);

                dir_inode.size += sizeof(DirEntry);
                dir_inode.modified_at = time(NULL);
                dir_inode.checksum = mfs_checksum(&dir_inode,
                    sizeof(dir_inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
                mfs_write_inode(fs, dir_inum, &dir_inode);
                return MFS_OK;
            }
        }
    }
    return MFS_ERR_FULL;
}

int dir_remove_entry(MicroFS *fs, uint32_t dir_inum, const char *name) {
    Inode dir_inode;
    if (mfs_read_inode(fs, dir_inum, &dir_inode) != MFS_OK) return MFS_ERR_IO;

    uint8_t block[BLOCK_SIZE];
    int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);

    for (int b = 0; b < MAX_DIRECT_BLOCKS; b++) {
        if (dir_inode.direct_blocks[b] == 0) break;
        uint32_t abs_block = DATA_START + dir_inode.direct_blocks[b];
        mfs_read_block(fs, abs_block, block);
        DirEntry *entries = (DirEntry *)block;

        for (int i = 0; i < entries_per_block; i++) {
            if (entries[i].inode_num != 0 && strcmp(entries[i].name, name) == 0) {
                memset(&entries[i], 0, sizeof(DirEntry));
                mfs_write_block(fs, abs_block, block);

                if (dir_inode.size >= sizeof(DirEntry))
                    dir_inode.size -= sizeof(DirEntry);
                dir_inode.modified_at = time(NULL);
                dir_inode.checksum = mfs_checksum(&dir_inode,
                    sizeof(dir_inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
                mfs_write_inode(fs, dir_inum, &dir_inode);
                return MFS_OK;
            }
        }
    }
    return MFS_ERR_NOTFOUND;
}

int dir_lookup(MicroFS *fs, uint32_t dir_inum, const char *name, uint32_t *result_inum) {
    Inode dir_inode;
    if (mfs_read_inode(fs, dir_inum, &dir_inode) != MFS_OK) return MFS_ERR_IO;
    if (dir_inode.type != INODE_DIR) return MFS_ERR_NOTDIR;

    uint8_t block[BLOCK_SIZE];
    int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);

    for (int b = 0; b < MAX_DIRECT_BLOCKS; b++) {
        if (dir_inode.direct_blocks[b] == 0) break;
        uint32_t abs_block = DATA_START + dir_inode.direct_blocks[b];
        if (mfs_read_block(fs, abs_block, block) != MFS_OK) return MFS_ERR_IO;

        DirEntry *entries = (DirEntry *)block;
        for (int i = 0; i < entries_per_block; i++) {
            if (entries[i].inode_num != 0 && strcmp(entries[i].name, name) == 0) {
                *result_inum = entries[i].inode_num;
                return MFS_OK;
            }
        }
    }
    return MFS_ERR_NOTFOUND;
}

/* =============================================
 * Symlink resolution
 * ============================================= */
int mfs_resolve_symlink(MicroFS *fs, uint32_t inum, char *resolved, int depth) {
    if (depth > 8) return MFS_ERR_SYMLOOP;
    Inode inode;
    if (mfs_read_inode(fs, inum, &inode) != MFS_OK) return MFS_ERR_IO;
    if (inode.type != INODE_SYMLINK) return MFS_ERR_INVALID;

    PathResult res;
    char target[60];
    strncpy(target, inode.symlink_target, sizeof(target) - 1);
    int ret = mfs_resolve_path(fs, target, &res);
    if (ret != MFS_OK) return ret;

    if (res.inode.type == INODE_SYMLINK)
        return mfs_resolve_symlink(fs, res.inode_num, resolved, depth + 1);

    strncpy(resolved, target, 511);
    return MFS_OK;
}

/* =============================================
 * Path resolution
 * ============================================= */
int mfs_resolve_path(MicroFS *fs, const char *path, PathResult *res) {
    if (!path || path[0] == '\0') return MFS_ERR_INVALID;

    char buf[512];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint32_t cur_inum = (buf[0] == '/') ? fs->sb.root_inode : fs->cwd_inode;

    res->parent_inode = cur_inum;
    res->inode_num    = cur_inum;
    res->basename[0]  = '\0';

    char *saveptr;
    char *token = strtok_r(buf, "/", &saveptr);
    if (!token) {
        /* Path is just "/" */
        return mfs_read_inode(fs, cur_inum, &res->inode);
    }

    while (token) {
        char *next = strtok_r(NULL, "/", &saveptr);

        if (strcmp(token, ".") == 0) { token = next; continue; }

        uint32_t found_inum;
        int ret = dir_lookup(fs, cur_inum, token, &found_inum);

        if (ret == MFS_ERR_NOTFOUND) {
            res->parent_inode = cur_inum;
            strncpy(res->basename, token, MAX_FILENAME - 1);
            res->inode_num = 0;
            if (!next) return MFS_ERR_NOTFOUND;
            return MFS_ERR_NOTFOUND;
        }
        if (ret != MFS_OK) return ret;

        Inode found_inode;
        if (mfs_read_inode(fs, found_inum, &found_inode) != MFS_OK) return MFS_ERR_IO;

        /* Follow symlinks for intermediate components */
        if (found_inode.type == INODE_SYMLINK && next) {
            char resolved[512];
            int sr = mfs_resolve_symlink(fs, found_inum, resolved, 0);
            if (sr != MFS_OK) return sr;
            PathResult symres;
            sr = mfs_resolve_path(fs, resolved, &symres);
            if (sr != MFS_OK) return sr;
            cur_inum = symres.inode_num;
        } else {
            res->parent_inode = cur_inum;
            cur_inum = found_inum;
            strncpy(res->basename, token, MAX_FILENAME - 1);
        }
        token = next;
    }

    res->inode_num = cur_inum;
    if (mfs_read_inode(fs, cur_inum, &res->inode) != MFS_OK) return MFS_ERR_IO;
    return MFS_OK;
}

/* =============================================
 * mkdir
 * ============================================= */
int mfs_mkdir(MicroFS *fs, const char *path, uint16_t perms) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret == MFS_OK) return MFS_ERR_EXISTS;
    if (ret != MFS_ERR_NOTFOUND) return ret;
    if (res.basename[0] == '\0') return MFS_ERR_INVALID;

    /* Check that the parent exists and is valid.
     * Also verify the basename matches the LAST component of path --
     * if it doesn't, there are missing intermediate directories. */
    {
        /* Extract the last component of path */
        const char *last_slash = strrchr(path, '/');
        const char *last_component = last_slash ? last_slash + 1 : path;
        if (strcmp(res.basename, last_component) != 0) {
            return MFS_ERR_NOTFOUND;  /* intermediate directory missing */
        }
    }

    int new_inum = mfs_alloc_inode(fs, INODE_DIR, perms);
    if (new_inum < 0) return new_inum;

    int new_blk = mfs_alloc_block(fs);
    if (new_blk < 0) { mfs_free_inode(fs, new_inum); return MFS_ERR_FULL; }

    uint8_t block[BLOCK_SIZE];
    memset(block, 0, BLOCK_SIZE);
    DirEntry *entries = (DirEntry *)block;
    entries[0].inode_num = new_inum;
    strncpy(entries[0].name, ".", MAX_FILENAME - 1);
    entries[1].inode_num = res.parent_inode;
    strncpy(entries[1].name, "..", MAX_FILENAME - 1);
    mfs_write_block(fs, DATA_START + new_blk, block);

    Inode new_inode;
    mfs_read_inode(fs, new_inum, &new_inode);
    new_inode.type = INODE_DIR;
    new_inode.permissions = perms;
    new_inode.hard_links = 2;
    new_inode.size = 2 * sizeof(DirEntry);
    new_inode.direct_blocks[0] = (uint32_t)new_blk;
    new_inode.checksum = mfs_checksum(&new_inode,
        sizeof(new_inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
    mfs_write_inode(fs, new_inum, &new_inode);

    ret = dir_add_entry(fs, res.parent_inode, new_inum, res.basename);
    if (ret != MFS_OK) {
        mfs_free_inode(fs, new_inum);
        mfs_free_block(fs, new_blk);
        return ret;
    }

    Inode parent;
    mfs_read_inode(fs, res.parent_inode, &parent);
    parent.hard_links++;
    mfs_write_inode(fs, res.parent_inode, &parent);
    return MFS_OK;
}

/* =============================================
 * rmdir
 * ============================================= */
int mfs_rmdir(MicroFS *fs, const char *path) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) return ret;
    if (res.inode.type != INODE_DIR) return MFS_ERR_NOTDIR;
    if (res.inode_num == fs->sb.root_inode) return MFS_ERR_PERM;

    DirEntry entries[MAX_DIR_ENTRIES * MAX_DIRECT_BLOCKS];
    int count = 0;
    dir_read_entries(fs, &res.inode, entries, &count);
    int real_entries = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, ".") != 0 && strcmp(entries[i].name, "..") != 0)
            real_entries++;
    }
    if (real_entries > 0) return MFS_ERR_NOTEMPTY;

    for (int b = 0; b < MAX_DIRECT_BLOCKS; b++)
        if (res.inode.direct_blocks[b]) mfs_free_block(fs, res.inode.direct_blocks[b]);

    dir_remove_entry(fs, res.parent_inode, res.basename);

    Inode parent;
    mfs_read_inode(fs, res.parent_inode, &parent);
    if (parent.hard_links > 1) parent.hard_links--;
    mfs_write_inode(fs, res.parent_inode, &parent);

    mfs_free_inode(fs, res.inode_num);
    return MFS_OK;
}

/* =============================================
 * readdir
 * ============================================= */
int mfs_readdir(MicroFS *fs, const char *path, DirEntry *entries, int *count) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) return ret;
    if (res.inode.type == INODE_SYMLINK) {
        char resolved[512];
        mfs_resolve_symlink(fs, res.inode_num, resolved, 0);
        return mfs_readdir(fs, resolved, entries, count);
    }
    if (res.inode.type != INODE_DIR) return MFS_ERR_NOTDIR;
    return dir_read_entries(fs, &res.inode, entries, count);
}

/* =============================================
 * chdir
 * ============================================= */
int mfs_chdir(MicroFS *fs, const char *path) {
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) return ret;
    if (res.inode.type != INODE_DIR) return MFS_ERR_NOTDIR;

    fs->cwd_inode = res.inode_num;

    if (path[0] == '/') {
        strncpy(fs->cwd_path, path, sizeof(fs->cwd_path) - 1);
        /* Normalize trailing slash */
        int len = strlen(fs->cwd_path);
        if (len > 1 && fs->cwd_path[len-1] == '/')
            fs->cwd_path[len-1] = '\0';
    } else if (strcmp(path, "..") == 0) {
        char *slash = strrchr(fs->cwd_path, '/');
        if (slash && slash != fs->cwd_path) *slash = '\0';
        else strncpy(fs->cwd_path, "/", sizeof(fs->cwd_path) - 1);
    } else if (strcmp(path, ".") != 0) {
        if (strcmp(fs->cwd_path, "/") != 0)
            strncat(fs->cwd_path, "/", sizeof(fs->cwd_path) - strlen(fs->cwd_path) - 1);
        strncat(fs->cwd_path, path, sizeof(fs->cwd_path) - strlen(fs->cwd_path) - 1);
    }
    return MFS_OK;
}
