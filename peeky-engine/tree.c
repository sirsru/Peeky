#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

//Utils

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

static char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

#define GROW(arr, n, cap)                                                         \
    do {                                                                          \
        if ((n) >= (cap)) {                                                       \
            (cap) = (cap) ? (cap) * 2 : 8;                                        \
            (arr) = xrealloc((arr), sizeof(*(arr)) * (size_t)(cap));              \
        }                                                                         \
    } while (0)

static int is_ws(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static int name_in(const char *s, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(*list, s)) return 1;
    return 0;
}

static int starts_ci(const char *s, const char *prefix) {
    return !strncasecmp(s, prefix, strlen(prefix));
}

//DOM

typedef enum { NS_HTML, NS_SVG, NS_MATHML } Ns;
typedef enum { N_DOCUMENT, N_FRAGMENT, N_ELEMENT, N_TEXT, N_COMMENT, N_DOCTYPE } NodeType;

typedef struct {
    char *prefix; //namespace designator
    char *name; //local name
    char *value;
} Attr;

typedef struct Node {
    NodeType type;
    Ns ns;
    char *name; 
    char *data;
    size_t len;
    char *pubid, *sysid;
    Attr *attrs;
    int attr_n, attr_cap;
    struct Node **kids;
    int kid_n, kid_cap;
    struct Node *parent;
    struct Node *content;
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

static void attrs_add(Attr **a, int *n, int *cap, const char *prefix, const char *name,
                      const char *value) {
    GROW(*a, *n, *cap);
    (*a)[*n].prefix = prefix ? xstrdup(prefix) : NULL;
    (*a)[*n].name = xstrdup(name);
    (*a)[*n].value = xstrdup(value);
    (*n)++;
}

static void attrs_free(Attr *a, int n) {
    for (int i = 0; i < n; i++) {
        free(a[i].prefix);
        free(a[i].name);
        free(a[i].value);
    }
    free(a);
}

static void free_all_nodes(void) {
    for (size_t i = 0; i < all_n; i++) {
        Node *n = all_nodes[i];
        attrs_free(n->attrs, n->attr_n);
        free(n->kids);
        free(n->name);
        free(n->data);
        free(n->pubid);
        free(n->sysid);
        free(n);
    }
    free(all_nodes);
}

static void node_detach(Node *n) {
    Node *p = n->parent;
    if (!p) return;
    for (int i = 0; i < p->kid_n; i++) {
        if (p->kids[i] == n) {
            memmove(&p->kids[i], &p->kids[i + 1], sizeof(Node *) * (size_t)(p->kid_n - i - 1));
            p->kid_n--;
            break;
        }
    }
    n->parent = NULL;
}

static void node_insert(Node *parent, Node *child, Node *before) {
    node_detach(child);
    GROW(parent->kids, parent->kid_n, parent->kid_cap);
    int at = parent->kid_n;
    if (before) {
        for (int i = 0; i < parent->kid_n; i++)
            if (parent->kids[i] == before) { at = i; break; }
    }
    memmove(&parent->kids[at + 1], &parent->kids[at], sizeof(Node *) * (size_t)(parent->kid_n - at));
    parent->kids[at] = child;
    parent->kid_n++;
    child->parent = parent;
}

static Node *create_element(Ns ns, const char *name, const Attr *attrs, int n) {
    Node *e = node_new(N_ELEMENT);
    e->ns = ns;
    e->name = xstrdup(name);
    for (int i = 0; i < n; i++)
        attrs_add(&e->attrs, &e->attr_n, &e->attr_cap, attrs[i].prefix, attrs[i].name, attrs[i].value);
    if (ns == NS_HTML && !strcmp(name, "template")) e->content = node_new(N_FRAGMENT);
    return e;
}

static int is_el(const Node *n, Ns ns, const char *name) {
    return n && n->type == N_ELEMENT && n->ns == ns && !strcmp(n->name, name);
}

static int is_html(const Node *n, const char *name) {
    return is_el(n, NS_HTML, name);
}

static int is_html_in(const Node *n, const char *const *list) {
    return n && n->type == N_ELEMENT && n->ns == NS_HTML && name_in(n->name, list);
}

static const char *attr_get(const Attr *a, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!a[i].prefix && !strcmp(a[i].name, name)) return a[i].value;
    return NULL;
}

//tokens

typedef enum { T_START, T_END, T_CHAR, T_COMMENT, T_DOCTYPE, T_EOF } TokType;
typedef enum { CK_WS, CK_NUL, CK_OTHER } CharKind;

typedef struct {
    TokType type;
    char *name;
    Attr *attrs;
    int attr_n, attr_cap;
    int self_closing;
    const char *data;
    size_t len;
    CharKind ck;
    char *dt_name, *dt_pub, *dt_sys;
    int dt_force_quirks;
} Token;

static void token_free(Token *t) {
    free(t->name);
    attrs_free(t->attrs, t->attr_n);
    free(t->dt_name);
    free(t->dt_pub);
    free(t->dt_sys);
}

static int tok_is_start(const Token *t, const char *n) {
    return t->type == T_START && !strcmp(t->name, n);
}
static int tok_start_in(const Token *t, const char *const *l) {
    return t->type == T_START && name_in(t->name, l);
}
static int tok_end_in(const Token *t, const char *const *l) {
    return t->type == T_END && name_in(t->name, l);
}
static int tok_ws(const Token *t) {
    return t->type == T_CHAR && t->ck == CK_WS;
}

//name lists

static const char *const L_IMPLIED[] = {"dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc", NULL};
static const char *const L_IMPLIED_THOROUGH[] = {"caption", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr", NULL};


//I really love typing!1!
static const char *const L_SPECIAL_HTML[] = {
    "address", "applet", "area", "article", "aside", "base", "basefont", "bgsound", "blockquote",
    "body", "br", "button", "caption", "center", "col", "colgroup", "dd", "details", "dir", "div",
    "dl", "dt", "embed", "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset",
    "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "iframe", "img",
    "input", "keygen", "li", "link", "listing", "main", "marquee", "menu", "meta", "nav", "noembed",
    "noframes", "noscript", "object", "ol", "p", "param", "plaintext", "pre", "script", "search",
    "section", "select", "source", "style", "summary", "table", "tbody", "td", "template",
    "textarea", "tfoot", "th", "thead", "title", "tr", "track", "ul", "wbr", "xmp", NULL};
static const char *const L_SPECIAL_MATHML[] = {"mi", "mo", "mn", "ms", "mtext", "annotation-xml", NULL};
static const char *const L_SPECIAL_SVG[] = {"foreignObject", "desc", "title", NULL};

static const char *const L_SCOPE_HTML[] = {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "template", NULL};
static const char *const L_TABLE_SCOPE[] = {"html", "table", "template", NULL};

static const char *const L_HEAD_START[] = {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title", NULL};
static const char *const L_BLOCK_P[] = {
    "address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl",
    "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "main", "menu", "nav", "ol",
    "p", "search", "section", "summary", "ul", NULL};
static const char *const L_HEADINGS[] = {"h1", "h2", "h3", "h4", "h5", "h6", NULL};
static const char *const L_END_BLOCK[] = {
    "address", "article", "aside", "blockquote", "button", "center", "details", "dialog", "dir",
    "div", "dl", "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "listing",
    "main", "menu", "nav", "ol", "pre", "search", "section", "summary", "ul", NULL};
static const char *const L_FORMATTING[] = {"a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u", NULL};
static const char *const L_FMT_SIMPLE[] = {"b", "big", "code", "em", "font", "i", "s", "small", "strike", "strong", "tt", "u", NULL};
static const char *const L_VOID_BODY[] = {"area", "br", "embed", "img", "keygen", "wbr", NULL};
static const char *const L_PARAM[] = {"param", "source", "track", NULL};
static const char *const L_BODY_IGNORE[] = {"caption", "col", "colgroup", "frame", "head", "tbody", "td", "tfoot", "th", "thead", "tr", NULL};
static const char *const L_TABLE_CTX[] = {"table", "template", "html", NULL};
static const char *const L_TBODY_CTX[] = {"tbody", "tfoot", "thead", "template", "html", NULL};
static const char *const L_ROW_CTX[] = {"tr", "template", "html", NULL};
static const char *const L_TEXT_TABLE[] = {"table", "tbody", "template", "tfoot", "thead", "tr", NULL};
static const char *const L_TABLE_SECTION[] = {"tbody", "tfoot", "thead", NULL};
static const char *const L_CELLS[] = {"td", "th", NULL};
static const char *const L_BREAKOUT[] = {
    "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed",
    "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta",
    "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup",
    "table", "tt", "u", "ul", "var", NULL};
static const char *const L_MATHML_TEXT_IP[] = {"mi", "mo", "mn", "ms", "mtext", NULL};

//state of parser

typedef enum {
    M_INITIAL, M_BEFORE_HTML, M_BEFORE_HEAD, M_IN_HEAD, M_AFTER_HEAD, M_IN_BODY, M_TEXT,
    M_IN_TABLE, M_IN_TABLE_TEXT, M_IN_CAPTION, M_IN_COLUMN_GROUP, M_IN_TABLE_BODY, M_IN_ROW,
    M_IN_CELL, M_IN_SELECT, M_IN_SELECT_IN_TABLE, M_IN_TEMPLATE, M_AFTER_BODY, M_IN_FRAMESET,
    M_AFTER_FRAMESET, M_AFTER_AFTER_BODY, M_AFTER_AFTER_FRAMESET
} Mode;

static Node *document;
static Node **open_els;
static int open_n, open_cap;
static Node **afe;
static int afe_n, afe_cap;
static Mode mode, orig_mode;
static Mode *tmpl_modes;
static int tmpl_n, tmpl_cap;
static Node *head_ptr, *form_ptr;
static int frameset_ok = 1, quirks, foster, skip_lf;
static char *ptc;
static size_t ptc_len, ptc_cap;
static int ptc_nonws;

static void process_token(Token *t);
static void run_mode(Mode m, Token *t);

//stack

static Node *cur(void) {
    return open_n ? open_els[open_n - 1] : NULL;
}

static void push(Node *n) {
    GROW(open_els, open_n, open_cap);
    open_els[open_n++] = n;
}

static Node *pop(void) {
    return open_n ? open_els[--open_n] : NULL;
}

static int stack_index(const Node *n) {
    for (int i = open_n - 1; i >= 0; i--)
        if (open_els[i] == n) return i;
    return -1;
}

static void stack_remove_at(int i) {
    memmove(&open_els[i], &open_els[i + 1], sizeof(Node *) * (size_t)(open_n - i - 1));
    open_n--;
}

static void stack_remove(const Node *n) {
    int i = stack_index(n);
    if (i >= 0) stack_remove_at(i);
}

static void stack_insert_at(int i, Node *n) {
    GROW(open_els, open_n, open_cap);
    memmove(&open_els[i + 1], &open_els[i], sizeof(Node *) * (size_t)(open_n - i));
    open_els[i] = n;
    open_n++;
}

static void pop_until_html(const char *name) {
    while (open_n) {
        Node *n = pop();
        if (is_html(n, name)) break;
    }
}

static void pop_until_html_in(const char *const *list) {
    while (open_n) {
        Node *n = pop();
        if (is_html_in(n, list)) break;
    }
}

static int has_template(void) {
    for (int i = 0; i < open_n; i++)
        if (is_html(open_els[i], "template")) return 1;
    return 0;
}

static int is_special(const Node *n) {
    if (n->type != N_ELEMENT) return 0;
    switch (n->ns) {
        case NS_HTML:   return name_in(n->name, L_SPECIAL_HTML);
        case NS_MATHML: return name_in(n->name, L_SPECIAL_MATHML);
        case NS_SVG:    return name_in(n->name, L_SPECIAL_SVG);
    }
    return 0;
}

//scopes

typedef enum { SC_DEFAULT, SC_LIST_ITEM, SC_BUTTON, SC_TABLE, SC_SELECT } Scope;

static int scope_boundary(const Node *n, Scope sc) {
    if (sc == SC_SELECT) return !(is_html(n, "optgroup") || is_html(n, "option"));
    if (sc == SC_TABLE) return n->ns == NS_HTML && name_in(n->name, L_TABLE_SCOPE);
    switch (n->ns) {
        case NS_HTML:
            if (name_in(n->name, L_SCOPE_HTML)) return 1;
            if (sc == SC_LIST_ITEM && (!strcmp(n->name, "ol") || !strcmp(n->name, "ul"))) return 1;
            if (sc == SC_BUTTON && !strcmp(n->name, "button")) return 1;
            return 0;
        case NS_MATHML: return name_in(n->name, L_SPECIAL_MATHML);
        case NS_SVG:    return name_in(n->name, L_SPECIAL_SVG);
    }
    return 0;
}

static int has_in_scope(const char *name, Scope sc) {
    for (int i = open_n - 1; i >= 0; i--) {
        const Node *n = open_els[i];
        if (is_html(n, name)) return 1;
        if (scope_boundary(n, sc)) return 0;
    }
    return 0;
}

static int has_in_scope_list(const char *const *list, Scope sc) {
    for (int i = open_n - 1; i >= 0; i--) {
        const Node *n = open_els[i];
        if (is_html_in(n, list)) return 1;
        if (scope_boundary(n, sc)) return 0;
    }
    return 0;
}

static int has_node_in_scope(const Node *target, Scope sc) {
    for (int i = open_n - 1; i >= 0; i--) {
        const Node *n = open_els[i];
        if (n == target) return 1;
        if (scope_boundary(n, sc)) return 0;
    }
    return 0;
}

static void gen_implied(const char *except) {
    for (;;) {
        Node *n = cur();
        if (!is_html_in(n, L_IMPLIED)) break;
        if (except && !strcmp(n->name, except)) break;
        pop();
    }
}

static void gen_implied_thorough(void) {
    for (;;) {
        Node *n = cur();
        if (!is_html_in(n, L_IMPLIED) && !is_html_in(n, L_IMPLIED_THOROUGH)) break;
        pop();
    }
}

static void close_p(void) {
    gen_implied("p");
    pop_until_html("p");
}

//insterting nodes

typedef struct {
    Node *parent;
    Node *before;   //insert before, null causes append
} Loc;

static Loc approp_place(Node *override) {
    Node *target = override ? override : cur();
    Loc loc = {target, NULL};

    if (foster && is_html_in(target, L_TEXT_TABLE) && !is_html(target, "template")) {
        int ti = -1, tpi = -1;
        for (int i = open_n - 1; i >= 0; i--) {
            if (tpi < 0 && is_html(open_els[i], "template")) tpi = i;
            if (ti < 0 && is_html(open_els[i], "table")) ti = i;
        }
        if (tpi >= 0 && (ti < 0 || tpi > ti)) {
            loc.parent = open_els[tpi];
        } else if (ti < 0) {
            loc.parent = open_els[0];
        } else if (open_els[ti]->parent) {
            loc.parent = open_els[ti]->parent;
            loc.before = open_els[ti];
        } else {
            loc.parent = open_els[ti - 1];
        }
    }
    if (is_html(loc.parent, "template") && !loc.before) loc.parent = loc.parent->content;
    return loc;
}

static int has_element_kid(const Node *p) {
    for (int i = 0; i < p->kid_n; i++)
        if (p->kids[i]->type == N_ELEMENT) return 1;
    return 0;
}

static Node *insert_el(Ns ns, const char *name, const Attr *attrs, int an) {
    Node *e = create_element(ns, name, attrs, an);
    Loc l = approp_place(NULL);
    if (!(l.parent->type == N_DOCUMENT && has_element_kid(l.parent)))
        node_insert(l.parent, e, l.before);
    push(e);
    return e;
}

static Node *insert_html(const Token *t) {
    return insert_el(NS_HTML, t->name, t->attrs, t->attr_n);
}

static Node *insert_html_named(const char *name) {
    return insert_el(NS_HTML, name, NULL, 0);
}

static void insert_chars(const char *s, size_t n) {
    if (n == 0) return;
    Loc l = approp_place(NULL);
    if (l.parent->type == N_DOCUMENT) return;

    Node *prev = NULL;
    if (l.before) {
        for (int i = 0; i < l.parent->kid_n; i++)
            if (l.parent->kids[i] == l.before) { prev = i ? l.parent->kids[i - 1] : NULL; break; }
    } else if (l.parent->kid_n) {
        prev = l.parent->kids[l.parent->kid_n - 1];
    }

    if (prev && prev->type == N_TEXT) {
        prev->data = xrealloc(prev->data, prev->len + n + 1);
        memcpy(prev->data + prev->len, s, n);
        prev->len += n;
        prev->data[prev->len] = '\0';
    } else {
        Node *t = node_new(N_TEXT);
        t->data = xstrndup(s, n);
        t->len = n;
        node_insert(l.parent, t, l.before);
    }
}

static void add_comment(Node *parent, Node *before, const char *s, size_t n) {
    Node *c = node_new(N_COMMENT);
    c->data = xstrndup(s, n);
    c->len = n;
    node_insert(parent, c, before);
}

static void insert_comment_tok(const Token *t) {
    Loc l = approp_place(NULL);
    add_comment(l.parent, l.before, t->data, t->len);
}

static void generic_raw(const Token *t) {
    insert_html(t);
    orig_mode = mode;
    mode = M_TEXT;
}

static void tmpl_push(Mode m) {
    GROW(tmpl_modes, tmpl_n, tmpl_cap);
    tmpl_modes[tmpl_n++] = m;
}

static void tmpl_pop(void) {
    if (tmpl_n) tmpl_n--;
}

static int afe_index(const Node *n) {
    for (int i = afe_n - 1; i >= 0; i--)
        if (afe[i] == n) return i;
    return -1;
}

static void afe_remove_at(int i) {
    memmove(&afe[i], &afe[i + 1], sizeof(Node *) * (size_t)(afe_n - i - 1));
    afe_n--;
}

static void afe_remove(const Node *n) {
    int i = afe_index(n);
    if (i >= 0) afe_remove_at(i);
}

static void afe_insert_at(int i, Node *n) {
    GROW(afe, afe_n, afe_cap);
    memmove(&afe[i + 1], &afe[i], sizeof(Node *) * (size_t)(afe_n - i));
    afe[i] = n;
    afe_n++;
}

static void afe_marker(void) {
    GROW(afe, afe_n, afe_cap);
    afe[afe_n++] = NULL;
}

static void afe_clear_to_marker(void) {
    while (afe_n > 0) {
        Node *e = afe[--afe_n];
        if (!e) break;
    }
}

static int same_element(const Node *a, const Node *b) {
    if (a->ns != b->ns || strcmp(a->name, b->name) || a->attr_n != b->attr_n) return 0;
    for (int i = 0; i < a->attr_n; i++) {
        int found = 0;
        for (int j = 0; j < b->attr_n && !found; j++) {
            const Attr *x = &a->attrs[i], *y = &b->attrs[j];
            found = !strcmp(x->name, y->name) && !strcmp(x->value, y->value) &&
                    ((!x->prefix && !y->prefix) || (x->prefix && y->prefix && !strcmp(x->prefix, y->prefix)));
        }
        if (!found) return 0;
    }
    return 1;
}

static void afe_push(Node *n) {
    int count = 0, first = -1;
    for (int i = afe_n - 1; i >= 0 && afe[i]; i--) {
        if (same_element(afe[i], n)) { count++; first = i; }
    }
    if (count >= 3) afe_remove_at(first);
    GROW(afe, afe_n, afe_cap);
    afe[afe_n++] = n;
}

static Node *clone_and_insert(const Node *old) {
    return insert_el(old->ns, old->name, old->attrs, old->attr_n);
}

static void reconstruct_afe(void) {
    if (afe_n == 0) return;
    int i = afe_n - 1;
    if (!afe[i] || stack_index(afe[i]) >= 0) return;
    while (i > 0) {
        Node *e = afe[i - 1];
        if (!e || stack_index(e) >= 0) break;
        i--;
    }
    for (; i < afe_n; i++) afe[i] = clone_and_insert(afe[i]);
}

static void afe_remove_adjust(int idx, int *bookmark) {
    afe_remove_at(idx);
    if (idx < *bookmark) (*bookmark)--;
}

static int adoption_agency(const char *subject) {
    Node *c = cur();
    if (is_html(c, subject) && afe_index(c) < 0) {
        pop();
        return 1;
    }

    for (int outer = 0; outer < 8; outer++) {
        Node *fe = NULL;
        int fe_afe = -1;
        for (int i = afe_n - 1; i >= 0; i--) {
            if (!afe[i]) break;
            if (afe[i]->ns == NS_HTML && !strcmp(afe[i]->name, subject)) {
                fe = afe[i];
                fe_afe = i;
                break;
            }
        }
        if (!fe) return 0;

        int fe_stack = stack_index(fe);
        if (fe_stack < 0) {
            afe_remove_at(fe_afe);
            return 1;
        }
        if (!has_node_in_scope(fe, SC_DEFAULT)) return 1;

        Node *fb = NULL;
        int fb_idx = -1;
        for (int i = fe_stack + 1; i < open_n; i++) {
            if (is_special(open_els[i])) { fb = open_els[i]; fb_idx = i; break; }
        }
        if (!fb) {
            open_n = fe_stack;
            afe_remove(fe);
            return 1;
        }

        Node *common = open_els[fe_stack - 1];
        int bookmark = fe_afe;
        Node *last = fb;
        int idx = fb_idx;
        int inner = 0;

        for (;;) {
            inner++;
            idx--;
            Node *node = open_els[idx];
            if (node == fe) break;
            if (inner > 3 && afe_index(node) >= 0) afe_remove_adjust(afe_index(node), &bookmark);
            int ai = afe_index(node);
            if (ai < 0) {
                stack_remove_at(idx);
                continue;
            }
            Node *nn = create_element(node->ns, node->name, node->attrs, node->attr_n);
            afe[ai] = nn;
            open_els[idx] = nn;
            node = nn;
            if (last == fb) bookmark = ai + 1;
            node_insert(node, last, NULL);
            last = node;
        }

        Loc l = approp_place(common);
        node_insert(l.parent, last, l.before);

        Node *nf = create_element(fe->ns, fe->name, fe->attrs, fe->attr_n);
        while (fb->kid_n) node_insert(nf, fb->kids[0], NULL);
        node_insert(fb, nf, NULL);

        afe_remove_adjust(afe_index(fe), &bookmark);
        afe_insert_at(bookmark, nf);

        stack_remove(fe);
        stack_insert_at(stack_index(fb) + 1, nf);
    }
    return 1;
}

static void clear_to_context(const char *const *ctx) {
    while (open_n && !is_html_in(cur(), ctx)) pop();
}

static void reset_mode(void) {
    int last = 0;
    for (int i = open_n - 1; i >= 0; i--) {
        Node *n = open_els[i];
        if (i == 0) last = 1;

        if (is_html(n, "select")) {
            if (!last) {
                for (int j = i - 1; j >= 0; j--) {
                    Node *a = open_els[j];
                    if (is_html(a, "template")) break;
                    if (is_html(a, "table")) { mode = M_IN_SELECT_IN_TABLE; return; }
                }
            }
            mode = M_IN_SELECT;
            return;
        }
        if ((is_html(n, "td") || is_html(n, "th")) && !last) { mode = M_IN_CELL; return; }
        if (is_html(n, "tr")) { mode = M_IN_ROW; return; }
        if (is_html_in(n, L_TABLE_SECTION)) { mode = M_IN_TABLE_BODY; return; }
        if (is_html(n, "caption")) { mode = M_IN_CAPTION; return; }
        if (is_html(n, "colgroup")) { mode = M_IN_COLUMN_GROUP; return; }
        if (is_html(n, "table")) { mode = M_IN_TABLE; return; }
        if (is_html(n, "template")) { mode = tmpl_modes[tmpl_n - 1]; return; }
        if (is_html(n, "head") && !last) { mode = M_IN_HEAD; return; }
        if (is_html(n, "body")) { mode = M_IN_BODY; return; }
        if (is_html(n, "frameset")) { mode = M_IN_FRAMESET; return; }
        if (is_html(n, "html")) { mode = head_ptr ? M_AFTER_HEAD : M_BEFORE_HEAD; return; }
        if (last) { mode = M_IN_BODY; return; }
    }
    mode = M_IN_BODY;
}

//doctype

static const char *const QUIRKS_PUBLIC_PREFIXES[] = {
    "+//Silmaril//dtd html Pro v0r11 19970101//",
    "-//AS//DTD HTML 3.0 asWedit + extensions//",
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//",
    "-//IETF//DTD HTML 2.0 Level 1//",
    "-//IETF//DTD HTML 2.0 Level 2//",
    "-//IETF//DTD HTML 2.0 Strict Level 1//",
    "-//IETF//DTD HTML 2.0 Strict Level 2//",
    "-//IETF//DTD HTML 2.0 Strict//",
    "-//IETF//DTD HTML 2.0//",
    "-//IETF//DTD HTML 2.1E//",
    "-//IETF//DTD HTML 3.0//",
    "-//IETF//DTD HTML 3.2 Final//",
    "-//IETF//DTD HTML 3.2//",
    "-//IETF//DTD HTML 3//",
    "-//IETF//DTD HTML Level 0//",
    "-//IETF//DTD HTML Level 1//",
    "-//IETF//DTD HTML Level 2//",
    "-//IETF//DTD HTML Level 3//",
    "-//IETF//DTD HTML Strict Level 0//",
    "-//IETF//DTD HTML Strict Level 1//",
    "-//IETF//DTD HTML Strict Level 2//",
    "-//IETF//DTD HTML Strict Level 3//",
    "-//IETF//DTD HTML Strict//",
    "-//IETF//DTD HTML//",
    "-//Metrius//DTD Metrius Presentational//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
    "-//Netscape Comm. Corp.//DTD HTML//",
    "-//Netscape Comm. Corp.//DTD Strict HTML//",
    "-//O'Reilly and Associates//DTD HTML 2.0//",
    "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
    "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//",
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
    "-//Spyglass//DTD HTML 2.0 Extended//",
    "-//Sun Microsystems Corp.//DTD HotJava HTML//",
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
    "-//W3C//DTD HTML 3 1995-03-24//",
    "-//W3C//DTD HTML 3.2 Draft//",
    "-//W3C//DTD HTML 3.2 Final//",
    "-//W3C//DTD HTML 3.2//",
    "-//W3C//DTD HTML 3.2S Draft//",
    "-//W3C//DTD HTML 4.0 Frameset//",
    "-//W3C//DTD HTML 4.0 Transitional//",
    "-//W3C//DTD HTML Experimental 19960712//",
    "-//W3C//DTD HTML Experimental 970421//",
    "-//W3C//DTD W3 HTML//",
    "-//W3O//DTD W3 HTML 3.0//",
    "-//WebTechs//DTD Mozilla HTML 2.0//",
    "-//WebTechs//DTD Mozilla HTML//",
    NULL};

static int doctype_is_quirks(const Token *t) {
    if (t->dt_force_quirks) return 1;
    if (!t->dt_name || strcmp(t->dt_name, "html")) return 1;
    const char *p = t->dt_pub, *s = t->dt_sys;
    if (p) {
        if (!strcasecmp(p, "-//W3O//DTD W3 HTML Strict 3.0//EN//") ||
            !strcasecmp(p, "-/W3C/DTD HTML 4.0 Transitional/EN") || !strcasecmp(p, "HTML"))
            return 1;
        for (int i = 0; QUIRKS_PUBLIC_PREFIXES[i]; i++)
            if (starts_ci(p, QUIRKS_PUBLIC_PREFIXES[i])) return 1;
        if (!s && (starts_ci(p, "-//W3C//DTD HTML 4.01 Frameset//") ||
                   starts_ci(p, "-//W3C//DTD HTML 4.01 Transitional//")))
            return 1;
    }
    if (s && !strcasecmp(s, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")) return 1;
    return 0;
}

static char *doctype_quoted(const char *s, size_t n, size_t *i, int *bad) {
    if (*i >= n || (s[*i] != '"' && s[*i] != '\'')) { *bad = 1; return NULL; }
    char q = s[(*i)++];
    size_t st = *i;
    while (*i < n && s[*i] != q) (*i)++;
    char *id = xstrndup(s + st, *i - st);
    if (*i < n) (*i)++;
    else *bad = 1;
    return id;
}

static void parse_doctype(Token *t, const char *s, size_t n) {
    size_t i = 0;
    while (i < n && is_ws(s[i])) i++;
    size_t st = i;
    while (i < n && !is_ws(s[i])) i++;
    if (i > st) {
        t->dt_name = xstrndup(s + st, i - st);
        for (char *c = t->dt_name; *c; c++) *c = (char)tolower((unsigned char)*c);
    } else {
        t->dt_force_quirks = 1;
    }
    while (i < n && is_ws(s[i])) i++;
    if (i + 6 <= n && !strncasecmp(s + i, "PUBLIC", 6)) {
        i += 6;
        while (i < n && is_ws(s[i])) i++;
        t->dt_pub = doctype_quoted(s, n, &i, &t->dt_force_quirks);
        while (i < n && is_ws(s[i])) i++;
        if (i < n) {
            int bad = 0;
            t->dt_sys = doctype_quoted(s, n, &i, &bad);
            if (bad) t->dt_force_quirks = 1;
        }
    } else if (i + 6 <= n && !strncasecmp(s + i, "SYSTEM", 6)) {
        i += 6;
        while (i < n && is_ws(s[i])) i++;
        t->dt_sys = doctype_quoted(s, n, &i, &t->dt_force_quirks);
    } else if (i < n) {
        t->dt_force_quirks = 1;
    }
}

//content helpers

static const char *const SVG_TAGS[] = {
    "altGlyph", "altGlyphDef", "altGlyphItem", "animateColor", "animateMotion", "animateTransform",
    "clipPath", "feBlend", "feColorMatrix", "feComponentTransfer", "feComposite", "feConvolveMatrix",
    "feDiffuseLighting", "feDisplacementMap", "feDistantLight", "feDropShadow", "feFlood",
    "feFuncA", "feFuncB", "feFuncG", "feFuncR", "feGaussianBlur", "feImage", "feMerge",
    "feMergeNode", "feMorphology", "feOffset", "fePointLight", "feSpecularLighting", "feSpotLight",
    "feTile", "feTurbulence", "foreignObject", "glyphRef", "linearGradient", "radialGradient",
    "textPath", NULL};

static const char *const SVG_ATTRS[] = {
    "attributeName", "attributeType", "baseFrequency", "baseProfile", "calcMode", "clipPathUnits",
    "diffuseConstant", "edgeMode", "filterUnits", "glyphRef", "gradientTransform", "gradientUnits",
    "kernelMatrix", "kernelUnitLength", "keyPoints", "keySplines", "keyTimes", "lengthAdjust",
    "limitingConeAngle", "markerHeight", "markerUnits", "markerWidth", "maskContentUnits",
    "maskUnits", "numOctaves", "pathLength", "patternContentUnits", "patternTransform",
    "patternUnits", "pointsAtX", "pointsAtY", "pointsAtZ", "preserveAlpha", "preserveAspectRatio",
    "primitiveUnits", "refX", "refY", "repeatCount", "repeatDur", "requiredExtensions",
    "requiredFeatures", "specularConstant", "specularExponent", "spreadMethod", "startOffset",
    "stdDeviation", "stitchTiles", "surfaceScale", "systemLanguage", "tableValues", "targetX",
    "targetY", "textLength", "viewBox", "viewTarget", "xChannelSelector", "yChannelSelector",
    "zoomAndPan", NULL};

static const char *fix_case(const char *name, const char *const *table) {
    for (int i = 0; table[i]; i++)
        if (!strcasecmp(table[i], name)) return table[i];
    return name;
}

static const char *const FOREIGN_ATTRS[][3] = {
    {"xlink:actuate", "xlink", "actuate"}, {"xlink:arcrole", "xlink", "arcrole"},
    {"xlink:href", "xlink", "href"},       {"xlink:role", "xlink", "role"},
    {"xlink:show", "xlink", "show"},       {"xlink:title", "xlink", "title"},
    {"xlink:type", "xlink", "type"},       {"xml:lang", "xml", "lang"},
    {"xml:space", "xml", "space"},         {"xmlns", "xmlns", "xmlns"},
    {"xmlns:xlink", "xmlns", "xlink"},
};

static Node *insert_foreign(const Token *t, Ns ns) {
    const char *name = ns == NS_SVG ? fix_case(t->name, SVG_TAGS) : t->name;
    Attr *a = NULL;
    int an = 0, ac = 0;
    for (int i = 0; i < t->attr_n; i++) {
        const char *aname = t->attrs[i].name;
        const char *prefix = NULL;
        if (ns == NS_MATHML && !strcmp(aname, "definitionurl")) aname = "definitionURL";
        if (ns == NS_SVG) aname = fix_case(aname, SVG_ATTRS);
        for (size_t k = 0; k < sizeof FOREIGN_ATTRS / sizeof FOREIGN_ATTRS[0]; k++) {
            if (!strcmp(aname, FOREIGN_ATTRS[k][0])) {
                prefix = FOREIGN_ATTRS[k][1];
                aname = FOREIGN_ATTRS[k][2];
                break;
            }
        }
        attrs_add(&a, &an, &ac, prefix, aname, t->attrs[i].value);
    }
    Node *e = insert_el(ns, name, a, an);
    attrs_free(a, an);
    return e;
}

static int is_mathml_text_ip(const Node *n) {
    return n->type == N_ELEMENT && n->ns == NS_MATHML && name_in(n->name, L_MATHML_TEXT_IP);
}

static int is_html_ip(const Node *n) {
    if (n->type != N_ELEMENT) return 0;
    if (n->ns == NS_MATHML && !strcmp(n->name, "annotation-xml")) {
        const char *enc = attr_get(n->attrs, n->attr_n, "encoding");
        return enc && (!strcasecmp(enc, "text/html") || !strcasecmp(enc, "application/xhtml+xml"));
    }
    if (n->ns == NS_SVG) return name_in(n->name, L_SPECIAL_SVG);
    return 0;
}

//insertion modes

static void add_missing_attrs(Node *e, const Token *t) {
    for (int i = 0; i < t->attr_n; i++) {
        if (!attr_get(e->attrs, e->attr_n, t->attrs[i].name))
            attrs_add(&e->attrs, &e->attr_n, &e->attr_cap, NULL, t->attrs[i].name, t->attrs[i].value);
    }
}

static void tok_set_name(Token *t, const char *name) {
    free(t->name);
    t->name = xstrdup(name);
}

static void mode_initial(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (tok_ws(t)) return;
            break;
        case T_COMMENT:
            add_comment(document, NULL, t->data, t->len);
            return;
        case T_DOCTYPE: {
            Node *d = node_new(N_DOCTYPE);
            d->name = xstrdup(t->dt_name ? t->dt_name : "");
            d->pubid = t->dt_pub ? xstrdup(t->dt_pub) : NULL;
            d->sysid = t->dt_sys ? xstrdup(t->dt_sys) : NULL;
            node_insert(document, d, NULL);
            quirks = doctype_is_quirks(t);
            mode = M_BEFORE_HTML;
            return;
        }
        default:
            break;
    }
    quirks = 1;
    mode = M_BEFORE_HTML;
    process_token(t);
}

static void mode_before_html(Token *t) {
    switch (t->type) {
        case T_DOCTYPE:
            return;
        case T_COMMENT:
            add_comment(document, NULL, t->data, t->len);
            return;
        case T_CHAR:
            if (tok_ws(t)) return;
            break;
        case T_START:
            if (!strcmp(t->name, "html")) {
                Node *e = create_element(NS_HTML, "html", t->attrs, t->attr_n);
                node_insert(document, e, NULL);
                push(e);
                mode = M_BEFORE_HEAD;
                return;
            }
            break;
        case T_END:
            if (!name_in(t->name, (const char *const[]){"head", "body", "html", "br", NULL})) return;
            break;
        default:
            break;
    }
    Node *e = create_element(NS_HTML, "html", NULL, 0);
    node_insert(document, e, NULL);
    push(e);
    mode = M_BEFORE_HEAD;
    process_token(t);
}

static void mode_before_head(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (tok_ws(t)) return;
            break;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_DOCTYPE:
            return;
        case T_START:
            if (!strcmp(t->name, "html")) { run_mode(M_IN_BODY, t); return; }
            if (!strcmp(t->name, "head")) {
                head_ptr = insert_html(t);
                mode = M_IN_HEAD;
                return;
            }
            break;
        case T_END:
            if (!name_in(t->name, (const char *const[]){"head", "body", "html", "br", NULL})) return;
            break;
        default:
            break;
    }
    head_ptr = insert_html_named("head");
    mode = M_IN_HEAD;
    process_token(t);
}

static void mode_in_head(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (tok_ws(t)) { insert_chars(t->data, t->len); return; }
            break;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_DOCTYPE:
            return;
        case T_START: {
            const char *n = t->name;
            if (!strcmp(n, "html")) { run_mode(M_IN_BODY, t); return; }
            if (!strcmp(n, "base") || !strcmp(n, "basefont") || !strcmp(n, "bgsound") ||
                !strcmp(n, "link") || !strcmp(n, "meta")) {
                insert_html(t);
                pop();
                return;
            }
            if (!strcmp(n, "title") || !strcmp(n, "noscript") || !strcmp(n, "noframes") ||
                !strcmp(n, "style")) {
                generic_raw(t);
                return;
            }
            if (!strcmp(n, "script")) {
                insert_html(t);
                orig_mode = mode;
                mode = M_TEXT;
                return;
            }
            if (!strcmp(n, "template")) {
                insert_html(t);
                afe_marker();
                frameset_ok = 0;
                mode = M_IN_TEMPLATE;
                tmpl_push(M_IN_TEMPLATE);
                return;
            }
            if (!strcmp(n, "head")) return;
            break;
        }
        case T_END: {
            const char *n = t->name;
            if (!strcmp(n, "head")) {
                pop();
                mode = M_AFTER_HEAD;
                return;
            }
            if (!strcmp(n, "template")) {
                if (!has_template()) return;
                gen_implied_thorough();
                pop_until_html("template");
                afe_clear_to_marker();
                tmpl_pop();
                reset_mode();
                return;
            }
            if (strcmp(n, "body") && strcmp(n, "html") && strcmp(n, "br")) return;
            break;
        }
        default:
            break;
    }
    pop();
    mode = M_AFTER_HEAD;
    process_token(t);
}

static void mode_after_head(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (tok_ws(t)) { insert_chars(t->data, t->len); return; }
            break;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_DOCTYPE:
            return;
        case T_START: {
            const char *n = t->name;
            if (!strcmp(n, "html")) { run_mode(M_IN_BODY, t); return; }
            if (!strcmp(n, "body")) {
                insert_html(t);
                frameset_ok = 0;
                mode = M_IN_BODY;
                return;
            }
            if (!strcmp(n, "frameset")) {
                insert_html(t);
                mode = M_IN_FRAMESET;
                return;
            }
            if (name_in(n, L_HEAD_START)) {
                push(head_ptr);
                run_mode(M_IN_HEAD, t);
                stack_remove(head_ptr);
                return;
            }
            if (!strcmp(n, "head")) return;
            break;
        }
        case T_END: {
            const char *n = t->name;
            if (!strcmp(n, "template")) { run_mode(M_IN_HEAD, t); return; }
            if (strcmp(n, "body") && strcmp(n, "html") && strcmp(n, "br")) return;
            break;
        }
        default:
            break;
    }
    insert_html_named("body");
    mode = M_IN_BODY;
    process_token(t);
}

static void any_other_end_tag(const Token *t) {
    for (int i = open_n - 1; i >= 0; i--) {
        Node *n = open_els[i];
        if (is_html(n, t->name)) {
            gen_implied(t->name);
            open_n = i;
            return;
        }
        if (is_special(n)) return;
    }
}

static void in_body_start(Token *t);

static void in_body_end(Token *t) {
    const char *n = t->name;

    if (!strcmp(n, "template")) { run_mode(M_IN_HEAD, t); return; }
    if (!strcmp(n, "body")) {
        if (!has_in_scope("body", SC_DEFAULT)) return;
        mode = M_AFTER_BODY;
        return;
    }
    if (!strcmp(n, "html")) {
        if (!has_in_scope("body", SC_DEFAULT)) return;
        mode = M_AFTER_BODY;
        process_token(t);
        return;
    }
    if (name_in(n, L_VOID_BODY)) return;
    if (name_in(n, L_END_BLOCK)) {
        if (!has_in_scope(n, SC_DEFAULT)) return;
        gen_implied(NULL);
        pop_until_html(n);
        return;
    }
    if (!strcmp(n, "form")) {
        if (!has_template()) {
            Node *nd = form_ptr;
            form_ptr = NULL;
            if (!nd || !has_node_in_scope(nd, SC_DEFAULT)) return;
            gen_implied(NULL);
            stack_remove(nd);
        } else {
            if (!has_in_scope("form", SC_DEFAULT)) return;
            gen_implied(NULL);
            pop_until_html("form");
        }
        return;
    }
    if (!strcmp(n, "p")) {
        if (!has_in_scope("p", SC_BUTTON)) insert_html_named("p");
        close_p();
        return;
    }
    if (!strcmp(n, "li")) {
        if (!has_in_scope("li", SC_LIST_ITEM)) return;
        gen_implied("li");
        pop_until_html("li");
        return;
    }
    if (!strcmp(n, "dd") || !strcmp(n, "dt")) {
        if (!has_in_scope(n, SC_DEFAULT)) return;
        gen_implied(n);
        pop_until_html(n);
        return;
    }
    if (name_in(n, L_HEADINGS)) {
        if (!has_in_scope_list(L_HEADINGS, SC_DEFAULT)) return;
        gen_implied(NULL);
        pop_until_html_in(L_HEADINGS);
        return;
    }
    if (name_in(n, L_FORMATTING)) {
        if (!adoption_agency(n)) any_other_end_tag(t);
        return;
    }
    if (!strcmp(n, "applet") || !strcmp(n, "marquee") || !strcmp(n, "object")) {
        if (!has_in_scope(n, SC_DEFAULT)) return;
        gen_implied(NULL);
        pop_until_html(n);
        afe_clear_to_marker();
        return;
    }
    any_other_end_tag(t);
}

static void in_body_start(Token *t) {
    const char *n = t->name;

    if (!strcmp(n, "html")) {
        if (has_template()) return;
        add_missing_attrs(open_els[0], t);
        return;
    }
    if (name_in(n, L_HEAD_START)) { run_mode(M_IN_HEAD, t); return; }
    if (!strcmp(n, "body")) {
        if (open_n < 2 || !is_html(open_els[1], "body") || has_template()) return;
        frameset_ok = 0;
        add_missing_attrs(open_els[1], t);
        return;
    }
    if (!strcmp(n, "frameset")) {
        if (open_n < 2 || !is_html(open_els[1], "body")) return;
        if (!frameset_ok) return;
        node_detach(open_els[1]);
        open_n = 1;
        insert_html(t);
        mode = M_IN_FRAMESET;
        return;
    }
    if (name_in(n, L_BLOCK_P)) {
        if (has_in_scope("p", SC_BUTTON)) close_p();
        insert_html(t);
        return;
    }
    if (name_in(n, L_HEADINGS)) {
        if (has_in_scope("p", SC_BUTTON)) close_p();
        if (is_html_in(cur(), L_HEADINGS)) pop();
        insert_html(t);
        return;
    }
    if (!strcmp(n, "pre") || !strcmp(n, "listing")) {
        if (has_in_scope("p", SC_BUTTON)) close_p();
        insert_html(t);
        skip_lf = 1;
        frameset_ok = 0;
        return;
    }
    if (!strcmp(n, "form")) {
        if (form_ptr && !has_template()) return;
        if (has_in_scope("p", SC_BUTTON)) close_p();
        Node *e = insert_html(t);
        if (!has_template()) form_ptr = e;
        return;
    }
    if (!strcmp(n, "li")) {
        frameset_ok = 0;
        for (int i = open_n - 1; i >= 0; i--) {
            Node *nd = open_els[i];
            if (is_html(nd, "li")) {
                gen_implied("li");
                pop_until_html("li");
                break;
            }
            if (is_special(nd) && !is_html(nd, "address") && !is_html(nd, "div") && !is_html(nd, "p")) break;
        }
        if (has_in_scope("p", SC_BUTTON)) close_p();
        insert_html(t);
        return;
    }
    if (!strcmp(n, "dd") || !strcmp(n, "dt")) {
        frameset_ok = 0;
        for (int i = open_n - 1; i >= 0; i--) {
            Node *nd = open_els[i];
            if (is_html(nd, "dd") || is_html(nd, "dt")) {
                gen_implied(nd->name);
                pop_until_html(nd->name);
                break;
            }
            if (is_special(nd) && !is_html(nd, "address") && !is_html(nd, "div") && !is_html(nd, "p")) break;
        }
        if (has_in_scope("p", SC_BUTTON)) close_p();
        insert_html(t);
        return;
    }
    if (!strcmp(n, "plaintext")) {
        if (has_in_scope("p", SC_BUTTON)) close_p();
        insert_html(t);
        return;
    }
    if (!strcmp(n, "button")) {
        if (has_in_scope("button", SC_DEFAULT)) {
            gen_implied(NULL);
            pop_until_html("button");
        }
        reconstruct_afe();
        insert_html(t);
        frameset_ok = 0;
        return;
    }
    if (!strcmp(n, "a")) {
        Node *ae = NULL;
        for (int i = afe_n - 1; i >= 0 && afe[i]; i--) {
            if (is_html(afe[i], "a")) { ae = afe[i]; break; }
        }
        if (ae) {
            adoption_agency("a");
            afe_remove(ae);
            stack_remove(ae);
        }
        reconstruct_afe();
        afe_push(insert_html(t));
        return;
    }
    if (name_in(n, L_FMT_SIMPLE)) {
        reconstruct_afe();
        afe_push(insert_html(t));
        return;
    }
    if (!strcmp(n, "nobr")) {
        reconstruct_afe();
        if (has_in_scope("nobr", SC_DEFAULT)) {
            adoption_agency("nobr");
            reconstruct_afe();
        }
        afe_push(insert_html(t));
        return;
    }
    if (!strcmp(n, "applet") || !strcmp(n, "marquee") || !strcmp(n, "object")) {
        reconstruct_afe();
        insert_html(t);
        afe_marker();
        frameset_ok = 0;
        return;
    }
    if (!strcmp(n, "table")) {
        if (!quirks && has_in_scope("p", SC_BUTTON)) close_p();
        insert_html(t);
        frameset_ok = 0;
        mode = M_IN_TABLE;
        return;
    }
    if (name_in(n, L_VOID_BODY)) {
        reconstruct_afe();
        insert_html(t);
        pop();
        frameset_ok = 0;
        return;
    }
    if (!strcmp(n, "input")) {
        reconstruct_afe();
        insert_html(t);
        pop();
        const char *type = attr_get(t->attrs, t->attr_n, "type");
        if (!type || strcasecmp(type, "hidden")) frameset_ok = 0;
        return;
    }
    if (name_in(n, L_PARAM)) {
        insert_html(t);
        pop();
        return;
    }
    if (!strcmp(n, "hr")) {
        if (has_in_scope("p", SC_BUTTON)) close_p();
        insert_html(t);
        pop();
        frameset_ok = 0;
        return;
    }
    if (!strcmp(n, "image")) {
        tok_set_name(t, "img");
        in_body_start(t);
        return;
    }
    if (!strcmp(n, "textarea")) {
        insert_html(t);
        skip_lf = 1;
        orig_mode = mode;
        frameset_ok = 0;
        mode = M_TEXT;
        return;
    }
    if (!strcmp(n, "xmp")) {
        if (has_in_scope("p", SC_BUTTON)) close_p();
        reconstruct_afe();
        frameset_ok = 0;
        generic_raw(t);
        return;
    }
    if (!strcmp(n, "iframe")) {
        frameset_ok = 0;
        generic_raw(t);
        return;
    }
    if (!strcmp(n, "noembed") || !strcmp(n, "noscript")) {
        generic_raw(t);
        return;
    }
    if (!strcmp(n, "select")) {
        reconstruct_afe();
        insert_html(t);
        frameset_ok = 0;
        if (mode == M_IN_TABLE || mode == M_IN_CAPTION || mode == M_IN_TABLE_BODY ||
            mode == M_IN_ROW || mode == M_IN_CELL)
            mode = M_IN_SELECT_IN_TABLE;
        else
            mode = M_IN_SELECT;
        return;
    }
    if (!strcmp(n, "optgroup") || !strcmp(n, "option")) {
        if (is_html(cur(), "option")) pop();
        reconstruct_afe();
        insert_html(t);
        return;
    }
    if (!strcmp(n, "rb") || !strcmp(n, "rtc")) {
        if (has_in_scope("ruby", SC_DEFAULT)) gen_implied(NULL);
        insert_html(t);
        return;
    }
    if (!strcmp(n, "rp") || !strcmp(n, "rt")) {
        if (has_in_scope("ruby", SC_DEFAULT)) gen_implied("rtc");
        insert_html(t);
        return;
    }
    if (!strcmp(n, "math") || !strcmp(n, "svg")) {
        reconstruct_afe();
        insert_foreign(t, n[0] == 'm' ? NS_MATHML : NS_SVG);
        if (t->self_closing) pop();
        return;
    }
    if (name_in(n, L_BODY_IGNORE)) return;

    reconstruct_afe();
    insert_html(t);
}

static void mode_in_body(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (t->ck == CK_NUL) return;
            reconstruct_afe();
            insert_chars(t->data, t->len);
            if (t->ck == CK_OTHER) frameset_ok = 0;
            return;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_DOCTYPE:
            return;
        case T_EOF:
            if (tmpl_n > 0) run_mode(M_IN_TEMPLATE, t);
            return;
        case T_START:
            in_body_start(t);
            return;
        case T_END:
            in_body_end(t);
            return;
    }
}

static void mode_text(Token *t) {
    switch (t->type) {
        case T_CHAR:
            insert_chars(t->data, t->len);
            return;
        case T_EOF:
            pop();
            mode = orig_mode;
            process_token(t);
            return;
        case T_END:
            pop();
            mode = orig_mode;
            return;
        default:
            return;
    }
}

static void table_anything_else(Token *t) {
    foster = 1;
    run_mode(M_IN_BODY, t);
    foster = 0;
}

static void mode_in_table(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (is_html_in(cur(), L_TEXT_TABLE)) {
                ptc_len = 0;
                ptc_nonws = 0;
                orig_mode = mode;
                mode = M_IN_TABLE_TEXT;
                process_token(t);
                return;
            }
            table_anything_else(t);
            return;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_DOCTYPE:
            return;
        case T_EOF:
            run_mode(M_IN_BODY, t);
            return;
        case T_START: {
            const char *n = t->name;
            if (!strcmp(n, "caption")) {
                clear_to_context(L_TABLE_CTX);
                afe_marker();
                insert_html(t);
                mode = M_IN_CAPTION;
                return;
            }
            if (!strcmp(n, "colgroup")) {
                clear_to_context(L_TABLE_CTX);
                insert_html(t);
                mode = M_IN_COLUMN_GROUP;
                return;
            }
            if (!strcmp(n, "col")) {
                clear_to_context(L_TABLE_CTX);
                insert_html_named("colgroup");
                mode = M_IN_COLUMN_GROUP;
                process_token(t);
                return;
            }
            if (name_in(n, L_TABLE_SECTION)) {
                clear_to_context(L_TABLE_CTX);
                insert_html(t);
                mode = M_IN_TABLE_BODY;
                return;
            }
            if (!strcmp(n, "td") || !strcmp(n, "th") || !strcmp(n, "tr")) {
                clear_to_context(L_TABLE_CTX);
                insert_html_named("tbody");
                mode = M_IN_TABLE_BODY;
                process_token(t);
                return;
            }
            if (!strcmp(n, "table")) {
                if (!has_in_scope("table", SC_TABLE)) return;
                pop_until_html("table");
                reset_mode();
                process_token(t);
                return;
            }
            if (!strcmp(n, "style") || !strcmp(n, "script") || !strcmp(n, "template")) {
                run_mode(M_IN_HEAD, t);
                return;
            }
            if (!strcmp(n, "input")) {
                const char *type = attr_get(t->attrs, t->attr_n, "type");
                if (!type || strcasecmp(type, "hidden")) { table_anything_else(t); return; }
                insert_html(t);
                pop();
                return;
            }
            if (!strcmp(n, "form")) {
                if (has_template() || form_ptr) return;
                form_ptr = insert_html(t);
                pop();
                return;
            }
            table_anything_else(t);
            return;
        }
        case T_END: {
            const char *n = t->name;
            if (!strcmp(n, "table")) {
                if (!has_in_scope("table", SC_TABLE)) return;
                pop_until_html("table");
                reset_mode();
                return;
            }
            if (!strcmp(n, "body") || !strcmp(n, "caption") || !strcmp(n, "col") ||
                !strcmp(n, "colgroup") || !strcmp(n, "html") || name_in(n, L_TABLE_SECTION) ||
                !strcmp(n, "td") || !strcmp(n, "th") || !strcmp(n, "tr"))
                return;
            if (!strcmp(n, "template")) { run_mode(M_IN_HEAD, t); return; }
            table_anything_else(t);
            return;
        }
    }
}

static void mode_in_table_text(Token *t) {
    if (t->type == T_CHAR) {
        if (t->ck == CK_NUL) return;
        if (ptc_len + t->len > ptc_cap) {
            ptc_cap = (ptc_len + t->len) * 2 + 16;
            ptc = xrealloc(ptc, ptc_cap);
        }
        memcpy(ptc + ptc_len, t->data, t->len);
        ptc_len += t->len;
        if (t->ck == CK_OTHER) ptc_nonws = 1;
        return;
    }
    if (ptc_len) {
        if (ptc_nonws) {
            foster = 1;
            reconstruct_afe();
            insert_chars(ptc, ptc_len);
            frameset_ok = 0;
            foster = 0;
        } else {
            insert_chars(ptc, ptc_len);
        }
    }
    ptc_len = 0;
    ptc_nonws = 0;
    mode = orig_mode;
    process_token(t);
}

static void mode_in_caption(Token *t) {
    if (t->type == T_END) {
        const char *n = t->name;
        if (!strcmp(n, "caption")) {
            if (!has_in_scope("caption", SC_TABLE)) return;
            gen_implied(NULL);
            pop_until_html("caption");
            afe_clear_to_marker();
            mode = M_IN_TABLE;
            return;
        }
        if (!strcmp(n, "table")) {
            if (!has_in_scope("caption", SC_TABLE)) return;
            gen_implied(NULL);
            pop_until_html("caption");
            afe_clear_to_marker();
            mode = M_IN_TABLE;
            process_token(t);
            return;
        }
        if (!strcmp(n, "body") || !strcmp(n, "col") || !strcmp(n, "colgroup") || !strcmp(n, "html") ||
            name_in(n, L_TABLE_SECTION) || !strcmp(n, "td") || !strcmp(n, "th") || !strcmp(n, "tr"))
            return;
    }
    if (t->type == T_START) {
        const char *n = t->name;
        if (!strcmp(n, "caption") || !strcmp(n, "col") || !strcmp(n, "colgroup") ||
            name_in(n, L_TABLE_SECTION) || !strcmp(n, "td") || !strcmp(n, "th") || !strcmp(n, "tr")) {
            if (!has_in_scope("caption", SC_TABLE)) return;
            gen_implied(NULL);
            pop_until_html("caption");
            afe_clear_to_marker();
            mode = M_IN_TABLE;
            process_token(t);
            return;
        }
    }
    run_mode(M_IN_BODY, t);
}

static void mode_in_column_group(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (tok_ws(t)) { insert_chars(t->data, t->len); return; }
            break;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_DOCTYPE:
            return;
        case T_EOF:
            run_mode(M_IN_BODY, t);
            return;
        case T_START:
            if (!strcmp(t->name, "html")) { run_mode(M_IN_BODY, t); return; }
            if (!strcmp(t->name, "col")) {
                insert_html(t);
                pop();
                return;
            }
            if (!strcmp(t->name, "template")) { run_mode(M_IN_HEAD, t); return; }
            break;
        case T_END:
            if (!strcmp(t->name, "colgroup")) {
                if (!is_html(cur(), "colgroup")) return;
                pop();
                mode = M_IN_TABLE;
                return;
            }
            if (!strcmp(t->name, "col")) return;
            if (!strcmp(t->name, "template")) { run_mode(M_IN_HEAD, t); return; }
            break;
    }
    if (!is_html(cur(), "colgroup")) return;
    pop();
    mode = M_IN_TABLE;
    process_token(t);
}

static void mode_in_table_body(Token *t) {
    if (t->type == T_START) {
        const char *n = t->name;
        if (!strcmp(n, "tr")) {
            clear_to_context(L_TBODY_CTX);
            insert_html(t);
            mode = M_IN_ROW;
            return;
        }
        if (!strcmp(n, "th") || !strcmp(n, "td")) {
            clear_to_context(L_TBODY_CTX);
            insert_html_named("tr");
            mode = M_IN_ROW;
            process_token(t);
            return;
        }
        if (!strcmp(n, "caption") || !strcmp(n, "col") || !strcmp(n, "colgroup") ||
            name_in(n, L_TABLE_SECTION)) {
            if (!has_in_scope_list(L_TABLE_SECTION, SC_TABLE)) return;
            clear_to_context(L_TBODY_CTX);
            pop();
            mode = M_IN_TABLE;
            process_token(t);
            return;
        }
    } else if (t->type == T_END) {
        const char *n = t->name;
        if (name_in(n, L_TABLE_SECTION)) {
            if (!has_in_scope(n, SC_TABLE)) return;
            clear_to_context(L_TBODY_CTX);
            pop();
            mode = M_IN_TABLE;
            return;
        }
        if (!strcmp(n, "table")) {
            if (!has_in_scope_list(L_TABLE_SECTION, SC_TABLE)) return;
            clear_to_context(L_TBODY_CTX);
            pop();
            mode = M_IN_TABLE;
            process_token(t);
            return;
        }
        if (!strcmp(n, "body") || !strcmp(n, "caption") || !strcmp(n, "col") ||
            !strcmp(n, "colgroup") || !strcmp(n, "html") || !strcmp(n, "td") ||
            !strcmp(n, "th") || !strcmp(n, "tr"))
            return;
    }
    run_mode(M_IN_TABLE, t);
}

static void mode_in_row(Token *t) {
    if (t->type == T_START) {
        const char *n = t->name;
        if (!strcmp(n, "th") || !strcmp(n, "td")) {
            clear_to_context(L_ROW_CTX);
            insert_html(t);
            mode = M_IN_CELL;
            afe_marker();
            return;
        }
        if (!strcmp(n, "caption") || !strcmp(n, "col") || !strcmp(n, "colgroup") ||
            name_in(n, L_TABLE_SECTION) || !strcmp(n, "tr")) {
            if (!has_in_scope("tr", SC_TABLE)) return;
            clear_to_context(L_ROW_CTX);
            pop();
            mode = M_IN_TABLE_BODY;
            process_token(t);
            return;
        }
    } else if (t->type == T_END) {
        const char *n = t->name;
        if (!strcmp(n, "tr")) {
            if (!has_in_scope("tr", SC_TABLE)) return;
            clear_to_context(L_ROW_CTX);
            pop();
            mode = M_IN_TABLE_BODY;
            return;
        }
        if (!strcmp(n, "table")) {
            if (!has_in_scope("tr", SC_TABLE)) return;
            clear_to_context(L_ROW_CTX);
            pop();
            mode = M_IN_TABLE_BODY;
            process_token(t);
            return;
        }
        if (name_in(n, L_TABLE_SECTION)) {
            if (!has_in_scope(n, SC_TABLE)) return;
            if (!has_in_scope("tr", SC_TABLE)) return;
            clear_to_context(L_ROW_CTX);
            pop();
            mode = M_IN_TABLE_BODY;
            process_token(t);
            return;
        }
        if (!strcmp(n, "body") || !strcmp(n, "caption") || !strcmp(n, "col") ||
            !strcmp(n, "colgroup") || !strcmp(n, "html") || !strcmp(n, "td") || !strcmp(n, "th"))
            return;
    }
    run_mode(M_IN_TABLE, t);
}

static void close_cell(void) {
    gen_implied(NULL);
    pop_until_html_in(L_CELLS);
    afe_clear_to_marker();
    mode = M_IN_ROW;
}

static void mode_in_cell(Token *t) {
    if (t->type == T_END) {
        const char *n = t->name;
        if (!strcmp(n, "td") || !strcmp(n, "th")) {
            if (!has_in_scope(n, SC_TABLE)) return;
            gen_implied(NULL);
            pop_until_html(n);
            afe_clear_to_marker();
            mode = M_IN_ROW;
            return;
        }
        if (!strcmp(n, "body") || !strcmp(n, "caption") || !strcmp(n, "col") ||
            !strcmp(n, "colgroup") || !strcmp(n, "html"))
            return;
        if (!strcmp(n, "table") || name_in(n, L_TABLE_SECTION) || !strcmp(n, "tr")) {
            if (!has_in_scope(n, SC_TABLE)) return;
            close_cell();
            process_token(t);
            return;
        }
    } else if (t->type == T_START) {
        const char *n = t->name;
        if (!strcmp(n, "caption") || !strcmp(n, "col") || !strcmp(n, "colgroup") ||
            name_in(n, L_TABLE_SECTION) || !strcmp(n, "td") || !strcmp(n, "th") || !strcmp(n, "tr")) {
            if (!has_in_scope_list(L_CELLS, SC_TABLE)) return;
            close_cell();
            process_token(t);
            return;
        }
    }
    run_mode(M_IN_BODY, t);
}

static void mode_in_select(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (t->ck == CK_NUL) return;
            insert_chars(t->data, t->len);
            return;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_DOCTYPE:
            return;
        case T_EOF:
            run_mode(M_IN_BODY, t);
            return;
        case T_START: {
            const char *n = t->name;
            if (!strcmp(n, "html")) { run_mode(M_IN_BODY, t); return; }
            if (!strcmp(n, "option")) {
                if (is_html(cur(), "option")) pop();
                insert_html(t);
                return;
            }
            if (!strcmp(n, "optgroup")) {
                if (is_html(cur(), "option")) pop();
                if (is_html(cur(), "optgroup")) pop();
                insert_html(t);
                return;
            }
            if (!strcmp(n, "hr")) {
                if (is_html(cur(), "option")) pop();
                if (is_html(cur(), "optgroup")) pop();
                insert_html(t);
                pop();
                return;
            }
            if (!strcmp(n, "select")) {
                if (!has_in_scope("select", SC_SELECT)) return;
                pop_until_html("select");
                reset_mode();
                return;
            }
            if (!strcmp(n, "input") || !strcmp(n, "keygen") || !strcmp(n, "textarea")) {
                if (!has_in_scope("select", SC_SELECT)) return;
                pop_until_html("select");
                reset_mode();
                process_token(t);
                return;
            }
            if (!strcmp(n, "script") || !strcmp(n, "template")) { run_mode(M_IN_HEAD, t); return; }
            return;
        }
        case T_END: {
            const char *n = t->name;
            if (!strcmp(n, "optgroup")) {
                if (is_html(cur(), "option") && open_n >= 2 && is_html(open_els[open_n - 2], "optgroup")) pop();
                if (is_html(cur(), "optgroup")) pop();
                return;
            }
            if (!strcmp(n, "option")) {
                if (is_html(cur(), "option")) pop();
                return;
            }
            if (!strcmp(n, "select")) {
                if (!has_in_scope("select", SC_SELECT)) return;
                pop_until_html("select");
                reset_mode();
                return;
            }
            if (!strcmp(n, "template")) { run_mode(M_IN_HEAD, t); return; }
            return;
        }
    }
}

static void mode_in_select_in_table(Token *t) {
    static const char *const L_TBL[] = {"caption", "table", "tbody", "tfoot", "thead", "tr", "td", "th", NULL};
    if (tok_start_in(t, L_TBL)) {
        pop_until_html("select");
        reset_mode();
        process_token(t);
        return;
    }
    if (tok_end_in(t, L_TBL)) {
        if (!has_in_scope(t->name, SC_TABLE)) return;
        pop_until_html("select");
        reset_mode();
        process_token(t);
        return;
    }
    run_mode(M_IN_SELECT, t);
}

static void mode_in_template(Token *t) {
    switch (t->type) {
        case T_CHAR:
        case T_COMMENT:
        case T_DOCTYPE:
            run_mode(M_IN_BODY, t);
            return;
        case T_START: {
            const char *n = t->name;
            if (name_in(n, L_HEAD_START)) { run_mode(M_IN_HEAD, t); return; }
            Mode m = M_IN_BODY;
            if (!strcmp(n, "caption") || !strcmp(n, "colgroup") || name_in(n, L_TABLE_SECTION)) m = M_IN_TABLE;
            else if (!strcmp(n, "col")) m = M_IN_COLUMN_GROUP;
            else if (!strcmp(n, "tr")) m = M_IN_TABLE_BODY;
            else if (!strcmp(n, "td") || !strcmp(n, "th")) m = M_IN_ROW;
            tmpl_pop();
            tmpl_push(m);
            mode = m;
            process_token(t);
            return;
        }
        case T_END:
            if (!strcmp(t->name, "template")) { run_mode(M_IN_HEAD, t); return; }
            return;
        case T_EOF:
            if (!has_template()) return;
            pop_until_html("template");
            afe_clear_to_marker();
            tmpl_pop();
            reset_mode();
            process_token(t);
            return;
    }
}

static void mode_after_body(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (tok_ws(t)) { run_mode(M_IN_BODY, t); return; }
            break;
        case T_COMMENT:
            add_comment(open_els[0], NULL, t->data, t->len);
            return;
        case T_DOCTYPE:
            return;
        case T_EOF:
            return;
        case T_START:
            if (!strcmp(t->name, "html")) { run_mode(M_IN_BODY, t); return; }
            break;
        case T_END:
            if (!strcmp(t->name, "html")) {
                mode = M_AFTER_AFTER_BODY;
                return;
            }
            break;
    }
    mode = M_IN_BODY;
    process_token(t);
}

static void mode_in_frameset(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (tok_ws(t)) insert_chars(t->data, t->len);
            return;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_START:
            if (!strcmp(t->name, "html")) { run_mode(M_IN_BODY, t); return; }
            if (!strcmp(t->name, "frameset")) { insert_html(t); return; }
            if (!strcmp(t->name, "frame")) {
                insert_html(t);
                pop();
                return;
            }
            if (!strcmp(t->name, "noframes")) { run_mode(M_IN_HEAD, t); return; }
            return;
        case T_END:
            if (!strcmp(t->name, "frameset")) {
                if (open_n == 1) return;
                pop();
                if (!is_html(cur(), "frameset")) mode = M_AFTER_FRAMESET;
            }
            return;
        default:
            return;
    }
}

static void mode_after_frameset(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (tok_ws(t)) insert_chars(t->data, t->len);
            return;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_START:
            if (!strcmp(t->name, "html")) { run_mode(M_IN_BODY, t); return; }
            if (!strcmp(t->name, "noframes")) { run_mode(M_IN_HEAD, t); return; }
            return;
        case T_END:
            if (!strcmp(t->name, "html")) mode = M_AFTER_AFTER_FRAMESET;
            return;
        default:
            return;
    }
}

static void mode_after_after_body(Token *t) {
    switch (t->type) {
        case T_COMMENT:
            add_comment(document, NULL, t->data, t->len);
            return;
        case T_DOCTYPE:
            run_mode(M_IN_BODY, t);
            return;
        case T_CHAR:
            if (tok_ws(t)) { run_mode(M_IN_BODY, t); return; }
            break;
        case T_START:
            if (!strcmp(t->name, "html")) { run_mode(M_IN_BODY, t); return; }
            break;
        case T_EOF:
            return;
        default:
            break;
    }
    mode = M_IN_BODY;
    process_token(t);
}

static void mode_after_after_frameset(Token *t) {
    switch (t->type) {
        case T_COMMENT:
            add_comment(document, NULL, t->data, t->len);
            return;
        case T_DOCTYPE:
            run_mode(M_IN_BODY, t);
            return;
        case T_CHAR:
            if (tok_ws(t)) run_mode(M_IN_BODY, t);
            return;
        case T_START:
            if (!strcmp(t->name, "html")) { run_mode(M_IN_BODY, t); return; }
            if (!strcmp(t->name, "noframes")) { run_mode(M_IN_HEAD, t); return; }
            return;
        default:
            return;
    }
}

static void foreign_breakout(Token *t) {
    while (open_n > 1) {
        Node *n = cur();
        if (n->ns == NS_HTML || is_mathml_text_ip(n) || is_html_ip(n)) break;
        pop();
    }
    process_token(t);
}

static void foreign_content(Token *t) {
    switch (t->type) {
        case T_CHAR:
            if (t->ck == CK_NUL) {
                insert_chars("\xEF\xBF\xBD", 3);
                return;
            }
            insert_chars(t->data, t->len);
            if (t->ck == CK_OTHER) frameset_ok = 0;
            return;
        case T_COMMENT:
            insert_comment_tok(t);
            return;
        case T_DOCTYPE:
            return;
        case T_START: {
            const char *n = t->name;
            if (name_in(n, L_BREAKOUT) ||
                (!strcmp(n, "font") && (attr_get(t->attrs, t->attr_n, "color") ||
                                        attr_get(t->attrs, t->attr_n, "face") ||
                                        attr_get(t->attrs, t->attr_n, "size")))) {
                foreign_breakout(t);
                return;
            }
            Ns ns = cur()->ns;
            insert_foreign(t, ns);
            if (t->self_closing) pop();
            return;
        }
        case T_END: {
            const char *n = t->name;
            if (!strcmp(n, "br") || !strcmp(n, "p")) {
                foreign_breakout(t);
                return;
            }
            if (!strcmp(n, "script") && is_el(cur(), NS_SVG, "script")) {
                pop();
                return;
            }
            int i = open_n - 1;
            for (;;) {
                if (i == 0) return;
                if (!strcasecmp(open_els[i]->name, n)) {
                    open_n = i;
                    return;
                }
                i--;
                if (open_els[i]->ns != NS_HTML) continue;
                run_mode(mode, t);
                return;
            }
        }
        default:
            return;
    }
}

static void run_mode(Mode m, Token *t) {
    switch (m) {
        case M_INITIAL:             mode_initial(t); break;
        case M_BEFORE_HTML:         mode_before_html(t); break;
        case M_BEFORE_HEAD:         mode_before_head(t); break;
        case M_IN_HEAD:             mode_in_head(t); break;
        case M_AFTER_HEAD:          mode_after_head(t); break;
        case M_IN_BODY:             mode_in_body(t); break;
        case M_TEXT:                mode_text(t); break;
        case M_IN_TABLE:            mode_in_table(t); break;
        case M_IN_TABLE_TEXT:       mode_in_table_text(t); break;
        case M_IN_CAPTION:          mode_in_caption(t); break;
        case M_IN_COLUMN_GROUP:     mode_in_column_group(t); break;
        case M_IN_TABLE_BODY:       mode_in_table_body(t); break;
        case M_IN_ROW:              mode_in_row(t); break;
        case M_IN_CELL:             mode_in_cell(t); break;
        case M_IN_SELECT:           mode_in_select(t); break;
        case M_IN_SELECT_IN_TABLE:  mode_in_select_in_table(t); break;
        case M_IN_TEMPLATE:         mode_in_template(t); break;
        case M_AFTER_BODY:          mode_after_body(t); break;
        case M_IN_FRAMESET:         mode_in_frameset(t); break;
        case M_AFTER_FRAMESET:      mode_after_frameset(t); break;
        case M_AFTER_AFTER_BODY:    mode_after_after_body(t); break;
        case M_AFTER_AFTER_FRAMESET: mode_after_after_frameset(t); break;
    }
}

static int use_html_rules(const Token *t) {
    const Node *n = cur();
    if (!n || n->ns == NS_HTML || t->type == T_EOF) return 1;
    if (is_mathml_text_ip(n)) {
        if (t->type == T_START && strcmp(t->name, "mglyph") && strcmp(t->name, "malignmark")) return 1;
        if (t->type == T_CHAR) return 1;
    }
    if (n->ns == NS_MATHML && !strcmp(n->name, "annotation-xml") && tok_is_start(t, "svg")) return 1;
    if (is_html_ip(n) && (t->type == T_START || t->type == T_CHAR)) return 1;
    return 0;
}

static void process_token(Token *t) {
    if (use_html_rules(t)) run_mode(mode, t);
    else foreign_content(t);
}

//token intake

static void feed(Token *t) {
    if (skip_lf) {
        skip_lf = 0;
        if (t->type == T_CHAR && t->len && t->data[0] == '\n') {
            t->data++;
            t->len--;
            if (!t->len) return;
        }
    }
    process_token(t);
}

static CharKind char_kind(char c) {
    if (c == '\0') return CK_NUL;
    return is_ws(c) ? CK_WS : CK_OTHER;
}

static void feed_text(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        CharKind k = char_kind(s[i]);
        size_t j = i + 1;
        while (k != CK_NUL && j < n && char_kind(s[j]) == k) j++;
        Token t;
        memset(&t, 0, sizeof t);
        t.type = T_CHAR;
        t.data = s + i;
        t.len = j - i;
        t.ck = k;
        feed(&t);
        i = j;
    }
}

static char *unescape(const char *s, size_t n, size_t *outlen, int replace_nul) {
    char *out = xcalloc(1, n * 3 + 1);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            switch (s[i]) {
                case 'n': out[w++] = '\n'; break;
                case 'r': out[w++] = '\r'; break;
                case '0':
                    if (replace_nul) { memcpy(out + w, "\xEF\xBF\xBD", 3); w += 3; }
                    else out[w++] = '\0';
                    break;
                default: out[w++] = s[i];
            }
        } else {
            out[w++] = s[i];
        }
    }
    out[w] = '\0';
    if (outlen) *outlen = w;
    return out;
}

static Token *pending;

static void flush_pending(void) {
    if (!pending) return;
    Token *t = pending;
    pending = NULL;
    feed(t);
    token_free(t);
    free(t);
}

static void feed_simple(TokType type, const char *arg, size_t n) {
    size_t len;
    char *text = unescape(arg, n, &len, type != T_CHAR);
    Token t;
    memset(&t, 0, sizeof t);
    t.type = type;
    if (type == T_CHAR) {
        feed_text(text, len);
    } else if (type == T_END) {
        t.name = text;
        text = NULL;
        feed(&t);
    } else if (type == T_COMMENT) {
        t.data = text;
        t.len = len;
        feed(&t);
    } else if (type == T_DOCTYPE) {
        parse_doctype(&t, text, len);
        feed(&t);
    }
    free(text);
    token_free(&t);
}

//output

static void indent(int depth) {
    fputs("| ", stdout);
    for (int i = 0; i < depth * 2; i++) putchar(' ');
}

typedef struct {
    char *disp;
    const Attr *a;
} AttrRef;

static int attr_ref_cmp(const void *x, const void *y) {
    return strcmp(((const AttrRef *)x)->disp, ((const AttrRef *)y)->disp);
}

static void print_node(const Node *n, int depth);

static void print_kids(const Node *n, int depth) {
    for (int i = 0; i < n->kid_n; i++) print_node(n->kids[i], depth);
}

static void print_node(const Node *n, int depth) {
    switch (n->type) {
        case N_DOCUMENT:
        case N_FRAGMENT:
            print_kids(n, depth);
            return;
        case N_DOCTYPE:
            indent(depth);
            printf("<!DOCTYPE %s", n->name);
            if ((n->pubid && *n->pubid) || (n->sysid && *n->sysid))
                printf(" \"%s\" \"%s\"", n->pubid ? n->pubid : "", n->sysid ? n->sysid : "");
            puts(">");
            return;
        case N_COMMENT:
            indent(depth);
            fputs("<!-- ", stdout);
            fwrite(n->data, 1, n->len, stdout);
            puts(" -->");
            return;
        case N_TEXT:
            indent(depth);
            putchar('"');
            fwrite(n->data, 1, n->len, stdout);
            puts("\"");
            return;
        case N_ELEMENT: {
            indent(depth);
            printf("<%s%s>\n", n->ns == NS_SVG ? "svg " : n->ns == NS_MATHML ? "math " : "", n->name);
            AttrRef *refs = xcalloc((size_t)n->attr_n + 1, sizeof *refs);
            for (int i = 0; i < n->attr_n; i++) {
                const Attr *a = &n->attrs[i];
                size_t l = strlen(a->name) + (a->prefix ? strlen(a->prefix) + 1 : 0);
                refs[i].disp = xcalloc(1, l + 1);
                if (a->prefix) sprintf(refs[i].disp, "%s %s", a->prefix, a->name);
                else strcpy(refs[i].disp, a->name);
                refs[i].a = a;
            }
            qsort(refs, (size_t)n->attr_n, sizeof *refs, attr_ref_cmp);
            for (int i = 0; i < n->attr_n; i++) {
                indent(depth + 1);
                printf("%s=\"%s\"\n", refs[i].disp, refs[i].a->value);
                free(refs[i].disp);
            }
            free(refs);
            if (n->content) {
                indent(depth + 1);
                puts("content");
                print_kids(n->content, depth + 2);
            } else {
                print_kids(n, depth + 1);
            }
            return;
        }
    }
}

//main :333333333333

int main(void) {
    document = node_new(N_DOCUMENT);
    mode = M_INITIAL;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, stdin)) != -1) {
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        char *label = line;
        char *arg = strchr(line, ' ');
        size_t arglen;
        if (arg) {
            *arg++ = '\0';
            arglen = (size_t)(line + len - arg);
        } else {
            arg = line + len;
            arglen = 0;
        }

        if (!strcmp(label, "ATTRIBUTE")) {
            if (!pending) continue;
            char *value = memchr(arg, ' ', arglen);
            size_t nlen = value ? (size_t)(value - arg) : arglen;
            char *name = xstrndup(arg, nlen);
            if (!attr_get(pending->attrs, pending->attr_n, name)) {
                char *v = value ? unescape(value + 1, arglen - nlen - 1, NULL, 1) : xstrdup("");
                attrs_add(&pending->attrs, &pending->attr_n, &pending->attr_cap, NULL, name, v);
                free(v);
            }
            free(name);
            continue;
        }

        flush_pending();

        if (!strcmp(label, "START_TAG") || !strcmp(label, "VOID_TAG") || !strcmp(label, "SELF_CLOSE")) {
            pending = xcalloc(1, sizeof *pending);
            pending->type = T_START;
            pending->name = unescape(arg, arglen, NULL, 1);
            pending->self_closing = !strcmp(label, "SELF_CLOSE");
        } else if (!strcmp(label, "END_TAG")) {
            feed_simple(T_END, arg, arglen);
        } else if (!strcmp(label, "TEXT")) {
            feed_simple(T_CHAR, arg, arglen);
        } else if (!strcmp(label, "COMMENT")) {
            feed_simple(T_COMMENT, arg, arglen);
        } else if (!strcmp(label, "DOCTYPE")) {
            feed_simple(T_DOCTYPE, arg, arglen);
        } else if (!strcmp(label, "UNTERMINATED")) {
        } else {
            fprintf(stderr, "unknown token '%s'\n", label);
        }
    }
    flush_pending();

    Token eof;
    memset(&eof, 0, sizeof eof);
    eof.type = T_EOF;
    feed(&eof);

    puts("#document");
    print_node(document, 0);

    free(line);
    free(ptc);
    free(open_els);
    free(afe);
    free(tmpl_modes);
    free_all_nodes();
    return 0;
}
