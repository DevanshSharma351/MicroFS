# MicroFS — A Unix-style Filesystem in C

> Build a working file system from scratch. Store it in a single 4MB virtual disk file.
> Learn how NTFS, ext4, and every OS on earth actually manages your files.

---

## What Is This?

MicroFS is a **fully functional inode-based filesystem** implemented in pure C. It stores everything — files, directories, metadata — inside a single binary `microfs.disk` file. You interact with it through a Unix-like shell.

This project teaches you the real internals of filesystems like ext4 and NTFS: how inodes work, how directories are just files containing name→inode mappings, how crash recovery via journaling works, and how the OS tracks free space with bitmaps.

---

## Features Implemented

| Feature | Status | Notes |
|---|---|---|
| Superblock | ✅ | Disk geometry, magic, checksums |
| Inode table | ✅ | 512 inodes, 192 bytes each |
| Free space bitmap | ✅ | Block & inode bitmaps |
| Regular files | ✅ | Create, read, write, delete |
| Directories | ✅ | With `.` and `..` entries |
| Subdirectories | ✅ | Nested paths |
| Path resolution | ✅ | Absolute, relative, `..` |
| Hard links | ✅ | Reference counting |
| Symbolic links | ✅ | With loop detection (max depth 8) |
| File permissions | ✅ | Unix `rwxrwxrwx` bits |
| Timestamps | ✅ | created, modified, accessed |
| Write-Ahead Journal | ✅ | Crash recovery (WAL) |
| `fsck` | ✅ | Consistency checker + repair |
| Interactive CLI | ✅ | Colorized shell |

---

## Disk Layout

```
Offset (blocks)    Content
───────────────    ─────────────────────────────────────────
Block 0            Superblock  (filesystem metadata)
Blocks 1–32        Journal / Write-Ahead Log (WAL)
Block 33           Inode bitmap  (1 bit per inode)
Blocks 34–37       Block bitmap  (1 bit per data block)
Blocks 38–165      Inode table  (512 × 192 bytes = 98,304 bytes)
Blocks 166–8191    Data blocks  (~4MB usable space)
```

One block = 512 bytes. Total disk = 4MB (8192 blocks).

---

## Key Data Structures

### Superblock (512 bytes, Block 0)
```c
typedef struct {
    uint32_t magic;           // MICROFS_MAGIC = 0x4D494346
    uint32_t version;
    uint32_t total_blocks;    // 8192
    uint32_t total_inodes;    // 512
    uint32_t free_blocks;     // tracked dynamically
    uint32_t free_inodes;
    uint32_t block_size;      // 512
    uint32_t root_inode;      // always 1
    // ... timestamps, journal location, checksum
} Superblock;
```

### Inode (192 bytes)
```c
typedef struct {
    uint32_t magic;               // INODE_MAGIC (sanity check)
    uint8_t  type;                // FILE / DIR / SYMLINK
    uint16_t permissions;         // rwxrwxrwx bits
    uint16_t uid, gid;
    uint16_t hard_links;          // reference count
    uint32_t size;
    time_t   created_at, modified_at, accessed_at;
    uint32_t direct_blocks[12];   // data block indices (max ~6KB)
    char     symlink_target[60];  // for symlinks: target path (inline)
    uint32_t checksum;
} Inode;
```

### Directory Entry (60 bytes)
```c
typedef struct {
    uint32_t inode_num;     // inode number (0 = free slot)
    char     name[56];      // filename (null-terminated)
} DirEntry;
```

A directory is just a file whose data blocks contain an array of `DirEntry` structures. This is exactly how ext2/3/4 works.

---

## How Crash Recovery Works (Write-Ahead Log)

Before any write that could leave the filesystem inconsistent, MicroFS:

1. **Logs** the original data to the journal with a `WRITE` record
2. **Commits** by writing a `COMMIT` record to the journal
3. **Applies** the actual write to disk

On mount after a crash:
- If there's a `COMMIT` record → the write completed, nothing to do
- If there's a `WRITE` but no `COMMIT` → incomplete write, restore the original data from the journal

This is the same principle used by ext4 in `journal` mode and SQLite's WAL.

```
Normal operation:
  [LOG original → journal] → [COMMIT] → [write to disk] → [checkpoint]

After crash (no COMMIT found):
  [restore original from journal] → filesystem back to last good state
```

---

## Building

```bash
# Prerequisites: gcc, make
make          # builds ./build/microfs

make test     # runs all 56 unit tests
make clean    # removes build artifacts
```

---

## Running the Shell

```bash
./build/microfs              # creates microfs.disk on first run
./build/microfs --format     # wipe and reformat disk
./build/microfs myfs.disk    # use a custom disk image
./build/microfs --fsck       # check filesystem and exit
```

---

## Shell Commands

```
File Operations:
  touch <path>              create empty file
  rm <path>                 delete file (decrements hard link count)
  write <path> <text...>    write text to file (overwrites)
  append <path> <text...>   append text to file
  cat <path>                print file contents
  cp <src> <dst>            copy file
  mv <src> <dst>            move/rename (implemented as link + unlink)
  stat <path>               show file metadata (inode, size, permissions, timestamps)
  truncate <path>           set file size to 0

Directory Operations:
  ls [-l] [path]            list directory (long format shows permissions)
  cd <path>                 change directory (supports . and ..)
  pwd                       print working directory
  mkdir <path>              create directory (single level only)
  rmdir <path>              remove directory (must be empty)

Links:
  ln <src> <dst>            create hard link (same inode, different name)
  ln -s <target> <link>     create symbolic link
  readlink <path>           read symlink target

Filesystem:
  df                        disk usage with visual bar
  fsck [-r]                 check consistency; -r to attempt repair
```

---

## Example Session

```
microfs:/$ mkdir docs
OK
microfs:/$ cd docs
microfs:/docs$ write hello.txt Hello, filesystem world!
OK
microfs:/docs$ ls -l
drwxr-xr-x    2    60B      1  ./
drwxr-xr-x    3    60B      1  ../
-rw-r--r--    1    25B      3  hello.txt
microfs:/docs$ stat hello.txt
  File: hello.txt
  Type: regular file
 Inode: 3
  Size: 25 bytes
 Links: 1
Access: -rw-r--r-- (0644)
microfs:/docs$ ln hello.txt hello_link.txt
OK
microfs:/docs$ ln -s /docs/hello.txt /symlink.txt
OK
microfs:/docs$ cat /symlink.txt
Hello, filesystem world!

microfs:/$ df
  Filesystem:   MicroFS v1
  Disk size:    4096 KB (8192 blocks)
  Data blocks:  8026 total, 8 used, 8018 free
  Inodes:       512 total, 4 used, 508 free
  Usage:        [░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 0%
```

---

## Code Structure

```
microfs/
├── include/
│   ├── microfs.h           ← Public API: all structs, constants, function declarations
│   └── microfs_internal.h  ← Internal cross-module function sharing
├── src/
│   ├── main.c              ← Entry point, argument parsing
│   ├── microfs_core.c      ← format, mount, I/O primitives, bitmap, inode allocation
│   ├── microfs_dir.c       ← Path resolution, mkdir, rmdir, readdir, chdir
│   ├── microfs_file.c      ← create, open, read, write, unlink, link, symlink
│   ├── microfs_journal.c   ← Write-Ahead Log + fsck
│   └── microfs_shell.c     ← Interactive CLI with ANSI colors
├── tests/
│   └── test_microfs.c      ← 56-test suite covering all operations
└── Makefile
```

---

## Things to Learn / Extend

This project is designed to teach. Here are natural next steps:

**Beginner extensions:**
- Add `chmod` and `chown` commands (permissions already stored, just need UI)
- Add `find` command (recursive directory traversal)
- Persist file handle position across close/reopen (seek support)

**Intermediate extensions:**
- **Indirect blocks**: currently max file size is ~6KB (12 direct blocks × 512 bytes). Add a 13th "indirect" block that points to 128 more blocks → max ~65KB
- **Directory ordering**: sort `ls` output alphabetically
- **Free space optimization**: track first free block to avoid full bitmap scans

**Advanced extensions:**
- **Block groups**: ext2-style clustering of bitmaps near their data (reduces seek time)
- **B-tree directories**: for large directories with hundreds of entries (like ext4 uses)
- **Compression**: compress data blocks transparently
- **Encryption**: encrypt block data before writing to disk
- **FUSE mount**: mount your `microfs.disk` as a real directory in Linux using FUSE

---

## Why Each Design Decision Was Made

**Single flat binary file as "disk"**: Simplifies I/O — `lseek` + `read`/`write` replicate what real disk drivers do. Every `mfs_read_block` is a `lseek` + `read`, exactly like a block device driver.

**Inode-based design**: Separates names from data. This is why you can rename a file without rewriting its contents, and why hard links work — multiple names pointing to one inode.

**Bitmaps for free space**: O(n/8) scan where n is total blocks. Real filesystems use more sophisticated structures (e2fsck's group descriptors, B-trees) but bitmaps are simple and correct.

**0 as "no block" sentinel**: Block index 0 in `direct_blocks[]` means "not allocated." Block 0 in the data area is permanently reserved so this sentinel is safe.

**Write-Ahead Log**: Without it, a crash mid-write leaves the filesystem in an unknown state. With a WAL, the worst case is re-doing or re-undoing the last incomplete operation.

---

## License

MIT — use this for learning, extend it, break it, rebuild it. That's the point.
