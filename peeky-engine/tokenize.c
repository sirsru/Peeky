#define _GNU_SOURCE
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IS_WS(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r' || (c) == '\f')

//utf8 ;-;
static size_t utf8(uint32_t cp, char *buf) {
    if (cp <= 0x7F)      { buf[0] = (char)cp; return 1; }
    if (cp <= 0x7FF)     { buf[0] = 0xC0 | (cp >> 6);  buf[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp <= 0xFFFF)    { buf[0] = 0xE0 | (cp >> 12); buf[1] = 0x80 | ((cp >> 6) & 0x3F); buf[2] = 0x80 | (cp & 0x3F); return 3; }
    buf[0] = 0xF0 | (cp >> 18); buf[1] = 0x80 | ((cp >> 12) & 0x3F);
    buf[2] = 0x80 | ((cp >> 6) & 0x3F); buf[3] = 0x80 | (cp & 0x3F);
    return 4;
}

static char *decode_entities(const char *s, size_t n, size_t *outlen) {
    char *out = malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            const char *ent = s + i;
            const char *semi = memchr(ent, ';', n - i);
            size_t elen = semi ? (size_t)(semi - ent) : 0;
            if (elen > 2 && elen <= 32) {
                uint32_t cp = 0;
                int found = 0;
                if (!strncmp(ent, "&amp;", 5))    { cp = '&';  found = 1; }
                else if (!strncmp(ent, "&lt;", 4))   { cp = '<';  found = 1; }
                else if (!strncmp(ent, "&gt;", 4))   { cp = '>';  found = 1; }
                else if (!strncmp(ent, "&quot;", 6)) { cp = '"';  found = 1; }
                else if (!strncmp(ent, "&#39;", 5))  { cp = '\''; found = 1; }
                else if (!strncmp(ent, "&apos;", 6)) { cp = '\''; found = 1; }
                else if (!strncmp(ent, "&nbsp;", 6)) { cp = 0xA0; found = 1; }
                else if (!strncmp(ent, "&copy;", 6)) { cp = 0xA9; found = 1; }
                else if (!strncmp(ent, "&reg;", 5))  { cp = 0xAE; found = 1; }
                else if (!strncmp(ent, "&times;", 7)) { cp = 0xD7; found = 1; }
                else if (!strncmp(ent, "&divide;", 8)) { cp = 0xF7; found = 1; }
                else {
                    if (ent[1] == '#' && elen > 2) {
                        const char *num = ent + 2;
                        if (num[0] == 'x' || num[0] == 'X') {
                            for (size_t k = 1; k < elen - 1; k++) {
                                int d = (num[k] >= '0' && num[k] <= '9') ? num[k] - '0' :
                                        (num[k] >= 'a' && num[k] <= 'f') ? num[k] - 'a' + 10 :
                                        (num[k] >= 'A' && num[k] <= 'F') ? num[k] - 'A' + 10 : -1;
                                if (d < 0) break;
                                cp = (cp << 4) | (uint32_t)d;
                                if (++found == (int)(elen - 2)) { found = 1; break; }
                            }
                            if (found) found = 1;
                        } else {
                            cp = 0; found = 0;
                            for (size_t k = 0; k < elen - 1; k++) {
                                int d = (num[k] >= '0' && num[k] <= '9') ? num[k] - '0' : -1;
                                if (d < 0) break;
                                cp = cp * 10 + (uint32_t)d;
                                found = 1;
                            }
                        }
                    }
                }
                if (found && cp > 0 && cp != 0xFFFD) {
                    if (cp >= 0xD800 && cp <= 0xDFFF) { cp = 0xFFFD; }
                    if (cp > 0x10FFFF) { cp = 0xFFFD; }
                    char tmp[4];
                    size_t clen = utf8(cp, tmp);
                    memcpy(out + w, tmp, clen);
                    w += clen;
                    i += elen;
                    continue;
                }
            }
        }
        out[w++] = s[i];
    }
    out[w] = '\0';
    if (outlen) *outlen = w;
    return out;
}

static void print_lower(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) putchar(tolower((unsigned char)s[i]));
}

static void print_escaped(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            default:   putchar(s[i]);
        }
    }
}

static void emit_line(const char *label, const char *s, size_t n) {
    fputs(label, stdout);
    putchar(' ');
    print_escaped(s, n);
    putchar('\n');
}

static int is_void(const char *name) {
    static const char *voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; voids[i]; i++)
        if (!strcmp(name, voids[i])) return 1;
    return 0;
}

//RAWTEXT elements
static int is_rawtext(const char *name) {
    return !strcmp(name, "script") || !strcmp(name, "style") ||
           !strcmp(name, "iframe") || !strcmp(name, "noembed") ||
           !strcmp(name, "noscript") || !strcmp(name, "xmp") ||
           !strcmp(name, "plaintext");
}

//RCDATA elements
static int is_rcdata(const char *name) {
    return !strcmp(name, "title") || !strcmp(name, "textarea");
}

//what am I even doing atp?
static int starts_markup(const char *p) {
    if (p[0] != '<') return 0;
    if (isalpha((unsigned char)p[1])) return 1;
    if (p[1] == '!' || p[1] == '?') return 1;
    if (p[1] == '/' && isalpha((unsigned char)p[2])) return 1;
    return 0;
}

static void emit_text(const char *s, size_t n) {
    if (n == 0) return;
    size_t dlen;
    char *dec = decode_entities(s, n, &dlen);
    if (!dec) return;
    fputs("TEXT ", stdout);
    print_escaped(dec, dlen);
    fputc('\n', stdout);
    free(dec);
}
//My code probably doesnt even work
static void emit_text_raw(const char *s, size_t n) {
    if (n == 0) return;
    char */* Named: &amp; &lt; &gt; &quot; &#39; &apos; etc. */
buf = malloc(n * 4 + 1);
    if (!buf) return;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\0') {
            memcpy(buf + w, "\xEF\xBF\xBD", 3);
            w += 3;
        } else {
            buf[w++] = s[i];
        }
    }
    fputs("TEXT ", stdout);
    print_escaped(buf, w);
    fputc('\n', stdout);
    free(buf);
}
//I should probably start testing it
static const char *parse_attrs(const char *p, int emit, int *self_close) {
    *self_close = 0;
    for (;;) {
        while (IS_WS(*p)) p++;
        if (!*p) return NULL;
        if (*p == '>') return p + 1;
        if (*p == '/') {
            p++;
            if (*p == '>') { *self_close = 1; return p + 1; }
            continue;
        }

        const char *ns = p++;
        while (*p && !IS_WS(*p) && *p != '=' && *p != '>' && *p != '/') p++;
        const char *ne = p;
        const char *vs = p;
        size_t vl = 0;

        while (IS_WS(*p)) p++;
        if (*p == '=') {
            p++;
            while (IS_WS(*p)) p++;
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                vs = p;
                while (*p && *p != q) p++;
                if (!*p) return NULL;
                vl = (size_t)(p - vs);
                p++;
            } else {
                vs = p;
                while (*p && !IS_WS(*p) && *p != '>') p++;
                vl = (size_t)(p - vs);
            }
        } else {
            p = ne;
        }

        if (emit) {
            size_t dlen;
            char *dec = decode_entities(vs, vl, &dlen);
            fputs("ATTRIBUTE ", stdout);
            print_lower(ns, (size_t)(ne - ns));
            putchar(' ');
            if (dec) {
                print_escaped(dec, dlen);
                free(dec);
            } else {
                print_escaped(vs, vl);
            }
            putchar('\n');
        }
    }
}

static const char *find_close(const char *p, size_t len, const char *name) {
    size_t nl = strlen(name);
    const char *end = p + len;
    for (; p + 2 + nl <= end; p++) {
        if (p[0] == '<' && p[1] == '/' &&
            strncasecmp(p + 2, name, nl) == 0) {
            char c = p[2 + nl];
            if (IS_WS(c) || c == '/' || c == '>') return p;
        }
    }
    return NULL;
}

static size_t g_buf_len;  //Might have NULs

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
    const char *p = buf;
    const char *buf_end = p + g_buf_len;

    while (p < buf_end) {
        if (starts_markup(p)) {
            //comments
            if (p + 3 < buf_end && !memcmp(p, "<!--", 4)) {
                const char *start = p + 4;
                size_t remain = buf_end - start;
                const char *end = memmem(start, remain, "-->", 3);
                if (end) {
                    emit_line("COMMENT", start, (size_t)(end - start));
                    p = end + 3;
                } else {
                    emit_line("COMMENT", start, remain);
                    break;
                }
                continue;
            }

            if (p + 1 < buf_end && p[1] == '!' && p + 8 < buf_end && !strncasecmp(p + 2, "doctype", 7)) {
                const char *start = p + 9;
                while (start < buf_end && IS_WS(*start)) start++;
                const char *end = start;
                while (end < buf_end && *end != '>') end++;
                if (end < buf_end) {
                    emit_line("DOCTYPE", start, (size_t)(end - start));
                    p = end + 1;
                } else {
                    emit_line("DOCTYPE", start, (size_t)(buf_end - start));
                    break;
                }
                continue;
            }

            if (p + 1 < buf_end && (p[1] == '!' || p[1] == '?')) {
                const char *start = p + (p[1] == '!' ? 2 : 1);
                const char *end = start;
                while (end < buf_end && *end != '>') end++;
                if (end < buf_end) {
                    emit_line("COMMENT", start, (size_t)(end - start));
                    p = end + 1;
                } else {
                    emit_line("COMMENT", start, (size_t)(buf_end - start));
                    break;
                }
                continue;
            }

            //start and end tags
            int is_end = (p[1] == '/');
            const char *ns = p + 1 + is_end;
            const char *ne = ns;
            while (ne < buf_end && !IS_WS(*ne) && *ne != '/' && *ne != '>') ne++;

            char name[256];
            size_t nl = (size_t)(ne - ns);
            if (nl > 255) nl = 255;
            for (size_t i = 0; i < nl; i++) name[i] = (char)tolower((unsigned char)ns[i]);
            name[nl] = '\0';

            int self_close;
            const char *end = parse_attrs(ne, 0, &self_close);
            if (!end) {
                printf("UNTERMINATED\n");
                break;
            }

            const char *label;
            if (is_end) {
                label = "END_TAG";
            } else if (is_void(name)) {
                label = "VOID_TAG";
            } else if (self_close) {
                label = "SELF_CLOSE";
            } else {
                label = "START_TAG";
            }
            printf("%s %s\n", label, name);
            if (!is_end) parse_attrs(ne, 1, &self_close);
            p = end;
            
            if (!is_end && (is_rawtext(name) || is_rcdata(name))) {
                size_t remain = buf_end - p;
                const char *close = find_close(p, remain, name);
                if (!close) close = buf_end;
                if (is_rcdata(name)) emit_text(p, (size_t)(close - p));
                else emit_text_raw(p, (size_t)(close - p));
                p = close;
            }
            continue;
        }
        const char *q = p + 1;
        while (q < buf_end && !starts_markup(q)) q++;
        emit_text(p, (size_t)(q - p));
        p = q;
    }

    free(buf);
    return 0;
}
