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
#include <unistd.h>
#include <termios.h>
#include <signal.h>
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

static void print_shell_intro(void) {
    print_banner();
    printf("Type " BOLD "help" RESET " for a list of commands.\n\n");
}

static void clear_and_repaint_header(void) {
    /* 2J: clear visible screen, 3J: clear scrollback (supported by most modern terminals). */
    printf("\033[2J\033[3J\033[H");
    print_shell_intro();
    fflush(stdout);
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
    printf("    clear                     clear the screen\n");
    printf("    help                      show this help\n");
    printf("    exit / quit               exit shell\n\n");
}

#define MAX_HISTORY 200
#define MAX_LINE_LEN 1024
#define MAX_CD_MATCHES 128

static struct termios g_saved_termios;
static volatile sig_atomic_t g_raw_mode_active = 0;

static void restore_terminal_mode(void) {
    if (g_raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
        g_raw_mode_active = 0;
    }
}

static void handle_restore_and_exit(int sig) {
    restore_terminal_mode();
    _exit(128 + sig);
}

static void install_terminal_restore_handlers(void) {
    static int installed = 0;
    if (installed) return;

    signal(SIGINT, handle_restore_and_exit);
    signal(SIGTERM, handle_restore_and_exit);
    signal(SIGQUIT, handle_restore_and_exit);
    signal(SIGHUP, handle_restore_and_exit);
    atexit(restore_terminal_mode);
    installed = 1;
}

static const char *k_shell_commands[] = {
    "help", "clear", "pwd", "cd", "ls", "mkdir", "rmdir", "touch", "rm",
    "cat", "write", "append", "stat", "cp", "mv", "truncate", "ln",
    "readlink", "df", "fsck", "exit", "quit"
};

static void build_prompt(MicroFS *fs, char *prompt, size_t prompt_len) {
    snprintf(prompt, prompt_len, BOLD CYAN "microfs:" RESET BOLD BLUE "%s" RESET "$ ", fs->cwd_path);
}

static void redraw_input(const char *prompt, const char *line) {
    printf("\r\033[2K%s%s", prompt, line);
    fflush(stdout);
}

static int collect_command_matches(const char *prefix, size_t *matched_idx, size_t max_idx) {
    int matches = 0;
    size_t prefix_len = strlen(prefix);

    for (size_t i = 0; i < sizeof(k_shell_commands) / sizeof(k_shell_commands[0]); i++) {
        if (strncmp(k_shell_commands[i], prefix, prefix_len) == 0) {
            if ((size_t)matches < max_idx)
                matched_idx[matches] = i;
            matches++;
        }
    }
    return matches;
}

static void longest_common_prefix(char *out, size_t out_len,
                                  size_t *matched_idx, int matches) {
    out[0] = '\0';
    if (matches <= 0 || out_len == 0) return;

    strncpy(out, k_shell_commands[matched_idx[0]], out_len - 1);
    out[out_len - 1] = '\0';

    for (int m = 1; m < matches; m++) {
        const char *cmd = k_shell_commands[matched_idx[m]];
        size_t j = 0;
        while (out[j] && cmd[j] && out[j] == cmd[j]) j++;
        out[j] = '\0';
    }
}

static void longest_common_prefix_strs(char *out, size_t out_len,
                                       char matches[][MAX_FILENAME], int count) {
    out[0] = '\0';
    if (count <= 0 || out_len == 0) return;

    strncpy(out, matches[0], out_len - 1);
    out[out_len - 1] = '\0';

    for (int m = 1; m < count; m++) {
        size_t j = 0;
        while (out[j] && matches[m][j] && out[j] == matches[m][j]) j++;
        out[j] = '\0';
    }
}

static int collect_cd_matches(MicroFS *fs, const char *arg,
                              char matches[][MAX_FILENAME], int max_matches,
                              char *base_prefix, size_t base_prefix_len,
                              char *name_prefix, size_t name_prefix_len) {
    char dir_path[512];
    const char *slash = strrchr(arg, '/');

    if (!slash) {
        strncpy(dir_path, fs->cwd_path, sizeof(dir_path) - 1);
        dir_path[sizeof(dir_path) - 1] = '\0';
        base_prefix[0] = '\0';
        strncpy(name_prefix, arg, name_prefix_len - 1);
        name_prefix[name_prefix_len - 1] = '\0';
    } else {
        size_t base_len = (size_t)(slash - arg) + 1; /* keep trailing slash */
        if (base_len >= base_prefix_len) base_len = base_prefix_len - 1;
        strncpy(base_prefix, arg, base_len);
        base_prefix[base_len] = '\0';

        strncpy(name_prefix, slash + 1, name_prefix_len - 1);
        name_prefix[name_prefix_len - 1] = '\0';

        if (base_len == 1 && base_prefix[0] == '/') {
            strncpy(dir_path, "/", sizeof(dir_path) - 1);
            dir_path[sizeof(dir_path) - 1] = '\0';
        } else {
            size_t dir_len = base_len - 1; /* strip trailing slash */
            if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
            strncpy(dir_path, base_prefix, dir_len);
            dir_path[dir_len] = '\0';
        }
    }

    DirEntry entries[MAX_DIR_ENTRIES * MAX_DIRECT_BLOCKS];
    int count = 0;
    int ret = mfs_readdir(fs, dir_path, entries, &count);
    if (ret != MFS_OK) return 0;

    int found = 0;
    size_t prefix_len = strlen(name_prefix);
    for (int i = 0; i < count && found < max_matches; i++) {
        if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0)
            continue;
        if (strncmp(entries[i].name, name_prefix, prefix_len) != 0)
            continue;

        Inode inode;
        if (mfs_read_inode(fs, entries[i].inode_num, &inode) != MFS_OK)
            continue;
        if (inode.type != INODE_DIR)
            continue;

        strncpy(matches[found], entries[i].name, MAX_FILENAME - 1);
        matches[found][MAX_FILENAME - 1] = '\0';
        found++;
    }
    return found;
}

static int read_command_line(MicroFS *fs, char *line, size_t line_len,
                             char history[][MAX_LINE_LEN], int *history_count) {
    if (!isatty(STDIN_FILENO)) {
        if (!fgets(line, (int)line_len, stdin)) return 0;
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        return 1;
    }

    struct termios oldt, raw;
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) return 0;
    raw = oldt;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    g_saved_termios = oldt;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return 0;
    g_raw_mode_active = 1;

    line[0] = '\0';
    int len = 0;
    int hist_pos = *history_count;
    char current_line[MAX_LINE_LEN];
    current_line[0] = '\0';

    char prompt[640];
    build_prompt(fs, prompt, sizeof(prompt));
    printf("%s", prompt);
    fflush(stdout);

    for (;;) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            restore_terminal_mode();
            return 0;
        }

        if (c == '\r' || c == '\n') {
            printf("\n");
            break;
        }

        if (c == 3) { /* Ctrl+C: cancel current input line */
            line[0] = '\0';
            printf("^C\n");
            restore_terminal_mode();
            return 1;
        }

        if (c == 4) { /* Ctrl+D: exit shell */
            printf("\n");
            restore_terminal_mode();
            return 0;
        }

        if (c == 127 || c == 8) {
            if (len > 0) {
                line[--len] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (c == 12) { /* Ctrl+L */
            clear_and_repaint_header();
            redraw_input(prompt, line);
            continue;
        }

        if (c == 27) { /* Escape sequences (arrows) */
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
            if (seq[0] == '[' && seq[1] == 'A') { /* Up */
                if (*history_count > 0 && hist_pos > 0) {
                    if (hist_pos == *history_count) {
                        strncpy(current_line, line, sizeof(current_line) - 1);
                        current_line[sizeof(current_line) - 1] = '\0';
                    }
                    hist_pos--;
                    strncpy(line, history[hist_pos], line_len - 1);
                    line[line_len - 1] = '\0';
                    len = (int)strlen(line);
                    redraw_input(prompt, line);
                }
            } else if (seq[0] == '[' && seq[1] == 'B') { /* Down */
                if (hist_pos < *history_count) {
                    hist_pos++;
                    if (hist_pos == *history_count) {
                        strncpy(line, current_line, line_len - 1);
                        line[line_len - 1] = '\0';
                    } else {
                        strncpy(line, history[hist_pos], line_len - 1);
                        line[line_len - 1] = '\0';
                    }
                    len = (int)strlen(line);
                    redraw_input(prompt, line);
                }
            }
            continue;
        }

        if (c == '\t') { /* Tab completion for command name */
            size_t cmd_len = strcspn(line, " \t");
            if (cmd_len == strlen(line)) {
                /* Complete command name when typing first token. */
                char token[64];
                if (cmd_len >= sizeof(token)) cmd_len = sizeof(token) - 1;
                strncpy(token, line, cmd_len);
                token[cmd_len] = '\0';

                size_t matched_idx[64];
                int matches = collect_command_matches(token, matched_idx,
                    sizeof(matched_idx) / sizeof(matched_idx[0]));

                if (matches == 0) {
                    printf("\a");
                    fflush(stdout);
                    continue;
                }

                if (matches == 1) {
                    snprintf(line, line_len, "%s ", k_shell_commands[matched_idx[0]]);
                    len = (int)strlen(line);
                    redraw_input(prompt, line);
                } else {
                    char lcp[64];
                    longest_common_prefix(lcp, sizeof(lcp), matched_idx, matches);
                    if (strlen(lcp) > strlen(token)) {
                        snprintf(line, line_len, "%s", lcp);
                        len = (int)strlen(line);
                    }
                    printf("\n");
                    for (int i = 0; i < matches; i++) {
                        printf("%s  ", k_shell_commands[matched_idx[i]]);
                    }
                    printf("\n");
                    redraw_input(prompt, line);
                }
                continue;
            }

            /* Path completion for: cd <arg> */
            char cmd[32];
            size_t cpy_len = cmd_len >= sizeof(cmd) ? sizeof(cmd) - 1 : cmd_len;
            strncpy(cmd, line, cpy_len);
            cmd[cpy_len] = '\0';

            if (strcmp(cmd, "cd") == 0) {
                const char *p = line + cmd_len;
                while (*p == ' ' || *p == '\t') p++;
                size_t arg_offset = (size_t)(p - line);

                size_t arg_len = strcspn(p, " \t");
                if (p[arg_len] != '\0') continue; /* only complete first argument */

                char arg[512];
                if (arg_len >= sizeof(arg)) arg_len = sizeof(arg) - 1;
                strncpy(arg, p, arg_len);
                arg[arg_len] = '\0';

                char matches[MAX_CD_MATCHES][MAX_FILENAME];
                char base_prefix[512], name_prefix[MAX_FILENAME];
                int mcount = collect_cd_matches(fs, arg, matches, MAX_CD_MATCHES,
                                               base_prefix, sizeof(base_prefix),
                                               name_prefix, sizeof(name_prefix));
                if (mcount == 0) {
                    printf("\a");
                    fflush(stdout);
                    continue;
                }

                char oldline[MAX_LINE_LEN];
                strncpy(oldline, line, sizeof(oldline) - 1);
                oldline[sizeof(oldline) - 1] = '\0';

                if (mcount == 1) {
                    snprintf(line, line_len, "%.*s%s%s/", (int)arg_offset, oldline,
                             base_prefix, matches[0]);
                    len = (int)strlen(line);
                    redraw_input(prompt, line);
                } else {
                    char lcp[MAX_FILENAME];
                    longest_common_prefix_strs(lcp, sizeof(lcp), matches, mcount);
                    if (strlen(lcp) > strlen(name_prefix)) {
                        snprintf(line, line_len, "%.*s%s%s", (int)arg_offset, oldline,
                                 base_prefix, lcp);
                        len = (int)strlen(line);
                    }

                    printf("\n");
                    for (int i = 0; i < mcount; i++)
                        printf("%s/  ", matches[i]);
                    printf("\n");
                    redraw_input(prompt, line);
                }
            }
            continue;
        }

        if (isprint(c) && len < (int)line_len - 1) {
            line[len++] = (char)c;
            line[len] = '\0';
            putchar((char)c);
            fflush(stdout);
        }
    }

    restore_terminal_mode();
    return 1;
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
    install_terminal_restore_handlers();
    print_shell_intro();

    char line[MAX_LINE_LEN];
    char *argv[MAX_ARGS];
    char history[MAX_HISTORY][MAX_LINE_LEN];
    int history_count = 0;

    while (1) {
        if (!read_command_line(fs, line, sizeof(line), history, &history_count)) break;
        int len = (int)strlen(line);
        if (len == 0) continue;

        if (history_count == 0 || strcmp(history[history_count - 1], line) != 0) {
            if (history_count < MAX_HISTORY) {
                strncpy(history[history_count], line, MAX_LINE_LEN - 1);
                history[history_count][MAX_LINE_LEN - 1] = '\0';
                history_count++;
            } else {
                for (int i = 1; i < MAX_HISTORY; i++)
                    strcpy(history[i - 1], history[i]);
                strncpy(history[MAX_HISTORY - 1], line, MAX_LINE_LEN - 1);
                history[MAX_HISTORY - 1][MAX_LINE_LEN - 1] = '\0';
            }
        }

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
        else if (strcmp(cmd, "clear") == 0) {
            clear_and_repaint_header();
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
