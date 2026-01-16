#define _POSIX_C_SOURCE 200809L
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Tokenizer for our grammar */
typedef enum {
    TOK_NONE = 0,
    TOK_NAME,
    TOK_PIPE,   /* '|' */
    TOK_SEMI,   /* ';' */
    TOK_AMP,    /* '&' */
    TOK_LT,     /* '<' */
    TOK_GT,     /* '>' */
    TOK_GTGT,   /* '>>' */
    TOK_EOF,
    TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    char *text; /* for TOK_NAME */
} Token;

typedef struct {
    const char *s;
    size_t pos;
    Token cur;
} Lexer;

/* helpers */
static int is_name_char(char c) {
    /* name -> any char except: | & > < ; whitespace */
    if (c == '|' || c == '&' || c == '>' || c == '<' || c == ';') return 0;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return 0;
    return 1;
}

static void token_free(Token *t) {
    if (!t) return;
    free(t->text);
    t->text = NULL;
    t->type = TOK_NONE;
}

static void lexer_init(Lexer *lx, const char *s) {
    lx->s = s ? s : "";
    lx->pos = 0;
    lx->cur.type = TOK_NONE;
    lx->cur.text = NULL;
}

/* produce next token into lx->cur (caller must free previous via token_free) */
static void lexer_next(Lexer *lx) {
    token_free(&lx->cur);
    const char *str = lx->s;
    size_t i = lx->pos;

    /* skip whitespace */
    while (str[i] && (str[i] == ' ' || str[i] == '\t' || str[i] == '\r' || str[i] == '\n')) i++;

    if (str[i] == '\0') {
        lx->cur.type = TOK_EOF;
        lx->pos = i;
        return;
    }

    char c = str[i];
    if (c == '|') {
        lx->cur.type = TOK_PIPE;
        lx->pos = i + 1;
        return;
    } else if (c == ';') {
        lx->cur.type = TOK_SEMI;
        lx->pos = i + 1;
        return;
    } else if (c == '&') {
        lx->cur.type = TOK_AMP;
        lx->pos = i + 1;
        return;
    } else if (c == '<') {
        lx->cur.type = TOK_LT;
        lx->pos = i + 1;
        return;
    } else if (c == '>') {
        /* check if next is '>' (>> ) */
        if (str[i+1] == '>') {
            lx->cur.type = TOK_GTGT;
            lx->pos = i + 2;
            return;
        } else {
            lx->cur.type = TOK_GT;
            lx->pos = i + 1;
            return;
        }
    } else {
        size_t start = i;
        while (str[i] && is_name_char(str[i])) i++;
        size_t len = i - start;
        char *buf = malloc(len + 1);
        if (!buf) {
            lx->cur.type = TOK_ERROR;
            lx->pos = i;
            return;
        }
        memcpy(buf, &str[start], len);
        buf[len] = '\0';
        lx->cur.type = TOK_NAME;
        lx->cur.text = buf;
        lx->pos = i;
        return;
    }
}


static int parse_atomic(Lexer *lx); 

static int parse_cmd_group(Lexer *lx) {
    if (!parse_atomic(lx)) return 0;
    while (lx->cur.type == TOK_PIPE) {
        lexer_next(lx);
        if (!parse_atomic(lx)) return 0;
    }
    if (lx->cur.type == TOK_AMP) {
        lexer_next(lx);
    }
    return 1;
}

static int parse_atomic(Lexer *lx) {
    if (lx->cur.type != TOK_NAME) return 0;
    lexer_next(lx);

    while (1) {
        if (lx->cur.type == TOK_NAME) {
            lexer_next(lx);
            continue;
        } else if (lx->cur.type == TOK_LT) {
            lexer_next(lx); 
            if (lx->cur.type != TOK_NAME) return 0;
            lexer_next(lx);
            continue;
        } else if (lx->cur.type == TOK_GT || lx->cur.type == TOK_GTGT) {
            lexer_next(lx);
            if (lx->cur.type != TOK_NAME) return 0;
            lexer_next(lx);
            continue;
        } else {
            break;
        }
    }
    return 1;
}

bool validate_syntax(const char *line) {
    if (!line) return false;
    Lexer lx;
    lexer_init(&lx, line);
    lexer_next(&lx); 

    if (!parse_cmd_group(&lx)) {
        token_free(&lx.cur);
        return false;
    }

    while (lx.cur.type == TOK_SEMI) {
        lexer_next(&lx);
        if (!parse_cmd_group(&lx)) {
            token_free(&lx.cur);
            return false;
        }
    }

    bool ok = (lx.cur.type == TOK_EOF);
    token_free(&lx.cur);
    return ok;
}

static int is_special_tok(const char *tok) {
    return (strcmp(tok, "|") == 0 || strcmp(tok, ">>") == 0 ||
            strcmp(tok, ";") == 0 || strcmp(tok, "&") == 0);
}

/* check a single character to see if it's a special token start */
static int is_special_char(char c) {
    return (c == '|' || c == ';' || c == '&' || c == '<' || c == '>');
}

/* Free token array returned by tokenize_special */
static void free_tokens(char **toks) {
    if (!toks) return;
    for (size_t i = 0; toks[i]; ++i) free(toks[i]);
    free(toks);
}

/* Tokenize input into tokens where special symbols are separate tokens */
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
        if (*p == '|' || *p == ';' || *p == '&' || *p == '<') {
            char t[2] = {*p, '\0'};
            arr[n] = strdup(t);
            if (!arr[n]) goto fail;
            ++n;
            ++p;
        } else if (*p == '>') {
            if (p[1] == '>') {
                arr[n] = strdup(">>");
                if (!arr[n]) goto fail;
                ++n;
                p += 2;
            } else {
                arr[n] = strdup(">");
                if (!arr[n]) goto fail;
                ++n;
                ++p;
            }
        } else {
            /* name token: stop at whitespace or any special character */
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && !is_special_char(*p)) ++p;
            size_t len = p - start;
            arr[n] = malloc(len + 1);
            if (!arr[n]) goto fail;
            memcpy(arr[n], start, len);
            arr[n][len] = '\0';
            ++n;
        }

        if (n + 1 >= cap) {
            cap *= 2;
            char **new_arr = realloc(arr, sizeof(char*) * cap);
            if (!new_arr) goto fail;
            arr = new_arr;
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

int parse_line(const char *line) {
    if (!line || !*line) return 1;
    size_t ntoks = 0;
    char **toks = tokenize_special(line, &ntoks);
    if (!toks) return 1;

    /* Basic validation */
    if (ntoks == 0) {
        free_tokens(toks);
        return 1;
    }

    /* Check for invalid special token at start */
    if (is_special_tok(toks[0])) {
        free_tokens(toks);
        return 1;
    }

    /* Check special tokens */
    for (size_t i = 0; i < ntoks; ++i) {
        if (strcmp(toks[i], "|") == 0 || strcmp(toks[i], ";") == 0) {
            /* Pipe and semicolon can't be at end and must have command after */
            if (i == ntoks - 1 || is_special_tok(toks[i + 1])) {
                free_tokens(toks);
                return 1;
            }
        } else if (strcmp(toks[i], "&") == 0) {
            /* & can appear at the end of command */
            if (i == 0 || (i < ntoks - 1 && strcmp(toks[i + 1], ";") != 0)) {
                free_tokens(toks);
                return 1;
            }
        }
    }

    /* Check IO redirections */
    for (size_t i = 0; i < ntoks; ++i) {
        if ((strcmp(toks[i], "<") == 0 || strcmp(toks[i], ">") == 0 || 
             strcmp(toks[i], ">>") == 0)) {
            if (i == ntoks - 1 || is_special_tok(toks[i + 1])) {
                free_tokens(toks);
                return 1;
            }
        }
    }

    free_tokens(toks);
    return 0;
}
