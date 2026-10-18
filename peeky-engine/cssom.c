#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>


//utils

static void die_oom(void) {
    fprintf(stderr, "out of memory\n");
    exit(1);
}

static void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p) die_oom();
    return p;
}

static void *xrealloc(void *old, size_t size) {
    void *p = realloc(old, size);
    if (!p) die_oom();
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xcalloc(1, n + 1);
    memcpy(p, s, n);
    return p;
}

#define GROW(arr, n, cap)                                                         \
    do {                                                                          \
        if ((n) >= (cap)) {                                                       \
            (cap) = (cap) ? (cap) * 2 : 8;                                      \
            (arr) = xrealloc((arr), sizeof(*(arr)) * (size_t)(cap));            \
        }                                                                         \
    } while (0)

static int is_ws_ch(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

//tokens

typedef enum {
    T_IDENT, T_FUNC, T_AT, T_HASH, T_STRING, T_NUMBER, T_DIMENSION,
    T_PERCENTAGE, T_URL, T_CDO, T_CDC, T_LBRACE, T_RBRACE, T_LPAREN,
    T_RPAREN, T_LBRACK, T_RBRACK, T_SEMI, T_COLON, T_COMMA, T_DELIM,
    T_EOF
} TokType;

typedef struct {
    TokType type;
    const char *start;
    size_t len;
} Tok;

static Tok *toks;
static int tok_n, tok_cap;

static void emit(TokType t, const char *s, size_t n) {
    GROW(toks, tok_n, tok_cap);
    toks[tok_n].type = t;
    toks[tok_n].start = s;
    toks[tok_n].len = n;
    tok_n++;
}

static int ident_start(const char *s, size_t len, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (isalpha(c) || c == '_' || c >= 0x80 || c == '\\') return 1;
    if (c == '-' && i + 1 < len) {
        unsigned char d = (unsigned char)s[i + 1];
        return isalpha(d) || d == '_' || d >= 0x80 || d == '\\' || d == '-';
    }
    return 0;
}

static int ident_char(unsigned char c) {
    return isalnum(c) || c == '_' || c == '-' || c >= 0x80;
}

static size_t scan_ident(const char *s, size_t len, size_t i) {
    for (;;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            if (i + 1 >= len) break;
            i += 2;
            continue;
        }
        if (!ident_char(c)) break;
        i++;
    }
    //stupid chud comments
    while (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
        size_t j = i + 2;
        while (j + 1 < len && !(s[j] == '*' && s[j + 1] == '/')) j++;
        if (j + 1 >= len) break;
        j += 2;
        if (j >= len || !ident_start(s, len, j)) break;
        i = scan_ident(s, len, j);
    }
    return i;
}

static size_t skip_comment(const char *s, size_t len, size_t i) {
    //more stupid comments
    i += 2;
    while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
    return i + 2 <= len ? i + 2 : len;
}

static void tokenize(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        char c = s[i];
        if (is_ws_ch(c)) { i++; continue; }
        if (c == '\\' && i + 1 >= len) {
            emit(T_DELIM, s + i, 1);
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && s[i + 1] == '*') { i = skip_comment(s, len, i); continue; }
        if (c == '<') {
            if (i + 4 <= len && !memcmp(s + i, "<!--", 4)) { emit(T_CDO, s + i, 4); i += 4; }
            else { emit(T_DELIM, s + i, 1); i++; }
            continue;
        }
        if (c == '-' && i + 3 <= len && !memcmp(s + i, "-->", 3)) { emit(T_CDC, s + i, 3); i += 3; continue; }
        if (c == '@') {
            size_t j = i + 1;
            if (j < len && ident_start(s, len, j)) {
                j = scan_ident(s, len, j);
                emit(T_AT, s + i, j - i);
                i = j;
            } else {
                emit(T_DELIM, s + i, 1);
                i++;
            }
            continue;
        }
        if (c == '#') {
            size_t j = i + 1;
            if (j < len && (ident_start(s, len, j) || isdigit((unsigned char)s[j]))) {
                while (j < len) {
                    unsigned char d = (unsigned char)s[j];
                    if (d == '\\') { if (j + 1 >= len) break; j += 2; continue; }
                    if (!ident_char(d) && !isdigit(d)) break;
                    j++;
                }
                emit(T_HASH, s + i, j - i);
                i = j;
            } else {
                emit(T_DELIM, s + i, 1);
                i++;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            char q = c;
            while (j < len && s[j] != q) {
                if (s[j] == '\\' && j + 1 < len) { j += 2; continue; }
                if (s[j] == '\n') break; //dumbass string
                j++;
            }
            if (j < len) j++; //newline stuff
            emit(T_STRING, s + i, j - i);
            i = j;
            continue;
        }
        if ((c == 'u' || c == 'U') && i + 3 < len && !strncasecmp(s + i, "url", 3) &&
            s[i + 3] == '(' && (i == 0 || !ident_char((unsigned char)s[i - 1]))) {
            size_t j = i + 4;
            while (j < len && is_ws_ch(s[j])) j++;
            if (j < len && (s[j] == '"' || s[j] == '\'')) {
                char q = s[j++];
                while (j < len && s[j] != q) {
                    if (s[j] == '\\') j++;
                    j++;
                }
                if (j < len) j++;
                while (j < len && is_ws_ch(s[j])) j++;
            } else {
                while (j < len && s[j] != ')' && !is_ws_ch(s[j])) {
                    if (s[j] == '\\') { if (j + 1 >= len) break; j++; }
                    j++;
                }
                while (j < len && is_ws_ch(s[j])) j++;
            }
            if (j < len && s[j] == ')') j++;
            emit(T_URL, s + i, j - i);
            i = j;
            continue;
        }
        if (ident_start(s, len, i)) {
            size_t j = scan_ident(s, len, i);
            if (j < len && s[j] == '(') {
                emit(T_FUNC, s + i, j + 1 - i);
                i = j + 1;
            } else {
                emit(T_IDENT, s + i, j - i);
                i = j;
            }
            continue;
        }
        if (isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < len && isdigit((unsigned char)s[i + 1])) ||
            ((c == '+' || c == '-') && i + 1 < len &&
             (isdigit((unsigned char)s[i + 1]) ||
              (s[i + 1] == '.' && i + 2 < len && isdigit((unsigned char)s[i + 2]))))) {
            size_t j = i;
            if (s[j] == '+' || s[j] == '-') j++;
            while (j < len && isdigit((unsigned char)s[j])) j++;
            if (j + 1 < len && s[j] == '.' && isdigit((unsigned char)s[j + 1])) {
                j++;
                while (j < len && isdigit((unsigned char)s[j])) j++;
            }
            if (j < len && (s[j] == 'e' || s[j] == 'E') && j + 1 < len &&
                (isdigit((unsigned char)s[j + 1]) ||
                 ((s[j + 1] == '+' || s[j + 1] == '-') && j + 2 < len &&
                  isdigit((unsigned char)s[j + 2])))) {
                j++;
                if (s[j] == '+' || s[j] == '-') j++;
                while (j < len && isdigit((unsigned char)s[j])) j++;
            }
            if (j < len && s[j] == '%') {
                emit(T_PERCENTAGE, s + i, j + 1 - i);
                i = j + 1;
            } else if (j < len && ident_start(s, len, j)) {
                j = scan_ident(s, len, j);
                emit(T_DIMENSION, s + i, j - i);
                i = j;
            } else {
                emit(T_NUMBER, s + i, j - i);
                i = j;
            }
            continue;
        }
        TokType t = T_DELIM;
        switch (c) {
            case '{': t = T_LBRACE; break;
            case '}': t = T_RBRACE; break;
            case '(': t = T_LPAREN; break;
            case ')': t = T_RPAREN; break;
            case '[': t = T_LBRACK; break;
            case ']': t = T_RBRACK; break;
            case ';': t = T_SEMI; break;
            case ':': t = T_COLON; break;
            case ',': t = T_COMMA; break;
        }
        emit(t, s + i, 1);
        i++;
    }
    emit(T_EOF, s + len, 0);
}

//CSSOM tree

typedef enum { N_RULE, N_AT, N_DECL } NodeType;

typedef struct Node {
    NodeType type;
    char *text; //rule selector
    char *arg;
    struct Node **kids;
    int kid_n, kid_cap;
} Node;

static Node **all_nodes;
static size_t all_n, all_cap;

static Node *node_new(NodeType t) {
    Node *n = xcalloc(1, sizeof *n);
    n->type = t;
    GROW(all_nodes, all_n, all_cap);
    all_nodes[all_n++] = n;
    return n;
}

static void add_kid(Node *parent, Node *kid) {
    GROW(parent->kids, parent->kid_n, parent->kid_cap);
    parent->kids[parent->kid_n++] = kid;
}

static void free_all_nodes(void) {
    for (size_t i = 0; i < all_n; i++) {
        Node *n = all_nodes[i];
        free(n->text);
        free(n->arg);
        free(n->kids);
        free(n);
    }
    free(all_nodes);
}

static const char *src;
static int tok_i;

static char *strip_comments(const char *a, const char *b) {
    char *out = xcalloc(1, (size_t)(b - a) + 1);
    size_t w = 0;
    const char *p = a;
    while (p < b) {
        if (*p == '"' || *p == '\'') {
            char q = *p;
            out[w++] = *p++;
            while (p < b && *p != q) {
                if (*p == '\\' && p + 1 < b) out[w++] = *p++;
                out[w++] = *p++;
            }
            if (p < b) out[w++] = *p++;
        } else if (*p == '/' && p + 1 < b && p[1] == '*') {
            p += 2;
            while (p < b && !(p[0] == '*' && p[1] == '/')) p++;
            if (p < b) p += 2;
        } else {
            if (*p == '\\' && p + 1 == b) {
                out[w++] = '\xEF'; out[w++] = '\xBF'; out[w++] = '\xBD';
                p++;
            } else {
                out[w++] = *p++;
            }
        }
    }
    out[w] = '\0';
    return out;
}

static char *slice_trim(const char *a, const char *b) {
    while (a < b && is_ws_ch(*a)) a++;
    while (b > a && is_ws_ch(b[-1])) b--;
    return strip_comments(a, b);
}

static TokType cur(void) {
    return toks[tok_i].type;
}

static void parse_rules(Node *parent);
static void parse_declarations(Node *parent);
static void parse_at(Node *parent);

static void parse_qualified(Node *parent) {
    int start = tok_i, depth = 0;
    while (tok_i < tok_n) {
        TokType t2 = cur();
        if (depth == 0 && (t2 == T_LBRACE || t2 == T_SEMI || t2 == T_RBRACE || t2 == T_EOF)) break;
        if (t2 == T_FUNC || t2 == T_LPAREN || t2 == T_LBRACK) depth++;
        else if ((t2 == T_RPAREN || t2 == T_RBRACK) && depth > 0) depth--;
        tok_i++;
    }
    if (tok_i >= tok_n || cur() != T_LBRACE) { 
        if (tok_i < tok_n && cur() == T_SEMI) tok_i++;
        return;
    }
    Node *n = node_new(N_RULE);
    n->text = slice_trim(toks[start].start, toks[tok_i].start);
    tok_i++;
    parse_declarations(n);
    if (tok_i < tok_n && cur() == T_RBRACE) tok_i++;
    add_kid(parent, n);
}


static void parse_at(Node *parent) {
    Tok *t = &toks[tok_i++];
    Node *n = node_new(N_AT);
    n->text = xstrndup(t->start + 1, t->len - 1);
    int prelude_start = tok_i;
    int depth = 0;
    while (tok_i < tok_n) {
        TokType t2 = cur();
        if (depth == 0 && (t2 == T_SEMI || t2 == T_LBRACE || t2 == T_EOF)) break;
        if (t2 == T_LPAREN || t2 == T_LBRACK) depth++;
        else if ((t2 == T_RPAREN || t2 == T_RBRACK) && depth > 0) depth--;
        tok_i++;
    }
    if (cur() == T_EOF) {
        n->arg = slice_trim(toks[prelude_start].start, toks[tok_n - 1].start);
        add_kid(parent, n);
        return;
    }
    if (cur() == T_SEMI) {
        n->arg = slice_trim(toks[prelude_start].start, toks[tok_i].start);
        tok_i++;
        add_kid(parent, n);
        return;
    }
    n->arg = slice_trim(toks[prelude_start].start, toks[tok_i].start);
    tok_i++;
    parse_declarations(n);
    if (tok_i < tok_n && cur() == T_RBRACE) tok_i++;
    add_kid(parent, n);
}

static int at_is_custom(const char *text) {
    return text[0] == '-' && text[1] == '-';
}

static void parse_declarations(Node *parent) {
    for (;;) {
        if (tok_i >= tok_n) return;
        TokType ty = cur();
        if (ty == T_RBRACE || ty == T_EOF) return;
        if (ty == T_SEMI || ty == T_CDO || ty == T_CDC) { tok_i++; continue; }
        if (ty == T_AT) { parse_at(parent); continue; }

        int name_start = tok_i;
        int custom = 0;
        int name_end_idx = -1;
        if (ty == T_IDENT && tok_i + 1 < tok_n && toks[tok_i + 1].type == T_COLON) {
            name_end_idx = tok_i + 1;
            custom = at_is_custom(toks[tok_i].start);
        } else if (ty == T_DELIM && tok_i + 2 < tok_n &&
                   toks[tok_i + 1].type == T_IDENT && toks[tok_i + 2].type == T_COLON) {
            name_end_idx = tok_i + 2;
            custom = at_is_custom(toks[tok_i + 1].start);
        }

        if (name_end_idx >= 0) {
            int j = name_end_idx + 1;
            int depth = 0;
            for (; j < tok_n; j++) {
                TokType t2 = toks[j].type;
                if (depth == 0 && (t2 == T_SEMI || t2 == T_RBRACE || t2 == T_EOF)) break;
                if (depth == 0 && t2 == T_LBRACE && !custom) break;   /* nested rule */
                if (t2 == T_FUNC || t2 == T_LPAREN || t2 == T_LBRACK || t2 == T_LBRACE) depth++;
                else if ((t2 == T_RPAREN || t2 == T_RBRACK || t2 == T_RBRACE) && depth > 0) depth--;
            }
            if (j < tok_n && toks[j].type == T_LBRACE) {
                //what am I even doing atp
                Node *r = node_new(N_RULE);
                r->text = slice_trim(toks[name_start].start, toks[j].start);
                tok_i = j + 1;
                parse_declarations(r);
                if (tok_i < tok_n && cur() == T_RBRACE) tok_i++;
                add_kid(parent, r);
                continue;
            }
            Node *d = node_new(N_DECL);
            d->text = slice_trim(toks[name_start].start, toks[name_end_idx].start);
            tok_i = name_end_idx + 1;
            d->arg = slice_trim(toks[tok_i].start, toks[j].start);
            if (j < tok_n && toks[j].type == T_SEMI) tok_i = j + 1;
            else tok_i = j;
            add_kid(parent, d);
            continue;
        }

        //surprised this much of my code works
        int j = tok_i, depth = 0;
        while (j < tok_n) {
            TokType t2 = toks[j].type;
            if (depth == 0 && (t2 == T_LBRACE || t2 == T_SEMI || t2 == T_RBRACE || t2 == T_EOF)) break;
            if (t2 == T_FUNC || t2 == T_LPAREN || t2 == T_LBRACK) depth++;
            else if ((t2 == T_RPAREN || t2 == T_RBRACK) && depth > 0) depth--;
            j++;
        }
        if (j >= tok_n || toks[j].type == T_RBRACE || toks[j].type == T_EOF) return;
        if (toks[j].type == T_SEMI) { tok_i = j + 1; continue; }

        Node *r = node_new(N_RULE);
        r->text = slice_trim(toks[tok_i].start, toks[j].start);
        tok_i = j + 1;
        parse_declarations(r);
        if (tok_i < tok_n && cur() == T_RBRACE) tok_i++;
        add_kid(parent, r);
    }
}

static void parse_rules(Node *parent) {
    for (;;) {
        if (tok_i >= tok_n) return;
        TokType ty = cur();
        if (ty == T_EOF) return;
        if (ty == T_RBRACE) { tok_i++; continue; }
        if (ty == T_CDO || ty == T_CDC) { tok_i++; continue; }
        if (ty == T_AT) parse_at(parent);
        else parse_qualified(parent);
    }
}

//output

static void print_q(const char *s) {
    putchar('"');
    for (; *s; s++) {
        switch (*s) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:   putchar(*s);
        }
    }
    putchar('"');
}

static void indent(int depth) {
    fputs("| ", stdout);
    for (int i = 0; i < depth * 2; i++) putchar(' ');
}

static void print_node(const Node *n, int depth) {
    switch (n->type) {
        case N_RULE:
            indent(depth);
            fputs("<rule ", stdout);
            print_q(n->text);
            fputs(">\n", stdout);
            break;
        case N_AT:
            indent(depth);
            fputs("<at ", stdout);
            print_q(n->text);
            if (n->arg && *n->arg) {
                putchar(' ');
                print_q(n->arg);
            }
            fputs(">\n", stdout);
            break;
        case N_DECL:
            indent(depth);
            fputs("<decl ", stdout);
            print_q(n->text);
            putchar(' ');
            print_q(n->arg ? n->arg : "");
            fputs(">\n", stdout);
            return;
    }
    for (int i = 0; i < n->kid_n; i++)
        print_node(n->kids[i], depth + 1);
}

//main >:3

static size_t g_buf_len;

static char *read_all(void) {
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    for (;;) {
        size_t r = fread(buf + n, 1, cap - n, stdin);
        n += r;
        if (n < cap) break;
        cap *= 2;
        char *nb = realloc(buf, cap + 1);
        if (!nb) { free(buf); return NULL; }
        buf = nb;
    }
    g_buf_len = n;
    buf[n] = '\0';
    return buf;
}

int main(void) {
    char *buf = read_all();
    if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }
    src = buf;

    tokenize(src, g_buf_len);

    Node root = {0};
    parse_rules(&root);

    puts("#stylesheet");
    for (int i = 0; i < root.kid_n; i++)
        print_node(root.kids[i], 0);

    free(buf);
    free(toks);
    free(root.kids);
    free_all_nodes();
    return 0;
}
