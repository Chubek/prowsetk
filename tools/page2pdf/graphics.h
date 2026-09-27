/* graphics.h -- public surface of the page2pdf print engine.
 *
 * page2pdf is a downstream consumer of serialized ProwseTk IR. It never links
 * against Flatworm and never touches the C++ DOM: the only thing crossing the
 * IR boundary is the event stream (start/attribute/text/end), which this
 * header accepts through renderer_start/renderer_attribute/renderer_text/
 * renderer_end. Everything after that point is layout and painting.
 *
 * The engine is a bounded CSS layout subset, not a browser. See README
 * "Page IR to PDF" for the supported subset and the documented gaps.
 */
#ifndef PROWSETK_PAGE2PDF_GRAPHICS_H
#define PROWSETK_PAGE2PDF_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <hpdf.h>

/* Hard bounds. Every one of these is a fail-fast limit, never a silent cap. */
#define PAGE2PDF_MAX_NESTING 512U
#define PAGE2PDF_MAX_TAG_BYTES 127U
#define PAGE2PDF_MAX_PAGES 512U

typedef struct GraphicNode GraphicNode;
typedef struct FontRegistry FontRegistry;

/* One open element while the event stream is being replayed. */
typedef struct {
    char tag[PAGE2PDF_MAX_TAG_BYTES + 1U];
    GraphicNode *node;
} ElementFrame;

/* Page geometry, in PostScript points (1/72"). */
typedef struct {
    float width;
    float height;
    float margin_top;
    float margin_right;
    float margin_bottom;
    float margin_left;
} PageGeometry;

typedef struct {
    /* --- PDF output --------------------------------------------------- */
    HPDF_Doc document;
    HPDF_Page pages[PAGE2PDF_MAX_PAGES];
    size_t page_count;
    HPDF_Font fallback_regular;
    HPDF_Font fallback_bold;
    HPDF_Font fallback_italic;
    HPDF_Font fallback_bold_italic;
    HPDF_Font fallback_mono;
    HPDF_STATUS error;
    HPDF_STATUS detail;

    /* --- event-stream replay ------------------------------------------ */
    ElementFrame frames[PAGE2PDF_MAX_NESTING];
    size_t frame_count;
    size_t node_count;
    size_t max_nodes;
    GraphicNode *root;
    const char *title;

    /* --- configuration ------------------------------------------------ */
    FontRegistry *fonts;
    PageGeometry geometry;
    bool have_geometry;
    const char *font_dir;
    bool no_embed_fonts;
    bool no_links;
    bool quiet;
} Renderer;

/* True when a Haru call has failed. `renderer->detail` carries Haru's code. */
bool renderer_failed(const Renderer *renderer);

/* Prepares the PDF document, the page geometry, and the font registry. */
bool renderer_init(Renderer *renderer);
void renderer_destroy(Renderer *renderer);

/* --- Event-stream sink (the IR boundary) --------------------------------- */

/* Pushes an element start at `depth`. `depth` must equal the open depth. */
bool renderer_start(Renderer *renderer, const char *tag, size_t depth);
/* Pushes an attribute onto the element at `depth` (the currently open one). */
bool renderer_attribute(Renderer *renderer, const char *name,
                        const unsigned char *value, size_t length, size_t depth);
/* Pushes a text node at `depth`. `tag` is the owning element path tag when the
 * encoding carries it, or NULL when it does not. */
bool renderer_text(Renderer *renderer, const unsigned char *text,
                   size_t length, size_t depth, const char *tag);
/* Pops the element open at `depth`. */
bool renderer_end(Renderer *renderer, const char *tag, size_t depth);

/* --- Output ------------------------------------------------------------- */

/* Runs the cascade, lays the document out, and paints every page. */
bool renderer_draw(Renderer *renderer);
/* Writes the PDF to `path`. */
bool renderer_save(Renderer *renderer, const char *path);
/* Writes a human-readable layout report (pages, boxes, text runs, fonts). */
void renderer_inspect(const Renderer *renderer, FILE *out);
/* Copy of the document title, or "" when the document has none. */
const char *renderer_title(const Renderer *renderer);

#endif /* PROWSETK_PAGE2PDF_GRAPHICS_H */
