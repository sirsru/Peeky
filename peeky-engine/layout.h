#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdio.h>

typedef struct RenderNode {
    char *tag;
    char *text;
    char **props;
    char **values;
    int prop_n, prop_cap;
    struct RenderNode **kids;
    int kid_n, kid_cap;
} RenderNode;

typedef struct {
    float x, y, width, height;
} LayoutRect;

typedef enum {
    DISPLAY_BLOCK,
    DISPLAY_INLINE,
    DISPLAY_NONE,
    DISPLAY_UNKNOWN
} DisplayType;

typedef struct LayoutBox {
    LayoutRect bounds;
    RenderNode *node;
    DisplayType display;
    struct LayoutBox **children;
    size_t child_count;
} LayoutBox;

RenderNode *parse_render_tree(FILE *fp);
void free_render_node(RenderNode *n);

LayoutBox *layout_tree(FILE *fp, float viewport_w, float viewport_h);
void free_layout_box(LayoutBox *box);

DisplayType node_display(RenderNode *n);

#endif
