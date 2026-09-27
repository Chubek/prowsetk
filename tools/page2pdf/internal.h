/* internal.h -- shared contracts inside the page2pdf print engine.
 *
 * The engine is stratified so that each concern owns one translation unit:
 *
 *   dom.c     event-stream replay -> node tree; node queries.
 *   css.c     CSS tokenizing, selector matching, cascade, computed values.
 *   fonts.c   TrueType/OpenType face discovery, parsing, and fallback.
 *   text.c    UTF-8, bidi, glyph shaping, measurement, line breaking.
 *   layout.c  box construction, block/inline/flex/table/float layout,
 *             fragmentation into pages, and the flat display list.
 *   paint.c   display list -> libHaru operators, plus the inspect report.
 *
 * dom.c knows nothing about CSS, css.c knows nothing about geometry, layout.c
 * never calls libHaru directly, and paint.c never inspects CSS text. Layout
 * emits a flat display list in document order; paint slices it per page.
 */
#ifndef PROWSETK_PAGE2PDF_INTERNAL_H
#define PROWSETK_PAGE2PDF_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphics.h"

/* ------------------------------------------------------------------ limits */

#define P2P_MAX_NODES 200000U
#define P2P_MAX_ITEMS 600000U
#define P2P_MAX_RULES 40000U
#define P2P_MAX_SELECTOR_BYTES 1024U
#define P2P_MAX_DECLARATION_BYTES 65536U
#define P2P_MAX_FACES 256U
#define P2P_MAX_FONT_BYTES (48U * 1024U * 1024U)
#define P2P_MAX_FONT_FILE_BYTES (24U * 1024U * 1024U)
#define P2P_MAX_FALLBACK_DEPTH 6
#define P2P_MAX_INLINE_NESTING 64U
#define P2P_MAX_TABLE_DEPTH 16U
#define P2P_MAX_GRID_TRACKS 64U
#define P2P_MAX_FLEX_LINES 64U
#define P2P_MAX_GLYPHS_PER_RUN 4096U
#define P2P_MAX_MARKERS 200000U
#define P2P_MAX_NAMED_COLORS 256

/* ------------------------------------------------------------------ memory */

void *p2p_alloc(size_t size);
void *p2p_calloc(size_t count, size_t size);
void *p2p_realloc(void *pointer, size_t size);
char *p2p_strdup(const unsigned char *bytes, size_t length);
char *p2p_strdup_c(const char *text);
/* printf into a fresh allocation. */
char *p2p_format(const char *format, ...);
bool p2p_oom(void);
void p2p_report_oom(void);

/* ------------------------------------------------------------------- units */

typedef enum {
    UNIT_AUTO = 0,
    UNIT_NUMBER,
    UNIT_PX,
    UNIT_PT,
    UNIT_PC,
    UNIT_IN,
    UNIT_CM,
    UNIT_MM,
    UNIT_Q,
    UNIT_EM,
    UNIT_REM,
    UNIT_EX,
    UNIT_CH,
    UNIT_PCT,
    UNIT_VW,
    UNIT_VH,
    UNIT_VMIN,
    UNIT_VMAX
} Unit;

typedef struct {
    double number;
    signed char unit;
} Value;

Value value_make(double number, Unit unit);
Value value_auto(void);
Value value_number(double number);
bool value_is_auto(Value value);
bool value_is_length(Value value);
bool value_is_number(Value value);
bool value_is_percentage(Value value);
bool value_is_zero(Value value);
double value_resolve_length(Value value, double reference, double font_size,
                            double root_font_size, double viewport_width,
                            double viewport_height, bool zero_for_auto);

/* ------------------------------------------------------------------ colors */

typedef struct {
    float r, g, b, a;
} Color;

#define COLOR_TRANSPARENT ((Color){0.0f, 0.0f, 0.0f, 0.0f})
#define COLOR_BLACK ((Color){0.0f, 0.0f, 0.0f, 1.0f})
#define COLOR_WHITE ((Color){1.0f, 1.0f, 1.0f, 1.0f})

/* Parses a CSS color. `current` resolves the `currentColor` keyword. */
bool color_parse(const char *value, Color current, Color *out);
const char *color_name_lookup(const char *name, Color *out);

/* ------------------------------------------------------------------- enums */

typedef enum {
    DISPLAY_NONE = 0,
    DISPLAY_BLOCK,
    DISPLAY_INLINE,
    DISPLAY_INLINE_BLOCK,
    DISPLAY_FLEX,
    DISPLAY_INLINE_FLEX,
    DISPLAY_GRID,
    DISPLAY_INLINE_GRID,
    DISPLAY_TABLE,
    DISPLAY_INLINE_TABLE,
    DISPLAY_TABLE_ROW_GROUP,
    DISPLAY_TABLE_HEADER_GROUP,
    DISPLAY_TABLE_ROW,
    DISPLAY_TABLE_CELL,
    DISPLAY_TABLE_COLUMN_GROUP,
    DISPLAY_TABLE_COLUMN,
    DISPLAY_TABLE_CAPTION,
    DISPLAY_LIST_ITEM,
    DISPLAY_CONTENTS
} Display;

typedef enum {
    ALIGN_AUTO = 0,
    ALIGN_LEFT,
    ALIGN_RIGHT,
    ALIGN_CENTER,
    ALIGN_JUSTIFY,
    ALIGN_START,
    ALIGN_END
} TextAlign;

typedef enum {
    VA_BASELINE = 0,
    VA_TOP,
    VA_MIDDLE,
    VA_BOTTOM,
    VA_SUB,
    VA_SUPER,
    VA_TEXT_TOP,
    VA_TEXT_BOTTOM
} VerticalAlign;

typedef enum {
    POS_STATIC = 0,
    POS_RELATIVE,
    POS_ABSOLUTE,
    POS_FIXED,
    POS_STICKY
} Position;

typedef enum {
    FLOAT_NONE = 0,
    FLOAT_LEFT,
    FLOAT_RIGHT
} FloatSide;

typedef enum {
    CLEAR_NONE = 0,
    CLEAR_LEFT,
    CLEAR_RIGHT,
    CLEAR_BOTH
} ClearSide;

typedef enum {
    OVERFLOW_VISIBLE = 0,
    OVERFLOW_HIDDEN,
    OVERFLOW_SCROLL,
    OVERFLOW_AUTO
} Overflow;

typedef enum {
    BORDER_NONE = 0,
    BORDER_SOLID,
    BORDER_DASHED,
    BORDER_DOTTED,
    BORDER_DOUBLE,
    BORDER_GROOVE,
    BORDER_RIDGE,
    BORDER_INSET,
    BORDER_OUTSET
} BorderStyle;

typedef enum {
    LIST_NONE = 0,
    LIST_DISC,
    LIST_CIRCLE,
    LIST_SQUARE,
    LIST_DECIMAL,
    LIST_DECIMAL_LEADING_ZERO,
    LIST_LOWER_ALPHA,
    LIST_UPPER_ALPHA,
    LIST_LOWER_ROMAN,
    LIST_UPPER_ROMAN,
    LIST_LOWER_GREEK,
    LIST_ARABIC_INDIAN,
    LIST_HEBREW,
    LIST_CJK_DECIMAL,
    LIST_CJK_CIRCLE
} ListStyle;

typedef enum {
    LIST_POSITION_OUTSIDE = 0,
    LIST_POSITION_INSIDE
} ListPosition;

typedef enum {
    WS_NORMAL = 0,
    WS_NOWRAP,
    WS_PRE,
    WS_PRE_WRAP,
    WS_PRE_LINE
} WhiteSpace;

typedef enum {
    TRANSFORM_NONE = 0,
    TRANSFORM_UPPERCASE,
    TRANSFORM_LOWERCASE,
    TRANSFORM_CAPITALIZE
} TextTransform;

typedef enum {
    OBJECT_FIT_FILL = 0,
    OBJECT_FIT_CONTAIN,
    OBJECT_FIT_COVER,
    OBJECT_FIT_NONE,
    OBJECT_FIT_SCALE_DOWN
} ObjectFit;

typedef enum {
    FLEX_ROW = 0,
    FLEX_ROW_REVERSE,
    FLEX_COLUMN,
    FLEX_COLUMN_REVERSE
} FlexDirection;

typedef enum {
    FLEX_WRAP_NOWRAP = 0,
    FLEX_WRAP_WRAP,
    FLEX_WRAP_WRAP_REVERSE
} FlexWrap;

typedef enum {
    FLEX_AUTO = 0,
    FLEX_START,
    FLEX_END,
    FLEX_CENTER,
    FLEX_SPACE_BETWEEN,
    FLEX_SPACE_AROUND,
    FLEX_SPACE_EVENLY,
    FLEX_STRETCH,
    FLEX_BASELINE
} FlexAlign;

typedef enum {
    BREAK_INSIDE_AUTO = 0,
    BREAK_INSIDE_AVOID,
    BREAK_INSIDE_FORBIDDEN
} BreakInside;

typedef enum {
    BREAK_AUTO = 0,
    BREAK_AVOID,
    BREAK_ALWAYS,
    BREAK_LEFT,
    BREAK_RIGHT,
    BREAK_PAGE
} BreakMode;

typedef enum {
    FONT_STYLE_NORMAL = 0,
    FONT_STYLE_ITALIC,
    FONT_STYLE_OBLIQUE
} FontStyle;

typedef enum {
    TABLE_AUTO = 0,
    TABLE_FIXED
} TableLayout;

typedef enum {
    BORDER_COLLAPSE_SEPARATE = 0,
    BORDER_COLLAPSE_COLLAPSE
} BorderCollapse;

#define DECO_UNDERLINE 0x1u
#define DECO_OVERLINE 0x2u
#define DECO_LINE_THROUGH 0x4u

/* Side order is always top, right, bottom, left. Corner order is
 * top-left, top-right, bottom-right, bottom-left. */
enum { SIDE_TOP = 0, SIDE_RIGHT = 1, SIDE_BOTTOM = 2, SIDE_LEFT = 3 };
enum { CORNER_TOP_LEFT = 0, CORNER_TOP_RIGHT = 1, CORNER_BOTTOM_RIGHT = 2,
       CORNER_BOTTOM_LEFT = 3 };

/* ------------------------------------------------------------------- style */

typedef struct {
    Display display;
    Position position;
    FloatSide floats;
    ClearSide clear;
    Overflow overflow;
    Overflow overflow_x;
    Overflow overflow_y;

    Value width, height;
    Value min_width, max_width, min_height, max_height;
    Value margin[4];
    Value padding[4];
    Value border_width[4];
    Color border_color[4];
    BorderStyle border_style[4];
    bool border_width_set[4];
    float border_radius[4];
    bool border_radius_set;
    bool box_sizing_border_box;

    Color foreground;
    Color background;
    bool background_set;
    char *background_image;      /* url() or gradient() token, or NULL */
    int background_repeat;       /* 0 repeat, 1 repeat-x, 2 repeat-y, 3 none */
    char *background_position;   /* raw token, resolved by paint */
    char *background_size;       /* raw token, resolved by paint */
    float opacity;

    Value font_size;
    int font_weight;
    FontStyle font_style;
    char *font_family;           /* owned, comma separated, as written */
    Value line_height;
    Unit line_height_unit;
    Value letter_spacing;
    Value word_spacing;

    TextAlign text_align;
    TextAlign text_align_last;
    Value text_indent;
    unsigned decoration;
    Color decoration_color;
    bool decoration_color_set;
    TextTransform text_transform;
    WhiteSpace white_space;
    Value word_break;
    Value overflow_wrap;
    VerticalAlign vertical_align;
    Value tab_size;
    ObjectFit object_fit;

    ListStyle list_style_type;
    ListPosition list_position;
    char *list_marker_text;      /* custom content, or NULL */

    TableLayout table_layout;
    BorderCollapse border_collapse;
    bool border_spacing_set;
    float border_spacing[2];
    Value caption_side;

    FlexDirection flex_direction;
    FlexWrap flex_wrap;
    FlexAlign justify_content;
    FlexAlign align_items;
    FlexAlign align_self;
    FlexAlign align_content;
    Value row_gap, column_gap;
    double flex_grow, flex_shrink;
    Value flex_basis;
    int order;

    Value grid_template_columns;
    Value grid_template_rows;
    Value grid_auto_columns;
    Value grid_auto_rows;
    Value grid_column_gap, grid_row_gap;
    int grid_auto_flow;           /* 0 row, 1 column, 2 dense */
    int grid_column_start, grid_row_start;
    int grid_column_span, grid_row_span;
    char *grid_column_rules[4];   /* grid-template-columns tokens */
    char *grid_row_rules[4];
    size_t grid_column_count, grid_row_count;

    int column_count;
    Value column_width;
    Value column_gap;
    bool column_rule_set;
    Color column_rule_color;
    float column_rule_width;

    BreakInside break_inside;
    BreakMode break_before;
    BreakMode break_after;
    int orphans;
    int widows;
    int page_break_inside;
    bool keep_with_next;

    int z_index;
    bool visibility_collapse;
} Style;

/* -------------------------------------------------------------------- node */

typedef struct GraphicAttribute {
    char *name;
    char *value;
    struct GraphicAttribute *next;
} GraphicAttribute;

/* Anonymous wrappers created by layout, not present in the IR. */
#define TAG_TEXT "#text"
#define TAG_ANON "::anonymous"
#define TAG_ANON_BLOCK "::anonymous-block"
#define TAG_ROOT "#document"
#define TAG_PSEUDO "::marker"

struct GraphicNode {
    char *tag;
    char *text;                  /* text payload, or <style>/<script> content */
    struct GraphicNode *parent;
    GraphicAttribute *attributes;
    struct GraphicNode *children, *last_child, *next;
    struct GraphicNode *prev;
    Style style;
    bool style_computed;
    bool is_text;
    bool generated;              /* ::before / ::after; style is pre-computed */
    /* Anonymous wrapper bookkeeping. */
    bool owns_children_box;      /* anonymous blocks own a real block box */
    /* Layout results, in document coordinates. */
    float x, y, width, height;
    float content_x, content_y, content_width, content_height;
    /* Intrinsic sizing cache, filled by the intrinsic pass. */
    float min_content_width, max_content_width;
    float min_content_height, max_content_height;
    /* Diagnostics surfaced by --inspect. */
    unsigned depth;
};

GraphicNode *node_find_attribute(const GraphicNode *node, const char *name);
const char *node_attribute(const GraphicNode *node, const char *name);
bool node_has_attribute(const GraphicNode *node, const char *name);
size_t node_index_among_siblings(const GraphicNode *node);
size_t node_type_index_among_siblings(const GraphicNode *node);
size_t node_type_sibling_count(const GraphicNode *node);
bool node_is_form_control(const GraphicNode *node);
const char *node_form_control_kind(const GraphicNode *node);
/* Ordinal used by :nth-child() over element siblings. 1-based. */
size_t node_element_ordinal(const GraphicNode *node);
size_t node_element_sibling_count(const GraphicNode *node);
/* Index among same-tag element siblings, 1-based. */
size_t node_tag_ordinal(const GraphicNode *node);
bool node_is_root(const GraphicNode *node);

/* --------------------------------------------------------------------- css */

/* Combinator that joins two compound selectors. */
typedef enum {
    COMB_DESCENDANT = 0,
    COMB_CHILD,
    COMB_NEXT_SIBLING,
    COMB_LATER_SIBLING
} Combinator;

typedef enum {
    SIMPLE_TYPE = 0,
    SIMPLE_UNIVERSAL,
    SIMPLE_ID,
    SIMPLE_CLASS,
    SIMPLE_ATTRIBUTE,
    SIMPLE_PSEUDO_CLASS,
    SIMPLE_PSEUDO_ELEMENT
} SimpleKind;

/* Attribute match operators. `ATTR_OP_EXISTS` needs no value. */
typedef enum {
    ATTR_OP_EXISTS = 0,
    ATTR_OP_EQUALS,
    ATTR_OP_INCLUDES,
    ATTR_OP_DASH_MATCH,
    ATTR_OP_PREFIX,
    ATTR_OP_SUFFIX,
    ATTR_OP_SUBSTRING
} AttributeOperator;

typedef struct {
    SimpleKind kind;
    char *name;                  /* type/id/class/attribute/pseudo name */
    char *value;                 /* attribute value or pseudo argument */
    AttributeOperator op;
} SimpleSelector;

typedef struct {
    SimpleSelector *simples;
    size_t count;
    /* How this compound relates to the one on its right. */
    Combinator combinator;
} CompoundSelector;

/* One comma-separated alternative of a rule's selector list. */
typedef struct {
    CompoundSelector *compound_list;
    size_t compounds;
    unsigned specificity_a, specificity_b, specificity_c;
    /* Bucket keys derived from the rightmost compound. Prevents testing every
     * rule against every node, which is what keeps the cascade affordable. */
    unsigned char key_kind[4];
    char *key_text[4];
    size_t key_count;
    /* Non-NULL for rules that target generated content. The pseudo-element is
     * lifted out of the compound, so the chain matches the *subject* element. */
    char *pseudo_element;
} SelectorAlternative;

typedef struct {
    SelectorAlternative *selectors;
    size_t count_selectors;
    char *declarations;          /* normal-weight declarations */
    char *declarations_important; /* !important declarations */
    unsigned order;
} CssRule;

typedef struct {
    CssRule *items;
    size_t count;
    size_t capacity;
} CssRuleList;

typedef struct {
    char *family;
    Value weight;
    FontStyle style;
    char *source;                /* data: URL or file path from src: */
} CssFontFace;

/* Viewport and root context handed to the cascade. */
typedef struct {
    double root_font_size;
    double viewport_width;
    double viewport_height;
    PageGeometry geometry;
    bool geometry_set;
} CssContext;

/* Computes the style for every node in the tree, in place, and materializes
 * `::before` / `::after` generated content as real nodes. */
bool css_compute_tree(GraphicNode *root, Renderer *renderer);
/* Parses a declaration block into a style, resolving relative units against
 * `context` and the supplied `font_size`. */
void css_apply_declarations(Style *style, const char *declarations,
                            const CssContext *context, double font_size);
/* Applies a declaration block in cascade order. `*font_size` is the running
 * font size: `font-size` and `font` declarations inside the block update it,
 * and `em` units in later declarations resolve against the updated value. */
void css_apply_cascade_block(Style *style, const char *declarations,
                             const CssContext *context, double *font_size);
/* Splits a declaration block into its normal and `!important` halves. Either
 * output may be NULL. Both are heap allocated. */
void css_split_important(const char *declarations, char **normal,
                         char **important);
/* Sets every property to its initial value. */
void css_style_initial(Style *style);
/* Frees every heap-owned string inside a style. Does not free the style. */
void style_release(Style *style);
/* Copies inheritable properties from `parent` into `style`. */
void css_inherit(Style *style, const Style *parent);
/* Resolves a single CSS length token in a context. Returns false when the
 * token is not a length. */
bool css_parse_length(const char *text, const CssContext *context,
                      double font_size, Value *out);
/* True when a comma-separated selector list matches `node`. Used for the
 * argument of :not() / :is() / :where() / :has(). */
bool css_selector_matches(const SelectorAlternative *alternative,
                          const GraphicNode *node);

/* ------------------------------------------------------------------- fonts */

typedef struct FontFace FontFace;

/* A view into one table of a font's owned bytes. */
typedef struct {
    const unsigned char *data;
    size_t length;
} TtfTable;

/* Parsed sfnt tables. `cmap` and `hmtx` own one heap block each; every other
 * table points into the face's own copy of the file bytes. */
typedef struct {
    TtfTable *cmap;
    TtfTable *hmtx;
    TtfTable loca;
    TtfTable head;
    TtfTable hhea;
    TtfTable glyf;
    uint16_t num_glyphs;
    int16_t index_to_loc_format;
    uint16_t units_per_em;
    uint32_t cmap_subtable_offset;
    uint16_t cmap_subtable_format;
    int num_h_metrics;
    uint16_t num_loca;
} TtfFont;

/* Vertical and horizontal metrics, normalized so one em is 1.0. */
typedef struct {
    uint16_t units_per_em;
    double ascender;
    double descender;    /* negative, below the baseline */
    double line_gap;
    int16_t x_min, y_min, x_max, y_max;
    uint16_t weight_class;
    bool italic;
} FontHeader;

struct FontFace {
    char *family;                /* lowercased family name */
    char *subfamily;             /* lowercased subfamily, may be "" */
    int weight;                  /* 100..900 */
    FontStyle style;
    bool monospace;
    bool is_base14;              /* one of the standard PDF fonts */
    bool synthetic_bold;         /* requested bolder than the face provides */
    unsigned char *data;         /* owned file bytes, NULL for base-14 */
    size_t data_length;
    char *label;                 /* origin, reported by --inspect */
    TtfFont ttf;                 /* parsed views into `data` */
    bool ttf_ready;
    FontHeader header;
    HPDF_Font hpdf;              /* loaded lazily by fonts_face_handle */
    struct FontRegistry *owner;
};

/* Font file parsing. `ttf_parse` fills `out`; release it with ttf_release. */
bool ttf_parse(const unsigned char *data, size_t length, TtfFont *out,
               FontHeader *header);
void ttf_release(TtfFont *font);
uint16_t ttf_glyph_index(const TtfFont *font, uint32_t codepoint);
uint16_t ttf_advance(const TtfFont *font, uint16_t glyph);
uint16_t ttf_num_glyphs(const TtfFont *font);
/* Union of the glyph's ink box, scaled to em units. */
bool ttf_glyph_bbox(const TtfFont *font, uint16_t glyph, double scale,
                    double *x0, double *y0, double *x1, double *y1);

FontRegistry *fonts_create(HPDF_Doc document, bool embed);
void fonts_destroy(FontRegistry *registry);
/* Registers a `data:` URL or a file path as the source for a family. */
bool fonts_register_source(FontRegistry *registry, const char *family,
                           const char *source, const char *label);
/* Adds the base-14 faces and scans the system font directories. */
void fonts_scan_system(FontRegistry *registry, const char *font_dir);
/* Resolves a font stack. Never returns NULL. */
const FontFace *fonts_resolve(FontRegistry *registry, const Style *style,
                              bool bold_needed, bool italic_needed);
/* Per-glyph fallback: resolves `codepoint` in `face`, or in another face that
 * covers it. Never returns NULL. */
const FontFace *fonts_resolve_glyph(FontRegistry *registry, const Style *style,
                                    const FontFace *face, uint32_t codepoint,
                                    bool *synthetic_bold);
size_t fonts_face_count(const FontRegistry *registry);
const FontFace *fonts_face_at(const FontRegistry *registry, size_t index);
/* Lazily loads and caches the libHaru handle for a face. */
HPDF_Font fonts_face_handle(FontFace *face);
/* Advance width of `codepoint` in `face`, in em units. */
double fonts_advance(const FontFace *face, uint32_t codepoint);
/* True when `face` has a glyph for `codepoint`. */
bool fonts_covers(const FontFace *face, uint32_t codepoint);
/* Vertical metrics for `face` at `size`, in px: ascent above the baseline,
 * descent below it, and the natural line height. */
void fonts_face_metrics(const FontFace *face, double size, double *ascent,
                        double *descent, double *line_height);

/* -------------------------------------------------------------------- text */

typedef struct {
    uint32_t codepoint;
    uint16_t glyph;
    const FontFace *face;
    bool synthetic_bold;
    bool synthetic_italic;
    double advance;              /* px, at the shape size */
    double offset_x;             /* px, mark placement */
    double offset_y;             /* px, positive is up */
    bool is_space;
    bool is_break;               /* a soft wrap opportunity sits before this */
} ShapedGlyph;

typedef struct {
    ShapedGlyph *glyphs;
    size_t count;
    size_t capacity;
    double width;
} GlyphRun;

/* UTF-8 decoding. Returns bytes consumed, or 0 on error. */
size_t utf8_decode(const unsigned char *text, size_t length, uint32_t *out);
size_t utf8_encode(uint32_t codepoint, char *out);
bool utf8_is_continuation(unsigned char byte);
/* True for a codepoint a font is expected to render as nothing. */
bool unicode_is_mark(uint32_t codepoint);
bool unicode_is_space(uint32_t codepoint);
bool unicode_is_format(uint32_t codepoint);
bool unicode_is_wide(uint32_t codepoint);
bool unicode_line_break_after(uint32_t codepoint);
bool unicode_breaks_at_space(uint32_t codepoint);

/* Shaping entry point: fills `run` with glyphs for `text` in `style`. */
bool text_shape(FontRegistry *registry, const Style *style, double font_size,
                const char *text, size_t length, GlyphRun *run,
                double letter_spacing, double word_spacing);
void glyph_run_clear(GlyphRun *run);
double glyph_run_width(const GlyphRun *run);
/* Applies text-transform and collapses whitespace per white-space. */
char *text_transform(const char *text, TextTransform transform, size_t *length);
/* Bi-directional resolution. Returns the visual order level per codepoint. */
bool text_bidi_levels(const char *text, size_t length, uint8_t *levels,
                      size_t *run_count);

/* ------------------------------------------------------------------ layout */

typedef enum {
    ITEM_BOX = 0,
    ITEM_TEXT,
    ITEM_IMAGE,
    ITEM_MARKER,
    ITEM_RULE
} ItemKind;

typedef struct {
    float x, y, width, height;
} Rect;

/* One shaped character on its way to the page. The codepoint is carried rather
 * than a local glyph index so that libHaru does the final mapping through the
 * face's own cmap; the advance is our own measurement, which is what keeps line
 * breaking and Haru's own metrics in agreement. */
typedef struct GlyphDraw {
    uint32_t codepoint;
    const FontFace *face;
    bool synthetic_bold;
    double advance;      /* measured advance, px, at the shape size */
    double shift_x;      /* extra dx in px, for mark positioning */
    double shift_y;      /* extra dy in px, positive is up */
} GlyphDraw;

typedef struct Item {
    ItemKind kind;
    size_t page;                 /* 0-based page index */
    Rect rect;                   /* document coordinates */
    Style style;                 /* copy; the node owning it may be freed */
    /* ITEM_BOX / ITEM_MARKER / ITEM_RULE */
    float border_width[4];
    BorderStyle border_style[4];
    Color border_color[4];
    float border_radius[4];
    /* ITEM_TEXT */
    GlyphDraw *glyphs;
    size_t glyph_count;
    double font_size;
    double baseline;             /* distance from rect.y to the baseline */
    Color text_color;
    char *text;                  /* owned; the source text, for --inspect */
    /* ITEM_IMAGE */
    unsigned char *image_data;   /* owned decoded bytes, or NULL */
    size_t image_length;
    bool image_png;
    bool draw_placeholder;       /* paint a broken/alt box instead of bytes */
    char *alt_text;              /* owned */
    /* link annotation */
    char *link_uri;              /* owned */
    char *link_title;            /* owned */
} Item;

typedef struct {
    Item *items;
    size_t count;
    size_t capacity;
    Item *out_of_flow;
    size_t out_of_flow_count;
    size_t out_of_flow_capacity;
    int page_count;
    float content_width;
    float content_height;
    float document_height;
    char *title;
} DisplayList;

/* Runs the cascade then produces the display list. */
bool layout_document(Renderer *renderer, GraphicNode *root, DisplayList *list);
void display_list_clear(DisplayList *list);

/* ------------------------------------------------------------------- paint */

bool paint_display_list(Renderer *renderer, const DisplayList *list);

/* --------------------------------------------------------------- utilities */

/* Case-insensitive ASCII compare. */
bool p2p_iequals(const char *a, const char *b);
bool p2p_starts_with(const char *text, const char *prefix);
bool p2p_ends_with(const char *text, const char *suffix);
/* Case-insensitive substring search. */
const char *p2p_istrstr(const char *haystack, const char *needle);
/* Skips leading ASCII whitespace. */
char *p2p_skip_space(char *text);
char *p2p_trim(char *text);
void p2p_lower(char *text);
float p2p_clampf(float value, float low, float high);
double p2p_clamp(double value, double low, double high);

#endif /* PROWSETK_PAGE2PDF_INTERNAL_H */
