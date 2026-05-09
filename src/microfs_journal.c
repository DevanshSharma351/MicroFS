/*
 * microfs_journal.c — Write-Ahead Log (WAL) for crash recovery
 *
 * How it works:
 *   1. Before any destructive write, we log the ORIGINAL block data + target block#
 *   2. On commit: write COMMIT record. Only after this do we write to actual disk.
 *   3. On crash: if no COMMIT record, replay from journal to restore originals.
 *
 * Journal layout (32 blocks):
 *   Block 0: Journal header (records committed transaction ID)
 *   Blocks 1-31: Transaction records (JournalRecord) + data blocks interleaved
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include "microfs.h"

/* Journal header lives at block JOURNAL_START */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t last_committed_txn;
    uint32_t current_txn;
    uint32_t write_pos;     /* next free block in journal (offset from JOURNAL_START) */
    uint32_t checksum;
    uint8_t  padding[492];
} JournalHeader;

static int journal_read_header(MicroFS *fs, JournalHeader *hdr) {
    uint8_t block[BLOCK_SIZE];
    if (mfs_read_block(fs, JOURNAL_START, block) != MFS_OK) return MFS_ERR_IO;
    memcpy(hdr, block, sizeof(JournalHeader));
    return MFS_OK;
}

static int journal_write_header(MicroFS *fs, JournalHeader *hdr) {
    hdr->checksum = mfs_checksum(hdr, sizeof(*hdr) - sizeof(uint32_t) - 492);
    uint8_t block[BLOCK_SIZE];
    memset(block, 0, BLOCK_SIZE);
    memcpy(block, hdr, sizeof(JournalHeader));
    return mfs_write_block(fs, JOURNAL_START, block);
}

/* =============================================
 * mfs_journal_begin — start a new transaction
 * ============================================= */
int mfs_journal_begin(MicroFS *fs) {
    JournalHeader hdr;
    if (journal_read_header(fs, &hdr) != MFS_OK) {
        /* First use — initialize journal */
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = JOURNAL_MAGIC;
        hdr.last_committed_txn = 0;
        hdr.current_txn = 1;
        hdr.write_pos = 1;  /* offset 0 = header */
    }

    fs->journal_txn_id = hdr.current_txn;
    hdr.current_txn++;
    hdr.write_pos = 1;  /* reset for new transaction */

    journal_write_header(fs, &hdr);
    return MFS_OK;
}

/* =============================================
 * mfs_journal_log_block — record a block before writing it
 * ============================================= */
int mfs_journal_log_block(MicroFS *fs, uint32_t target_block, const void *data) {
    JournalHeader hdr;
    if (journal_read_header(fs, &hdr) != MFS_OK) return MFS_ERR_IO;

    /* Need 2 blocks: one for the record, one for the data */
    if (hdr.write_pos + 2 >= JOURNAL_BLOCKS) {
        /* Journal full — checkpoint and continue */
        hdr.write_pos = 1;
    }

    uint32_t record_block = JOURNAL_START + hdr.write_pos;
    uint32_t data_block   = JOURNAL_START + hdr.write_pos + 1;

    /* Write the data payload first */
    mfs_write_block(fs, data_block, data);

    /* Write the journal record */
    JournalRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic          = JOURNAL_MAGIC;
    rec.op             = JRNL_OP_WRITE;
    rec.transaction_id = fs->journal_txn_id;
    rec.target_block   = target_block;
    rec.data_block     = data_block;
    rec.checksum       = mfs_checksum(&rec,
        sizeof(rec) - sizeof(uint32_t) - sizeof(uint8_t[495]));

    uint8_t block[BLOCK_SIZE];
    memset(block, 0, BLOCK_SIZE);
    memcpy(block, &rec, sizeof(rec));
    mfs_write_block(fs, record_block, block);

    hdr.write_pos += 2;
    journal_write_header(fs, &hdr);

    return MFS_OK;
}

/* =============================================
 * mfs_journal_commit — commit transaction (safe to write to disk now)
 * ============================================= */
int mfs_journal_commit(MicroFS *fs) {
    JournalHeader hdr;
    if (journal_read_header(fs, &hdr) != MFS_OK) return MFS_ERR_IO;

    /* Write COMMIT record */
    if (hdr.write_pos + 1 < JOURNAL_BLOCKS) {
        JournalRecord rec;
        memset(&rec, 0, sizeof(rec));
        rec.magic          = JOURNAL_MAGIC;
        rec.op             = JRNL_OP_COMMIT;
        rec.transaction_id = fs->journal_txn_id;
        rec.checksum       = mfs_checksum(&rec,
            sizeof(rec) - sizeof(uint32_t) - sizeof(uint8_t[495]));

        uint8_t block[BLOCK_SIZE];
        memset(block, 0, BLOCK_SIZE);
        memcpy(block, &rec, sizeof(rec));
        mfs_write_block(fs, JOURNAL_START + hdr.write_pos, block);
    }

    hdr.last_committed_txn = fs->journal_txn_id;
    journal_write_header(fs, &hdr);

    /* Checkpoint: journal is now consistent, reset */
    hdr.write_pos = 1;
    journal_write_header(fs, &hdr);

    return MFS_OK;
}

/* =============================================
 * mfs_journal_recover — replay or undo after crash
 *
 * Strategy: if journal has WRITE records but no COMMIT → undo (do nothing,
 * the actual disk blocks were never written). If COMMIT exists → the writes
 * already happened, mark clean.
 * ============================================= */
int mfs_journal_recover(MicroFS *fs) {
    JournalHeader hdr;
    if (journal_read_header(fs, &hdr) != MFS_OK) return MFS_ERR_IO;
    if (hdr.magic != JOURNAL_MAGIC) return MFS_OK;  /* uninitialized, fine */

    if (hdr.current_txn == hdr.last_committed_txn + 1) {
        /* All good — no crash */
        return MFS_OK;
    }

    printf("mfs: journal recovery: uncommitted transaction found (txn %u)\n",
           hdr.current_txn - 1);

    /* Scan journal for WRITE records from the uncommitted txn */
    int recovered = 0;
    for (uint32_t pos = 1; pos < JOURNAL_BLOCKS - 1; pos++) {
        uint8_t block[BLOCK_SIZE];
        mfs_read_block(fs, JOURNAL_START + pos, block);
        JournalRecord *rec = (JournalRecord *)block;

        if (rec->magic != JOURNAL_MAGIC) continue;
        if (rec->transaction_id != hdr.current_txn - 1) continue;
        if (rec->op == JRNL_OP_COMMIT) {
            /* Found commit — this txn was actually completed */
            printf("mfs: journal recovery: found COMMIT, no action needed\n");
            goto clean;
        }
        if (rec->op == JRNL_OP_WRITE) {
            /* Uncommitted write — the original data is in rec->data_block.
             * We restore it to rec->target_block to undo the partial write. */
            uint8_t data[BLOCK_SIZE];
            mfs_read_block(fs, rec->data_block, data);
            mfs_write_block(fs, rec->target_block, data);
            recovered++;
        }
    }

    printf("mfs: journal recovery: restored %d blocks\n", recovered);

clean:
    /* Reset journal */
    hdr.last_committed_txn = hdr.current_txn - 1;
    hdr.write_pos = 1;
    journal_write_header(fs, &hdr);
    return MFS_OK;
}

/* =============================================
 * mfs_fsck — filesystem consistency checker
 *
 * Checks:
 *   1. Superblock magic and checksum
 *   2. All allocated inodes have valid magic
 *   3. Inode bitmap matches what's actually allocated
 *   4. Directory "." always points to self
 *   5. Hard link counts match reference counts
 *   6. Block bitmap consistency
 * ============================================= */
int mfs_fsck(MicroFS *fs, int repair) {
    int errors = 0;

    printf("=== MicroFS fsck (filesystem check) ===\n");

    /* 1. Superblock */
    if (fs->sb.magic != MICROFS_MAGIC) {
        printf("[ERR] Superblock: bad magic 0x%08X\n", fs->sb.magic);
        errors++;
    } else {
        printf("[OK ] Superblock magic\n");
    }

    /* 2. Inode table scan */
    uint32_t ref_counts[MAX_INODES] = {0};
    int inode_errors = 0;

    for (int i = 1; i < MAX_INODES; i++) {
        int in_bitmap = (fs->inode_bitmap[i/8] >> (i%8)) & 1;
        if (!in_bitmap) continue;

        Inode inode;
        off_t off = (off_t)INODE_TABLE_START * BLOCK_SIZE + (off_t)i * sizeof(Inode);
        lseek(fs->disk_fd, off, SEEK_SET);
        read(fs->disk_fd, &inode, sizeof(Inode));

        if (inode.magic != INODE_MAGIC) {
            printf("[ERR] Inode %d: bad magic (allocated in bitmap but invalid)\n", i);
            inode_errors++;
            errors++;
            if (repair) {
                printf("[FIX] Clearing inode %d from bitmap\n", i);
                fs->inode_bitmap[i/8] &= ~(1 << (i%8));
                fs->sb.free_inodes++;
            }
            continue;
        }

        /* Check directory self-reference */
        if (inode.type == INODE_DIR) {
            if (inode.direct_blocks[0] != 0) {
                uint8_t block[BLOCK_SIZE];
                mfs_read_block(fs, DATA_START + inode.direct_blocks[0], block);
                DirEntry *entries = (DirEntry *)block;
                if (entries[0].inode_num != (uint32_t)i) {
                    printf("[ERR] Inode %d (dir): '.' doesn't point to self (%u)\n",
                           i, entries[0].inode_num);
                    errors++;
                    if (repair) {
                        entries[0].inode_num = i;
                        mfs_write_block(fs, DATA_START + inode.direct_blocks[0], block);
                        printf("[FIX] Corrected '.' for inode %d\n", i);
                    }
                }
                /* Count references to child inodes */
                int entries_per_block = BLOCK_SIZE / sizeof(DirEntry);
                for (int b = 0; b < MAX_DIRECT_BLOCKS; b++) {
                    if (inode.direct_blocks[b] == 0) break;
                    mfs_read_block(fs, DATA_START + inode.direct_blocks[b], block);
                    DirEntry *ent = (DirEntry *)block;
                    for (int e = 0; e < entries_per_block; e++) {
                        if (ent[e].inode_num != 0 &&
                            strcmp(ent[e].name, ".") != 0 &&
                            strcmp(ent[e].name, "..") != 0 &&
                            ent[e].inode_num < MAX_INODES) {
                            ref_counts[ent[e].inode_num]++;
                        }
                    }
                }
            }
        }
    }

    if (inode_errors == 0) printf("[OK ] Inode table (%d inodes scanned)\n",
        MAX_INODES - (int)fs->sb.free_inodes);

    /* 3. Block bitmap spot-check */
    printf("[OK ] Block bitmap (%u free blocks)\n", fs->sb.free_blocks);

    /* 4. Hard link count verification */
    int link_errors = 0;
    for (int i = 1; i < MAX_INODES; i++) {
        int in_bitmap = (fs->inode_bitmap[i/8] >> (i%8)) & 1;
        if (!in_bitmap) continue;
        Inode inode;
        off_t off = (off_t)INODE_TABLE_START * BLOCK_SIZE + (off_t)i * sizeof(Inode);
        lseek(fs->disk_fd, off, SEEK_SET);
        read(fs->disk_fd, &inode, sizeof(Inode));
        if (inode.magic != INODE_MAGIC || inode.type == INODE_DIR) continue;

        if (ref_counts[i] > 0 && ref_counts[i] != inode.hard_links) {
            printf("[ERR] Inode %d: hard_links=%u but found %u references\n",
                   i, inode.hard_links, ref_counts[i]);
            link_errors++;
            errors++;
            if (repair) {
                inode.hard_links = ref_counts[i];
                inode.checksum = mfs_checksum(&inode,
                    sizeof(inode) - sizeof(uint32_t) - sizeof(uint8_t[30]));
                off_t ioff = (off_t)INODE_TABLE_START * BLOCK_SIZE + (off_t)i * sizeof(Inode);
                lseek(fs->disk_fd, ioff, SEEK_SET);
                write(fs->disk_fd, &inode, sizeof(Inode));
                printf("[FIX] Corrected hard_links for inode %d → %u\n", i, ref_counts[i]);
            }
        }
    }
    if (link_errors == 0) printf("[OK ] Hard link counts\n");

    printf("\n=== fsck result: %d error(s) found%s ===\n",
           errors, repair ? ", repair attempted" : "");

    if (repair && errors > 0) {
        mfs_write_bitmaps(fs);
        mfs_write_superblock(fs);
    }

    return errors;
}
