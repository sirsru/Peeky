#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

//makes the render tree :O

static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xcalloc(n + 1, 1);
    memcpy(p, s, n);
    return p;
}

static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

#define GROW(a, n, cap)                      \
    do {                                     \
        if ((n) == (cap)) {                  \
            (cap) = (cap) ? (cap) * 2 : 16;  \
            (a) = xrealloc((a), (cap) * sizeof *(a)); \
        }                                    \
    } while (0)

static int is_ws_ch(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

//reader

typedef struct {
    int depth;
    char *content;
} Line;

static Line *lines;
static int line_n, line_cap;

//probably some newline fuckery
static void read_dump(FILE *fp) {
    char *buf = NULL;
    size_t cap = 0;
    ssize_t len;
    int cont = 0; // 1 ends with " and 2 ends with -->

    while ((len = getline(&buf, &cap, fp)) != -1) {
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';

        if (cont) {
            Line *prev = &lines[line_n - 1];
            size_t old = strlen(prev->content);
            prev->content = xrealloc(prev->content, old + len + 2);
            prev->content[old] = '\n';
            memcpy(prev->content + old + 1, buf, (size_t)len + 1);
            if ((cont == 1 && len > 0 && buf[len - 1] == '"') ||
                (cont == 2 && len >= 3 && !strcmp(buf + len - 3, "-->")))
                cont = 0;
            continue;
        }

        if (!strncmp(buf, "#document", 9) || !strncmp(buf, "#stylesheet", 11))
            continue;
        if (strncmp(buf, "| ", 2))
            continue;

        const char *p = buf + 2;
        int spaces = 0;
        while (*p == ' ') {
            spaces++;
            p++;
        }
        GROW(lines, line_n, line_cap);
        lines[line_n].depth = spaces / 2;
        lines[line_n].content = xstrdup(p);
        line_n++;

        const char *c = lines[line_n - 1].content;
        size_t cl = strlen(c);
        if (!strncmp(c, "<!--", 4)) {
            if (cl < 3 || strcmp(c + cl - 3, "-->"))
                cont = 2;
        } else {
            const char *eq = strchr(c, '=');
            if ((c[0] == '"' || (eq && eq[1] == '"')) &&
                (!cl || c[cl - 1] != '"'))
                cont = 1;
        }
    }
    free(buf);
}

//unqoute stuff
static char *unquote(const char **p) {
    while (**p == ' ')
        (*p)++;
    if (**p != '"')
        return NULL;
    (*p)++;
    size_t n = 0, cap = 16;
    char *out = xcalloc(cap, 1);
    while (**p && **p != '"') {
        char e;
        if (**p == '\\' && (*p)[1]) {
            (*p)++;
            switch (**p) {
            case 'n': e = '\n'; break;
            case 'r': e = '\r'; break;
            case 't': e = '\t'; break;
            default: e = **p;
            }
            (*p)++;
        } else {
            e = **p;
            (*p)++;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            out = xrealloc(out, cap);
        }
        out[n++] = e;
    }
    if (**p == '"')
        (*p)++;
    out[n] = '\0';
    return out;
}

//CSSOM

typedef struct {
    char *prop, *value;
    int important;
} Decl;

typedef struct {
    const char *comp;
    size_t len;
    int child;
} SelPart;

typedef struct {
    SelPart *parts;
    int n;
    int spec;
} SelComplex;

typedef struct {
    SelComplex *complexes;
    int n;
} CompiledSelector;

typedef struct {
    char *selector;
    Decl *decls;
    int decl_n, decl_cap;
    CompiledSelector compiled;
} Rule;

static Rule *rules;
static int rule_n, rule_cap;

static void compile_selector(char *selector, CompiledSelector *out);

static int is_font_prop(const char *prop) {
    return !strncmp(prop, "font", 4);
}

static void strip_important(char *v, int *imp) {
    size_t n = strlen(v);
    while (n && is_ws_ch(v[n - 1]))
        n--;
    v[n] = '\0';
    if (n >= 10 && !strcasecmp(v + n - 10, "!important")) {
        *imp = 1;
        n -= 10;
        while (n && is_ws_ch(v[n - 1]))
            n--;
        v[n] = '\0';
    }
}

static void parse_css_level(int depth, int *i) {
    while (*i < line_n) {
        if (lines[*i].depth < depth)
            return;
        if (lines[*i].depth > depth) {
            (*i)++;
            continue;
        }
        const char *c = lines[*i].content;

        if (!strncmp(c, "<rule ", 6)) {
            const char *p = c + 6;
            char *sel = unquote(&p);
            Rule *r = NULL;
            if (sel && *sel) {
                GROW(rules, rule_n, rule_cap);
                r = &rules[rule_n++];
                r->selector = sel;
                r->decls = NULL;
                r->decl_n = r->decl_cap = 0;
                r->compiled.complexes = NULL;
                r->compiled.n = 0;
                compile_selector(sel, &r->compiled);
            } else {
                free(sel);
            }
            (*i)++;

            while (*i < line_n && lines[*i].depth == depth + 1 &&
                   !strncmp(lines[*i].content, "<decl ", 6)) {
                const char *q = lines[*i].content + 6;
                char *prop = unquote(&q);
                if (*q == ' ')
                    q++;
                char *val = unquote(&q);
                int imp = 0;
                if (val)
                    strip_important(val, &imp);
                if (r && prop && val && !is_font_prop(prop)) {
                    GROW(r->decls, r->decl_n, r->decl_cap);
                    r->decls[r->decl_n].prop = prop;
                    r->decls[r->decl_n].value = val;
                    r->decls[r->decl_n].important = imp;
                    r->decl_n++;
                    prop = val = NULL;
                }
                free(prop);
                free(val);
                (*i)++;
            }
            //nested stuff
            parse_css_level(depth + 1, i);

        } else if (!strncmp(c, "<at ", 4)) {
            const char *p = c + 4;
            char *name = unquote(&p);
            (*i)++;
            //skip fonts because... well I cant really render those can I?
            int skip = !name || !strncmp(name, "font", 4) ||
                       !strcmp(name, "page") ||
                       (strlen(name) >= 9 &&
                        !strcmp(name + strlen(name) - 9, "keyframes"));
            free(name);
            if (skip) {
                while (*i < line_n && lines[*i].depth > depth)
                    (*i)++;
            } else {
                parse_css_level(depth + 1, i);
            }
        } else {
            (*i)++;
        }
    }
}

//DOM

enum { D_DOCUMENT, D_ELEMENT, D_TEXT };

typedef struct DomNode {
    int type;
    char *ns;    //html, svg, math
    char *name;  //tag name
    char *text;  //raw text
    char **anames, **avalues;
    int attr_n, attr_cap;
    struct DomNode **kids;
    int kid_n, kid_cap;
    struct DomNode *parent;
} DomNode;

static DomNode *node_new(int type) {
    DomNode *n = xcalloc(1, sizeof *n);
    n->type = type;
    return n;
}

static void add_kid(DomNode *n, DomNode *k) {
    GROW(n->kids, n->kid_n, n->kid_cap);
    n->kids[n->kid_n++] = k;
    k->parent = n;
}

static void add_attr(DomNode *n, const char *name, const char *value) {
    if (n->attr_n == n->attr_cap) {
        n->attr_cap = n->attr_cap ? n->attr_cap * 2 : 16;
        n->anames = xrealloc(n->anames, (size_t)n->attr_cap * sizeof *n->anames);
        n->avalues = xrealloc(n->avalues, (size_t)n->attr_cap * sizeof *n->avalues);
    }
    n->anames[n->attr_n] = xstrdup(name);
    n->avalues[n->attr_n] = xstrdup(value);
    n->attr_n++;
}

static const char *attr_get(DomNode *n, const char *name) {
    for (int i = 0; i < n->attr_n; i++)
        if (!strcmp(n->anames[i], name))
            return n->avalues[i];
    return NULL;
}

static DomNode *parse_element(const char *c) {
    const char *s = c + 1;
    size_t len = strlen(c);
    const char *end = (len && c[len - 1] == '>') ? c + len - 1 : c + len;
    while (s < end && is_ws_ch(*s))
        s++;
    const char *t1 = s;
    while (s < end && !is_ws_ch(*s))
        s++;
    const char *q = s;
    while (q < end && is_ws_ch(*q))
        q++;

    DomNode *n = node_new(D_ELEMENT);
    if (q < end) {
        n->ns = xstrndup(t1, (size_t)(s - t1));
        n->name = xstrndup(q, (size_t)(end - q));
    } else {
        n->ns = xstrdup("html");
        n->name = xstrndup(t1, (size_t)(s - t1));
    }
    return n;
}

static DomNode *dom_parse(void) {
    DomNode *doc = node_new(D_DOCUMENT);
    DomNode **stack = NULL;
    int sp = 0, scap = 0;
    GROW(stack, sp, scap);
    stack[sp++] = doc;

    for (int i = 0; i < line_n; i++) {
        int d = lines[i].depth;
        char *c = lines[i].content;
        if (d + 1 >= scap) {
            scap = d + 2;
            stack = xrealloc(stack, (size_t)scap * sizeof *stack);
        }
        DomNode *parent = stack[d];
        if (!parent)
            continue;

        if (!strncmp(c, "<!--", 4) || !strncmp(c, "<!DOCTYPE", 9))
            continue;
        if (!strcmp(c, "content")) {
            while (i + 1 < line_n && lines[i + 1].depth > d)
                i++;
            continue;
        }
        if (c[0] == '<') {
            DomNode *e = parse_element(c);
            add_kid(parent, e);
            stack[d + 1] = e;
        } else if (c[0] == '"') {
            DomNode *t = node_new(D_TEXT);
            size_t n = strlen(c);
            const char *s = c + 1;
            if (n >= 2 && c[n - 1] == '"')
                n -= 2;
            else
                n -= 1;
            t->text = xstrndup(s, n);
            add_kid(parent, t);
        } else {
            char *eq = strchr(c, '=');
            if (eq && parent->type == D_ELEMENT) {
                *eq = '\0';
                char *name = xstrdup(c);
                char *v = eq + 1;
                size_t n = strlen(v);
                if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
                    v[n - 1] = '\0';
                    v++;
                }
                add_attr(parent, name, v);
                free(name);
            }
        }
    }
    free(stack);
    return doc;
}

//selector matching

static int class_has_len(DomNode *e, const char *cl, size_t n) {
    const char *v = attr_get(e, "class");
    if (!v)
        return 0;
    const char *p = v;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *st = p;
        while (*p && *p != ' ')
            p++;
        if ((size_t)(p - st) == n && !strncmp(st, cl, n))
            return 1;
    }
    return 0;
}

static const char *attr_get_len(DomNode *e, const char *name, size_t len) {
    for (int i = 0; i < e->attr_n; i++)
        if (strlen(e->anames[i]) == len && !strncmp(e->anames[i], name, len))
            return e->avalues[i];
    return NULL;
}

static int is_first_child(DomNode *e) {
    DomNode *parent = e->parent;
    if (!parent) return 0;
    for (int i = 0; i < parent->kid_n; i++) {
        if (parent->kids[i] == e)
            return i == 0;
    }
    return 0;
}

static int is_empty(DomNode *e) {
    return e->kid_n == 0;
}

static int pseudo_match_len(DomNode *e, const char *pseudo, size_t len,
                            int is_element);

static int match_compound_len(const char *s, size_t len, DomNode *e) {
    if (e->type != D_ELEMENT)
        return 0;
    const char *p = s;
    const char *end = s + len;
    int matched = 0;
    while (p < end) {
        while (p < end && *p == ' ')
            p++;
        if (p >= end)
            break;
        if (*p == '*') {
            p++;
            matched = 1;
        } else if (*p == '.') {
            p++;
            const char *st = p;
            while (p < end && *p != '.' && *p != '#' && *p != '[' && *p != ':' &&
                   *p != ' ')
                p++;
            if (!class_has_len(e, st, (size_t)(p - st)))
                return 0;
            matched = 1;
        } else if (*p == '#') {
            p++;
            const char *st = p;
            while (p < end && *p != '.' && *p != '#' && *p != '[' && *p != ':' &&
                   *p != ' ')
                p++;
            const char *id = attr_get(e, "id");
            size_t n = (size_t)(p - st);
            int ok = id && strlen(id) == n && !memcmp(st, id, n);
            if (!ok)
                return 0;
            matched = 1;
        } else if (*p == ':') {
            p++;
            int is_element = 0;
            if (p < end && *p == ':') {
                p++;
                is_element = 1;
            }
            const char *st = p;
            while (p < end && *p != '.' && *p != '#' && *p != '[' && *p != ':' &&
                   *p != ' ')
                p++;
            size_t plen = (size_t)(p - st);
            if (plen > 0) {
                if (pseudo_match_len(e, st, plen, is_element))
                    matched = 1;
                else if (!is_element)
                    return 0;
            } else {
                if (!is_element)
                    return 0;
            }
        } else if (*p == '[') {
            p++;
            const char *ast = p;
            while (p < end && *p != ']' && *p != '=' && *p != '~' && *p != '|' &&
                   *p != '^' && *p != '$' && *p != '*')
                p++;
            const char *have = attr_get_len(e, ast, (size_t)(p - ast));
            int ok = 0;
            if (have) {
                if (p < end && *p == '=') {
                    p++;
                    const char *vst = p;
                    while (p < end && *p != ']')
                        p++;
                    size_t wn = (size_t)(p - vst);
                    char *want = xstrndup(vst, wn);
                    if (wn >= 2 && want[0] == '"' && want[wn - 1] == '"') {
                        want[wn - 1] = '\0';
                        memmove(want, want + 1, wn - 2);
                        want[wn - 2] = '\0';
                        wn -= 2;
                    }
                    ok = strlen(have) == wn && !memcmp(want, have, wn);
                    free(want);
                } else {
                    ok = 1;
                }
            }
            if (p < end && *p == ']')
                p++;
            if (!ok)
                return 0;
            matched = 1;
        } else if (isalpha((unsigned char)*p)) {
            const char *st = p;
            while (p < end && *p != '.' && *p != '#' && *p != '[' && *p != ':' &&
                   *p != ' ')
                p++;
            size_t tlen = (size_t)(p - st);
            int ok = strlen(e->name) == tlen && !strncasecmp(st, e->name, tlen);
            if (!ok)
                return 0;
            matched = 1;
        } else {
            p++; //no idea what the fuck this would be
        }
    }
    return matched;
}

static int pseudo_match_len(DomNode *e, const char *pseudo, size_t len,
                            int is_element) {
    if (!e || e->type != D_ELEMENT)
        return 0;
    if (is_element)
        return 0;
    if (len == 11 && !memcmp(pseudo, "first-child", 11))
        return is_first_child(e);
    if (len == 5 && !memcmp(pseudo, "empty", 5))
        return is_empty(e);
    if (len == 4 && !memcmp(pseudo, "root", 4))
        return e->parent && e->parent->type == D_DOCUMENT;
    return 0;
}

static int match_complex_compiled(SelComplex *cx, DomNode *e) {
    if (!cx->n ||
        !match_compound_len(cx->parts[cx->n - 1].comp, cx->parts[cx->n - 1].len, e))
        return 0;
    int pi = cx->n - 2;
    DomNode *cur = e;
    while (pi >= 0 && cur) {
        if (cx->parts[pi + 1].child) {
            cur = cur->parent;
            if (!cur ||
                !match_compound_len(cx->parts[pi].comp, cx->parts[pi].len, cur))
                return 0;
        } else {
            DomNode *a = cur->parent;
            while (a &&
                   !match_compound_len(cx->parts[pi].comp, cx->parts[pi].len, a))
                a = a->parent;
            if (!a)
                return 0;
            cur = a;
        }
        pi--;
    }
    return pi < 0;
}

static void compile_selector(char *selector, CompiledSelector *out) {
    out->complexes = NULL;
    out->n = 0;
    int cap = 0;
    const char *p = selector;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const char *ss = p;
        int depth = 0;
        while (*p && (*p != ',' || depth > 0)) {
            if (*p == '[')
                depth++;
            else if (*p == ']' && depth)
                depth--;
            p++;
        }
        const char *qend = ss + (p - ss);
        SelPart *parts = NULL;
        int np = 0, pcap = 0;
        int child = 0;
        const char *q = ss;
        while (q < qend) {
            while (q < qend && *q == ' ')
                q++;
            if (q >= qend)
                break;
            if (*q == '>') {
                child = 1;
                q++;
                continue;
            }
            const char *st = q;
            while (q < qend && *q != ' ' && *q != '>')
                q++;
            GROW(parts, np, pcap);
            parts[np].comp = st;
            parts[np].len = (size_t)(q - st);
            parts[np].child = child;
            child = 0;
            np++;
        }
        int spec = 0;
        for (int i = 0; i < np; i++) {
            const char *s = parts[i].comp;
            size_t slen = parts[i].len;
            if (slen && isalpha((unsigned char)*s))
                spec += 1;
            for (size_t j = 0; j < slen; j++) {
                char c = s[j];
                if (c == '#')
                    spec += 100;
                else if (c == ':') {
                    if (j + 1 < slen && s[j + 1] == ':') {
                        spec += 1;
                        j++;
                    } else
                        spec += 10;
                } else if (c == '.' || c == '[')
                    spec += 10;
            }
        }
        GROW(out->complexes, out->n, cap);
        out->complexes[out->n].parts = parts;
        out->complexes[out->n].n = np;
        out->complexes[out->n].spec = spec;
        out->n++;
        if (*p == ',')
            p++;
    }
}

//cascade

typedef struct {
    int important, spec, order;
    char *prop, *val;
} Cand;

static Cand *cands;
static int cand_n, cand_cap;

static void add_cand(int important, int spec, int order, const char *prop,
                     const char *val) {
    if (is_font_prop(prop))
        return;
    GROW(cands, cand_n, cand_cap);
    cands[cand_n].important = important;
    cands[cand_n].spec = spec;
    cands[cand_n].order = order;
    cands[cand_n].prop = xstrdup(prop);
    cands[cand_n].val = xstrdup(val);
    cand_n++;
}

static int cand_cmp(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    if (x->important != y->important)
        return x->important - y->important;
    if (x->spec != y->spec)
        return x->spec - y->spec;
    return x->order - y->order;
}

//expand shorthand

static void expand_margin(const char *val) {
    float v[4] = {0,0,0,0};
    int n = 0;
    const char *p = val;
    while (*p && n < 4) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *end;
        v[n++] = strtof(p, &end);
        if (end == p) break;
        p = end;
        while (*p && !isspace((unsigned char)*p)) p++;
    }
    if (n == 1) { v[1] = v[2] = v[3] = v[0]; }
    else if (n == 2) { v[2] = v[0]; v[3] = v[1]; }
    else if (n == 3) { v[3] = v[1]; }
    const char *sides[] = {"margin-top", "margin-right", "margin-bottom", "margin-left"};
    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "%gpx", v[i]);
        add_cand(0, 0, 0, sides[i], buf);
    }
}

static void expand_padding(const char *val) {
    float v[4] = {0,0,0,0};
    int n = 0;
    const char *p = val;
    while (*p && n < 4) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *end;
        v[n++] = strtof(p, &end);
        if (end == p) break;
        p = end;
        while (*p && !isspace((unsigned char)*p)) p++;
    }
    if (n == 1) { v[1] = v[2] = v[3] = v[0]; }
    else if (n == 2) { v[2] = v[0]; v[3] = v[1]; }
    else if (n == 3) { v[3] = v[1]; }
    const char *sides[] = {"padding-top", "padding-right", "padding-bottom", "padding-left"};
    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "%gpx", v[i]);
        add_cand(0, 0, 0, sides[i], buf);
    }
}

static void expand_border(const char *val) {
    const char *p = val;
    while (*p == ' ') p++;
    if (isdigit((unsigned char)*p) || *p == '.') {
        char *end;
        float w = strtof(p, &end);
        if (end != p) {
            char buf[32];
            snprintf(buf, sizeof buf, "%gpx", w);
            add_cand(0, 0, 0, "border-width", buf);
            p = end;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
    } else if (*p == '0') {
        add_cand(0, 0, 0, "border-width", "0px");
        p++;
    }
    while (*p == ' ') p++;
    //check style
    if (!strncmp(p, "solid", 5) || !strncmp(p, "dashed", 6) ||
        !strncmp(p, "dotted", 6) || !strncmp(p, "double", 6) ||
        !strncmp(p, "none", 4) || !strncmp(p, "hidden", 6) ||
        !strncmp(p, "outset", 6) || !strncmp(p, "inset", 5) ||
        !strncmp(p, "groove", 6) || !strncmp(p, "ridge", 5)) {
        size_t slen = strcspn(p, " ");
        char *style = xstrndup(p, slen);
        add_cand(0, 0, 0, "border-style", style);
        free(style);
        p += slen;
    }
    while (*p == ' ') p++;
    //check COLOR
    if (*p) {
        add_cand(0, 0, 0, "border-color", p);
    }
}

static void expand_background(const char *val) {
    if (strstr(val, "url(")) {
        const char *url = strstr(val, "url(");
        url += 4;
        while (*url == ' ') url++;
        if (*url == '"' || *url == '\'') url++;
        const char *end = url;
        while (*end && *end != ')' && *end != '"' && *end != '\'') end++;
        char *img = xstrndup(url, (size_t)(end - url));
        add_cand(0, 0, 0, "background-image", img);
        free(img);
    }
}

static void expand_shorthand(const char *prop, const char *val) {
    if (!strcmp(prop, "margin")) expand_margin(val);
    else if (!strcmp(prop, "padding")) expand_padding(val);
    else if (!strcmp(prop, "border")) expand_border(val);
    else if (!strcmp(prop, "background")) expand_background(val);
    else if (!strcmp(prop, "border-top")) expand_border(val);
    else if (!strcmp(prop, "border-right")) expand_border(val);
    else if (!strcmp(prop, "border-bottom")) expand_border(val);
    else if (!strcmp(prop, "border-left")) expand_border(val);
}

static void collect_ua(DomNode *e) {
    const char *v;
    if (!strcmp(e->name, "center"))
        add_cand(0, -2, -2, "text-align", "center");
    if ((v = attr_get(e, "align")) && (!strcasecmp(v, "center") || !strcasecmp(v, "right")))
        add_cand(0, -1, -1, "text-align", v);
    if (!strcmp(e->name, "body")) {
        add_cand(0, -4, -4, "margin", "8px");
    }
    if (!strcmp(e->name, "h1")) {
        add_cand(0, -4, -4, "font-size", "2em");
        add_cand(0, -4, -4, "margin", "0.67em 0");
    }
    if (!strcmp(e->name, "h2")) {
        add_cand(0, -4, -4, "font-size", "1.5em");
        add_cand(0, -4, -4, "margin", "0.83em 0");
    }
    if (!strcmp(e->name, "h3")) {
        add_cand(0, -4, -4, "font-size", "1.17em");
        add_cand(0, -4, -4, "margin", "1em 0");
    }
    if (!strcmp(e->name, "h4")) {
        add_cand(0, -4, -4, "font-size", "1em");
        add_cand(0, -4, -4, "margin", "1.33em 0");
    }
    if (!strcmp(e->name, "h5")) {
        add_cand(0, -4, -4, "font-size", "0.83em");
        add_cand(0, -4, -4, "margin", "1.67em 0");
    }
    if (!strcmp(e->name, "h6")) {
        add_cand(0, -4, -4, "font-size", "0.67em");
        add_cand(0, -4, -4, "margin", "2.33em 0");
    }
    if (!strcmp(e->name, "ul") || !strcmp(e->name, "ol")) {
        add_cand(0, -4, -4, "padding-left", "40px");
    }
    if (!strcmp(e->name, "li")) {
        add_cand(0, -4, -4, "display", "list-item");
    }
    if (!strcmp(e->name, "a")) {
        add_cand(0, -4, -4, "color", "blue");
        add_cand(0, -4, -4, "text-decoration", "underline");
    }
    if (!strcmp(e->name, "a:visited")) {
        add_cand(0, -4, -4, "color", "purple");
    }
    if (!strcmp(e->name, "input")) {
        add_cand(0, -4, -4, "border", "1px solid");
        add_cand(0, -4, -4, "padding", "1px");
    }
    if (!strcmp(e->name, "table")) {
        add_cand(0, -4, -4, "border-collapse", "collapse");
    }
    if (!strcmp(e->name, "td") || !strcmp(e->name, "th")) {
        add_cand(0, -4, -4, "border", "1px solid");
        add_cand(0, -4, -4, "padding", "1px");
    }
    if (!strcmp(e->name, "blockquote")) {
        add_cand(0, -4, -4, "margin-left", "40px");
        add_cand(0, -4, -4, "margin-right", "40px");
        add_cand(0, -4, -4, "border-left", "3px solid");
        add_cand(0, -4, -4, "padding-left", "10px");
    }
    if (!strcmp(e->name, "pre")) {
        add_cand(0, -4, -4, "white-space", "pre");
    }
    if (!strcmp(e->name, "hr")) {
        add_cand(0, -4, -4, "border-top", "1px solid");
    }
    if (!strcmp(e->name, "nav") || !strcmp(e->name, "header") ||
        !strcmp(e->name, "footer") || !strcmp(e->name, "section") ||
        !strcmp(e->name, "article") || !strcmp(e->name, "main") ||
        !strcmp(e->name, "aside") || !strcmp(e->name, "figure") ||
        !strcmp(e->name, "figcaption") || !strcmp(e->name, "details") ||
        !strcmp(e->name, "summary") || !strcmp(e->name, "dialog")) {
        add_cand(0, -4, -4, "display", "block");
    }
}

static void collect_presentational(DomNode *e) {
    const char *v;
    if ((v = attr_get(e, "bgcolor")))
        add_cand(0, -1, -1, "background-color", v);
    if (!strcmp(e->name, "font") && (v = attr_get(e, "color")))
        add_cand(0, -1, -1, "color", v);
    if (!strcmp(e->name, "body") && (v = attr_get(e, "text")))
        add_cand(0, -1, -1, "color", v);
    if (!strcmp(e->name, "hr") && (v = attr_get(e, "color")))
        add_cand(0, -1, -1, "background-color", v);
    if (!strcmp(e->name, "img")) {
        char b[48];
        if ((v = attr_get(e, "width")) && !strchr(v, '%')) {
            snprintf(b, sizeof b, "%spx", v); add_cand(0, -1, -1, "width", b);
        }
        if ((v = attr_get(e, "height")) && !strchr(v, '%')) {
            snprintf(b, sizeof b, "%spx", v); add_cand(0, -1, -1, "height", b);
        }
        if ((v = attr_get(e, "src"))) {
            add_cand(0, -1, -1, "image-src", v);
        }
    }
    if (!strcmp(e->name, "frame")) {
        if ((v = attr_get(e, "src")))
            add_cand(0, -1, -1, "image-src", v);
    }
}

static void collect_inline(DomNode *e) {
    const char *s = attr_get(e, "style");
    if (!s) return;
    char *buf = xstrdup(s);
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char *decl_start = p;
        while (*p && *p != ';') p++;
        int decl_len = (int)(p - decl_start);
        if (*p == ';') p++;
        char *colon = NULL;
        for (int i = 0; i < decl_len; i++) {
            if (decl_start[i] == ':') { colon = decl_start + i; break; }
        }
        if (!colon) continue;
        int prop_len = (int)(colon - decl_start);
        char *prop = xstrndup(decl_start, (size_t)prop_len);
        char *val = xstrndup(colon + 1, (size_t)(decl_len - prop_len - 1));
        char *pe = prop + prop_len - 1;
        while (pe >= prop && (isspace((unsigned char)*pe) || *pe == ' ')) *pe-- = '\0';
        char *vs = val;
        while (*vs == ' ' || *vs == '\t') vs++;
        char *ve = vs + strlen(vs) - 1;
        while (ve > vs && (*ve == ' ' || *ve == '\t' || *ve == '\r' || *ve == '\n')) *ve-- = '\0';
        int important = 0;
        size_t vlen = strlen(vs);
        if (vlen > 9 && !strcmp(vs + vlen - 9, "!important")) {
            important = 1;
            vs[vlen - 9] = '\0';
            ve = vs + strlen(vs) - 1;
            while (ve > vs && (*ve == ' ' || *ve == '\t')) *ve-- = '\0';
        }
        if (!strcmp(prop, "margin") || !strcmp(prop, "padding") ||
            !strcmp(prop, "border") || !strcmp(prop, "background") ||
            !strcmp(prop, "border-top") || !strcmp(prop, "border-right") ||
            !strcmp(prop, "border-bottom") || !strcmp(prop, "border-left")) {
            expand_shorthand(prop, vs);
        } else {
            add_cand(important, 1000, 0, prop, vs);
        }
        free(prop);
        free(val);
    }
    free(buf);
}

static void collect_cands(DomNode *e) {
    collect_ua(e);
    collect_presentational(e);
    for (int i = 0; i < rule_n; i++) {
        Rule *r = &rules[i];
        int best = -1;
        for (int c = 0; c < r->compiled.n; c++) {
            if (match_complex_compiled(&r->compiled.complexes[c], e)) {
                int s = r->compiled.complexes[c].spec;
                if (s > best)
                    best = s;
            }
        }
        if (best < 0)
            continue;
        for (int k = 0; k < r->decl_n; k++) {
            const char *prop = r->decls[k].prop;
            const char *val = r->decls[k].value;
            if (!strcmp(prop, "margin") || !strcmp(prop, "padding") ||
                !strcmp(prop, "border") || !strcmp(prop, "background") ||
                !strcmp(prop, "border-top") || !strcmp(prop, "border-right") ||
                !strcmp(prop, "border-bottom") || !strcmp(prop, "border-left")) {
                expand_shorthand(prop, val);
            } else {
                add_cand(r->decls[k].important, best, i, prop, val);
            }
        }
    }
    collect_inline(e);
}

typedef struct {
    char *prop, *val;
} Pair;

typedef struct {
    Pair *p;
    int n, cap;
} Map;

static void map_set(Map *m, const char *prop, const char *val) {
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->p[i].prop, prop)) {
            free(m->p[i].val);
            m->p[i].val = xstrdup(val);
            return;
        }
    GROW(m->p, m->n, m->cap);
    m->p[m->n].prop = xstrdup(prop);
    m->p[m->n].val = xstrdup(val);
    m->n++;
}

static const char *map_get(Map *m, const char *prop) {
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->p[i].prop, prop))
            return m->p[i].val;
    return NULL;
}

static void map_free(Map *m) {
    for (int i = 0; i < m->n; i++) {
        free(m->p[i].prop);
        free(m->p[i].val);
    }
    free(m->p);
}

static int is_inherited(const char *prop);

static char *substitute_vars(const char *src, Map *cm, int depth) {
    if (depth > 8)
        return NULL; //I aint doing more than that vro
    size_t cap = strlen(src) * 2 + 16;
    char *out = xcalloc(1, cap);
    size_t w = 0;
    const char *p = src;
    while (*p) {
        if (!strncmp(p, "var(", 4)) {
            const char *start = p + 4;
            int d = 1;
            const char *q = start;
            while (*q && d > 0) {
                if (*q == '(') d++;
                else if (*q == ')') d--;
                q++;
            }
            if (d != 0) {
                if (w + 4 >= cap) { cap *= 2; out = xrealloc(out, cap); }
                memcpy(out + w, "var(", 4); w += 4;
                p += 4;
                continue;
            }
            size_t inner_len = (size_t)((q - 1) - start);
            char *inner = xstrndup(start, inner_len);
            char *name = inner;
            char *fallback = NULL;
            int pd = 0;
            for (char *c = inner; *c; c++) {
                if (*c == '(' || *c == '[') pd++;
                else if (*c == ')' || *c == ']') pd--;
                else if (*c == ',' && pd == 0) {
                    *c = '\0';
                    fallback = c + 1;
                    break;
                }
            }
            while (*name == ' ' || *name == '\t') name++;
            char *ne = name + strlen(name);
            while (ne > name && (ne[-1] == ' ' || ne[-1] == '\t' || ne[-1] == ','))
                *--ne = '\0';
            if (fallback)
                while (*fallback == ' ' || *fallback == '\t') fallback++;
            const char *repl = NULL;
            if (name[0] == '-' && name[1] == '-')
                repl = map_get(cm, name);
            char *final_repl = NULL;
            int ok = 0;
            if (repl) {
                final_repl = substitute_vars(repl, cm, depth + 1);
                ok = final_repl != NULL;
            } else if (fallback && *fallback) {
                final_repl = substitute_vars(fallback, cm, depth + 1);
                ok = final_repl != NULL;
            }
            free(inner);
            if (!ok) { free(out); return NULL; }
            size_t rl = strlen(final_repl);
            if (w + rl + 1 >= cap) { cap = (w + rl + 1) * 2; out = xrealloc(out, cap); }
            memcpy(out + w, final_repl, rl); w += rl;
            free(final_repl);
            p = q;
        } else {
            if (w + 1 >= cap) { cap *= 2; out = xrealloc(out, cap); }
            out[w++] = *p++;
        }
    }
    out[w] = '\0';
    return out;
}

static void substitute_map(Map *out, Map *parent) {
    Map result = {0};
    for (int i = 0; i < out->n; i++) {
        const char *prop = out->p[i].prop;
        const char *val = out->p[i].val;
        if (prop[0] == '-' && prop[1] == '-') {
            map_set(&result, prop, val); //custom props stay raw
            continue;
        }
        char *sub = substitute_vars(val, out, 0);
        if (!sub) {
            //your shi is cooked vro, time to go back to old values vro
            if (is_inherited(prop) && parent) {
                const char *inh = map_get(parent, prop);
                if (inh)
                    map_set(&result, prop, inh);
            }
            continue;
        }
        map_set(&result, prop, sub);
        free(sub);
    }
    map_free(out);
    *out = result;
}

static const char *INHERITED[] = {
    "color",          "direction",     "visibility",    "white-space",
    "text-align",     "text-indent",   "text-transform", "letter-spacing",
    "word-spacing",   "line-height",   "list-style-type", "list-style-position",
    "list-style-image", "cursor",      "tab-size",      "hyphens",
    "quotes",         "text-size-adjust", NULL};

static int is_inherited(const char *prop) {
    if (prop[0] == '-' && prop[1] == '-')
        return 1; //custom properties inherit down the tree
    for (int i = 0; INHERITED[i]; i++)
        if (!strcmp(INHERITED[i], prop))
            return 1;
    return 0;
}

static void compute_style(DomNode *e, Map *parent, Map *out) {
    if (parent)
        for (int i = 0; i < parent->n; i++)
            if (is_inherited(parent->p[i].prop))
                map_set(out, parent->p[i].prop, parent->p[i].val);
    const char *bgcolor = attr_get(e, "bgcolor");
    if (bgcolor) map_set(out, "background-color", bgcolor);
    const char *color = attr_get(e, "color");
    if (color) map_set(out, "color", color);
    const char *background = attr_get(e, "background");
    if (background) map_set(out, "background-image", background);
    const char *width = attr_get(e, "width");
    if (width) map_set(out, "width", width);
    const char *height = attr_get(e, "height");
    if (height) map_set(out, "height", height);
    const char *cols = attr_get(e, "cols");
    if (cols) map_set(out, "cols", cols);
    const char *rows = attr_get(e, "rows");
    if (rows) map_set(out, "rows", rows);
    const char *align = attr_get(e, "align");
    if (align) map_set(out, "text-align", align);
    const char *valign = attr_get(e, "valign");
    if (valign) map_set(out, "vertical-align", valign);
    const char *border = attr_get(e, "border");
    if (border) map_set(out, "border-width", border);
    const char *cellpadding = attr_get(e, "cellpadding");
    if (cellpadding) map_set(out, "padding", cellpadding);
    const char *cellspacing = attr_get(e, "cellspacing");
    if (cellspacing) map_set(out, "border-spacing", cellspacing);
    cand_n = 0;
    collect_cands(e);
    if (cand_n > 1)
        qsort(cands, (size_t)cand_n, sizeof *cands, cand_cmp);
    for (int i = 0; i < cand_n; i++) {
        map_set(out, cands[i].prop, cands[i].val);
        free(cands[i].prop);
        free(cands[i].val);
    }
    substitute_map(out, parent);
}

//render tree!!!1!!

static const char *NON_RENDERING[] = {
    "head",   "script", "style",  "meta",   "link",
    "title",  "base",   "basefont", "noscript", "template", NULL};

static int name_in(const char *name, const char **list) {
    for (int i = 0; list[i]; i++)
        if (!strcasecmp(name, list[i]))
            return 1;
    return 0;
}

static void indent(int depth) {
    fputs("| ", stdout);
    for (int i = 0; i < depth; i++)
        fputs("  ", stdout);
}

static void print_esc(const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default: putchar((unsigned char)*s);
        }
    }
}

static int all_ws(const char *s) {
    for (; *s; s++)
        if (!is_ws_ch(*s))
            return 0;
    return 1;
}

static void render_node(DomNode *n, int depth, Map *parent_style) {
    if (n->type == D_TEXT) {
        if (!all_ws(n->text)) {
            indent(depth);
            putchar('"');
            print_esc(n->text);
            puts("\"");
        }
        return;
    }
    if (name_in(n->name, NON_RENDERING))
        return;

    Map style = {0};
    compute_style(n, parent_style, &style);
    const char *disp = map_get(&style, "display");
    if (disp && !strcmp(disp, "none")) {
        map_free(&style);
        return;
    }
    if (!disp && attr_get(n, "hidden")) {
        map_free(&style);
        return;
    }

    indent(depth);
    if (!strcmp(n->ns, "svg"))
        printf("<svg %s>\n", n->name);
    else if (!strcmp(n->ns, "math"))
        printf("<math %s>\n", n->name);
    else
        printf("<%s>\n", n->name);

    for (int i = 0; i < style.n; i++) {
        indent(depth + 1);
        printf("%s: ", style.p[i].prop);
        print_esc(style.p[i].val);
        putchar('\n');
    }
    for (int i = 0; i < n->kid_n; i++)
        render_node(n->kids[i], depth + 1, &style);
    map_free(&style);
}

static FILE *open_input(const char *arg) {
    if (!strcmp(arg, "-"))
        return stdin;
    FILE *fp = fopen(arg, "r");
    if (!fp) {
        fprintf(stderr, "render-tree: cannot open %s\n", arg);
        exit(1);
    }
    return fp;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: render-tree <cssom-dump> <dom-dump>\n");
        return 1;
    }

    FILE *f = open_input(argv[1]);
    read_dump(f);
    if (f != stdin)
        fclose(f);
    int i = 0;
    parse_css_level(0, &i);

    for (int k = 0; k < line_n; k++)
        free(lines[k].content);
    free(lines);
    lines = NULL;
    line_n = 0;
    line_cap = 0;
    f = open_input(argv[2]);
    read_dump(f);
    if (f != stdin)
        fclose(f);
    DomNode *doc = dom_parse();

    puts("#render-tree");
    for (int k = 0; k < doc->kid_n; k++)
        render_node(doc->kids[k], 0, NULL);

    for (int k = 0; k < line_n; k++)
        free(lines[k].content);
    free(lines);
    for (int k = 0; k < rule_n; k++) {
        Rule *r = &rules[k];
        for (int c = 0; c < r->compiled.n; c++)
            free(r->compiled.complexes[c].parts);
        free(r->compiled.complexes);
        free(r->selector);
        for (int d = 0; d < r->decl_n; d++) {
            free(r->decls[d].prop);
            free(r->decls[d].value);
        }
        free(r->decls);
    }
    free(rules);
    return 0;
}
