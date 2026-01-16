#define _POSIX_C_SOURCE 200809L
#include "exec.h"
#include "intrinsics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

bg_job *find_job_by_id(int id);
bg_job *unlink_job(bg_job *job);

int do_hop(char **argv) {
    size_t nargs = 0;
    while (argv[nargs]) nargs++;
    return handle_hop_args(argv + 1, nargs - 1);
}

int do_reveal(char **argv) {
    size_t nargs = 0;
    while (argv[nargs]) nargs++;
    return handle_reveal_args(argv + 1, nargs - 1);
}

int do_log(char **argv) {
    size_t nargs = 0;
    while (argv[nargs]) nargs++;
    char *dummy = NULL;
    int res = handle_log_args(argv + 1, nargs - 1, &dummy);
    if (res == 2) {
        exec_run_line(dummy);
        free(dummy);
    }
    return res;
}

/* Foreground process group id, used by SIGINT handler */
static pid_t shell_pid = 0;
volatile sig_atomic_t fg_pgid = 0;

static void sigint_handler(int signo) {
    (void)signo;
    if (fg_pgid > 0) {
        /* send SIGINT to the foreground process group */
        kill(-fg_pgid, SIGINT);
    }
}

/* SIGTSTP handler to send SIGTSTP to foreground process group */
static void sigtstp_handler(int signo) {
    (void)signo;
    if (fg_pgid > 0) {
        kill(-fg_pgid, SIGTSTP);
    }
}

/* Delimiters for tokenization */
static int is_delim_char(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '|' || c == '<' || c == '>' || c == ';' || c == '&');
}

/* Tokenize input into tokens where special symbols are separate tokens:
 * tokens are malloc'd strings and array is NULL-terminated.
 * Caller must free tokens and each token string.
 */
static char **tokenize_special(const char *line, size_t *out_count) {
    if (!line) { if (out_count) *out_count = 0; return NULL; }
    size_t cap = 16, n = 0;
    char **arr = malloc(sizeof(char*) * cap);
    if (!arr) return NULL;

    const char *p = line;
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (!*p) break;

        /* check special tokens */
        if (*p == '|') {
            char *t = strdup("|");
            if (!t) goto fail;
            arr[n++] = t;
            ++p;
        } else if (*p == '<') {
            char *t = strdup("<");
            if (!t) goto fail;
            arr[n++] = t;
            ++p;
        } else if (*p == '>') {
            if (p[1] == '>') {
                char *t = strdup(">>");
                if (!t) goto fail;
                arr[n++] = t;
                p += 2;
            } else {
                char *t = strdup(">");
                if (!t) goto fail;
                arr[n++] = t;
                ++p;
            }
        } else if (*p == ';') {
            char *t = strdup(";");
            if (!t) goto fail;
            arr[n++] = t;
            ++p;
        } else if (*p == '&') {
            char *t = strdup("&");
            if (!t) goto fail;
            arr[n++] = t;
            ++p;
        } else {
            /* name token: read until delim char */
            const char *start = p;
            while (*p && !is_delim_char(*p)) ++p;
            size_t len = (size_t)(p - start);
            char *t = malloc(len + 1);
            if (!t) goto fail;
            memcpy(t, start, len);
            t[len] = '\0';
            arr[n++] = t;
        }

        if (n + 2 >= cap) {
            cap *= 2;
            char **tmp = realloc(arr, sizeof(char*) * cap);
            if (!tmp) goto fail;
            arr = tmp;
        }
    }

    arr[n] = NULL;
    if (out_count) *out_count = n;
    return arr;

fail:
    for (size_t i = 0; i < n; ++i) free(arr[i]);
    free(arr);
    if (out_count) *out_count = 0;
    return NULL;
}

/* Free token array returned by tokenize_special */
static void free_tokens(char **toks) {
    if (!toks) return;
    for (size_t i = 0; toks[i]; ++i) free(toks[i]);
    free(toks);
}

/* A command node for a single stage in a pipeline */
typedef struct {
    char **argv;     /* NULL-terminated argv (malloc'd) */
    char *infile;    /* or NULL */
    char *outfile;   /* or NULL */
    int append;      /* 1 for >>, 0 for > */
} CmdNode;

/* Free a CmdNode */
static void free_cmdnode(CmdNode *c) {
    if (!c) return;
    if (c->argv) {
        for (size_t i = 0; c->argv[i]; ++i) free(c->argv[i]);
        free(c->argv);
    }
    free(c->infile);
    free(c->outfile);
    c->argv = NULL;
    c->infile = NULL;
    c->outfile = NULL;
}

/* Build commands until first ';' or '&' token (first cmd_group only) */
static int build_pipeline_from_tokens(char **toks, size_t ntoks, CmdNode **out_cmds, size_t *out_ncmds) {
    if (!toks) return -1;
    /* Use all tokens in this command group */
    size_t limit = ntoks;
    /* Now split by '|' into command token ranges */
    size_t start = 0;
    size_t cmdcap = 8, cmdcount = 0;
    CmdNode *cmds = malloc(sizeof(CmdNode) * cmdcap);
    if (!cmds) return -1;

    while (start < limit) {
        /* find next '|' or end */
        size_t end = start;
        while (end < limit && strcmp(toks[end], "|") != 0) ++end;
        /* parse tokens [start, end) into one CmdNode */
        CmdNode node;
        node.argv = NULL;
        node.infile = NULL;
        node.outfile = NULL;
        node.append = 0;

        /* build argv: first pass count arguments that are not redirections */
        size_t argc = 0;
        for (size_t i = start; i < end; ++i) {
            if (strcmp(toks[i], "<") == 0 || strcmp(toks[i], ">") == 0 || strcmp(toks[i], ">>") == 0) {
                ++i; /* skip next token when counting; assume valid syntax from parser */
                continue;
            } else {
                ++argc;
            }
        }
        /* allocate argv array (argc + 1) */
        node.argv = malloc(sizeof(char*) * (argc + 1));
        if (!node.argv) { free_cmdnode(&node); goto fail_nodes; }
        size_t ai = 0;
        size_t i = start;
        while (i < end) {
            if (strcmp(toks[i], "<") == 0) {
                /* input */
                if (i + 1 >= end) { /* malformed */ free_cmdnode(&node); goto fail_nodes; }
                free(node.infile);
                node.infile = strdup(toks[i+1]);
                i += 2;
            } else if (strcmp(toks[i], ">") == 0) {
                if (i + 1 >= end) { free_cmdnode(&node); goto fail_nodes; }
                free(node.outfile);
                node.outfile = strdup(toks[i+1]);
                node.append = 0;
                i += 2;
            } else if (strcmp(toks[i], ">>") == 0) {
                if (i + 1 >= end) { free_cmdnode(&node); goto fail_nodes; }
                free(node.outfile);
                node.outfile = strdup(toks[i+1]);
                node.append = 1;
                i += 2;
            } else {
                node.argv[ai++] = strdup(toks[i]);
                i += 1;
            }
        }
        node.argv[ai] = NULL;
        /* Store command */
        if (cmdcount + 1 >= cmdcap) {
            cmdcap *= 2;
            CmdNode *tmp = realloc(cmds, sizeof(CmdNode) * cmdcap);
            if (!tmp) { free_cmdnode(&node); goto fail_nodes; }
            cmds = tmp;
        }
        cmds[cmdcount++] = node;

        /* advance past '|' */
        start = end + 1;
    }

    *out_cmds = cmds;
    *out_ncmds = cmdcount;
    return 0;

fail_nodes:
    for (size_t k = 0; k < cmdcount; ++k) free_cmdnode(&cmds[k]);
    free(cmds);
    return -1;
}

/* Close an array of pipes (n pipes => array size n x 2) */
static void close_pipes(int (*pipes)[2], size_t n) {
    if (!pipes) return;
    for (size_t i = 0; i < n; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

/* Run the parsed pipeline of commands.
 * Returns 0 on normal completion, -1 on failure (alloc/parse).
 */
static int run_cmd_pipeline(CmdNode *cmds, size_t ncmds, const char *leader_cmd) {
    if (ncmds == 0) return 0;

    /* Create pipes: ncmds-1 pipes */
    size_t npipes = (ncmds > 1) ? ncmds - 1 : 0;
    int (*pipes)[2] = NULL;
    if (npipes) {
        pipes = malloc(sizeof(int[2]) * npipes);
        if (!pipes) return -1;
        for (size_t i = 0; i < npipes; ++i) {
            if (pipe(pipes[i]) != 0) {
                /* close previous pipes */
                for (size_t j = 0; j < i; ++j) {
                    close(pipes[j][0]); close(pipes[j][1]);
                }
                free(pipes);
                return -1;
            }
        }
    }

    pid_t *pids = malloc(sizeof(pid_t) * ncmds);
    if (!pids) { if (pipes) close_pipes(pipes, npipes); free(pipes); return -1; }

    pid_t leader = -1;

    for (size_t i = 0; i < ncmds; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            /* fork failed: close pipes and continue (attempt to run remaining?) */
            pids[i] = -1;
            continue;
        } else if (pid == 0) {
            /* Child */
            /* If not first, redirect stdin from previous pipe read end */
            if (i > 0) {
                if (dup2(pipes[i-1][0], STDIN_FILENO) < 0) { _exit(1); }
            }
            /* If not last, redirect stdout to current pipe write end */
            if (i < ncmds - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) { _exit(1); }
            }
            /* Close all pipe fds in child */
            if (pipes) {
                for (size_t j = 0; j < npipes; ++j) {
                    close(pipes[j][0]); close(pipes[j][1]);
                }
            }

            /* Handle redirections in child */
            if (cmds[i].infile) {
                int infd = open(cmds[i].infile, O_RDONLY);
                if (infd < 0) {
                    /* per spec */
                    printf("No such file or directory\n");
                    _exit(1);
                }
                if (dup2(infd, STDIN_FILENO) < 0) { close(infd); _exit(1); }
                close(infd);
            }
            if (cmds[i].outfile) {
                int flags = O_WRONLY | O_CREAT;
                if (cmds[i].append) flags |= O_APPEND;
                else flags |= O_TRUNC;
                int outfd = open(cmds[i].outfile, flags, 0644);
                if (outfd < 0) {
                    printf("Unable to create file for writing\n");
                    _exit(1);
                }
                if (dup2(outfd, STDOUT_FILENO) < 0) { close(outfd); _exit(1); }
                close(outfd);
            }

            /* Exec the command if there is an argv */
            if (!cmds[i].argv || !cmds[i].argv[0]) {
                /* nothing to exec */
                _exit(0);
            }
            if (strcmp(cmds[i].argv[0], "hop") == 0) {
                do_hop(cmds[i].argv);
                _exit(0);
            } else if (strcmp(cmds[i].argv[0], "reveal") == 0) {
                do_reveal(cmds[i].argv);
                _exit(0);
            } else if (strcmp(cmds[i].argv[0], "log") == 0) {
                do_log(cmds[i].argv);
                _exit(0);
            } else if (strcmp(cmds[i].argv[0], "activities") == 0) {
                print_activities();
                _exit(0);
            } else if (strcmp(cmds[i].argv[0], "ping") == 0) {
                size_t nargs = 0;
                while (cmds[i].argv[nargs]) nargs++;
                if (nargs != 3) {
                    printf("Invalid syntax!\n");
                    _exit(1);
                }
                char *endptr;
                long pid = strtol(cmds[i].argv[1], &endptr, 10);
                if (*endptr != '\0') {
                    printf("Invalid syntax!\n");
                    _exit(1);
                }
                long sig = strtol(cmds[i].argv[2], &endptr, 10);
                if (*endptr != '\0') {
                    printf("Invalid syntax!\n");
                    _exit(1);
                }
                int actual_sig = (int)(sig % 32);
                if (actual_sig <= 0) actual_sig += 32;
                if (kill((pid_t)pid, actual_sig) < 0) {
                    if (errno == ESRCH) printf("No such process found\n");
                    else perror("kill");
                } else {
                    printf("Sent signal %ld to process with pid %ld\n", sig, pid);
                }
                _exit(0);
            } else if (strcmp(cmds[i].argv[0], "fg") == 0 || strcmp(cmds[i].argv[0], "bg") == 0) {
                int is_fg = (strcmp(cmds[i].argv[0], "fg") == 0);
                size_t nargs = 0;
                while (cmds[i].argv[nargs]) nargs++;
                int job_num = -1;
                if (nargs == 1) {
                    if (!job_list) {
                        printf("No such job\n");
                        _exit(1);
                    }
                    job_num = job_list->job_id;
                } else if (nargs == 2) {
                    char *endptr;
                    long v = strtol(cmds[i].argv[1], &endptr, 10);
                    if (*endptr != '\0') {
                        printf("No such job\n");
                        _exit(1);
                    }
                    job_num = (int)v;
                } else {
                    printf("Invalid syntax!\n");
                    _exit(1);
                }
                bg_job *job = find_job_by_id(job_num);
                if (!job) {
                    printf("No such job\n");
                    _exit(1);
                }
                if (is_fg) {
                    if (job->stopped) {
                        if (kill(job->pid, SIGCONT) < 0) perror("kill");
                        job->stopped = 0;
                    }
                    unlink_job(job);
                    printf("%s\n", job->command);
                    fflush(stdout);
                    fg_pgid = job->pid;
                    int st;
                    pid_t w = waitpid(-job->pid, &st, WUNTRACED);
                    if (w > 0 && WIFSTOPPED(st)) {
                        add_stopped_job(job->pid, job->command);
                    }
                    fg_pgid = 0;
                    free(job->command);
                    free(job);
                } else {
                    if (!job->stopped) {
                        printf("Job already running\n");
                    } else {
                        if (kill(job->pid, SIGCONT) < 0) {
                            if (errno == ESRCH) printf("No such job\n");
                            else perror("kill");
                        } else {
                            job->stopped = 0;
                            printf("[%d] %s &\n", job->job_id, job->command);
                        }
                    }
                }
                _exit(0);
            } else {
                execvp(cmds[i].argv[0], cmds[i].argv);
                printf("Command not found!\n");
                _exit(127);
            }
        } else {
            /* Parent */
            pids[i] = pid;
            if (leader == -1) leader = pid;
            /* put child into leader's process group */
            if (setpgid(pid, leader) != 0) {
                /* ignore errors */
            }
            /* parent continues to spawn next */
        }
    }

    /* Parent: close all pipe fds (it doesn't use them) */
    if (pipes) {
        for (size_t j = 0; j < npipes; ++j) {
            close(pipes[j][0]); close(pipes[j][1]);
        }
        free(pipes);
    }

    /* Set foreground pgid for signal handler */
    if (leader > 0) fg_pgid = leader;

    /* Non-blocking wait loop: poll stdin for EOF/EOT while reaping children.
     * This allows immediate detection of Ctrl-D even while a foreground pipeline
     * is running. If EOF/EOT is seen, call handle_eof_exit() which will cleanup.
     */
    {
        int remaining = 0;
        for (size_t i = 0; i < ncmds; ++i) if (pids[i] > 0) remaining++;
        int status;
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;

        while (remaining > 0) {
            /* Check for any child state changes without blocking */
            for (size_t i = 0; i < ncmds; ++i) {
                if (pids[i] <= 0) continue;
                pid_t w = waitpid(pids[i], &status, WNOHANG | WUNTRACED);
                if (w == 0) continue; /* still running */
                if (w == -1) {
                    /* error; treat as gone */
                    pids[i] = -1;
                    remaining--;
                    continue;
                }
                /* w > 0: child changed state */
                if (WIFSTOPPED(status)) {
                    /* Move entire pipeline to background as stopped */
                    add_stopped_job(leader, leader_cmd ? leader_cmd : (cmds[0].argv ? cmds[0].argv[0] : ""));
                    /* mark all pids as handled to break outer wait */
                    remaining = 0;
                    break;
                } else {
                    /* exited or signaled: mark this pid as handled */
                    pids[i] = -1;
                    remaining--;
                }
            }

            /* Poll stdin briefly to detect EOF/EOT */
            int pres = poll(&pfd, 1, 100); /* 100 ms */
            if (pres > 0) {
                if (pfd.revents & POLLIN) {
                    /* Attempt to read (non-destructive read) */
                    char buf[16];
                    ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
                    if (r == 0) {
                        /* EOF on terminal (Ctrl-D at empty line) */
                        handle_eof_exit(); /* does not return */
                    } else if (r > 0) {
                        /* If an explicit EOT char was sent (rare in canonical mode),
                         * detect it and exit. Otherwise, we consumed input that
                         * might belong to the user; best-effort: if EOT present exit.
                         */
                        for (ssize_t bi = 0; bi < r; ++bi) {
                            if ((unsigned char)buf[bi] == 4) { /* EOT */
                                handle_eof_exit();
                            }
                        }
                        /* In canonical mode this read returns pending line data
                         * (or part of it). We don't try to reinsert it; the
                         * typical case of Ctrl-D at empty line is handled above.
                         */
                    }
                } else if (pfd.revents & (POLLHUP | POLLERR)) {
                    /* treat as EOF */
                    handle_eof_exit();
                }
            }

            /* Small sleep to avoid busy-looping when nothing changes */
            if (remaining > 0) {
                /* If no child state change detected this iteration, sleep shortly */
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
                nanosleep(&ts, NULL);
            }
        }
        free(pids);
    }

    /* Clear foreground pgid */
    fg_pgid = 0;
    return 0;
}

/* Global job list */
bg_job *job_list = NULL;
static int next_job_id = 1;

void add_background_job(pid_t pid, const char *cmd) {
    bg_job *new_job = malloc(sizeof(bg_job));
    new_job->pid = pid;
    new_job->job_id = next_job_id++;
    new_job->command = strdup(cmd);
    new_job->stopped = 0;
    new_job->next = job_list;
    job_list = new_job;
    
    printf("[%d] %d\n", new_job->job_id, pid);
    fflush(stdout);
}

void add_stopped_job(pid_t pid, const char *cmd) {
    bg_job *new_job = malloc(sizeof(bg_job));
    new_job->pid = pid;
    new_job->job_id = next_job_id++;
    new_job->command = strdup(cmd);
    new_job->stopped = 1;
    new_job->next = job_list;
    job_list = new_job;
    printf("[%d] Stopped %s\n", new_job->job_id, new_job->command);
    fflush(stdout);
}

void check_background_jobs() {
    bg_job *job = job_list;
    bg_job *prev = NULL;
    int job_finished = 0;
    while (job != NULL) {
        int status;
        pid_t result = waitpid(job->pid, &status, WNOHANG);
        if (result > 0) {
            /* Print completion status */
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                printf("\n%s with pid %d exited normally\n", job->command, job->pid);
            } else {
                printf("\n%s with pid %d exited abnormally\n", job->command, job->pid);
            }
            fflush(stdout);
            /* Remove job from list */
            if (prev) {
                prev->next = job->next;
            } else {
                job_list = job->next;
            }
            bg_job *tmp = job;
            job = job->next;
            free(tmp->command);
            free(tmp);
            job_finished = 1;
        } else {
            prev = job;
            job = job->next;
        }
    }
    if (job_finished) {
        printf("\n");
        fflush(stdout);
    }
}

void execute_sequential_commands(char **commands, int count) {
    for (int i = 0; i < count; i++) {
        // Trim whitespace
        char *cmd = commands[i];
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        
        if (strlen(cmd) > 0) {
            // Execute each command in sequence
            execute_command(cmd);
            // Wait for completion before next command
            wait(NULL);
        }
    }
}

void execute_background_command(char *command) {
    pid_t pid = fork();
    
    if (pid == 0) {
        /* Child process */
        /* Create new process group */
        setpgid(0, 0);
        
        /* Redirect stdin to /dev/null */
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd != -1) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        
        /* Execute the command */
        execute_command(command);
        
        /* Exit with status based on command success */
        if (errno) {
            exit(1);
        }
        exit(0);
    } else if (pid > 0) {
        /* Parent process */
        setpgid(pid, pid);
        add_background_job(pid, command);
    } else {
        perror("fork failed");
    }
}

void execute_command(char *command) {
    pid_t pid = fork();
    
    if (pid == 0) {
        // Child process
        char *args[256];
        int arg_count = 0;
        
        // Parse command into arguments
        char *token = strtok(command, " \t\n");
        while (token != NULL && arg_count < 255) {
            args[arg_count++] = token;
            token = strtok(NULL, " \t\n");
        }
        args[arg_count] = NULL;
        
        // Execute command
        execvp(args[0], args);
        
        // If execvp returns, there was an error
        printf("Command not found!\n");
        exit(1);
    } else if (pid < 0) {
        perror("fork failed");
    }
}

/* Comparator for sorting bg_job pointers by command name */
static int cmp_bg_job(const void *a, const void *b) {
    const bg_job *const *pa = a;
    const bg_job *const *pb = b;
    return strcmp((*pa)->command, (*pb)->command);
}

/* Print activities: list all processes spawned by shell that are running or stopped */
void print_activities() {
    /* First, update job list, removing any terminated processes */
    bg_job *prev = NULL;
    bg_job *cur = job_list;
    while (cur) {
        int status;
        pid_t res = waitpid(cur->pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (res == 0) {
            /* still running or stopped; leave it */
            prev = cur;
            cur = cur->next;
            continue;
        } else if (res == -1) {
            /* error - assume process gone, remove it */
            bg_job *tmp = cur;
            if (prev) prev->next = cur->next; else job_list = cur->next;
            cur = cur->next;
            free(tmp->command); free(tmp);
            continue;
        } else {
            /* process changed state or exited; if exited, remove; if stopped, keep and mark */
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                bg_job *tmp = cur;
                if (prev) prev->next = cur->next; else job_list = cur->next;
                cur = cur->next;
                free(tmp->command); free(tmp);
                continue;
            } else {
                prev = cur;
                cur = cur->next;
                continue;
            }
        }
    }

    /* Collect remaining jobs into array for sorting */
    size_t cap = 8, n = 0;
    bg_job **arr = malloc(sizeof(bg_job*) * cap);
    bg_job *it = job_list;
    while (it) {
        if (n + 1 >= cap) { cap *= 2; arr = realloc(arr, sizeof(bg_job*) * cap); }
        arr[n++] = it;
        it = it->next;
    }
    /* Sort by command name lexicographically */
    qsort(arr, n, sizeof(bg_job*), cmp_bg_job);

    /* Print each as: [pid] : command_name - State */
    for (size_t i = 0; i < n; ++i) {
        const char *state = arr[i]->stopped ? "Stopped" : "Running";
        printf("[%d] : %s - %s\n", arr[i]->pid, arr[i]->command, state);
    }
    free(arr);
}

void kill_all_children(void) {
    bg_job *cur = job_list;
    while (cur) {
        kill(cur->pid, SIGKILL);
        cur = cur->next;
    }
    /* free job list */
    cur = job_list;
    while (cur) {
        bg_job *tmp = cur;
        cur = cur->next;
        free(tmp->command);
        free(tmp);
    }
    job_list = NULL;
}

/* Cleanup function run at process exit: kill children and print logout */
static void cleanup_on_exit(void) {
    /* kill children and free list */
    bg_job *cur = job_list;
    while (cur) {
        kill(cur->pid, SIGKILL);
        cur = cur->next;
    }
    /* free job list */
    cur = job_list;
    while (cur) {
        bg_job *tmp = cur;
        cur = cur->next;
        free(tmp->command);
        free(tmp);
    }
    job_list = NULL;
    /* Only print logout if this is the original shell process */
    if (getpid() == shell_pid) {
        printf("\nlogout\n");
        fflush(stdout);
    }
}

void init_job_list() {
    job_list = NULL;
    next_job_id = 1;
    shell_pid = getpid();
    /* Install SIGINT handler so shell doesn't exit on Ctrl-C */
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    /* Install SIGTSTP handler so shell doesn't stop on Ctrl-Z */
    struct sigaction sa2;
    sa2.sa_handler = sigtstp_handler;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa2, NULL);
    /* Ensure cleanup_on_exit runs when shell exits (e.g., on EOF) */
    atexit(cleanup_on_exit);
}

void handle_eof_exit(void) {
    /* Rely on atexit-registered cleanup_on_exit to kill children and print logout */
    exit(0);
}

/* Helper: find job by job_id */
bg_job *find_job_by_id(int id) {
    bg_job *it = job_list;
    while (it) {
        if (it->job_id == id) return it;
        it = it->next;
    }
    return NULL;
}

/* Helper: remove job from list and return it (not freeing) */
bg_job *unlink_job(bg_job *job) {
    bg_job *it = job_list, *prev = NULL;
    while (it) {
        if (it == job) {
            if (prev) prev->next = it->next; else job_list = it->next;
            it->next = NULL;
            return it;
        }
        prev = it;
        it = it->next;
    }
    return NULL;
}

/* Top-level execution function */
int exec_run_line(const char *line) {
    if (!line) return -1;
    size_t ntoks = 0;
    char **toks = tokenize_special(line, &ntoks);
    if (!toks) return -1;

    /* After tokenizing */
    if (ntoks == 1 && strcmp(toks[0], "activities") == 0) {
        print_activities();
        free_tokens(toks);
        return 0;
    }

    /* Handle built-in: ping */
    if (ntoks >= 1 && strcmp(toks[0], "ping") == 0) {
        if (ntoks != 3) {
            printf("Invalid syntax!\n");
            free_tokens(toks);
            return 0;
        }
        char *endptr;
        long pid = strtol(toks[1], &endptr, 10);
        if (*endptr != '\0') {
            printf("Invalid syntax!\n");
            free_tokens(toks);
            return 0;
        }
        long sig = strtol(toks[2], &endptr, 10);
        if (*endptr != '\0') {
            printf("Invalid syntax!\n");
            free_tokens(toks);
            return 0;
        }
        int actual_sig = (int)(sig % 32);
        if (actual_sig <= 0) actual_sig += 32; /* map 0 to 32? keep positive */
        if (kill((pid_t)pid, actual_sig) < 0) {
            if (errno == ESRCH) printf("No such process found\n");
            else perror("kill");
        } else {
            printf("Sent signal %ld to process with pid %ld\n", sig, pid);
        }
        free_tokens(toks);
        return 0;
    }

    /* Handle fg/bg builtins */
    if (ntoks >= 1 && (strcmp(toks[0], "fg") == 0 || strcmp(toks[0], "bg") == 0)) {
        int is_fg = (strcmp(toks[0], "fg") == 0);
        int job_num = -1;
        if (ntoks == 1) {
            /* pick most recent job */
            if (!job_list) {
                printf("No such job\n");
                free_tokens(toks);
                return 0;
            }
            job_num = job_list->job_id;
        } else if (ntoks == 2) {
            char *endptr;
            long v = strtol(toks[1], &endptr, 10);
            if (*endptr != '\0') { printf("No such job\n"); free_tokens(toks); return 0; }
            job_num = (int)v;
        } else {
            printf("Invalid syntax!\n"); free_tokens(toks); return 0;
        }

        bg_job *job = find_job_by_id(job_num);
        if (!job) { printf("No such job\n"); free_tokens(toks); return 0; }

        if (is_fg) {
            /* Bring to foreground */
            /* If stopped, send SIGCONT */
            if (job->stopped) {
                if (kill(job->pid, SIGCONT) < 0) perror("kill");
                job->stopped = 0;
            }
            /* Remove from job list and wait */
            bg_job *uj = unlink_job(job);
            if (!uj) { printf("No such job\n"); free_tokens(toks); return 0; }
            printf("%s\n", uj->command);
            fflush(stdout);
            /* Wait for process group */
            fg_pgid = uj->pid;
            int st; pid_t w = waitpid(-uj->pid, &st, WUNTRACED);
            if (w > 0 && WIFSTOPPED(st)) {
                /* move back to background as stopped */
                add_stopped_job(uj->pid, uj->command);
            }
            fg_pgid = 0;
            free(uj->command); free(uj);
            free_tokens(toks);
            return 0;
        } else {
            /* bg: resume stopped job in background */
            if (!job->stopped) {
                printf("Job already running\n"); free_tokens(toks); return 0;
            }
            if (kill(job->pid, SIGCONT) < 0) {
                if (errno == ESRCH) printf("No such job\n");
                else perror("kill");
            } else {
                job->stopped = 0;
                printf("[%d] %s &\n", job->job_id, job->command);
            }
            free_tokens(toks);
            return 0;
        }
    }

    /* Process all commands in sequence */
    size_t start = 0;
    while (start < ntoks) {
        /* Find next ';' or end */
        size_t end = start;
        while (end < ntoks && strcmp(toks[end], ";") != 0) end++;
        
        /* Check if command ends with & */
        int is_background = 0;
        if (end > start) {
            size_t last = end - 1;
            /* Handle case where command might end with & */
            if (strcmp(toks[last], "&") == 0) {
                is_background = 1;
                end = last; /* Don't include & in command building */
            }
        }
        
        /* Build and run this command group */
        char *cmd = malloc(strlen(line) + 1);
        cmd[0] = '\0';
        
        for (size_t i = start; i < end; i++) {
            strcat(cmd, toks[i]);
            if (i < end - 1) strcat(cmd, " ");
        }
        
        /* Build pipeline for this command */
        CmdNode *cmds = NULL;
        size_t ncmds = 0;
        int r = build_pipeline_from_tokens(toks + start, end - start, &cmds, &ncmds);
        
        if (r == 0 && ncmds > 0) {
            if (is_background) {
                pid_t pid = fork();
                if (pid == 0) {
                    /* Child */
                    setpgid(0, 0); /* Set new process group */
                    close(STDIN_FILENO); /* Background processes can't read from terminal */
                    run_cmd_pipeline(cmds, ncmds, cmd);
                    exit(0);
                } else if (pid > 0) {
                    /* Parent */
                    setpgid(pid, pid); /* Ensure child is in its own group */
                    add_background_job(pid, cmd);
                }
            } else {
                run_cmd_pipeline(cmds, ncmds, cmd);
            }
            for (size_t i = 0; i < ncmds; ++i) free_cmdnode(&cmds[i]);
            free(cmds);
        }
        
        free(cmd);
        start = is_background ? end + 2 : end + 1;  /* Skip the semicolon/& */
    }

    free_tokens(toks);
    return 0;
}
