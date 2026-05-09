/*
 * microfs_shell.c — Interactive command-line shell for MicroFS
 *
 * Commands:
 *   ls [-l]          list directory
 *   cd <path>        change directory
 *   pwd              print working directory
 *   mkdir <path>     create directory
 *   rmdir <path>     remove empty directory
 *   touch <path>     create empty file
 *   rm <path>        delete file
 *   write <path> <text>   write text to file
 *   append <path> <text>  append text to file
 *   cat <path>       print file contents
 *   cp <src> <dst>   copy file
 *   mv <src> <dst>   rename/move file
 *   ln <src> <dst>   hard link
 *   ln -s <tgt> <lnk> symbolic link
 *   readlink <path>  read symlink target
 *   stat <path>      show file metadata
 *   df               disk free space
 *   fsck [-r]        check filesystem (optionally repair)
 *   help             show commands
 *   exit/quit        exit shell
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "microfs.h"
#include "microfs_internal.h"

/* ANSI colors */
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define CYAN    "\033[36m"
#define WHITE   "\033[37m"
#define MAGENTA "\033[35m"
#define DIM     "\033[2m"

static void print_banner(void) {
    printf("\n");
    printf(BOLD CYAN "  ███╗   ███╗██╗ ██████╗██████╗  ██████╗ ███████╗███████╗\n" RESET);
    printf(BOLD CYAN "  ████╗ ████║██║██╔════╝██╔══██╗██╔═══██╗██╔════╝██╔════╝\n" RESET);
    printf(BOLD CYAN "  ██╔████╔██║██║██║     ██████╔╝██║   ██║█████╗  ███████╗\n" RESET);
    printf(BOLD CYAN "  ██║╚██╔╝██║██║██║     ██╔══██╗██║   ██║██╔══╝  ╚════██║\n" RESET);
    printf(BOLD CYAN "  ██║ ╚═╝ ██║██║╚██████╗██║  ██║╚██████╔╝██║     ███████║\n" RESET);
    printf(BOLD CYAN "  ╚═╝     ╚═╝╚═╝ ╚═════╝╚═╝  ╚═╝ ╚═════╝ ╚═╝     ╚══════╝\n" RESET);
    printf(DIM "  inode-based virtual filesystem in C  |  v1.0\n" RESET);
    printf("\n");
}

static void print_help(void) {
    printf(BOLD "\nAvailable Commands:\n" RESET);
    printf(GREEN "  File Operations:\n" RESET);
    printf("    touch <path>              create empty file\n");
    printf("    rm <path>                 delete file\n");
    printf("    write <path> <text...>    write text to file\n");
    printf("    append <path> <text...>   append text to file\n");
    printf("    cat <path>                print file contents\n");
    printf("    cp <src> <dst>            copy file\n");
    printf("    mv <src> <dst>            move/rename file\n");
    printf("    stat <path>               show file metadata\n");
    printf("    truncate <path>           truncate file to 0 bytes\n");
    printf(GREEN "  Directory Operations:\n" RESET);
    printf("    ls [-l] [path]            list directory\n");
    printf("    cd <path>                 change directory\n");
    printf("    pwd                       print working directory\n");
    printf("    mkdir <path>              create directory\n");
    printf("    rmdir <path>              remove empty directory\n");
    printf(GREEN "  Links:\n" RESET);
    printf("    ln <src> <dst>            create hard link\n");
    printf("    ln -s <target> <link>     create symbolic link\n");
    printf("    readlink <path>           read symlink target\n");
    printf(GREEN "  Filesystem:\n" RESET);
    printf("    df                        show disk usage\n");
    printf("    fsck [-r]                 check/repair filesystem\n");
    printf(GREEN "  Shell:\n" RESET);
    printf("    help                      show this help\n");
    printf("    exit / quit               exit shell\n\n");
}

/* =============================================
 * ls command — optionally with -l for long format
 * ============================================= */
static void cmd_ls(MicroFS *fs, const char *path, int long_format) {
    DirEntry entries[MAX_DIR_ENTRIES * MAX_DIRECT_BLOCKS];
    int count = 0;

    int ret = mfs_readdir(fs, path, entries, &count);
    if (ret != MFS_OK) {
        printf(RED "ls: %s: %s\n" RESET, path, mfs_strerror(ret));
        return;
    }

    if (!long_format) {
        for (int i = 0; i < count; i++) {
            Inode inode;
            if (mfs_read_inode(fs, entries[i].inode_num, &inode) != MFS_OK) continue;

            if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) {
                printf(DIM "%s  " RESET, entries[i].name);
            } else if (inode.type == INODE_DIR) {
                printf(BOLD BLUE "%s/  " RESET, entries[i].name);
            } else if (inode.type == INODE_SYMLINK) {
                printf(BOLD CYAN "%s@  " RESET, entries[i].name);
            } else {
                printf("%s  ", entries[i].name);
            }
        }
        printf("\n");
        return;
    }

    /* Long format: permissions links owner size date name */
    printf(DIM "%-10s %4s %6s %6s %s\n" RESET,
           "Permissions", "Links", "Size", "Inode", "Name");
    printf(DIM "---------- ----- ------ ------ ----\n" RESET);

    for (int i = 0; i < count; i++) {
        Inode inode;
        if (mfs_read_inode(fs, entries[i].inode_num, &inode) != MFS_OK) continue;

        char perm_str[12];
        mfs_perm_string(inode.permissions, inode.type, perm_str);

        char size_str[16];
        mfs_format_size(inode.size, size_str);

        char time_str[20];
        time_t mtime = inode.modified_at; struct tm *tm_info = localtime(&mtime);
        strftime(time_str, sizeof(time_str), "%b %d %H:%M", tm_info);

        printf("%s %4u %6s %6u  ", perm_str, inode.hard_links,
               size_str, entries[i].inode_num);

        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0) {
            printf(DIM "%s" RESET, entries[i].name);
        } else if (inode.type == INODE_DIR) {
            printf(BOLD BLUE "%s/" RESET, entries[i].name);
        } else if (inode.type == INODE_SYMLINK) {
            printf(BOLD CYAN "%s" RESET " -> " CYAN "%s" RESET,
                   entries[i].name, inode.symlink_target);
        } else if (inode.permissions & PERM_OWNER_X) {
            printf(GREEN "%s" RESET, entries[i].name);
        } else {
            printf("%s", entries[i].name);
        }
        printf("\n");
    }
}

/* =============================================
 * stat command
 * ============================================= */
static void cmd_stat(MicroFS *fs, const char *path) {
    Inode inode;
    PathResult res;
    int ret = mfs_resolve_path(fs, path, &res);
    if (ret != MFS_OK) { printf(RED "stat: %s\n" RESET, mfs_strerror(ret)); return; }
    inode = res.inode;

    char perm_str[12];
    mfs_perm_string(inode.permissions, inode.type, perm_str);

    const char *type_str =
        inode.type == INODE_DIR ? "directory" :
        inode.type == INODE_SYMLINK ? "symbolic link" : "regular file";

    printf(BOLD "  File:" RESET " %s\n", path);
    printf(BOLD "  Type:" RESET " %s\n", type_str);
    printf(BOLD " Inode:" RESET " %u\n", res.inode_num);
    printf(BOLD "  Size:" RESET " %u bytes\n", inode.size);
    printf(BOLD " Links:" RESET " %u\n", inode.hard_links);
    printf(BOLD "Access:" RESET " %s (%04o)\n", perm_str, inode.permissions);

    char tbuf[32];
    struct tm *t;
    { time_t ct = inode.created_at; t = localtime(&ct); }
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", t);
    printf(BOLD "Create:" RESET " %s\n", tbuf);
    { time_t mt = inode.modified_at; t = localtime(&mt); }
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", t);
    printf(BOLD "Modify:" RESET " %s\n", tbuf);

    if (inode.type != INODE_DIR) {
        printf(BOLD "Blocks:" RESET " ");
        for (int b = 0; b < MAX_DIRECT_BLOCKS; b++) {
            if (inode.direct_blocks[b]) printf("%u ", inode.direct_blocks[b]);
        }
        printf("\n");
    }

    if (inode.type == INODE_SYMLINK)
        printf(BOLD "Target:" RESET " %s\n", inode.symlink_target);
}

/* =============================================
 * df command — disk free
 * ============================================= */
static void cmd_df(MicroFS *fs) {
    uint32_t total_data  = DATA_BLOCKS;
    uint32_t used_blocks = total_data - fs->sb.free_blocks;
    uint32_t pct_used    = total_data > 0 ? (used_blocks * 100) / total_data : 0;

    printf(BOLD "\n  Filesystem:   " RESET "MicroFS v%u\n", fs->sb.version);
    printf(BOLD "  Disk size:    " RESET "%u KB (%u blocks)\n",
           (TOTAL_BLOCKS * BLOCK_SIZE) / 1024, TOTAL_BLOCKS);
    printf(BOLD "  Data blocks:  " RESET "%u total, %u used, %u free\n",
           total_data, used_blocks, fs->sb.free_blocks);
    printf(BOLD "  Inodes:       " RESET "%u total, %u used, %u free\n",
           MAX_INODES, MAX_INODES - fs->sb.free_inodes, fs->sb.free_inodes);
    printf(BOLD "  Usage:        " RESET "[");

    int bar_width = 40;
    int filled = (bar_width * pct_used) / 100;
    const char *color = pct_used > 80 ? RED : pct_used > 50 ? YELLOW : GREEN;
    printf("%s", color);
    for (int i = 0; i < bar_width; i++)
        printf(i < filled ? "█" : "░");
    printf(RESET "] %u%%\n\n", pct_used);
}

/* =============================================
 * cat command
 * ============================================= */
static void cmd_cat(MicroFS *fs, const char *path) {
    int fd = mfs_open(fs, path, 0);
    if (fd < 0) { printf(RED "cat: %s: %s\n" RESET, path, mfs_strerror(fd)); return; }

    FileHandle *fh = mfs_get_handle(fd);
    char buf[BLOCK_SIZE + 1];
    int n;
    while ((n = mfs_read(fs, fh, buf, BLOCK_SIZE)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    printf("\n");
    mfs_close(fh);
}

/* =============================================
 * write/append command
 * ============================================= */
static void cmd_write(MicroFS *fs, const char *path, const char *text, int append_mode) {
    /* Create if doesn't exist */
    PathResult res;
    if (mfs_resolve_path(fs, path, &res) == MFS_ERR_NOTFOUND)
        mfs_create(fs, path, PERM_DEFAULT_FILE);

    if (!append_mode)
        mfs_truncate(fs, path);

    int fd = mfs_open(fs, path, 1);
    if (fd < 0) { printf(RED "write: %s\n" RESET, mfs_strerror(fd)); return; }

    FileHandle *fh = mfs_get_handle(fd);

    /* If append, seek to end */
    if (append_mode) {
        Inode inode;
        mfs_read_inode(fs, fh->inode_num, &inode);
        fh->offset = inode.size;
    }

    mfs_write(fs, fh, text, strlen(text));
    /* Add newline */
    mfs_write(fs, fh, "\n", 1);
    mfs_close(fh);
    printf(GREEN "OK\n" RESET);
}

/* =============================================
 * cp command
 * ============================================= */
static void cmd_cp(MicroFS *fs, const char *src, const char *dst) {
    int src_fd = mfs_open(fs, src, 0);
    if (src_fd < 0) { printf(RED "cp: %s: %s\n" RESET, src, mfs_strerror(src_fd)); return; }

    mfs_create(fs, dst, PERM_DEFAULT_FILE);
    int dst_fd = mfs_open(fs, dst, 1);
    if (dst_fd < 0) {
        mfs_close(mfs_get_handle(src_fd));
        printf(RED "cp: %s: %s\n" RESET, dst, mfs_strerror(dst_fd));
        return;
    }

    FileHandle *sfh = mfs_get_handle(src_fd);
    FileHandle *dfh = mfs_get_handle(dst_fd);
    char buf[BLOCK_SIZE];
    int n;
    while ((n = mfs_read(fs, sfh, buf, BLOCK_SIZE)) > 0)
        mfs_write(fs, dfh, buf, n);

    mfs_close(sfh);
    mfs_close(dfh);
    printf(GREEN "OK\n" RESET);
}

/* =============================================
 * mv command
 * ============================================= */
static void cmd_mv(MicroFS *fs, const char *src, const char *dst) {
    /* mv = link + unlink */
    int ret = mfs_link(fs, src, dst);
    if (ret != MFS_OK) { printf(RED "mv: %s\n" RESET, mfs_strerror(ret)); return; }
    ret = mfs_unlink(fs, src);
    if (ret != MFS_OK) {
        mfs_unlink(fs, dst);
        printf(RED "mv: unlink failed: %s\n" RESET, mfs_strerror(ret));
        return;
    }
    printf(GREEN "OK\n" RESET);
}

/* =============================================
 * Tokenize input line
 * ============================================= */
#define MAX_ARGS 16
static int tokenize(char *line, char *argv[]) {
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* Reconstruct remaining args after index i as a single string */
static void join_args(char *argv[], int start, int argc, char *out, int outlen) {
    out[0] = '\0';
    for (int i = start; i < argc; i++) {
        strncat(out, argv[i], outlen - strlen(out) - 1);
        if (i < argc - 1)
            strncat(out, " ", outlen - strlen(out) - 1);
    }
}

/* =============================================
 * Main shell loop
 * ============================================= */
void run_shell(MicroFS *fs) {
    print_banner();
    printf("Type " BOLD "help" RESET " for a list of commands.\n\n");

    char line[1024];
    char *argv[MAX_ARGS];

    while (1) {
        /* Prompt: microfs:/current/path$ */
        printf(BOLD CYAN "microfs:" RESET BOLD BLUE "%s" RESET "$ ", fs->cwd_path);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        /* Strip trailing newline */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        int argc = tokenize(line, argv);
        if (argc == 0) continue;

        char *cmd = argv[0];

        /* ---- dispatch ---- */
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            printf("Goodbye!\n");
            break;
        }
        else if (strcmp(cmd, "help") == 0) {
            print_help();
        }
        else if (strcmp(cmd, "pwd") == 0) {
            printf("%s\n", fs->cwd_path);
        }
        else if (strcmp(cmd, "cd") == 0) {
            const char *path = argc > 1 ? argv[1] : "/";
            int ret = mfs_chdir(fs, path);
            if (ret != MFS_OK) printf(RED "cd: %s: %s\n" RESET, path, mfs_strerror(ret));
        }
        else if (strcmp(cmd, "ls") == 0) {
            int long_fmt = 0;
            const char *path = fs->cwd_path;
            for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "-l") == 0) long_fmt = 1;
                else path = argv[i];
            }
            cmd_ls(fs, path, long_fmt);
        }
        else if (strcmp(cmd, "mkdir") == 0) {
            if (argc < 2) { printf("Usage: mkdir <path>\n"); continue; }
            int ret = mfs_mkdir(fs, argv[1], PERM_DEFAULT_DIR);
            if (ret != MFS_OK) printf(RED "mkdir: %s\n" RESET, mfs_strerror(ret));
            else printf(GREEN "OK\n" RESET);
        }
        else if (strcmp(cmd, "rmdir") == 0) {
            if (argc < 2) { printf("Usage: rmdir <path>\n"); continue; }
            int ret = mfs_rmdir(fs, argv[1]);
            if (ret != MFS_OK) printf(RED "rmdir: %s\n" RESET, mfs_strerror(ret));
            else printf(GREEN "OK\n" RESET);
        }
        else if (strcmp(cmd, "touch") == 0) {
            if (argc < 2) { printf("Usage: touch <path>\n"); continue; }
            int ret = mfs_create(fs, argv[1], PERM_DEFAULT_FILE);
            if (ret != MFS_OK) printf(RED "touch: %s\n" RESET, mfs_strerror(ret));
            else printf(GREEN "OK\n" RESET);
        }
        else if (strcmp(cmd, "rm") == 0) {
            if (argc < 2) { printf("Usage: rm <path>\n"); continue; }
            int ret = mfs_unlink(fs, argv[1]);
            if (ret != MFS_OK) printf(RED "rm: %s\n" RESET, mfs_strerror(ret));
            else printf(GREEN "OK\n" RESET);
        }
        else if (strcmp(cmd, "cat") == 0) {
            if (argc < 2) { printf("Usage: cat <path>\n"); continue; }
            cmd_cat(fs, argv[1]);
        }
        else if (strcmp(cmd, "write") == 0) {
            if (argc < 3) { printf("Usage: write <path> <text...>\n"); continue; }
            char text[800];
            join_args(argv, 2, argc, text, sizeof(text));
            cmd_write(fs, argv[1], text, 0);
        }
        else if (strcmp(cmd, "append") == 0) {
            if (argc < 3) { printf("Usage: append <path> <text...>\n"); continue; }
            char text[800];
            join_args(argv, 2, argc, text, sizeof(text));
            cmd_write(fs, argv[1], text, 1);
        }
        else if (strcmp(cmd, "stat") == 0) {
            if (argc < 2) { printf("Usage: stat <path>\n"); continue; }
            cmd_stat(fs, argv[1]);
        }
        else if (strcmp(cmd, "cp") == 0) {
            if (argc < 3) { printf("Usage: cp <src> <dst>\n"); continue; }
            cmd_cp(fs, argv[1], argv[2]);
        }
        else if (strcmp(cmd, "mv") == 0) {
            if (argc < 3) { printf("Usage: mv <src> <dst>\n"); continue; }
            cmd_mv(fs, argv[1], argv[2]);
        }
        else if (strcmp(cmd, "truncate") == 0) {
            if (argc < 2) { printf("Usage: truncate <path>\n"); continue; }
            int ret = mfs_truncate(fs, argv[1]);
            if (ret != MFS_OK) printf(RED "truncate: %s\n" RESET, mfs_strerror(ret));
            else printf(GREEN "OK\n" RESET);
        }
        else if (strcmp(cmd, "ln") == 0) {
            if (argc < 3) { printf("Usage: ln [-s] <src> <dst>\n"); continue; }
            if (strcmp(argv[1], "-s") == 0) {
                if (argc < 4) { printf("Usage: ln -s <target> <linkname>\n"); continue; }
                int ret = mfs_symlink(fs, argv[2], argv[3]);
                if (ret != MFS_OK) printf(RED "ln: %s\n" RESET, mfs_strerror(ret));
                else printf(GREEN "OK\n" RESET);
            } else {
                int ret = mfs_link(fs, argv[1], argv[2]);
                if (ret != MFS_OK) printf(RED "ln: %s\n" RESET, mfs_strerror(ret));
                else printf(GREEN "OK\n" RESET);
            }
        }
        else if (strcmp(cmd, "readlink") == 0) {
            if (argc < 2) { printf("Usage: readlink <path>\n"); continue; }
            char buf[512];
            int ret = mfs_readlink(fs, argv[1], buf, sizeof(buf));
            if (ret != MFS_OK) printf(RED "readlink: %s\n" RESET, mfs_strerror(ret));
            else printf("%s\n", buf);
        }
        else if (strcmp(cmd, "df") == 0) {
            cmd_df(fs);
        }
        else if (strcmp(cmd, "fsck") == 0) {
            int repair = (argc > 1 && strcmp(argv[1], "-r") == 0);
            int errors = mfs_fsck(fs, repair);
            if (errors == 0) printf(GREEN "Filesystem is clean.\n" RESET);
        }
        else {
            printf(RED "Unknown command: %s" RESET " (type " BOLD "help" RESET " for help)\n", cmd);
        }
    }
}
