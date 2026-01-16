#define _POSIX_C_SOURCE 200809L
#include "prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char *g_shell_home = NULL;
static char *g_username = NULL;
static char *g_hostname = NULL;

static char *safe_strdup(const char *s) {
    if (!s) return NULL;
    char *r = strdup(s);
    return r;
}

int prompt_init(void) {
    char cwd[PATH_MAX+1];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return -1;
    }
    g_shell_home = safe_strdup(cwd);
    if (g_shell_home) setenv("HOME", g_shell_home, 1);

    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) g_username = safe_strdup(pw->pw_name);
    else {
        const char *u = getenv("USER");
        g_username = safe_strdup(u ? u : "unknown");
    }

    long hn_max = sysconf(_SC_HOST_NAME_MAX);
    if (hn_max < 1) hn_max = 255;
    char *hbuf = malloc(hn_max + 1);
    if (hbuf == NULL) {
        g_hostname = safe_strdup("unknown");
    } else {
        if (gethostname(hbuf, hn_max + 1) == 0) {
            g_hostname = safe_strdup(hbuf);
        } else {
            g_hostname = safe_strdup("unknown");
        }
        free(hbuf);
    }

    return 0;
}

static char *build_display_path(void) {
    char cwd[PATH_MAX+1];
    if (!getcwd(cwd, sizeof(cwd))) {
        const char *pwd = getenv("PWD");
        if (!pwd) return strdup("?");
        return strdup(pwd);
    }

    if (g_shell_home != NULL) {
        size_t hlen = strlen(g_shell_home);
        if (hlen == 1 && g_shell_home[0] == '/') {
            return strdup(cwd);
        }
        if (strncmp(cwd, g_shell_home, hlen) == 0 &&
            (cwd[hlen] == '/' || cwd[hlen] == '\0')) {
            if (cwd[hlen] == '\0') {
                return strdup("~");
            } else {
                const char *rest = cwd + hlen + 1;
                size_t needed = strlen(rest) + 3; 
                char *out = malloc(needed);
                if (!out) return strdup("~");
                snprintf(out, needed, "~/%s", rest);
                return out;
            }
        }
    }
    return strdup(cwd);
}

void prompt_print(void) {
    char *display = build_display_path();
    if (!display) display = strdup("?");
    printf("<%s@%s:%s> ", g_username ? g_username : "unknown",
           g_hostname ? g_hostname : "unknown", display);
    fflush(stdout);
    free(display);
}

void prompt_cleanup(void) {
    free(g_shell_home); g_shell_home = NULL;
    free(g_username); g_username = NULL;
    free(g_hostname); g_hostname = NULL;
}
