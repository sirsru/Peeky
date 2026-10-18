#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define MAX_COLORS 256

//turns layout into drawing :yipee:

//Utils

static void die(const char *msg) {
    fputs(msg, stderr);
    putc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

#define GROW(a, n, cap)                         \
    do {                                        \
        if ((n) >= (cap)) {                     \
            (cap) = (cap) ? (cap) * 2 : 16;     \
            (a)   = xrealloc((a), (cap)*sizeof *(a)); \
        }                                       \
    } while (0)

typedef uint32_t Color;                 // 0xRRGGBB
#define COLOR_R(c) (((c) >> 16) & 0xff)
#define COLOR_G(c) (((c) >>  8) & 0xff)
#define COLOR_B(c) ((c) & 0xff)
#define RGB(r,g,b) ((((uint32_t)(r) & 0xff) << 16) | \
                    (((uint32_t)(g) & 0xff) << 8)  | \
                    ((uint32_t)(b) & 0xff))

#define BG_DEFAULT  RGB(255,255,255)
#define FG_DEFAULT  RGB(0,0,0)

//Glyphs to render images with

static const char *const BLOCKS[16] = {
    " ", "\xe2\x96\x97", "\xe2\x96\x96", "\xe2\x96\x84",
    "\xe2\x96\x9d", "\xe2\x96\x90", "\xe2\x96\x9e", "\xe2\x96\x9f",
    "\xe2\x96\x98", "\xe2\x96\x9a", "\xe2\x96\x8c", "\xe2\x96\x99",
    "\xe2\x96\x80", "\xe2\x96\x9c", "\xe2\x96\x9b", "\xe2\x96\x88",
};

static int luma(Color c) {
    return (int)(0.299 * COLOR_R(c) + 0.587 * COLOR_G(c) + 0.114 * COLOR_B(c));
}

//nearest neighbor TTwTT

static Color brightest(Color *p, int n) {
    int best = 0, best_l = -1;
    for (int i = 0; i < n; i++) {
        int l = luma(p[i]);
        if (l > best_l) { best_l = l; best = i; }
    }
    return p[best];
}

static Color darkest(Color *p, int n) {
    int best = 0, best_l = 256;
    for (int i = 0; i < n; i++) {
        int l = luma(p[i]);
        if (l < best_l) { best_l = l; best = i; }
    }
    return p[best];
}

//color parsing

static const struct { const char *name; Color c; } NAMED[] = {
    {"black",   RGB(0,0,0)},   {"silver",   RGB(192,192,192)},
    {"gray",    RGB(128,128,128)}, {"white",  RGB(255,255,255)},
    {"maroon",  RGB(128,0,0)}, {"red",      RGB(255,0,0)},
    {"purple",  RGB(128,0,128)}, {"fuchsia", RGB(255,0,255)},
    {"green",   RGB(0,128,0)}, {"lime",     RGB(0,255,0)},
    {"olive",   RGB(128,128,0)}, {"yellow", RGB(255,255,0)},
    {"navy",    RGB(0,0,128)}, {"blue",     RGB(0,0,255)},
    {"teal",    RGB(0,128,128)}, {"aqua",   RGB(0,255,255)},
    {"orange",  RGB(255,165,0)}, {"pink",   RGB(255,192,203)},
    {"brown",   RGB(165,42,42)}, {"cyan",   RGB(0,255,255)},
    {NULL, 0}
};

static int hexdigit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

//more color parsing
static int parse_hsl(const char *s, Color *out) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "hsl", 3)) return 0;
    p += 3;
    if (*p == 'a') p++;
    if (*p != '(') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    float h = strtof(p, (char **)&p);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ',') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    float s_val = strtof(p, (char **)&p);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '%') { p++; }
    if (s_val > 100) s_val = 100;
    s_val /= 100.0f;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ',') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    float l = strtof(p, (char **)&p);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '%') { p++; }
    if (l > 100) l = 100;
    l /= 100.0f;

    float c = (1 - fabsf(2 * l - 1)) * s_val;
    float x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    float m = l - c / 2;
    float r1, g1, b1;
    if (h < 60) { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300) { r1 = x; g1 = 0; b1 = c; }
    else { r1 = c; g1 = 0; b1 = x; }
    uint8_t r = (uint8_t)((r1 + m) * 255 + 0.5f);
    uint8_t g = (uint8_t)((g1 + m) * 255 + 0.5f);
    uint8_t b = (uint8_t)((b1 + m) * 255 + 0.5f);
    *out = RGB(r, g, b);
    return 1;
}

//wowie even more colors
static int parse_rgba(const char *s, Color *out) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "rgba(", 5)) return 0;
    p += 5;
    uint8_t v[4] = {0,0,0,255};
    for (int k = 0; k < 4; k++) {
        while (*p == ' ' || *p == ',') p++;
        if (k == 3) {
            float a = strtof(p, (char **)&p);
            if (a < 0) a = 0;
            if (a > 1) a = 1;
            v[3] = (uint8_t)(a * 255 + 0.5f);
        } else {
            int val = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '%') { p++; val = val * 255 / 100; }
            if (val > 255) val = 255;
            v[k] = (uint8_t)val;
        }
    }
    *out = RGB(v[0], v[1], v[2]);
    return 1;
}

//More colors!1!
static int parse_color(const char *s, Color *out) {
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '#') {
        const char *p = s + 1;
        int n = 0;
        while (hexdigit(p[n]) >= 0) n++;
        if (isalnum((unsigned char)p[n])) return 0;
        if (n == 3 || n == 4) { //rgb and rgba
            *out = RGB(hexdigit(p[0]) * 17, hexdigit(p[1]) * 17, hexdigit(p[2]) * 17);
            return 1;
        }
        if (n == 6 || n == 8) { // #rrggbb and #rrggbbaa
            *out = RGB(hexdigit(p[0]) * 16 + hexdigit(p[1]),
                       hexdigit(p[2]) * 16 + hexdigit(p[3]),
                       hexdigit(p[4]) * 16 + hexdigit(p[5]));
            return 1;
        }
        return 0;
    }
    if (!strncmp(s, "rgb(", 4)) {
        const char *p = s + 4;
        while (*p == ' ' || *p == ',') p++;
        int ok = 1;
        uint8_t v[3] = {0,0,0};
        for (int k = 0; k < 3; k++) {
            while (*p == ' ' || *p == ',') p++;
            if (*p >= '0' && *p <= '9') {
                int val = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '%') { p++; if (val > 100) val = 100; val = val * 255 / 100; }
                if (val > 255) val = 255;
                v[k] = (uint8_t)val;
            } else if (*p == '%') {
                int val = atoi(p);
                if (val > 100) val = 100;
                v[k] = (uint8_t)(val * 255 / 100);
                while (*p >= '0' && *p <= '9') p++;
                if (*p == '%') p++;
            } else { ok = 0; break; }
        }
        if (ok) { *out = RGB(v[0], v[1], v[2]); return 1; }
        return 0;
    }
    
    if (!strncmp(s, "rgba(", 5)) return parse_rgba(s, out);
    if (!strncmp(s, "hsl", 3)) return parse_hsl(s, out);
    
    for (int i = 0; NAMED[i].name; i++) {
        size_t n = strlen(NAMED[i].name);
        if (!strncasecmp(s, NAMED[i].name, n) && (s[n] == ' ' || s[n] == '\0' || s[n] == ',' || s[n] == ')')) {
            *out = NAMED[i].c;
            return 1;
        }
    }
    return 0;
}

//colors yet again
static int find_color(const char *val, Color *out) {
    if (parse_color(val, out)) return 1;
    //try to find a color
    for (const char *p = val; *p; p++) {
        if (*p == '#' || !strncmp(p, "rgb(", 4) || !strncmp(p, "rgba(", 5) || !strncmp(p, "hsl(", 4)) {
            if (parse_color(p, out)) return 1;
        }
    }
    //try keywords
    for (int i = 0; NAMED[i].name; i++) {
        const char *p = val;
        while ((p = strcasestr(p, NAMED[i].name)) != NULL) {
            size_t n = strlen(NAMED[i].name);
            int ok = 1;
            if (p > val && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '(' && p[-1] != ',' && p[-1] != ')' && p[-1] != ';' && p[-1] != '{' && p[-1] != '}') ok = 0;
            if (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '(' && p[n] != ',' && p[n] != ')' && p[n] != ';' && p[n] != '{' && p[n] != '}') ok = 0;
            if (ok) { *out = NAMED[i].c; return 1; }
            p++;
        }
    }
    return 0;
}

//layout box tree
typedef struct Box {
    int is_text; //1 is text, 0 is elemt
    char *label; //name or content
    int x, y, w, h;
    Color fg; //inherit text color
    Color bg;             /* background color (elements only) */
    int has_fg, has_bg;
    int has_border; //1 is border width or color
    float border_w; //actual width
    Color border_color; //you guessed it! The color!!!1!!
    char *image_src;
    struct Box **kids;
    int kid_n, kid_cap;
} Box;

static Box *new_box(int is_text, const char *label) {
    Box *b = xmalloc(sizeof *b);
    memset(b, 0, sizeof *b);
    b->is_text = is_text;
    b->label = strdup(label ? label : "");
    b->fg = FG_DEFAULT; b->bg = BG_DEFAULT;
    b->has_border = 0; b->border_w = 0; b->border_color = BG_DEFAULT;
    return b;
}

static void add_kid(Box *p, Box *k) {
    GROW(p->kids, p->kid_n, p->kid_cap);
    p->kids[p->kid_n++] = k;
}

static void free_boxes(Box *b);

static char *read_value(char **pp) {
    char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    char *out;
    if (*p == '"') {
        p++;
        const char *st = p;
        while (*p && *p != '"') p++;
        out = xmalloc((size_t)(p - st) + 1);
        memcpy(out, st, (size_t)(p - st));
        out[p - st] = '\0';
        if (*p == '"') p++;
        *pp = p;
    } else {
        const char *st = p;
        int depth = 0;
        while (*p && ((*p != ' ' && *p != '\t') || depth > 0)) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        out = xmalloc((size_t)(p - st) + 1);
        memcpy(out, st, (size_t)(p - st));
        out[p - st] = '\0';
        *pp = p;
    }
    return out;
}

//parse one line
static Box *parse_line(char *content) {
    char *p = content;
    while (*p == ' ' || *p == '\t') p++;

    Box *b;
    if (*p == '<') {
        char *ge = strchr(p, '>');
        if (!ge) return NULL;
        size_t n = (size_t)(ge - p - 1);
        char *tag = xmalloc(n + 1);
        memcpy(tag, p + 1, n);
        tag[n] = '\0';
        b = new_box(0, tag);
        free(tag);
        p = ge + 1;
    } else if (*p == '"') {
        p++;
        size_t cap = 16, n = 0;
        char *buf = xmalloc(cap);
        while (*p && *p != '"') {
            char c;
            if (*p == '\\' && p[1]) { p++; c = *p;
                if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
            } else c = *p;
            if (n + 1 >= cap) { cap *= 2; buf = xrealloc(buf, cap); }
            buf[n++] = c;
            p++;
        }
        if (*p == '"') p++;
        buf[n] = '\0';
        b = new_box(1, buf);
        free(buf);
    } else {
        return NULL;
    }

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '{') {
        p++;
        float v[4] = {0,0,0,0};
        for (int i = 0; i < 4; i++) {
            while (*p == ' ' || *p == '\t') p++;
            v[i] = strtof(p, &p);
        }
        while (*p && *p != '}') p++;
        if (*p == '}') p++;
        b->x = (int)lroundf(v[0]); b->y = (int)lroundf(v[1]);
        b->w = (int)lroundf(v[2]); b->h = (int)lroundf(v[3]);
    }

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "fg=", 3)) {
            p += 3;
            char *val = read_value(&p);
            Color c;
            if (find_color(val, &c)) { b->fg = c; b->has_fg = 1; }
            free(val);
        } else if (!strncmp(p, "bg=", 3)) {
            p += 3;
            char *val = read_value(&p);
            Color c;
            if (find_color(val, &c)) { b->bg = c; b->has_bg = 1; }
            free(val);
        } else if (!strncmp(p, "image-src=", 10)) {
            p += 10;
            char *val = read_value(&p);
            b->image_src = val;
        } else if (!strncmp(p, "border-width=", 13)) {
            p += 13;
            char *val = read_value(&p);
            char *end = NULL;
            b->border_w = strtof(val, &end);
            b->has_border = 1;
            free(val);
        } else if (!strncmp(p, "border-color=", 13)) {
            p += 13;
            char *val = read_value(&p);
            if (find_color(val, &b->border_color)) b->has_border = 1;
            free(val);
        } else {
            p++;
        }
    }
    return b;
}

//read layout dump :3
static Box *read_dump(FILE *fp, int *out_w, int *out_h) {
    char *line = NULL; size_t cap = 0; ssize_t len;
    Box *stack[256] = {0}; int sp = 0;
    Box *root = NULL;
    int first = 1;
    while ((len = getline(&line, &cap, fp)) != -1) {
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;
        if (first) {
            first = 0;
            continue;
        }
        if (line[0] != '|') continue;
        char *p = line + 1;
        int spaces = 0;
        while (*p == ' ') { spaces++; p++; }
        int depth = spaces / 2;
        while (depth + 1 < sp) sp--;
        Box *b = parse_line(p);
        if (!b) continue;
        if (depth == 0 && !root) {
            root = b;
            *out_w = b->w; *out_h = b->h;
            stack[0] = b; sp = 1;
            stack[1] = b; sp = 2;
        } else {
            Box *parent = (sp > 0) ? stack[depth] : NULL;
            if (!parent) {
                //if ts happens my shitass code got screwed somewhere earlier
                free_boxes(b);
                continue;
            }
            add_kid(parent, b);
            if (depth + 1 < 256) { stack[depth + 1] = b; sp = depth + 2; }
        }
    }
    free(line);
    return root;
}

//fwamebuffew

static Color *fb; //width * height, row major
static int fb_w, fb_h;

static void fb_init(int w, int h) {
    fb_w = w; fb_h = h;
    fb = xmalloc((size_t)fb_w * fb_h * sizeof *fb);
    for (int i = 0; (size_t)i < (size_t)fb_w * fb_h; i++)
        fb[i] = BG_DEFAULT;
}

static void fb_fill_rect(Color c, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        Color *row = fb + (size_t)yy * fb_w + x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}

static Color fb_sample(int x, int y) {
    if (x < 0) x = 0;
    if (x >= fb_w) x = fb_w - 1;
    if (y < 0) y = 0;
    if (y >= fb_h) y = fb_h - 1;
    return fb[(size_t)y * fb_w + x];
}

//Nearest neighbor strikes again
static Color down2(int sx, int sy) {
    return fb_sample(sx*2, sy*2);
}

//background stuff

static int is_replaced(Box *b) {
    if (b->is_text) return 0;
    return (strcmp(b->label, "img") == 0 ||
            strcmp(b->label, "input") == 0 ||
            strcmp(b->label, "textarea") == 0 ||
            strcmp(b->label, "select") == 0);
}

static void paint_boxes(Box *b, Color inherit_fg, Color inherit_bg) {
    if (b->is_text) return;
    Color bg = b->has_bg ? b->bg : inherit_bg;
    Color fg = b->has_fg ? b->fg : inherit_fg;
    if (!b->is_text && !is_replaced(b)) {
        int x0 = b->x, y0 = b->y, x1 = b->x + b->w, y1 = b->y + b->h;
        if (b->has_bg) {
            if (b->w > 0 && x1 == x0) x1 = x0 + 1;
            if (b->h > 0 && y1 == y0) y1 = y0 + 1;
            x0 &= ~1; y0 &= ~1;
            x1 = (x1 + 1) & ~1; y1 = (y1 + 1) & ~1;
        }
        fb_fill_rect(bg, x0, y0, x1 - x0, y1 - y0);
    }
    if (!b->is_text && b->has_border && b->border_w > 0) {
        int x0 = b->x, y0 = b->y, x1 = b->x + b->w, y1 = b->y + b->h;
        int bw = (int)b->border_w;
        if (bw < 1) bw = 1;
        int x2 = x1 - bw, y2 = y1 - bw;
        if (x2 < x0) x2 = x0;
        if (y2 < y0) y2 = y0;
        fb_fill_rect(b->border_color, x0, y0, x1 - x0, bw); //top
        fb_fill_rect(b->border_color, x0, y2, x1 - x0, y1 - y2); // bottom
        fb_fill_rect(b->border_color, x0, y0, bw, y1 - y0); // left
        fb_fill_rect(b->border_color, x2, y0, x1 - x2, y1 - y0); // right
    }
    for (int i = 0; i < b->kid_n; i++)
        paint_boxes(b->kids[i], fg, bg);
}

//character grid

typedef struct {
    const char *glyph;
    Color fg, bg;
} Cell;

static Cell *grid; //cols*rows
static int  g_cols, g_rows;

static void cell_from_source(Cell *c, int cx, int cy) {
    int sx0 = cx * 2, sy0 = cy * 2;
    Color q[4];
    q[0] = down2(sx0,     sy0);
    q[1] = down2(sx0 + 1, sy0);
    q[2] = down2(sx0,     sy0 + 1);
    q[3] = down2(sx0 + 1, sy0 + 1);
    int lums[4];
    for (int i = 0; i < 4; i++) lums[i] = luma(q[i]);
    int mean = (lums[0]+lums[1]+lums[2]+lums[3]) / 4;

    Color bright[4]; int bn = 0;
    Color dark[4];   int dn = 0;
    for (int i = 0; i < 4; i++) {
        if (lums[i] > mean) bright[bn++] = q[i];
        else                 dark[dn++]   = q[i];
    }

    c->fg = bn ? brightest(bright, bn) : brightest(q, 4);
    c->bg = dn ? darkest(dark,   dn)   : darkest(q, 4);

    unsigned idx = 0;
    if (lums[0] > mean) idx |= 8;
    if (lums[1] > mean) idx |= 4;
    if (lums[2] > mean) idx |= 2;
    if (lums[3] > mean) idx |= 1;
    c->glyph = BLOCKS[idx];
}

static void build_grid(void) {
    int s_w = fb_w / 2, s_h = fb_h / 2;
    g_cols = s_w / 2;
    g_rows = s_h / 2;
    if (g_cols < 1) g_cols = 1;
    if (g_rows < 1) g_rows = 1;
    grid = xmalloc((size_t)g_cols * g_rows * sizeof *grid);
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++)
            cell_from_source(&grid[(size_t)r * g_cols + c], c, r);
}

//text overlay...

static const char *ascii_glyph(unsigned char ch) {
    static char glyphs[256][2];
    static int   inited = 0;
    if (!inited) {
        for (int i = 0; i < 256; i++) {
            glyphs[i][0] = (char)i;
            glyphs[i][1] = '\0';
        }
        inited = 1;
    }
    return glyphs[ch];
}

static void place_text_ascii(Box *b, Color fg, Color bg) {
    if (!b->is_text) return;
    const char *t = b->label;
    size_t n = strlen(t);
    if (n == 0) return;
    int cc0 = b->x / 4;
    int rr0 = b->y / 4;
    int ncells = (b->w > 0) ? (b->w / 4) : (int)n;
    if (ncells > (int)n) ncells = (int)n;
    if (rr0 < 0) rr0 = 0;
    for (int i = 0; i < ncells; i++) {
        int cc = cc0 + i;
        if (cc < 0 || cc >= g_cols) break;
        if (rr0 >= g_rows) break;
        unsigned char ch = (unsigned char)t[i];
        if (ch < 32) ch = ' ';
        Cell *c = &grid[(size_t)rr0 * g_cols + cc];
        c->glyph = ascii_glyph(ch);
        c->fg = fg;
        c->bg = bg;
    }
}

static void overlay_text(Box *b, Color fg, Color bg) {
    if (!b->is_text) {
        if (b->has_fg) fg = b->fg;
        if (b->has_bg) bg = b->bg;
        for (int i = 0; i < b->kid_n; i++)
            overlay_text(b->kids[i], fg, bg);
        return;
    }
    place_text_ascii(b, b->has_fg ? b->fg : fg, bg);
}

static void draw_rect_border(int x0, int y0, int x1, int y1, Color fg, Color bg) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= g_cols) x1 = g_cols - 1;
    if (y1 >= g_rows) y1 = g_rows - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int x = x0; x <= x1; x++) {
        Cell *c = &grid[(size_t)y0 * g_cols + x];
        c->glyph = "\xe2\x94\x80"; c->fg = fg; c->bg = bg;
        c = &grid[(size_t)y1 * g_cols + x];
        c->glyph = "\xe2\x94\x80"; c->fg = fg; c->bg = bg;
    }
    for (int y = y0; y <= y1; y++) {
        Cell *c = &grid[(size_t)y * g_cols + x0];
        c->glyph = "\xe2\x94\x82"; c->fg = fg; c->bg = bg;
        c = &grid[(size_t)y * g_cols + x1];
        c->glyph = "\xe2\x94\x82"; c->fg = fg; c->bg = bg;
    }
    grid[(size_t)y0 * g_cols + x0].glyph = "\xe2\x94\x8c";
    grid[(size_t)y0 * g_cols + x1].glyph = "\xe2\x94\x90";
    grid[(size_t)y1 * g_cols + x0].glyph = "\xe2\x94\x94";
    grid[(size_t)y1 * g_cols + x1].glyph = "\xe2\x94\x98";
}

static void place_alt_text(int x0, int y0, int x1, int y1, const char *text, Color fg, Color bg) {
    if (!text || !*text) text = "IMG";
    int cx = (x0 + x1) / 2;
    int cy = (y0 + y1) / 2;
    int len = strlen(text);
    int start_x = cx - len / 2;
    for (int i = 0; i < len; i++) {
        int x = start_x + i;
        if (x < x0 + 1 || x > x1 - 1) continue;
        if (cy < y0 || cy > y1) continue;
        Cell *c = &grid[(size_t)cy * g_cols + x];
        c->glyph = ascii_glyph((unsigned char)text[i]);
        c->fg = fg;
        c->bg = bg;
    }
}

static char *g_base_dir = NULL;

static char *resolve_path(const char *src) {
    if (!src) return NULL;
    if (src[0] == '/' || (src[0] && src[1] == ':')) return xstrdup(src);
    if (!g_base_dir) return xstrdup(src);
    size_t n = strlen(g_base_dir) + 1 + strlen(src) + 1;
    char *out = xmalloc(n);
    snprintf(out, n, "%s/%s", g_base_dir, src);
    return out;
}

static void cell_rect(Box *b, int *c0, int *r0, int *c1, int *r1) {
    *c0 = b->x / 4;  *r0 = b->y / 4;
    *c1 = (b->x + b->w + 3) / 4;  *r1 = (b->y + b->h + 3) / 4;
}

static float s2l[256];
static int s2l_init;
static void init_lut(void) {
    if (s2l_init) return;
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0f;
        s2l[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    s2l_init = 1;
}
static uint8_t l2s(float l) {
    if (l < 0) l = 0;
    if (l > 1) l = 1;
    float c = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1 / 2.4f) - 0.055f;
    return (uint8_t)(c * 255.0f + 0.5f);
}

static Color sample_area(const unsigned char *d, int w, int h, float u0, float v0, float u1, float v1) {
    int x = (int)lroundf(((u0 + u1) / 2.0f) * w);
    int y = (int)lroundf(((v0 + v1) / 2.0f) * h);
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    const unsigned char *p = d + ((size_t)y * w + x) * 4;
    float a = p[3] / 255.0f;
    float r = s2l[p[0]] * a + (1 - a);
    float g = s2l[p[1]] * a + (1 - a);
    float b = s2l[p[2]] * a + (1 - a);
    return RGB(l2s(r), l2s(g), l2s(b));
}

static Color avg(Color *p, int n) {
    long r = 0, g = 0, b = 0;
    for (int i = 0; i < n; i++) { r += COLOR_R(p[i]); g += COLOR_G(p[i]); b += COLOR_B(p[i]); }
    return RGB(r / n, g / n, b / n);
}

static void render_image(Box *b) {
    if (!b->image_src) return;
    init_lut();
    char *path = resolve_path(b->image_src);
    if (!path) return;
    int cc0, rr0, cc1, rr1;
    cell_rect(b, &cc0, &rr0, &cc1, &rr1);
    int full_w = cc1 - cc0, full_h = rr1 - rr0;
    if (full_w <= 0 || full_h <= 0) { free(path); return; }

    int w, h, ch;
    unsigned char *data = stbi_load(path, &w, &h, &ch, 4);
    free(path);
    if (!data) return;

    int dw = full_w * 2, dh = full_h * 2;
    Color *resized = xmalloc((size_t)dw * dh * sizeof *resized);
    for (int dy = 0; dy < dh; dy++) {
        int sy0 = (dy * h) / dh;
        int sy1 = ((dy + 1) * h) / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = (dx * w) / dw;
            int sx1 = ((dx + 1) * w) / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            long r = 0, g = 0, b = 0; int n = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    const unsigned char *p = data + ((size_t)yy * w + xx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            resized[(size_t)dy * dw + dx] = RGB(r / n, g / n, b / n);
        }
    }
    for (int cy = rr0; cy < rr1; cy++) {
        if (cy < 0 || cy >= g_rows) continue;
        for (int cx = cc0; cx < cc1; cx++) {
            if (cx < 0 || cx >= g_cols) continue;
            int ox = (cx - cc0) * 2;
            int oy = (cy - rr0) * 2;
            Color q[4];
            q[0] = resized[(size_t)oy * dw + ox];
            q[1] = resized[(size_t)oy * dw + ox + 1];
            q[2] = resized[(size_t)(oy + 1) * dw + ox];
            q[3] = resized[(size_t)(oy + 1) * dw + ox + 1];
            int lums[4];
            for (int i = 0; i < 4; i++) lums[i] = luma(q[i]);
            int mean = (lums[0] + lums[1] + lums[2] + lums[3]) / 4;
            Color bright[4], dark[4];
            int bn = 0, dn = 0;
            for (int i = 0; i < 4; i++) {
                if (lums[i] > mean) bright[bn++] = q[i];
                else                dark[dn++] = q[i];
            }
            Cell *cell = &grid[(size_t)cy * g_cols + cx];
            cell->fg = bn ? avg(bright, bn)   : avg(q, 4);
            cell->bg = dn ? avg(dark,   dn)   : avg(q, 4);
            unsigned idx = 0;
            if (lums[0] > mean) idx |= 8;
            if (lums[1] > mean) idx |= 4;
            if (lums[2] > mean) idx |= 2;
            if (lums[3] > mean) idx |= 1;
            cell->glyph = BLOCKS[idx];
        }
    }
    free(resized);
    stbi_image_free(data);
}

static void overlay_replaced(Box *b) {
    if (b->is_text) return;
    if (strcmp(b->label, "img") == 0) {
        if (b->image_src) {
            render_image(b);
        } else {
            int cc0, rr0, cc1, rr1;
            cell_rect(b, &cc0, &rr0, &cc1, &rr1);
            cc1--; rr1--;
            if (cc0 < 0) cc0 = 0;
            if (rr0 < 0) rr0 = 0;
            if (cc1 >= g_cols) cc1 = g_cols - 1;
            if (rr1 >= g_rows) rr1 = g_rows - 1;
            if (cc0 <= cc1 && rr0 <= rr1) {
                Color fg = b->has_fg ? b->fg : FG_DEFAULT;
                Color bg = b->has_bg ? b->bg : BG_DEFAULT;
                draw_rect_border(cc0, rr0, cc1, rr1, fg, bg);
                place_alt_text(cc0, rr0, cc1, rr1, b->label, fg, bg);
            }
        }
    }
    for (int i = 0; i < b->kid_n; i++)
        overlay_replaced(b->kids[i]);
}

static Color xterm_rgb(int i) {
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    if (i >= 232) { int v = 8 + 10 * (i - 232); return RGB(v, v, v); }
    i -= 16;
    return RGB(lv[i / 36], lv[(i / 6) % 6], lv[i % 6]);
}

static int sqd(Color a, Color b) {
    int dr = COLOR_R(a) - COLOR_R(b), dg = COLOR_G(a) - COLOR_G(b), db = COLOR_B(a) - COLOR_B(b);
    return 3 * dr * dr + 4 * dg * dg + 2 * db * db;
}

static int xterm_index(Color c) {
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    int ch[3] = {COLOR_R(c), COLOR_G(c), COLOR_B(c)}, q[3];
    for (int k = 0; k < 3; k++) {
        int bi = 0, bd = 1 << 30;
        for (int i = 0; i < 6; i++) {
            int d = abs(ch[k] - lv[i]);
            if (d < bd) { bd = d; bi = i; }
        }
        q[k] = bi;
    }
    int cube = 16 + 36 * q[0] + 6 * q[1] + q[2];
    int avg3 = (ch[0] + ch[1] + ch[2]) / 3;
    int gi = (avg3 - 8 + 5) / 10;
    if (gi < 0) gi = 0;
    if (gi > 23) gi = 23;
    int gray = 232 + gi;
    return sqd(c, xterm_rgb(gray)) < sqd(c, xterm_rgb(cube)) ? gray : cube;
}

static void emit_grid(void) {
    static unsigned char used[256];
    int n = 0;
    for (int i = 0; i < g_cols * g_rows; i++) {
        int f = xterm_index(grid[i].fg), b = xterm_index(grid[i].bg);
        if (!used[f]) { used[f] = 1; n++; }
        if (!used[b]) { used[b] = 1; n++; }
    }
    printf("#palette %d\n", n);
    for (int i = 16; i < 256; i++)
        if (used[i]) {
            Color c = xterm_rgb(i);
            printf("%d %d %d %d\n", i, COLOR_R(c), COLOR_G(c), COLOR_B(c));
        }
    printf("#paint %d %d\n", g_cols, g_rows);
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            Cell *cell = &grid[(size_t)r * g_cols + c];
            printf("%s|%d|%d", cell->glyph, xterm_index(cell->fg), xterm_index(cell->bg));
            if (c + 1 < g_cols) putchar(' ');
        }
        putchar('\n');
    }
}

//palette and outpuet
static Color palette[MAX_COLORS];
static int   palette_n;

static int palette_lookup(Color c) {
    for (int i = 0; i < palette_n; i++)
        if (palette[i] == c) return i;
    return -1;
}

//nearest palette color
static int palette_nearest(Color c) {
    int best = 0, bestd = INT_MAX;
    for (int i = 0; i < palette_n; i++) {
        int dr = COLOR_R(c) - COLOR_R(palette[i]);
        int dg = COLOR_G(c) - COLOR_G(palette[i]);
        int db = COLOR_B(c) - COLOR_B(palette[i]);
        int d = (3 * dr * dr + 4 * dg * dg + 2 * db * db);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static int palette_add(Color c) {
    int id = palette_lookup(c);
    if (id >= 0) return id;
    if (palette_n < MAX_COLORS) {
        palette[palette_n] = c;
        return palette_n++;
    }
    return palette_nearest(c);
}

//ansi fuckery
static void emit_ansi(void) {
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            Cell *cell = &grid[(size_t)r * g_cols + c];
            Color fg = cell->fg, bg = cell->bg;
            int fg_eq_bg = (fg == bg);
            const char *g = cell->glyph;
            if (fg_eq_bg) {
                printf("\033[48;2;%d;%d;%dm ", COLOR_R(bg), COLOR_G(bg), COLOR_B(bg));
            } else {
                printf("\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm%s",
                       COLOR_R(fg), COLOR_G(fg), COLOR_B(fg),
                       COLOR_R(bg), COLOR_G(bg), COLOR_B(bg), g);
            }
        }
        fputs("\033[0m\n", stdout);
    }
}

//claenup

static void free_boxes(Box *b) {
    if (!b) return;
    for (int i = 0; i < b->kid_n; i++) free_boxes(b->kids[i]);
    free(b->kids);
    free(b->label);
    free(b);
}

static FILE *open_input(const char *arg) {
    if (!strcmp(arg, "-")) return stdin;
    FILE *fp = fopen(arg, "r");
    if (!fp) { fprintf(stderr, "paint: cannot open %s\n", arg); exit(1); }
    return fp;
}

int main(int argc, char **argv) {
    int ansi = 0;
    const char *arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ansi")) ansi = 1;
        else if (!strcmp(argv[i], "--base-dir") && i + 1 < argc) {
            g_base_dir = argv[++i];
        } else {
            arg = argv[i];
        }
    }
    FILE *fp = arg ? open_input(arg) : stdin;

    int vw = 0, vh = 0;
    Box *root = read_dump(fp, &vw, &vh);
    if (!root || vw <= 0 || vh <= 0) {
        fprintf(stderr, "paint: empty or malformed layout dump\n");
        return 1;
    }
    if (fp != stdin) fclose(fp);

    fb_w = vw; if (fb_w % 2) fb_w--;
    fb_h = vh; if (fb_h % 2) fb_h--;
    if (fb_w < 2) fb_w = 2;
    if (fb_h < 2) fb_h = 2;
    fb_init(fb_w, fb_h);

    Color inherit_fg = (root->has_fg ? root->fg : FG_DEFAULT);
    Color inherit_bg = (root->has_bg ? root->bg : BG_DEFAULT);
    paint_boxes(root, inherit_fg, inherit_bg);
    build_grid();
    overlay_text(root, inherit_fg, inherit_bg);
    overlay_replaced(root);

    if (ansi) emit_ansi();
    else      emit_grid();

    free(grid);
    free(fb);
    free_boxes(root);
    return 0;
}
