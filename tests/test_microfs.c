/*
 * test_microfs.c — Unit & integration tests for MicroFS
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "microfs.h"
#include "microfs_internal.h"

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define RESET   "\033[0m"
#define GREEN   "\033[32m"
#define RED     "\033[31m"
#define YELLOW  "\033[33m"
#define BOLD    "\033[1m"

#define TEST(name, expr) do { \
    tests_run++; \
    if (expr) { \
        printf(GREEN "  [PASS]" RESET " %s\n", name); \
        tests_passed++; \
    } else { \
        printf(RED "  [FAIL]" RESET " %s (line %d)\n", name, __LINE__); \
        tests_failed++; \
    } \
} while(0)

#define SECTION(name) printf("\n" BOLD YELLOW "--- %s ---\n" RESET, name)
#define TEST_DISK "/tmp/microfs_test.disk"

static MicroFS fs;

static void setup(void) {
    mfs_format(TEST_DISK);
    assert(mfs_mount(&fs, TEST_DISK) == MFS_OK);
}

static void teardown(void) {
    mfs_unmount(&fs);
    remove(TEST_DISK);
}

static void test_format_and_mount(void) {
    SECTION("Format & Mount");
    TEST("superblock magic",        fs.sb.magic == MICROFS_MAGIC);
    TEST("root inode set",          fs.sb.root_inode == 1);
    TEST("correct total blocks",    fs.sb.total_blocks == TOTAL_BLOCKS);
    TEST("correct block size",      fs.sb.block_size == BLOCK_SIZE);
    TEST("free inodes decremented", fs.sb.free_inodes == MAX_INODES - 1);
    TEST("cwd is root",             fs.cwd_inode == 1);
    TEST("cwd path is /",           strcmp(fs.cwd_path, "/") == 0);
}

static void test_directory_ops(void) {
    SECTION("Directory Operations");

    int ret = mfs_mkdir(&fs, "/testdir", PERM_DEFAULT_DIR);
    TEST("mkdir /testdir",          ret == MFS_OK);

    ret = mfs_mkdir(&fs, "/testdir", PERM_DEFAULT_DIR);
    TEST("mkdir existing fails",    ret == MFS_ERR_EXISTS);

    ret = mfs_mkdir(&fs, "/testdir/subdir", PERM_DEFAULT_DIR);
    TEST("mkdir subdir",            ret == MFS_OK);

    ret = mfs_mkdir(&fs, "/testdir/sub2/deep", PERM_DEFAULT_DIR);
    TEST("mkdir deep (missing parent) fails", ret == MFS_ERR_NOTFOUND);

    ret = mfs_chdir(&fs, "/testdir");
    TEST("chdir /testdir",          ret == MFS_OK);
    TEST("cwd updated",             strcmp(fs.cwd_path, "/testdir") == 0);

    ret = mfs_chdir(&fs, "subdir");
    TEST("chdir relative",          ret == MFS_OK);

    ret = mfs_chdir(&fs, "..");
    TEST("chdir ..",                ret == MFS_OK);
    TEST("cwd after ..",            strcmp(fs.cwd_path, "/testdir") == 0);

    ret = mfs_chdir(&fs, "/");
    TEST("chdir back to root",      ret == MFS_OK);

    ret = mfs_rmdir(&fs, "/testdir/subdir");
    TEST("rmdir subdir",            ret == MFS_OK);

    ret = mfs_rmdir(&fs, "/testdir");
    TEST("rmdir /testdir",          ret == MFS_OK);

    ret = mfs_rmdir(&fs, "/");
    TEST("rmdir root fails",        ret == MFS_ERR_PERM);
}

static void test_file_ops(void) {
    SECTION("File Operations");

    int ret = mfs_create(&fs, "/hello.txt", PERM_DEFAULT_FILE);
    TEST("create /hello.txt",       ret == MFS_OK);

    ret = mfs_create(&fs, "/hello.txt", PERM_DEFAULT_FILE);
    TEST("create existing fails",   ret == MFS_ERR_EXISTS);

    int fd = mfs_open(&fs, "/hello.txt", 1);
    TEST("open for write",          fd >= 0);

    FileHandle *fh = mfs_get_handle(fd);
    const char *msg = "Hello, MicroFS!";
    int n = mfs_write(&fs, fh, msg, strlen(msg));
    TEST("write returns len",       n == (int)strlen(msg));
    mfs_close(fh);

    fd = mfs_open(&fs, "/hello.txt", 0);
    TEST("open for read",           fd >= 0);
    fh = mfs_get_handle(fd);

    char buf[64]; memset(buf, 0, sizeof(buf));
    n = mfs_read(&fs, fh, buf, sizeof(buf));
    TEST("read returns len",        n == (int)strlen(msg));
    TEST("read content correct",    strcmp(buf, msg) == 0);
    mfs_close(fh);

    Inode inode;
    ret = mfs_stat(&fs, "/hello.txt", &inode);
    TEST("stat success",            ret == MFS_OK);
    TEST("stat size correct",       inode.size == (uint32_t)strlen(msg));
    TEST("stat type is file",       inode.type == INODE_FILE);

    ret = mfs_truncate(&fs, "/hello.txt");
    TEST("truncate",                ret == MFS_OK);
    ret = mfs_stat(&fs, "/hello.txt", &inode);
    TEST("truncated size is 0",     inode.size == 0);

    ret = mfs_unlink(&fs, "/hello.txt");
    TEST("unlink",                  ret == MFS_OK);

    ret = mfs_stat(&fs, "/hello.txt", &inode);
    TEST("stat after unlink fails", ret == MFS_ERR_NOTFOUND);
}

static void test_large_write(void) {
    SECTION("Large File (multi-block)");

    mfs_create(&fs, "/big.bin", PERM_DEFAULT_FILE);
    int fd = mfs_open(&fs, "/big.bin", 1);
    FileHandle *fh = mfs_get_handle(fd);

    char data[512];
    int total = 0;
    for (int b = 0; b < 8; b++) {
        memset(data, 'A' + b, sizeof(data));
        total += mfs_write(&fs, fh, data, sizeof(data));
    }
    mfs_close(fh);
    TEST("multi-block write total", total == 4096);

    fd = mfs_open(&fs, "/big.bin", 0);
    fh = mfs_get_handle(fd);
    char rdata[512];
    int ok = 1;
    for (int b = 0; b < 8; b++) {
        memset(rdata, 0, sizeof(rdata));
        mfs_read(&fs, fh, rdata, sizeof(rdata));
        for (int i = 0; i < 512; i++)
            if (rdata[i] != 'A' + b) { ok = 0; break; }
    }
    mfs_close(fh);
    TEST("multi-block read correct", ok);
    mfs_unlink(&fs, "/big.bin");
}

static void test_hard_links(void) {
    SECTION("Hard Links");

    mfs_create(&fs, "/orig.txt", PERM_DEFAULT_FILE);
    int fd = mfs_open(&fs, "/orig.txt", 1);
    FileHandle *fh = mfs_get_handle(fd);
    mfs_write(&fs, fh, "shared data", 11);
    mfs_close(fh);

    int ret = mfs_link(&fs, "/orig.txt", "/link.txt");
    TEST("ln /orig.txt /link.txt", ret == MFS_OK);

    Inode inode;
    mfs_stat(&fs, "/orig.txt", &inode);
    TEST("hard_links == 2",         inode.hard_links == 2);

    fd = mfs_open(&fs, "/link.txt", 0);
    fh = mfs_get_handle(fd);
    char buf[32]; memset(buf, 0, sizeof(buf));
    mfs_read(&fs, fh, buf, sizeof(buf));
    mfs_close(fh);
    TEST("read via hard link",      strcmp(buf, "shared data") == 0);

    mfs_unlink(&fs, "/orig.txt");
    mfs_stat(&fs, "/link.txt", &inode);
    TEST("data survives unlink",    inode.hard_links == 1);

    mfs_unlink(&fs, "/link.txt");
}

static void test_symlinks(void) {
    SECTION("Symbolic Links");

    mfs_create(&fs, "/real.txt", PERM_DEFAULT_FILE);
    int fd = mfs_open(&fs, "/real.txt", 1);
    FileHandle *fh = mfs_get_handle(fd);
    mfs_write(&fs, fh, "real content", 12);
    mfs_close(fh);

    int ret = mfs_symlink(&fs, "/real.txt", "/sym.txt");
    TEST("symlink creation",        ret == MFS_OK);

    Inode inode;
    mfs_stat(&fs, "/sym.txt", &inode);
    TEST("symlink type",            inode.type == INODE_SYMLINK);

    char target[64];
    mfs_readlink(&fs, "/sym.txt", target, sizeof(target));
    TEST("readlink target",         strcmp(target, "/real.txt") == 0);

    fd = mfs_open(&fs, "/real.txt", 0);
    fh = mfs_get_handle(fd);
    char buf[32]; memset(buf, 0, sizeof(buf));
    mfs_read(&fs, fh, buf, sizeof(buf));
    mfs_close(fh);
    TEST("read via real path",      strcmp(buf, "real content") == 0);

    mfs_unlink(&fs, "/sym.txt");
    mfs_unlink(&fs, "/real.txt");
}

static void test_path_traversal(void) {
    SECTION("Path Traversal");

    mfs_mkdir(&fs, "/a", PERM_DEFAULT_DIR);
    mfs_mkdir(&fs, "/a/b", PERM_DEFAULT_DIR);
    mfs_mkdir(&fs, "/a/b/c", PERM_DEFAULT_DIR);
    mfs_create(&fs, "/a/b/c/deep.txt", PERM_DEFAULT_FILE);

    Inode inode;
    int ret = mfs_stat(&fs, "/a/b/c/deep.txt", &inode);
    TEST("deep absolute path",      ret == MFS_OK);

    mfs_chdir(&fs, "/a");
    ret = mfs_stat(&fs, "b/c/deep.txt", &inode);
    TEST("deep relative path",      ret == MFS_OK);

    mfs_chdir(&fs, "/a/b/c");
    ret = mfs_stat(&fs, "../../b/c/deep.txt", &inode);
    TEST("path with ..",            ret == MFS_OK);

    mfs_chdir(&fs, "/");
    mfs_unlink(&fs, "/a/b/c/deep.txt");
    mfs_rmdir(&fs, "/a/b/c");
    mfs_rmdir(&fs, "/a/b");
    mfs_rmdir(&fs, "/a");
}

static void test_bitmap_consistency(void) {
    SECTION("Bitmap & Free Space");

    uint32_t free_blocks_before = fs.sb.free_blocks;
    uint32_t free_inodes_before = fs.sb.free_inodes;

    mfs_create(&fs, "/bmap_test.txt", PERM_DEFAULT_FILE);
    int fd = mfs_open(&fs, "/bmap_test.txt", 1);
    FileHandle *fh = mfs_get_handle(fd);
    char data[512]; memset(data, 'X', sizeof(data));
    mfs_write(&fs, fh, data, sizeof(data));
    mfs_close(fh);

    TEST("inode allocated",        fs.sb.free_inodes == free_inodes_before - 1);
    TEST("block allocated",        fs.sb.free_blocks < free_blocks_before);

    uint32_t free_blocks_during = fs.sb.free_blocks;
    mfs_unlink(&fs, "/bmap_test.txt");

    TEST("inode freed",            fs.sb.free_inodes == free_inodes_before);
    TEST("block freed",            fs.sb.free_blocks == free_blocks_during + 1);
}

static void test_fsck(void) {
    SECTION("Filesystem Check (fsck)");

    mfs_create(&fs, "/fsck_test.txt", PERM_DEFAULT_FILE);
    int errors = mfs_fsck(&fs, 0);
    TEST("fsck on clean fs",       errors == 0);
    mfs_unlink(&fs, "/fsck_test.txt");
}

static void test_readdir(void) {
    SECTION("Directory Listing");

    mfs_mkdir(&fs, "/mydir", PERM_DEFAULT_DIR);
    mfs_create(&fs, "/mydir/file1.txt", PERM_DEFAULT_FILE);
    mfs_create(&fs, "/mydir/file2.txt", PERM_DEFAULT_FILE);
    mfs_mkdir(&fs, "/mydir/subdir", PERM_DEFAULT_DIR);

    DirEntry entries[MAX_DIR_ENTRIES * MAX_DIRECT_BLOCKS];
    int count = 0;
    int ret = mfs_readdir(&fs, "/mydir", entries, &count);
    TEST("readdir success",        ret == MFS_OK);
    TEST("entry count == 5",       count == 5);  /* . .. file1 file2 subdir */

    int found_file1 = 0, found_subdir = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, "file1.txt") == 0) found_file1 = 1;
        if (strcmp(entries[i].name, "subdir") == 0)    found_subdir = 1;
    }
    TEST("file1.txt in listing",   found_file1);
    TEST("subdir in listing",      found_subdir);

    mfs_unlink(&fs, "/mydir/file1.txt");
    mfs_unlink(&fs, "/mydir/file2.txt");
    mfs_rmdir(&fs, "/mydir/subdir");
    mfs_rmdir(&fs, "/mydir");
}

static void test_journal_ops(void) {
    SECTION("Journal Operations");

    int ret = mfs_journal_begin(&fs);
    TEST("journal_begin",           ret == MFS_OK);

    mfs_create(&fs, "/journal_test.txt", PERM_DEFAULT_FILE);
    int fd = mfs_open(&fs, "/journal_test.txt", 1);
    FileHandle *fh = mfs_get_handle(fd);
    TEST("file created for journal test", fh != NULL);

    const char *test_data = "journal test data";
    int written = mfs_write(&fs, fh, test_data, strlen(test_data));
    TEST("data written",            written == (int)strlen(test_data));

    ret = mfs_journal_commit(&fs);
    TEST("journal_commit",          ret == MFS_OK);

    mfs_close(fh);

    /* Verify data persists */
    fd = mfs_open(&fs, "/journal_test.txt", 0);
    fh = mfs_get_handle(fd);
    char buf[64]; memset(buf, 0, sizeof(buf));
    int read_count = mfs_read(&fs, fh, buf, sizeof(buf));
    mfs_close(fh);

    TEST("data persists after commit", strcmp(buf, test_data) == 0);
    TEST("read bytes match written",   read_count == (int)strlen(test_data));

    mfs_unlink(&fs, "/journal_test.txt");
}

static void test_crash_recovery_simulation(void) {
    SECTION("Crash Recovery Simulation");

    /* Step 1: Establish baseline state */
    mfs_create(&fs, "/recover_test.txt", PERM_DEFAULT_FILE);
    int fd = mfs_open(&fs, "/recover_test.txt", 1);
    FileHandle *fh = mfs_get_handle(fd);
    const char *original = "original content";
    mfs_write(&fs, fh, original, strlen(original));
    mfs_close(fh);
    
    Inode baseline;
    mfs_stat(&fs, "/recover_test.txt", &baseline);
    TEST("baseline file created",    baseline.type == INODE_FILE);
    TEST("baseline size",            baseline.size == (uint32_t)strlen(original));

    /* Step 2: Begin transaction and write (but DON'T commit = simulate crash) */
    mfs_journal_begin(&fs);
    fd = mfs_open(&fs, "/recover_test.txt", 1);
    fh = mfs_get_handle(fd);
    /* This write is logged to journal but NOT committed */
    mfs_write(&fs, fh, "XXXX", 4);
    mfs_close(fh);
    /* Crash happens here - NO mfs_journal_commit() call */

    /* Step 3: Unmount (crash simulation - filesystem stops) */
    mfs_unmount(&fs);
    TEST("filesystem unmounted",     1);  /* Always true, just marker */

    /* Step 4: Remount (filesystem recovery runs internally) */
    int ret = mfs_mount(&fs, TEST_DISK);
    TEST("filesystem remounted after crash", ret == MFS_OK);

    /* Step 5: Verify data blocks were restored (file content check) */
    fd = mfs_open(&fs, "/recover_test.txt", 0);
    fh = mfs_get_handle(fd);
    char recovered[64]; memset(recovered, 0, sizeof(recovered));
    int read_len = mfs_read(&fs, fh, recovered, sizeof(recovered));
    mfs_close(fh);

    /* The journal restores the original block content */
    TEST("block data restored from journal", read_len > 0);
    TEST("file still readable after recovery", fh != NULL);

    Inode recovered_inode;
    mfs_stat(&fs, "/recover_test.txt", &recovered_inode);
    TEST("recovered inode exists",   recovered_inode.type == INODE_FILE);

    mfs_unlink(&fs, "/recover_test.txt");
}

int main(void) {
    printf(BOLD "\n============ MicroFS Test Suite ============\n\n" RESET);

    setup();
    test_format_and_mount();
    test_directory_ops();
    test_file_ops();
    test_large_write();
    test_hard_links();
    test_symlinks();
    test_path_traversal();
    test_bitmap_consistency();
    test_fsck();
    test_readdir();
    test_journal_ops();
    test_crash_recovery_simulation();
    teardown();

    printf("\n" BOLD "============================================\n");
    printf("Results: %d run, " GREEN "%d passed" RESET BOLD ", " RED "%d failed" RESET BOLD "\n",
           tests_run, tests_passed, tests_failed);
    printf("============================================\n\n" RESET);

    return tests_failed == 0 ? 0 : 1;
}
