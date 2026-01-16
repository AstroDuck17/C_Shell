#define _POSIX_C_SOURCE 200809L
#include "intrinsics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Make HIST_MAX a compile-time constant so we can use it to size file-scope arrays */
#ifndef HIST_MAX
#define HIST_MAX 15
#endif

/* History persistence file name within $HOME */
static const char *HIST_FILENAME = ".osh_history";

/* In-memory history: oldest..newest */
static char *history_buf[HIST_MAX];
static size_t history_count = 0;
static int history_dirty = 0;

/* prev cwd used by '-' argument */
static char prev_cwd[PATH_MAX+1];
static int prev_cwd_set = 0;

/* helpers */
static char *join_path_home(const char *name) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    size_t n = strlen(home) + 1 + strlen(name) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/%s", home, name);
    return p;
}

static void free_history_in_memory(void) {
    for (size_t i = 0; i < history_count; ++i) {
        free(history_buf[i]);
        history_buf[i] = NULL;
    }
    history_count = 0;
    history_dirty = 0;
}

static int load_history_from_file(void) {
    char *path = join_path_home(HIST_FILENAME);
    if (!path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) {
        free(path);
        /* silent: history file may not exist */
        return 0;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    /* We'll keep only the last HIST_MAX entries (oldest..newest) */
    char *tmp_buf[HIST_MAX * 2]; /* temporary flexible buffer */
    size_t tmp_count = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        /* trim trailing newline */
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) continue;
        tmp_buf[tmp_count++] = strdup(line);
        if (tmp_count >= sizeof(tmp_buf)/sizeof(tmp_buf[0])) break;
    }
    free(line);
    fclose(f);
    free(path);

    /* Keep only the last HIST_MAX entries */
    size_t start = 0;
    if (tmp_count > HIST_MAX) start = tmp_count - HIST_MAX;
    for (size_t i = start; i < tmp_count; ++i) {
        history_buf[history_count++] = tmp_buf[i]; /* transfer ownership */
    }
    /* free any earlier entries from tmp_buf[] */
    for (size_t i = 0; i < start; ++i) free(tmp_buf[i]);
    history_dirty = 0;
    return 0;
}

static int save_history_to_file(void) {
    char *path = join_path_home(HIST_FILENAME);
    if (!path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) {
        free(path);
        return -1;
    }
    for (size_t i = 0; i < history_count; ++i) {
        fprintf(f, "%s\n", history_buf[i]);
    }
    fclose(f);
    free(path);
    history_dirty = 0;
    return 0;
}

/* return 1 if the provided line contains an atomic command whose command name
 * is exactly "log" (the first name of any atomic). We only need to detect the
 * atomic names, not arbitrary occurrences of "log".
 */
static int line_contains_atomic_log(const char *line) {
    if (!line) return 0;
    const char *s = line;
    while (*s) {
        /* skip whitespace */
        while (*s && isspace((unsigned char)*s)) ++s;
        if (!*s) break;
        /* skip separators */
        if (*s == '|' || *s == ';' || *s == '&') {
            ++s;
            continue;
        }
        /* start of an atomic command; the first token is its name */
        const char *start = s;
        while (*s && !isspace((unsigned char)*s) && *s != '|' && *s != ';' && *s != '&' && *s != '<' && *s != '>' ) ++s;
        size_t len = (size_t)(s - start);
        if (len == 3 && strncmp(start, "log", 3) == 0) {
            return 1;
        }
        /* skip rest of this atomic until separator or end */
        while (*s && *s != '|' && *s != ';' && *s != '&') ++s;
    }
    return 0;
}

/* Add to history (older->newer). Returns 1 if added, 0 if skipped, -1 on error */
int intrinsics_record_command(const char *line) {
    if (!line) return 0;
    /* check atomic 'log' presence */
    if (line_contains_atomic_log(line)) return 0;

    /* exact duplicate prevention vs last stored (most recent) */
    if (history_count > 0 && strcmp(history_buf[history_count - 1], line) == 0) {
        return 0; /* identical to previous, do not store */
    }

    /* If the command exists anywhere in history already, remove that occurrence
     * so we keep history entries unique and move this command to the newest slot.
     */
    for (size_t i = 0; i < history_count; ++i) {
        if (strcmp(history_buf[i], line) == 0) {
            free(history_buf[i]);
            /* shift left entries after i */
            if (i + 1 < history_count) {
                memmove(&history_buf[i], &history_buf[i+1], sizeof(char*) * (history_count - i - 1));
            }
            history_count--;
            break;
        }
    }

    char *copy = strdup(line);
    if (!copy) return -1;

    if (history_count == HIST_MAX) {
        /* drop oldest */
        free(history_buf[0]);
        /* shift */
        memmove(&history_buf[0], &history_buf[1], sizeof(char*) * (HIST_MAX - 1));
        history_buf[HIST_MAX - 1] = copy;
    } else {
        history_buf[history_count++] = copy;
    }
    history_dirty = 1;
    /* persist immediately so history survives crashes between commands */
    if (save_history_to_file() != 0) {
        /* non-fatal, but report */
        return 1;
    }
    return 1;
}

/* Public init/cleanup */
int intrinsics_init(void) {
    free_history_in_memory();
    if (load_history_from_file() != 0) {
        /* continue even on load failure */
    }
    prev_cwd_set = 0;
    prev_cwd[0] = '\0';
    return 0;
}

void intrinsics_cleanup(void) {
    /* persist if dirty */
    if (history_dirty) save_history_to_file();
    free_history_in_memory();
}

/* Utility: split a line into tokens by whitespace (returns dynamic array)
 * The caller must free the returned array and its contents.
 * The array is NULL-terminated.
 */
static char **tokenize_whitespace(const char *line, size_t *out_count) {
    if (!line) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    size_t cap = 8;
    size_t n = 0;
    char **arr = malloc(sizeof(char*) * cap);
    if (!arr) return NULL;

    const char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) ++p;
        size_t len = (size_t)(p - start);
        char *tok = malloc(len + 1);
        if (!tok) {
            /* cleanup */
            for (size_t i = 0; i < n; ++i) free(arr[i]);
            free(arr);
            return NULL;
        }
        memcpy(tok, start, len);
        tok[len] = '\0';
        if (n + 1 >= cap) {
            cap *= 2;
            char **tmp = realloc(arr, sizeof(char*) * cap);
            if (!tmp) {
                for (size_t i = 0; i < n; ++i) free(arr[i]);
                free(tok);
                free(arr);
                return NULL;
            }
            arr = tmp;
        }
        arr[n++] = tok;
    }
    /* null-terminate */
    arr[n] = NULL;
    if (out_count) *out_count = n;
    return arr;
}

/* ----------- hop implementation ----------- */
/* Attempt chdir(target). On success update prev_cwd to old_cwd and return 0.
 * On failure print "No such directory!" and return -1.
 */
static int do_chdir_and_update_prev(const char *target) {
    char oldcwd[PATH_MAX+1];
    if (!getcwd(oldcwd, sizeof(oldcwd))) {
        oldcwd[0] = '\0';
    }
    if (chdir(target) != 0) {
        printf("No such directory!\n");
        return -1;
    }
    /* update prev */
    if (oldcwd[0] != '\0') {
        strncpy(prev_cwd, oldcwd, sizeof(prev_cwd)-1);
        prev_cwd[sizeof(prev_cwd)-1] = '\0';
        prev_cwd_set = 1;
    }
    return 0;
}

/* Process hop arguments sequentially */
int handle_hop_args(char **args, size_t nargs) {
    if (nargs == 0) {
        /* treat as "~" */
        const char *home = getenv("HOME");
        if (!home) {
            printf("No such directory!\n");
            return 1;
        }
        do_chdir_and_update_prev(home);
        return 1;
    }
    for (size_t i = 0; i < nargs; ++i) {
        const char *a = args[i];
        if (strcmp(a, "~") == 0) {
            const char *home = getenv("HOME");
            if (!home) {
                printf("No such directory!\n");
                continue;
            }
            do_chdir_and_update_prev(home);
        } else if (strcmp(a, ".") == 0) {
            /* do nothing */
            continue;
        } else if (strcmp(a, "..") == 0) {
            do_chdir_and_update_prev("..");
        } else if (strcmp(a, "-") == 0) {
            if (!prev_cwd_set) {
                printf("No such directory!\n");
                continue;
            }
            do_chdir_and_update_prev(prev_cwd);
        } else {
            /* name: relative or absolute path */
            do_chdir_and_update_prev(a);
        }
    }
    return 1;
}

/* ----------- reveal implementation ----------- */

static int cmp_str_ascii(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa, sb); /* strcmp works for ASCII lexicographic order */
}

/* List directory 'dirpath'. flags: show_all (include .hidden), line_by_line.
 * Returns 1 on handled, 0 on syntax error (prints message), -1 on other error.
 */
static int list_directory(const char *dirpath, int show_all, int line_by_line) {
    DIR *d = opendir(dirpath);
    if (!d) {
        printf("No such directory!\n");
        return 1;
    }
    struct dirent *ent;
    size_t cap = 64;
    size_t n = 0;
    char **names = malloc(sizeof(char*) * cap);
    if (!names) { closedir(d); return -1; }
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (!show_all) {
            if (name[0] == '.') continue;
        }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (n + 1 >= cap) {
            cap *= 2;
            char **tmp = realloc(names, sizeof(char*) * cap);
            if (!tmp) break;
            names = tmp;
        }
        names[n++] = strdup(name);
    }
    closedir(d);
    if (n == 0) {
        /* print empty line */
        putchar('\n');
        for (size_t i = 0; i < n; ++i) free(names[i]);
        free(names);
        return 1;
    }
    /* sort */
    qsort(names, n, sizeof(char*), cmp_str_ascii);
    if (line_by_line) {
        for (size_t i = 0; i < n; ++i) {
            printf("%s\n", names[i]);
            free(names[i]);
        }
    } else {
        /* print space-separated on single line */
        for (size_t i = 0; i < n; ++i) {
            if (i) putchar(' ');
            fputs(names[i], stdout);
            free(names[i]);
        }
        putchar('\n');
    }
    free(names);
    return 1;
}

/* Parse reveal args and dispatch */
int handle_reveal_args(char **args, size_t nargs) {
    int show_all = 0;
    int line_by_line = 0;
    char *dir_arg = NULL;
    size_t nonflag_count = 0;

    for (size_t i = 0; i < nargs; ++i) {
        char *t = args[i];
        if (t[0] == '-' && t[1] != '\0') {
            /* flags cluster: e.g., -la, -aaaa */
            for (size_t j = 1; t[j]; ++j) {
                if (t[j] == 'a') show_all = 1;
                else if (t[j] == 'l') line_by_line = 1;
                else {
                    /* invalid flag */
                    printf("reveal: Invalid Syntax!\n");
                    return 0;
                }
            }
        } else {
            /* non-flag argument -> directory indicator */
            nonflag_count++;
            if (nonflag_count > 1) {
                printf("reveal: Invalid Syntax!\n");
                return 0;
            }
            dir_arg = t;
        }
    }

    /* Determine directory path */
    char target[PATH_MAX+1];
    if (!dir_arg) {
        /* default: current working directory */
        if (!getcwd(target, sizeof(target))) {
            printf("No such directory!\n");
            return 1;
        }
    } else if (strcmp(dir_arg, "~") == 0) {
        const char *home = getenv("HOME");
        if (!home) { printf("No such directory!\n"); return 1; }
        strncpy(target, home, sizeof(target)-1);
        target[sizeof(target)-1] = '\0';
    } else if (strcmp(dir_arg, ".") == 0) {
        if (!getcwd(target, sizeof(target))) { printf("No such directory!\n"); return 1; }
    } else if (strcmp(dir_arg, "..") == 0) {
        /* use relative ".." against CWD */
        strncpy(target, "..", sizeof(target)-1);
        target[sizeof(target)-1] = '\0';
    } else if (strcmp(dir_arg, "-") == 0) {
        if (!prev_cwd_set) { printf("No such directory!\n"); return 1; }
        strncpy(target, prev_cwd, sizeof(target)-1);
        target[sizeof(target)-1] = '\0';
    } else {
        /* name path (relative/absolute) */
        strncpy(target, dir_arg, sizeof(target)-1);
        target[sizeof(target)-1] = '\0';
    }

    /* If dir_arg is ".." or a relative path, we need to form a path relative to cwd.
     * list_directory() uses opendir(path) which accepts relative paths, so it's okay.
     */
    return list_directory(target, show_all, line_by_line);
}

/* ----------- log implementation ----------- */

static void print_history_oldest_to_newest(void) {
    for (size_t i = 0; i < history_count; ++i) {
        printf("%s\n", history_buf[i]);
    }
}

/* handle log command. out_reexec_cmd will be set if we need to re-execute; caller will free.
 * returns codes same as intrinsics_handle (1,2,-1)
 */
int handle_log_args(char **args, size_t nargs, char **out_reexec_cmd) {
    if (nargs == 0) {
        /* print stored commands oldest->newest */
        print_history_oldest_to_newest();
        return 1;
    }
    if (nargs == 1) {
        if (strcmp(args[0], "purge") == 0) {
            /* clear */
            free_history_in_memory();
            /* persist */
            save_history_to_file();
            return 1;
        } else {
            printf("log: Invalid Syntax!\n");
            return 1;
        }
    }
    /* Allow "log execute <index>" possibly followed by more tokens (e.g. pipes).
     * If there are trailing tokens, compose a new command string:
     *   "<stored_cmd> <trailing tokens...>"
     * and return 2 with out_reexec_cmd set to that malloc'd string.
     */
    if (nargs >= 2 && strcmp(args[0], "execute") == 0) {
        /* parse index */
        char *endptr = NULL;
        long idx = strtol(args[1], &endptr, 10);
        if (endptr == args[1] || *endptr != '\0' || idx <= 0) {
            printf("log: Invalid Syntax!\n");
            return 1;
        }
        /* index is 1-based newest->oldest */
        if (history_count == 0) {
            printf("log: Invalid Syntax!\n");
            return 1;
        }
        if ((size_t)idx > history_count) {
            printf("log: Invalid Syntax!\n");
            return 1;
        }
        size_t pos = history_count - (size_t)idx; /* newest -> index 1 */
        const char *stored_cmd = history_buf[pos];
        if (!stored_cmd) {
            printf("log: Invalid Syntax!\n");
            return 1;
        }
        /* If there are no trailing tokens, just return the stored command. */
        if (nargs == 2) {
            *out_reexec_cmd = strdup(stored_cmd);
            if (!*out_reexec_cmd) return -1;
            return 2;
        }
        /* Build "<stored_cmd> <args[2]> <args[3]> ..." */
        size_t needed = strlen(stored_cmd) + 1; /* for possible space and NUL */
        for (size_t i = 2; i < nargs; ++i) {
            needed += strlen(args[i]) + 1; /* space or terminator */
        }
        char *buf = malloc(needed);
        if (!buf) return -1;
        buf[0] = '\0';
        strcat(buf, stored_cmd);
        for (size_t i = 2; i < nargs; ++i) {
            strcat(buf, " ");
            strcat(buf, args[i]);
        }
        *out_reexec_cmd = buf;
        return 2;
    }
    /* anything else is syntax error */
    printf("log: Invalid Syntax!\n");
    return 1;
}

/* Top-level handle function */
int intrinsics_handle(const char *line, char **out_reexec_cmd) {
    if (!line) return 0;
    size_t ntoks = 0;
    char **toks = tokenize_whitespace(line, &ntoks);
    if (!toks) return 0;

    if (ntoks == 0) { free(toks); return 0; }

    /* determine first token (command name) */
    const char *cmd = toks[0];

    if (strcmp(cmd, "hop") == 0) {
        /* hop: arguments are tokens[1..] */
        int res = handle_hop_args(&toks[1], ntoks > 0 ? ntoks - 1 : 0);
        for (size_t i = 0; i < ntoks; ++i) free(toks[i]);
        free(toks);
        if (res < 0) return -1;
        return 1;
    } else if (strcmp(cmd, "reveal") == 0) {
        int res = handle_reveal_args(&toks[1], ntoks > 0 ? ntoks - 1 : 0);
        for (size_t i = 0; i < ntoks; ++i) free(toks[i]);
        free(toks);
        if (res < 0) return -1;
        return 1;
    } else if (strcmp(cmd, "log") == 0) {
        int res = handle_log_args(&toks[1], ntoks > 0 ? ntoks - 1 : 0, out_reexec_cmd);
        for (size_t i = 0; i < ntoks; ++i) free(toks[i]);
        free(toks);
        if (res == -1) return -1;
        return res; /* 1 or 2 */
    }

    /* not an intrinsic */
    for (size_t i = 0; i < ntoks; ++i) free(toks[i]);
    free(toks);
    return 0;
}
