#ifndef MICROFS_INTERNAL_H
#define MICROFS_INTERNAL_H

/*
 * microfs_internal.h — Internal function prototypes shared between
 *                      microfs_dir.c and microfs_file.c
 *
 * These are NOT part of the public API (not in microfs.h).
 */

#include "microfs.h"

/* Directory entry manipulation — defined in microfs_dir.c */
int dir_add_entry(MicroFS *fs, uint32_t dir_inum, uint32_t new_inum,
                  const char *name);
int dir_remove_entry(MicroFS *fs, uint32_t dir_inum, const char *name);
int dir_lookup(MicroFS *fs, uint32_t dir_inum, const char *name,
               uint32_t *result_inum);

/* File handle table — defined in microfs_file.c */
extern FileHandle fh_table[];
void mfs_close(FileHandle *fh);
FileHandle *mfs_get_handle(int fd);

#endif /* MICROFS_INTERNAL_H */
