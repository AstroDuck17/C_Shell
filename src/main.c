#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>

#include "prompt.h"
#include "parser.h"
#include "intrinsics.h"
#include "exec.h"

/* Save original terminal attributes so we can restore on exit */
static struct termios g_orig_termios;
static int g_termios_saved = 0;

static void restore_terminal_mode(void) {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_termios_saved = 0;
    }
}

/* Read one line in non-canonical mode. Returns malloc'd string (without newline).
 * On Ctrl-D (EOT) this function will call handle_eof_exit() and not return.
 * Returns NULL only on unrecoverable error (but handle_eof_exit will normally exit).
 */
static char *read_input_line(void) {
    size_t cap = 256;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (1) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) {
            /* EOF or error on read -> treat as EOF */
            free(buf);
            handle_eof_exit(); /* does not return */
            return NULL;
        }
        if ((unsigned char)c == 4) { /* Ctrl-D (EOT) */
            free(buf);
            handle_eof_exit(); /* does not return */
            return NULL;
        }
        if (c == '\r' || c == '\n') {
            /* echo newline and finish */
            const char nl = '\n';
            write(STDOUT_FILENO, &nl, 1);
            break;
        }
        if (c == 127 || c == '\b') {
            /* backspace: remove last char if any and erase on terminal */
            if (len > 0) {
                const char bs_seq[] = { '\b', ' ', '\b' };
                len--;
                write(STDOUT_FILENO, bs_seq, sizeof(bs_seq));
            }
            continue;
        }
        /* ordinary character: append and echo */
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            char *t = realloc(buf, ncap);
            if (!t) { free(buf); return NULL; }
            buf = t; cap = ncap;
        }
        buf[len++] = c;
        write(STDOUT_FILENO, &c, 1);
    }
    /* Null-terminate and return */
    buf[len] = '\0';
    return buf;
}

int main(void) {
    if (prompt_init() != 0) {
        fprintf(stderr, "Failed to initialize prompt: %s\n", strerror(errno));
        /* continue anyway; prompt will use defaults */
    }

    if (intrinsics_init() != 0) {
        /* non-fatal */
    }

    char *line = NULL;

    init_job_list();

    /* Switch terminal to non-canonical mode so Ctrl-D is seen immediately.
     * Save original attributes and register restore handler so that
     * handle_eof_exit() -> exit() will still restore the terminal.
     */
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
        struct termios t = g_orig_termios;
        /* disable canonical mode and disable ECHO so we can echo once manually */
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
            g_termios_saved = 1;
            atexit(restore_terminal_mode);
        }
    }

    while (1) {
        prompt_print();
        /* Read a line in non-canonical mode, detect Ctrl-D immediately */
        char *rl = read_input_line();
        if (!rl) {
            /* read_input_line will call handle_eof_exit on Ctrl-D; if it ever
             * returns NULL we treat as EOF/error */
            handle_eof_exit();
            break;
        }
        /* replace previous line buffer */
        free(line);
        line = rl;

        /* If the input is empty (only whitespace), just continue */
        int allws = 1;
        for (size_t i = 0; line[i]; ++i) {
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') {
                allws = 0; break;
            }
        }
        if (allws) continue;

        /* Validate syntax per Part A grammar */
        if (!validate_syntax(line)) {
            printf("Invalid Syntax!\n");
            continue;
        }

        /* Record the user's command in history (intrinsics_record_command
         * will skip storing if the command contains atomic 'log' or it is a
         * duplicate of the previous entry). Do this BEFORE handling intrinsics.
         * Note: when log execute returns a command to re-executed, do NOT call
         * intrinsics_record_command on that re-executed command (spec requirement).
         */
        intrinsics_record_command(line);

        /* Try to handle intrinsics.
         * If intrinsics_handle returns 0 -> not an intrinsic (Part C will execute)
         * If it returns 1 -> intrinsic handled, continue loop
         * If it returns 2 -> intrinsic handled and provided a command to re-execute:
         *                    out_reexec_cmd contains malloc'd string that MUST be free()'d
         *                    and MUST NOT be recorded in history by the caller.
         */
        char *reexec = NULL;
        int hres = intrinsics_handle(line, &reexec);
        if (hres == 0) {
            /* Not an intrinsic: execute the line (normal execution path). */
            exec_run_line(line);
            check_background_jobs();
            continue;
        } else if (hres == 1) {
            /* handled, nothing more to do */
            continue;
        } else if (hres == 2) {
            /* intrinsics wants us to re-execute a stored command (log execute).
             * We must NOT record the re-executed command. Handle it now.
             *
             * The reexec returned by intrinsics_handle is malloc'd; we will
             * free it (and any nested reexecs) here.
             *
             * Strategy:
             *  - validate syntax of reexec
             *  - try to handle it as an intrinsic (intrinsics_handle) WITHOUT recording
             *  - if not an intrinsic, execute via exec_run_line()
             *  - if intrinsics_handle returns yet another reexec (nested), follow it
             *    until a terminal action occurs.
             */
            char *current = reexec;      /* takes ownership */
            reexec = NULL;
            while (current) {
                /* Validate before doing anything */
                if (!validate_syntax(current)) {
                    printf("Invalid Syntax!\n");
                    free(current);
                    current = NULL;
                    break;
                }

                /* Try intrinsic handler on the reexec command (do NOT record) */
                char *next_reexec = NULL;
                int nested = intrinsics_handle(current, &next_reexec);
                if (nested == 1) {
                    /* intrinsic handled; done */
                    free(current);
                    current = NULL;
                    break;
                } else if (nested == 2) {
                    /* got another reexec; free current and follow chain */
                    free(current);
                    current = next_reexec; /* take ownership and loop */
                    next_reexec = NULL;
                    continue;
                } else if (nested == 0) {
                    /* not an intrinsic -> execute it (do NOT record) */
                    exec_run_line(current);
                    free(current);
                    current = NULL;
                    break;
                } else {
                    /* nested == -1 or error: intrinsics printed error; stop */
                    free(current);
                    current = NULL;
                    break;
                }
            }
            /* ensure any leftover reexec (shouldn't be) is freed */
            if (reexec) free(reexec);
            check_background_jobs();
            continue;
        } else { /* hres == -1 */
            /* error already printed by intrinsics; continue */
            continue;
        }
    }

    free(line);
    intrinsics_cleanup();
    prompt_cleanup();
    /* restore terminal mode if not already restored */
    restore_terminal_mode();
    return 0;
}
