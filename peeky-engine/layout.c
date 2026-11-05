#define _POSIX_C_SOURCE 200809L

#include "layout.h"
#include <stdlib.h>
#include <string.h>

#define SCALE 1.0f //use css px instead
#define TEXT_CHAR_W 7.0f
#define TEXT_LINE_H 18.0f
#define CELL_ASPECT 2.0f
#define MAX_PAGE_H 16000.0f

//Nobody looking at my code anyways so might as well put some absolute bs in here


static float g_kx = 1.0f, g_ky = 1.0f;
static float g_css_per_cell;
static float g_viewport_h;
static int g_list_counter = 0;
static int g_li_ordinal = 0;
static int g_list_is_ol = 0;
typedef struct { float v; int is_rel; int is_pct; } Track;

static RenderNode **g_tmp_nodes = NULL;
static size_t g_tmp_n = 0, g_tmp_cap = 0;

static void tmp_node_track(RenderNode *n) {
    if (g_tmp_n >= g_tmp_cap) {
        g_tmp_cap = g_tmp_cap ? g_tmp_cap * 2 : 16;
        g_tmp_nodes = realloc(g_tmp_nodes, g_tmp_cap * sizeof(RenderNode *));
    }
    g_tmp_nodes[g_tmp_n++] = n;
}

static void tmp_nodes_free(void) {
    for (size_t i = 0; i < g_tmp_n; i++)
        free_render_node(g_tmp_nodes[i]);
    free(g_tmp_nodes);
    g_tmp_nodes = NULL;
    g_tmp_n = 0;
    g_tmp_cap = 0;
}

static const char *node_prop(RenderNode *n, const char *prop);
static LayoutBox *build_box(RenderNode *, float, float, float);
static void shift_box(LayoutBox *, float, float);
static void push_child(LayoutBox *box, LayoutBox *c);

static LayoutBox *box_new(RenderNode *n, float x, float y, float w, float h) {
    LayoutBox *b = calloc(1, sizeof *b);
    b->node = n; b->display = DISPLAY_BLOCK;
    b->bounds = (LayoutRect){x, y, w, h};
    return b;
}

static int parse_track_list(const char *s, Track **out) {
    Track *tr = NULL; int n = 0, cap = 0; const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *st = p;
        while (*p && *p != ',') p++;
        if (p == st) { if (*p) p++; continue; }
        const char *en = p;
        while (en > st && (en[-1] == ' ' || en[-1] == '\t')) en--;
        char tok[64]; size_t tl = (size_t)(en - st);
        if (tl >= sizeof tok) tl = sizeof tok - 1;
        memcpy(tok, st, tl); tok[tl] = '\0';
        if (*p == ',') p++;
        float v = 0.0f; int is_rel = 0, is_pct = 0;
        size_t tlen = strlen(tok);
        if (tlen && tok[tlen - 1] == '%') { tok[tlen - 1] = '\0'; v = strtof(tok, NULL); is_pct = 1; }
        else if (tlen && tok[tlen - 1] == '*') { tok[tlen - 1] = '\0'; v = strtof(tok, NULL); if (v <= 0) v = 1; is_rel = 1; }
        else v = strtof(tok, NULL);
        if (n >= cap) { cap = cap ? cap * 2 : 8; tr = realloc(tr, sizeof(*tr) * (size_t)cap); }
        tr[n].v = v; tr[n].is_rel = is_rel; tr[n].is_pct = is_pct; n++;
    }
    if (n == 0) { tr = malloc(sizeof(*tr)); tr[0].v = 100; tr[0].is_rel = 0; tr[0].is_pct = 1; n = 1; }
    *out = tr; return n;
}

static void split_tracks(float total, Track *tr, int n, float **out) {
    float *sizes = malloc(sizeof(float) * (size_t)n);
    float px_sum = 0, pct_sum = 0, rel_sum = 0; int rel_n = 0;
    for (int i = 0; i < n; i++) {
        if (tr[i].is_rel) { rel_sum += tr[i].v; rel_n++; }
        else if (tr[i].is_pct) pct_sum += tr[i].v;
        else px_sum += tr[i].v;
    }
    float remaining = total - px_sum - (pct_sum / 100.0f) * total;
    if (remaining < 0) remaining = 0;
    for (int i = 0; i < n; i++) {
        if (tr[i].is_rel) sizes[i] = rel_sum > 0 ? remaining * (tr[i].v / rel_sum) : remaining / (float)rel_n;
        else if (tr[i].is_pct) sizes[i] = tr[i].v / 100.0f * total;
        else sizes[i] = tr[i].v;
    }
    *out = sizes;
}

static LayoutBox *build_frameset_box(RenderNode *n, float x, float y, float w, float h) {
    LayoutBox *box = box_new(n, x, y, w, h);
    Track *rows = NULL, *cols = NULL;
    int rn = parse_track_list(node_prop(n, "rows") ?: "100%", &rows);
    int cn = parse_track_list(node_prop(n, "cols") ?: "100%", &cols);
    float *rs = NULL, *cs = NULL;
    split_tracks(h, rows, rn, &rs);
    split_tracks(w, cols, cn, &cs);
    int ki = 0;
    float cy = y;
    for (int r = 0; r < rn; r++) {
        float cx = x;
        for (int c = 0; c < cn; c++) {
            while (ki < n->kid_n && (!n->kids[ki] || !n->kids[ki]->tag)) ki++;
            if (ki >= n->kid_n) break;
            RenderNode *k = n->kids[ki++];
            LayoutBox *cb = !strcmp(k->tag, "frameset")
                ? build_frameset_box(k, cx, cy, cs[c], rs[r])
                : box_new(k, cx, cy, cs[c], rs[r]);
            push_child(box, cb);
            cx += cs[c];
        }
        cy += rs[r];
    }
    free(rows); free(cols); free(rs); free(cs);
    return box;
}

static RenderNode *node_new(const char *tag, const char *text) {
    RenderNode *n = calloc(1, sizeof(RenderNode));
    n->tag = tag ? strdup(tag) : NULL;
    n->text = text ? strdup(text) : NULL;
    return n;
}

static void add_prop(RenderNode *n, const char *prop, const char *value) {
    if (n->prop_n >= n->prop_cap) {
        n->prop_cap = n->prop_cap ? n->prop_cap * 2 : 4;
        n->props = realloc(n->props, (size_t)n->prop_cap * sizeof(char *));
        n->values = realloc(n->values, (size_t)n->prop_cap * sizeof(char *));
    }
    n->props[n->prop_n] = strdup(prop);
    n->values[n->prop_n] = strdup(value);
    n->prop_n++;
}

static void add_kid(RenderNode *parent, RenderNode *child) {
    if (parent->kid_n >= parent->kid_cap) {
        parent->kid_cap = parent->kid_cap ? parent->kid_cap * 2 : 4;
        parent->kids = realloc(parent->kids, (size_t)parent->kid_cap * sizeof(RenderNode *));
    }
    parent->kids[parent->kid_n++] = child;
}

static void unescape_text(char *s) {
    char *r = s, *w = s;
    for (; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
                case 'n': *w++ = '\n'; break;
                case 'r': *w++ = '\r'; break;
                case 't': *w++ = '\t'; break;
                case '\\': *w++ = '\\'; break;
                case '"': *w++ = '"'; break;
                default: *w++ = *r; break;
            }
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void print_esc(const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout);  break;
        case '\r': fputs("\\r", stdout);  break;
        case '\t': fputs("\\t", stdout);  break;
        default: putchar((unsigned char)*s);
        }
    }
}

RenderNode *parse_render_tree(FILE *fp) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    RenderNode *root = node_new(NULL, NULL);
    size_t stack_cap = 32;
    RenderNode **stack = malloc(sizeof(RenderNode *) * stack_cap);
    int top = 0;
    stack[top++] = root;

    int in_tree = 0;
    while ((len = getline(&line, &cap, fp)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (!in_tree) {
            if (!strncmp(line, "#render-tree", 12)) in_tree = 1;
            continue;
        }
        if (len == 0) continue;
        if (line[0] != '|') continue;

        int depth = 0;
        char *p = line + 1;
        while (*p == ' ') { depth++; p++; }
        depth /= 2;

        if (depth > top) depth = top;
        RenderNode *parent = stack[depth];

        char *content = p;
        if (!*content) continue;

        int content_len = (int)strlen(content);
        if (content_len == 0) continue;
        if (content[0] == '<' && content[content_len - 1] == '>') {
            if (content[1] == '/') continue;
            char *tag = malloc(content_len);
            memcpy(tag, content + 1, content_len - 2);
            tag[content_len - 2] = '\0';
            RenderNode *n = node_new(tag, NULL);
            free(tag);
            add_kid(parent, n);
            if ((size_t)depth + 2 > stack_cap) {
                while ((size_t)depth + 2 > stack_cap) stack_cap *= 2;
                stack = realloc(stack, sizeof(RenderNode *) * stack_cap);
            }
            stack[depth + 1] = n;
            top = depth + 1;
        } else if (content[0] == '"' && content[content_len - 1] == '"') {
            char *txt = malloc(content_len);
            memcpy(txt, content + 1, content_len - 2);
            txt[content_len - 2] = '\0';
            unescape_text(txt);
            RenderNode *n = node_new(NULL, txt);
            free(txt);
            add_kid(parent, n);
        } else {
            char *colon = strchr(content, ':');
            if (colon && colon > content) {
                *colon = '\0';
                char *prop = content;
                char *val = colon + 1;
                while (*val == ' ') val++;
                add_prop(parent, prop, val);
                *colon = ':';
            }
        }
    }
    free(line);
    free(stack);
    return root;
}

void free_render_node(RenderNode *n) {
    if (!n) return;
    free(n->tag);
    free(n->text);
    for (int i = 0; i < n->prop_n; i++) {
        free(n->props[i]);
        free(n->values[i]);
    }
    free(n->props);
    free(n->values);
    for (int i = 0; i < n->kid_n; i++)
        free_render_node(n->kids[i]);
    free(n->kids);
    free(n);
}

static float parse_len(RenderNode *n, const char *prop, float base) {
    const char *v = node_prop(n, prop);
    if (!v) return 0.0f;
    char *end = NULL;
    float x = strtof(v, &end);
    if (end == v) return 0.0f;
    if (*end == '%') return x * base / 100.0f;
    return x * SCALE;
}

static float parse_prop_f(RenderNode *n, const char *prop) {
    for (int i = 0; i < n->prop_n; i++) {
        if (!strcmp(n->props[i], prop)) {
            char *end = NULL;
            float v = strtof(n->values[i], &end);
            if (end != n->values[i]) return v * SCALE;
        }
    }
    return 0.0f;
}

static const char *INLINE_TAGS[] = {"span","a","b","i","u","em","strong","small",
    "big","font","code","cite","label","abbr","sub","sup","img","input",
    "select","button","textarea","br",NULL};

static int prop_is(RenderNode *n, const char *p, const char *val) {
    const char *v = node_prop(n, p);
    return v && !strcmp(v, val);
}

static int node_align(RenderNode *n) {            //0 is left, 1 is center, 2 is right
    const char *v = node_prop(n, "text-align");
    if (!v) return 0;
    if (!strcmp(v, "center")) return 1;
    if (!strcmp(v, "right") || !strcmp(v, "end")) return 2;
    return 0;
}

static void push_child(LayoutBox *box, LayoutBox *c) {
    box->children = realloc(box->children, sizeof(LayoutBox *) * (box->child_count + 1));
    box->children[box->child_count++] = c;
}

static void align_line(LayoutBox *box, size_t from, float used, float w, int align) {
    float slack = w - used;
    if (!align || slack <= 0) return;
    float dx = (align == 1) ? slack / 2 : slack;
    for (size_t i = from; i < box->child_count; i++)
        shift_box(box->children[i], dx, 0);
}

//inline text stuff
static float layout_text_words(LayoutBox *box, RenderNode *k, float x0, float y, float w,
                               float *line_x, float *line_h, size_t *line_start, int align) {
    const char *t = k->text;
    size_t n = strlen(t), i = 0;
    float cm_l = parse_prop_f(k, "margin-left");
    float cm_r = parse_prop_f(k, "margin-right");
    while (i < n) {
        while (i < n && t[i] == ' ') {
            *line_x += g_css_per_cell;
            i++;
        }
        size_t start = i;
        while (i < n && t[i] != ' ') i++;
        size_t wlen = i - start;
        if (wlen == 0) continue;
        float wd = g_css_per_cell * (float)wlen;
        float bx = *line_x;
        if (bx > x0 && bx + cm_l + wd + cm_r > x0 + w) {
            align_line(box, *line_start, bx - x0, w, align);
            y += *line_h;
            *line_x = x0; *line_h = 0; *line_start = box->child_count;
            bx = x0;
        }
        char *word = malloc(wlen + 1);
        memcpy(word, t + start, wlen);
        word[wlen] = '\0';
        RenderNode *wn = node_new(NULL, word);
        free(word);
        tmp_node_track(wn);
        LayoutBox *cb = calloc(1, sizeof *cb);
        cb->node = wn;
        cb->display = DISPLAY_INLINE;
        cb->bounds.x = bx;
        cb->bounds.y = y;
        cb->bounds.width = wd;
        cb->bounds.height = TEXT_LINE_H;
        push_child(box, cb);
        if (TEXT_LINE_H > *line_h) *line_h = TEXT_LINE_H;
        *line_x = bx + cm_l + wd + cm_r;
    }
    return y;
}

static float layout_inline_run(LayoutBox *box, RenderNode *n, int i0, int i1,
                               float x0, float y, float w, int align) {
    size_t line_start = box->child_count;
    float line_x = x0, line_h = 0;
    for (int i = i0; i < i1; i++) {
        RenderNode *k = n->kids[i];
        if (node_display(k) == DISPLAY_NONE) continue;
        if (k->tag && !strcmp(k->tag, "br")) {
            LayoutBox *b = calloc(1, sizeof *b);
            b->node = k;
            b->display = DISPLAY_INLINE;
            b->bounds.x = line_x;
            b->bounds.y = y;
            push_child(box, b);
            align_line(box, line_start, line_x - x0, w, align);
            y += line_h > 0 ? line_h : TEXT_LINE_H;
            line_x = x0; line_h = 0;
            line_start = box->child_count;
            continue;
        }
        if (!k->tag && k->text) {
            y = layout_text_words(box, k, x0, y, w, &line_x, &line_h, &line_start, align);
            continue;
        }
        float cm_l = parse_prop_f(k, "margin-left");
        float cm_r = parse_prop_f(k, "margin-right");
        float bx = line_x, by = y;
        LayoutBox *c = build_box(k, line_x, y, w);
        if (line_x > x0 && line_x + cm_l + c->bounds.width + cm_r > x0 + w) {
            align_line(box, line_start, line_x - x0, w, align);
            y += line_h;
            line_x = x0; line_h = 0;
            line_start = box->child_count;
            shift_box(c, x0 - bx, y - by);
        }
        push_child(box, c);
        if (c->bounds.height > line_h) line_h = c->bounds.height;
        line_x += cm_l + c->bounds.width + cm_r;
    }
    align_line(box, line_start, line_x - x0, w, align);
    return y + line_h;
}

static LayoutBox *build_box(RenderNode *, float, float, float);

DisplayType node_display(RenderNode *n) {
    if (!n) return DISPLAY_UNKNOWN;
    if (n->tag) {
        for (int i = 0; INLINE_TAGS[i]; i++)
            if (!strcmp(n->tag, INLINE_TAGS[i])) return DISPLAY_INLINE;
    }
    for (int i = 0; i < n->prop_n; i++) {
        if (!strcmp(n->props[i], "display")) {
            if (!strcmp(n->values[i], "inline")) return DISPLAY_INLINE;
            if (!strcmp(n->values[i], "inline-block")) return DISPLAY_INLINE;
            if (!strcmp(n->values[i], "none")) return DISPLAY_NONE;
            return DISPLAY_BLOCK;
        }
    }
    if (!n->tag) return DISPLAY_INLINE;
    return DISPLAY_BLOCK;
}

static void shift_box(LayoutBox *b, float dx, float dy) {
    b->bounds.x += dx;
    b->bounds.y += dy;
    for (size_t i = 0; i < b->child_count; i++)
        shift_box(b->children[i], dx, dy);
}

static LayoutBox *build_box(RenderNode *n, float x, float y, float avail_w) {
    LayoutBox *box = calloc(1, sizeof(LayoutBox));
    box->node = n;
    box->children = NULL;
    box->child_count = 0;

    box->display = node_display(n);
    if (box->display == DISPLAY_NONE) return box;
    if (n->tag && !strcmp(n->tag, "frameset")) {
        LayoutBox *fb = build_frameset_box(n, x, y, avail_w, g_viewport_h);
        free(box);
        return fb;
    }

    if (!n->tag && n->text) {
        box->bounds.x = x;
        box->bounds.y = y;
        box->bounds.width = g_css_per_cell * (float)strlen(n->text);
        box->bounds.height = TEXT_LINE_H;
        return box;
    }

    float m_l = parse_prop_f(n, "margin-left");
    float m_r = parse_prop_f(n, "margin-right");
    float m_t = parse_prop_f(n, "margin-top");
    float b_l = parse_prop_f(n, "border-left-width");
    float b_r = parse_prop_f(n, "border-right-width");
    float b_t = parse_prop_f(n, "border-top-width");
    float b_b = parse_prop_f(n, "border-bottom-width");
    float p_l = parse_prop_f(n, "padding-left");
    float p_r = parse_prop_f(n, "padding-right");
    float p_t = parse_prop_f(n, "padding-top");
    float p_b = parse_prop_f(n, "padding-bottom");
    float w_prop = parse_len(n, "width", avail_w - m_l - m_r);
    float h_prop = parse_len(n, "height", 0.0f);
    
    float content_w = avail_w - m_l - m_r - b_l - b_r - p_l - p_r;
    if (content_w < 0) content_w = 0;

    float width = (w_prop > 0) ? w_prop : content_w;
    float height = h_prop > 0 ? h_prop : 20.0f;

    box->bounds.x = x + m_l;
    box->bounds.y = y + m_t;
    box->bounds.width = width;
    box->bounds.height = height;

    float child_x = box->bounds.x + b_l + p_l;
    float child_y = y + m_t + b_t + p_t;
    float child_w = width - b_l - b_r - p_l - p_r;
    if (child_w < 0) child_w = 0;

    float marker_w = 0.0f;
    float marker_x = child_x;
    char mbuf[16] = {0};
    int is_li = n->tag && !strcmp(n->tag, "li");
    if (is_li) {
        if (g_list_is_ol)
            snprintf(mbuf, sizeof mbuf, "%d. ", g_li_ordinal);
        else
            snprintf(mbuf, sizeof mbuf, "* ");
        marker_w = g_css_per_cell * (float)strlen(mbuf);
        child_x += marker_w;
        child_w -= marker_w;
        if (child_w < 0) child_w = 0;
        marker_x = child_x - marker_w;
    }

    int is_replaced = n->tag && (!strcmp(n->tag, "img") || !strcmp(n->tag, "input") ||
                             !strcmp(n->tag, "textarea") || !strcmp(n->tag, "select"));
    if (is_replaced && (w_prop > 0 || h_prop > 0)) {
        float rw = w_prop > 0 ? w_prop : h_prop * (4.0f / 3.0f);
        float rh = h_prop > 0 ? h_prop : w_prop * 0.75f;
        if (rw > content_w && rw > 0) { rh *= content_w / rw; rw = content_w; }
        box->bounds.x = x + m_l;
        box->bounds.y = y + m_t;
        box->bounds.width = rw;
        box->bounds.height = rh;
        return box;
    }

    //Tables work now !11!
    if (n->tag && !strcmp(n->tag, "tr")) {
        int *cell_idx = malloc(sizeof(int) * (size_t)n->kid_n);
        int cell_n = 0;
        for (int i = 0; i < n->kid_n; i++) {
            RenderNode *k = n->kids[i];
            if (!k->tag) continue;
            if (!strcmp(k->tag, "td") || !strcmp(k->tag, "th"))
                cell_idx[cell_n++] = i;
        }
        float *cw = malloc(sizeof(float) * (size_t)cell_n);
        float fixed_total = 0.0f;
        int auto_n = 0;
        for (int ci = 0; ci < cell_n; ci++) {
            float wp = parse_len(n->kids[cell_idx[ci]], "width", child_w);
            if (wp > 0) { cw[ci] = wp; fixed_total += wp; }
            else { cw[ci] = -1.0f; auto_n++; }
        }
        float auto_w = auto_n > 0 ? (child_w - fixed_total) / (float)auto_n : 0.0f;
        if (auto_w < 0.0f) auto_w = 0.0f;
        for (int ci = 0; ci < cell_n; ci++)
            if (cw[ci] < 0.0f) cw[ci] = auto_w;
        float cur_x = child_x;
        float max_h = 0;
        for (int ci = 0; ci < cell_n; ci++) {
            RenderNode *k = n->kids[cell_idx[ci]];
            LayoutBox *c = build_box(k, cur_x, child_y, cw[ci]);
            push_child(box, c);
            if (c->bounds.height > max_h) max_h = c->bounds.height;
            cur_x += cw[ci];
        }
        free(cw);
        free(cell_idx);
        box->bounds.height = max_h + p_t + p_b + b_t + b_b;
        return box;
    }

    int all_inline = n->kid_n > 0;
    for (int i = 0; i < n->kid_n && all_inline; i++) {
        if (node_display(n->kids[i]) != DISPLAY_INLINE) all_inline = 0;
    }

    if (w_prop > 0 && w_prop < content_w) {
        float slack = content_w - w_prop;
        int la = prop_is(n, "margin-left", "auto"), ra = prop_is(n, "margin-right", "auto");
        if (la && ra) m_l += slack / 2;
        else if (la) m_l += slack;
    }

    if (box->display == DISPLAY_INLINE) {
        int align = node_align(n);
        float end_y = layout_inline_run(box, n, 0, n->kid_n, child_x, child_y, child_w, align);
        box->bounds.height = (end_y - child_y) + p_t + p_b + b_t + b_b;
        float mx = child_x;
        for (size_t i = 0; i < box->child_count; i++) {
            float ex = box->children[i]->bounds.x + box->children[i]->bounds.width;
            if (ex > mx) mx = ex;
        }
        box->bounds.width = mx - child_x;
        if (box->bounds.width < 0.0f) box->bounds.width = 0.0f;
    } else {
        int align = node_align(n);
        float y = child_y;
        int i = 0;
        if (n->tag && (!strcmp(n->tag, "ol") || !strcmp(n->tag, "ul")))
            g_list_counter = 0;
        while (i < n->kid_n) {
            DisplayType d = node_display(n->kids[i]);
            if (d == DISPLAY_NONE) { i++; continue; }
            if (d == DISPLAY_INLINE) {
                int j = i;
                while (j < n->kid_n && node_display(n->kids[j]) != DISPLAY_BLOCK) j++;
                y = layout_inline_run(box, n, i, j, child_x, y, child_w, align);
                i = j;
            } else {
                if (n->kids[i]->tag && !strcmp(n->kids[i]->tag, "li")) {
                    g_list_counter++;
                    g_li_ordinal = g_list_counter;
                    g_list_is_ol = n->tag && !strcmp(n->tag, "ol");
                }
                float cm_t = parse_prop_f(n->kids[i], "margin-top");
                float cm_b = parse_prop_f(n->kids[i], "margin-bottom");
                LayoutBox *c = build_box(n->kids[i], child_x, y, child_w);
                push_child(box, c);
                y += cm_t + c->bounds.height + cm_b;
                i++;
            }
        }
        if (h_prop <= 0)
            box->bounds.height = box->child_count
                ? (y - child_y) + p_t + p_b + b_t + b_b
                : p_t + p_b + b_t + b_b + 20.0f;
    }

    if (is_li && marker_w > 0) {
        RenderNode *mn = node_new(NULL, mbuf);
        tmp_node_track(mn);
        LayoutBox *mb = calloc(1, sizeof *mb);
        mb->node = mn;
        mb->display = DISPLAY_INLINE;
        mb->bounds.x = marker_x;
        mb->bounds.y = child_y;
        mb->bounds.width = marker_w;
        mb->bounds.height = TEXT_LINE_H;
        push_child(box, mb);
    }
    return box;
}

LayoutBox *layout_tree(FILE *fp, float viewport_w, float viewport_h) {
    RenderNode *root = parse_render_tree(fp);
    float scale_w = viewport_w * SCALE;
    float scale_h = viewport_h * SCALE;
    g_viewport_h = scale_h;

    LayoutBox *layout = calloc(1, sizeof(LayoutBox));
    layout->node = root;
    layout->children = NULL;
    layout->child_count = 0;

    layout->bounds.x = 0;
    layout->bounds.y = 0;
    layout->bounds.width = scale_w;
    layout->bounds.height = scale_h;

    float current_y = 0;
    for (int i = 0; i < root->kid_n; i++) {
        if (node_display(root->kids[i]) == DISPLAY_NONE) continue;
        float cm_t = parse_prop_f(root->kids[i], "margin-top");
        float cm_b = parse_prop_f(root->kids[i], "margin-bottom");
        LayoutBox *child = build_box(root->kids[i], 0, current_y, scale_w);
        layout->children = realloc(layout->children, sizeof(LayoutBox *) * (layout->child_count + 1));
        layout->children[layout->child_count++] = child;
        current_y += cm_t + child->bounds.height + cm_b;
    }

    float content_h = current_y;
    if (content_h > MAX_PAGE_H) content_h = MAX_PAGE_H;
    if (content_h > scale_h) {
        float rows_f = content_h * g_ky / 4.0f;
        int r = (int)rows_f;
        if ((float)r < rows_f) r++;
        layout->bounds.height = (float)r * 4.0f / g_ky;
    }
    return layout;
}

void free_layout_box(LayoutBox *box) {
    if (!box) return;
    for (size_t i = 0; i < box->child_count; i++)
        free_layout_box(box->children[i]);
    free(box->children);
    free(box);
}

//output

static void indent(int depth) {
    fputs("| ", stdout);
    for (int i = 0; i < depth; i++)
        fputs("  ", stdout);
}

static const char *node_prop(RenderNode *n, const char *prop) {
    const char *found = NULL;
    if (!n) return NULL;
    for (int i = 0; i < n->prop_n; i++)
        if (!strcmp(n->props[i], prop))
            found = n->values[i];
    return found;
}

static const char *bg_from_shorthand(const char *v) {
    if (!v) return NULL;
    if (strchr(v, ' ') || strstr(v, "url(")) return NULL;
    return v;
}

 //one box per line. Uncolored stuff fall back to defaults
static void print_layout(LayoutBox *box, int depth, const char *inh_fg) {
    if (box->display == DISPLAY_NONE) return;

    indent(depth);
    RenderNode *n = box->node;
    if (n) {
        if (n->tag)
            printf("<%s>", n->tag);
        else if (n->text) {
            putchar('"');
            print_esc(n->text);
            putchar('"');
        } else
            fputs("<root>", stdout);
    } else {
        fputs("<root>", stdout);
    }
    printf(" {%g %g %g %g}",
           box->bounds.x * g_kx, box->bounds.y * g_ky,
           box->bounds.width * g_kx, box->bounds.height * g_ky);

    const char *fg = node_prop(n, "color");
    if (!fg) fg = inh_fg;
    const char *bg = node_prop(n, "background-color");
    if (!bg) bg = bg_from_shorthand(node_prop(n, "background"));
    const char *img_src = node_prop(n, "image-src");
    const char *bw = node_prop(n, "border-width");
    const char *bc = node_prop(n, "border-color");

    if (fg) printf(" fg=\"%s\"", fg);
    if (bg) printf(" bg=\"%s\"", bg);
    if (img_src) printf(" image-src=\"%s\"", img_src);
    if (bw && bw[0] && strcmp(bw, "0") && strcmp(bw, "0px")) printf(" border-width=\"%s\"", bw);
    if (bc && bc[0]) printf(" border-color=\"%s\"", bc);
    putchar('\n');

    for (size_t i = 0; i < box->child_count; i++)
        print_layout(box->children[i], depth + 1, fg);
}

static FILE *open_input(const char *arg) {
    if (!strcmp(arg, "-"))
        return stdin;
    FILE *fp = fopen(arg, "r");
    if (!fp) {
        fprintf(stderr, "layout: cannot open %s\n", arg);
        exit(1);
    }
    return fp;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: layout <browser-w> <cols> <rows> [render-tree-dump]\n");
        return 1;
    }
    float bw = strtof(argv[1], NULL);
    int cols = atoi(argv[2]);
    int rows = atoi(argv[3]);
    if (bw > cols * TEXT_CHAR_W) bw = cols * TEXT_CHAR_W;
    g_kx = cols * 4.0f / bw;
    g_ky = g_kx / CELL_ASPECT;
    g_css_per_cell = bw / (float)cols;
    float bh = rows * 4.0f / g_ky;
    FILE *fp = (argc >= 5) ? open_input(argv[4]) : stdin;

    LayoutBox *layout = layout_tree(fp, bw, bh);

    puts("#layout");
    print_layout(layout, 0, NULL);
    tmp_nodes_free();

    RenderNode *root = layout->node;
    free_layout_box(layout);
    free_render_node(root);
    if (fp != stdin)
        fclose(fp);
    return 0;
}
