#ifndef PROWSETK_PAGE2PDF_GRAPHICS_H
#define PROWSETK_PAGE2PDF_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <hpdf.h>

#define PAGE2PDF_MAX_NESTING 256U
#define PAGE2PDF_MAX_TAG_BYTES 127U

typedef struct GraphicNode GraphicNode;

typedef struct {
    char tag[PAGE2PDF_MAX_TAG_BYTES + 1U];
    GraphicNode *node;
} ElementFrame;

typedef struct {
    HPDF_Doc document;
    HPDF_Page page;
    HPDF_Font regular;
    HPDF_Font bold;
    HPDF_Font mono;
    HPDF_Page pages[128];
    size_t page_count;
    ElementFrame frames[PAGE2PDF_MAX_NESTING];
    size_t frame_count;
    size_t node_count;
    GraphicNode *root;
    HPDF_STATUS error;
    HPDF_STATUS detail;
} Renderer;

bool renderer_init(Renderer *renderer);
void renderer_destroy(Renderer *renderer);
bool renderer_start(Renderer *renderer, const char *tag, size_t depth);
bool renderer_attribute(Renderer *renderer, const char *name,
                        const unsigned char *value, size_t length, size_t depth);
bool renderer_text(Renderer *renderer, const unsigned char *text,
                   size_t length, size_t depth, const char *tag);
bool renderer_end(Renderer *renderer, const char *tag, size_t depth);
bool renderer_draw(Renderer *renderer);

#endif
