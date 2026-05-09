/*
 * main.c — MicroFS entry point
 *
 * Usage:
 *   ./microfs                  (creates/uses microfs.disk)
 *   ./microfs <disk_image>     (use a specific disk image)
 *   ./microfs --format         (format a new disk)
 *   ./microfs --fsck           (run filesystem check only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "microfs.h"

extern void run_shell(MicroFS *fs);

int main(int argc, char *argv[]) {
    const char *disk_path = "microfs.disk";
    int do_format = 0, do_fsck = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--format") == 0) do_format = 1;
        else if (strcmp(argv[i], "--fsck") == 0) do_fsck = 1;
        else disk_path = argv[i];
    }

    /* Check if disk exists */
    FILE *f = fopen(disk_path, "rb");
    int disk_exists = (f != NULL);
    if (f) fclose(f);

    if (do_format || !disk_exists) {
        printf("MicroFS: %s disk image...\n",
               do_format ? "formatting" : "creating new");
        if (mfs_format(disk_path) != MFS_OK) {
            fprintf(stderr, "fatal: failed to format disk\n");
            return 1;
        }
    }

    MicroFS fs;
    if (mfs_mount(&fs, disk_path) != MFS_OK) {
        fprintf(stderr, "fatal: failed to mount %s\n", disk_path);
        return 1;
    }

    printf("MicroFS: mounted '%s' (%u KB, %u inodes free)\n",
           disk_path,
           (TOTAL_BLOCKS * BLOCK_SIZE) / 1024,
           fs.sb.free_inodes);

    if (do_fsck) {
        mfs_fsck(&fs, 0);
        mfs_unmount(&fs);
        return 0;
    }

    run_shell(&fs);

    mfs_unmount(&fs);
    printf("MicroFS: disk synced and unmounted.\n");
    return 0;
}
