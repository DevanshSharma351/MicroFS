#ifndef MICROFS_H
#define MICROFS_H

#include <stdint.h>
#include <time.h>

/* =========================================================
 * MicroFS — A Unix-style inode-based filesystem in a file
 * =========================================================
 *
 * Disk Layout (4MB virtual disk):
 * +-----------------+
 * | Superblock      |  Block 0         (1 block  = 512 bytes)
 * +-----------------+
 * | Journal         |  Blocks 1-32     (32 blocks = 16KB WAL)
 * +-----------------+
 * | Inode Bitmap    |  Block 33        (tracks free inodes)
 * +-----------------+
 * | Block Bitmap    |  Blocks 34-37    (tracks free data blocks)
 * +-----------------+
 * | Inode Table     |  Blocks 38-165   (128 blocks = 512 inodes)
 * +-----------------+
 * | Data Blocks     |  Blocks 166-8191 (remaining ~4MB)
 * +-----------------+
 */

/* ----------- Disk geometry ----------- */
#define BLOCK_SIZE          512
#define TOTAL_BLOCKS        8192           /* 4MB disk */
#define MAX_INODES          512
#define MAX_FILENAME        56             /* bytes, including null terminator */
#define MAX_DIRECT_BLOCKS   12
#define MAX_FILE_SIZE       (MAX_DIRECT_BLOCKS * BLOCK_SIZE)  /* ~6KB direct only */

/* ----------- Layout constants ----------- */
#define SUPERBLOCK_BLOCK    0
#define JOURNAL_START       1
#define JOURNAL_BLOCKS      32
#define INODE_BITMAP_BLOCK  33
#define BLOCK_BITMAP_START  34
#define BLOCK_BITMAP_BLOCKS 4
#define INODE_TABLE_START   38
#define INODE_TABLE_BLOCKS  128
#define DATA_START          (INODE_TABLE_START + INODE_TABLE_BLOCKS)  /* block 166 */
#define DATA_BLOCKS         (TOTAL_BLOCKS - DATA_START)

/* ----------- Magic numbers ----------- */
#define MICROFS_MAGIC       0x4D494346   /* "MICF" */
#define JOURNAL_MAGIC       0x4A4F5552   /* "JOUR" */
#define INODE_MAGIC         0xD34DB33F
#define SUPERBLOCK_VERSION  1

/* ----------- Inode types ----------- */
#define INODE_FREE          0x00
#define INODE_FILE          0x01
#define INODE_DIR           0x02
#define INODE_SYMLINK       0x03

/* ----------- Permission bits (Unix-style) ----------- */
#define PERM_OWNER_R        0400
#define PERM_OWNER_W        0200
#define PERM_OWNER_X        0100
#define PERM_GROUP_R        0040
#define PERM_GROUP_W        0020
#define PERM_GROUP_X        0010
#define PERM_OTHER_R        0004
#define PERM_OTHER_W        0002
#define PERM_OTHER_X        0001
#define PERM_DEFAULT_FILE   0644
#define PERM_DEFAULT_DIR    0755

/* ----------- Directory entry constants ----------- */
#define MAX_DIR_ENTRIES     16    /* per directory block */

/* ----------- Journal operation types ----------- */
#define JRNL_OP_BEGIN       0x01
#define JRNL_OP_WRITE       0x02
#define JRNL_OP_COMMIT      0x03
#define JRNL_OP_CHECKPOINT  0x04

/* ----------- Error codes ----------- */
#define MFS_OK              0
#define MFS_ERR_FULL       -1
#define MFS_ERR_EXISTS     -2
#define MFS_ERR_NOTFOUND   -3
#define MFS_ERR_NOTDIR     -4
#define MFS_ERR_NOTFILE    -5
#define MFS_ERR_PERM       -6
#define MFS_ERR_IO         -7
#define MFS_ERR_CORRUPT    -8
#define MFS_ERR_NOTEMPTY   -9
#define MFS_ERR_INVALID    -10
#define MFS_ERR_SYMLOOP    -11

/* =============================================
 * Core On-Disk Structures
 * ============================================= */

/* Superblock — describes the entire filesystem */
typedef struct __attribute__((packed)) {
    uint32_t magic;               /* MICROFS_MAGIC */
    uint32_t version;             /* filesystem version */
    uint32_t total_blocks;        /* total disk blocks */
    uint32_t total_inodes;        /* max inodes */
    uint32_t free_blocks;         /* current free data blocks */
    uint32_t free_inodes;         /* current free inodes */
    uint32_t block_size;          /* bytes per block */
    uint32_t inode_table_start;   /* first block of inode table */
    uint32_t data_start;          /* first data block */
    uint32_t root_inode;          /* inode number of root directory */
    uint32_t journal_start;       /* first journal block */
    uint32_t journal_size;        /* journal blocks */
    time_t   created_at;          /* filesystem creation time */
    time_t   last_mounted;        /* last mount time */
    uint32_t mount_count;         /* how many times mounted */
    uint32_t checksum;            /* simple XOR checksum of above fields */
    uint8_t  padding[436];        /* pad to 512 bytes */
} Superblock;

/* Inode — describes a single file/directory/symlink */
typedef struct __attribute__((packed)) {
    uint32_t magic;                          /* INODE_MAGIC */
    uint8_t  type;                           /* INODE_FILE / INODE_DIR / INODE_SYMLINK */
    uint16_t permissions;                    /* Unix-style rwxrwxrwx */
    uint16_t uid;                            /* owner user ID */
    uint16_t gid;                            /* owner group ID */
    uint16_t hard_links;                     /* number of hard links */
    uint32_t size;                           /* file size in bytes */
    time_t   created_at;
    time_t   modified_at;
    time_t   accessed_at;
    uint32_t direct_blocks[MAX_DIRECT_BLOCKS]; /* data block numbers (0 = unused) */
    char     symlink_target[60];             /* for symlinks: target path */
    uint32_t checksum;                       /* XOR of inode fields */
    uint8_t  padding[30];                    /* pad to 192 bytes */
} Inode;

/* Directory entry — one file/dir inside a directory */
typedef struct __attribute__((packed)) {
    uint32_t inode_num;           /* inode number (0 = free slot) */
    char     name[MAX_FILENAME];  /* filename (null-terminated) */
} DirEntry;                       /* 60 bytes */

/* Journal record — one write operation being logged */
typedef struct __attribute__((packed)) {
    uint32_t magic;               /* JOURNAL_MAGIC */
    uint8_t  op;                  /* JRNL_OP_* */
    uint32_t transaction_id;
    uint32_t target_block;        /* which disk block this applies to */
    uint32_t data_block;          /* which journal block holds the data */
    uint32_t checksum;
    uint8_t  padding[491];        /* pad to 512 bytes */
} JournalRecord;

/* =============================================
 * In-Memory Runtime Structures
 * ============================================= */

/* Open file handle */
typedef struct {
    uint32_t inode_num;
    uint32_t offset;          /* current read/write position */
    int      flags;           /* O_RDONLY, O_RDWR, etc. */
    int      valid;           /* is this slot in use? */
} FileHandle;

/* Main filesystem context — kept in memory while mounted */
typedef struct {
    int       disk_fd;           /* file descriptor for the disk image */
    Superblock sb;               /* cached superblock */
    uint8_t   inode_bitmap[MAX_INODES / 8];   /* 1 bit per inode */
    uint8_t   block_bitmap[DATA_BLOCKS / 8];   /* 1 bit per data block */
    uint32_t  cwd_inode;         /* current working directory inode */
    char      cwd_path[512];     /* current path string (for display) */
    uint32_t  journal_txn_id;    /* next journal transaction ID */
} MicroFS;

/* Result of path resolution */
typedef struct {
    uint32_t inode_num;
    Inode    inode;
    uint32_t parent_inode;
    char     basename[MAX_FILENAME];
} PathResult;

/* =============================================
 * Function Declarations
 * ============================================= */

/* --- Core init --- */
int  mfs_format(const char *disk_path);
int  mfs_mount(MicroFS *fs, const char *disk_path);
void mfs_unmount(MicroFS *fs);

/* --- Superblock & bitmap I/O --- */
int  mfs_read_superblock(MicroFS *fs);
int  mfs_write_superblock(MicroFS *fs);
int  mfs_read_bitmaps(MicroFS *fs);
int  mfs_write_bitmaps(MicroFS *fs);

/* --- Inode operations --- */
int  mfs_read_inode(MicroFS *fs, uint32_t inum, Inode *out);
int  mfs_write_inode(MicroFS *fs, uint32_t inum, const Inode *in);
int  mfs_alloc_inode(MicroFS *fs, uint8_t type, uint16_t perms);
int  mfs_free_inode(MicroFS *fs, uint32_t inum);

/* --- Block operations --- */
int  mfs_alloc_block(MicroFS *fs);
int  mfs_free_block(MicroFS *fs, uint32_t block_num);
int  mfs_read_block(MicroFS *fs, uint32_t block_num, void *buf);
int  mfs_write_block(MicroFS *fs, uint32_t block_num, const void *buf);

/* --- File operations --- */
int  mfs_create(MicroFS *fs, const char *path, uint16_t perms);
int  mfs_open(MicroFS *fs, const char *path, int flags);
int  mfs_read(MicroFS *fs, FileHandle *fh, void *buf, uint32_t len);
int  mfs_write(MicroFS *fs, FileHandle *fh, const void *buf, uint32_t len);
int  mfs_unlink(MicroFS *fs, const char *path);
int  mfs_stat(MicroFS *fs, const char *path, Inode *out);
int  mfs_truncate(MicroFS *fs, const char *path);
int  mfs_link(MicroFS *fs, const char *src, const char *dst);
int  mfs_symlink(MicroFS *fs, const char *target, const char *linkpath);
int  mfs_readlink(MicroFS *fs, const char *path, char *buf, uint32_t bufsz);

/* --- Directory operations --- */
int  mfs_mkdir(MicroFS *fs, const char *path, uint16_t perms);
int  mfs_rmdir(MicroFS *fs, const char *path);
int  mfs_rmdir_recursive(MicroFS *fs, const char *path);
int  mfs_readdir(MicroFS *fs, const char *path, DirEntry *entries, int *count);
int  mfs_chdir(MicroFS *fs, const char *path);

/* --- Path resolution --- */
int  mfs_resolve_path(MicroFS *fs, const char *path, PathResult *res);
int  mfs_resolve_symlink(MicroFS *fs, uint32_t inum, char *resolved, int depth);

/* --- Journal (WAL) --- */
int  mfs_journal_begin(MicroFS *fs);
int  mfs_journal_log_block(MicroFS *fs, uint32_t target_block, const void *data);
int  mfs_journal_commit(MicroFS *fs);
int  mfs_journal_recover(MicroFS *fs);

/* --- Filesystem check --- */
int  mfs_fsck(MicroFS *fs, int repair);

/* --- Utilities --- */
uint32_t mfs_checksum(const void *data, size_t len);
void     mfs_perror(int err);
const char *mfs_strerror(int err);
void     mfs_perm_string(uint16_t perms, uint8_t type, char *out);
void     mfs_format_size(uint32_t bytes, char *out);

/* --- Block allocation from inode --- */
int  mfs_inode_get_block(MicroFS *fs, Inode *inode, uint32_t block_index, int allocate);

#endif /* MICROFS_H */
