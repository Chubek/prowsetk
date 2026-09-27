/* A bounded, layout-before-paint PDF renderer for serialized page IR.
 * This is a small print layout engine, not a browser layout implementation. */
#include "graphics.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_WIDTH 612.0f
#define PAGE_HEIGHT 792.0f
#define PAGE_MARGIN 36.0f
#define MAX_NODES 100000U
#define MAX_RUNS 200000U
#define MAX_RULES 256U

typedef struct GraphicAttribute {
    char *name;
    char *value;
    struct GraphicAttribute *next;
} GraphicAttribute;

typedef struct {
    float r, g, b;
} Color;

typedef struct {
    Color foreground;
    Color background;
    Color border_color;
    float font_size;
    float padding;
    float margin;
    float border;
    float width;
    float height;
    float min_height;
    bool has_background;
    bool has_width;
    bool has_height;
    bool hidden;
    bool block;
    bool bold;
    bool mono;
    bool center;
} Style;

typedef struct GraphicRun {
    struct GraphicRun *next;
    struct GraphicRun *line_next;
    char text[256];
    float x, top, size;
    Color color;
    bool bold, mono;
} GraphicRun;

struct GraphicNode {
    char *tag;
    char *text;
    struct GraphicNode *parent;
    GraphicAttribute *attributes;
    GraphicNode *children, *last_child, *next;
    GraphicRun *runs, *last_run;
    Style style;
    float x, top, width, height;
};

typedef struct {
    char selector[80];
    char *declarations;
} Rule;

typedef struct {
    float x, width, y, line_x, line_top, line_height;
    GraphicRun *line_first, *line_last;
    bool center;
} Flow;

static char *copy_text(const unsigned char *value, size_t length) {
    char *copy;
    if (length > 16U * 1024U * 1024U) return NULL;
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) {
        if (length != 0U) memcpy(copy, value, length);
        copy[length] = '\0';
    }
    return copy;
}

static GraphicNode *new_node(Renderer *renderer, const char *tag,
                             const unsigned char *text, size_t length) {
    GraphicNode *node;
    if (renderer->node_count >= MAX_NODES) return NULL;
    node = (GraphicNode *)calloc(1U, sizeof(*node));
    if (node == NULL) return NULL;
    node->tag = copy_text((const unsigned char *)tag, strlen(tag));
    if (text != NULL) node->text = copy_text(text, length);
    if (node->tag == NULL || (text != NULL && node->text == NULL)) {
        free(node->tag);
        free(node->text);
        free(node);
        return NULL;
    }
    ++renderer->node_count;
    return node;
}

static void append_child(GraphicNode *parent, GraphicNode *child) {
    child->parent = parent;
    if (parent->last_child != NULL) parent->last_child->next = child;
    else parent->children = child;
    parent->last_child = child;
}

bool renderer_start(Renderer *renderer, const char *tag, size_t depth) {
    GraphicNode *node;
    if (depth != renderer->frame_count || depth >= PAGE2PDF_MAX_NESTING) return false;
    node = new_node(renderer, tag, NULL, 0U);
    if (node == NULL) return false;
    append_child(depth == 0U ? renderer->root : renderer->frames[depth - 1U].node,
                 node);
    memcpy(renderer->frames[depth].tag, tag, strlen(tag) + 1U);
    renderer->frames[depth].node = node;
    ++renderer->frame_count;
    return true;
}

bool renderer_attribute(Renderer *renderer, const char *name,
                        const unsigned char *value, size_t length, size_t depth) {
    GraphicAttribute *attr;
    GraphicNode *node;
    if (renderer->frame_count == 0U || depth + 1U != renderer->frame_count)
        return false;
    node = renderer->frames[depth].node;
    attr = (GraphicAttribute *)calloc(1U, sizeof(*attr));
    if (attr == NULL) return false;
    attr->name = copy_text((const unsigned char *)name, strlen(name));
    attr->value = copy_text(value, length);
    if (attr->name == NULL || attr->value == NULL) {
        free(attr->name);
        free(attr->value);
        free(attr);
        return false;
    }
    attr->next = node->attributes;
    node->attributes = attr;
    return true;
}

bool renderer_text(Renderer *renderer, const unsigned char *text,
                   size_t length, size_t depth, const char *tag) {
    GraphicNode *node;
    GraphicNode *parent;
    if (depth != renderer->frame_count ||
        (tag != NULL && (depth == 0U ||
         strcmp(tag, renderer->frames[depth - 1U].tag) != 0))) return false;
    if (length >= 3U && text[0] == 0xefU && text[1] == 0xbbU &&
        text[2] == 0xbfU) {
        text += 3U;
        length -= 3U;
    }
    node = new_node(renderer, "#text", text, length);
    if (node == NULL) return false;
    parent = depth == 0U ? renderer->root : renderer->frames[depth - 1U].node;
    append_child(parent, node);
    return true;
}

bool renderer_end(Renderer *renderer, const char *tag, size_t depth) {
    if (renderer->frame_count == 0U || depth + 1U != renderer->frame_count ||
        strcmp(tag, renderer->frames[depth].tag) != 0) return false;
    --renderer->frame_count;
    return true;
}

static void free_tree(GraphicNode *node) {
    while (node != NULL) {
        GraphicNode *next = node->next;
        GraphicAttribute *attr = node->attributes;
        GraphicRun *run = node->runs;
        free_tree(node->children);
        while (attr != NULL) {
            GraphicAttribute *after = attr->next;
            free(attr->name);
            free(attr->value);
            free(attr);
            attr = after;
        }
        while (run != NULL) {
            GraphicRun *after = run->next;
            free(run);
            run = after;
        }
        free(node->tag);
        free(node->text);
        free(node);
        node = next;
    }
}

static void hpdf_error_handler(HPDF_STATUS error, HPDF_STATUS detail,
                               void *user_data) {
    Renderer *renderer = (Renderer *)user_data;
    if (renderer != NULL && renderer->error == HPDF_OK) {
        renderer->error = error;
        renderer->detail = detail;
    }
}

static HPDF_Page page_at(Renderer *renderer, size_t index) {
    if (index >= PAGE2PDF_MAX_PAGES) return NULL;
    while (renderer->page_count <= index) {
        HPDF_Page page = HPDF_AddPage(renderer->document);
        if (page == NULL || HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER,
                                               HPDF_PAGE_PORTRAIT) != HPDF_OK)
            return NULL;
        renderer->pages[renderer->page_count++] = page;
    }
    return renderer->pages[index];
}

bool renderer_init(Renderer *renderer) {
    memset(renderer, 0, sizeof(*renderer));
    renderer->error = HPDF_OK;
    renderer->document = HPDF_New(hpdf_error_handler, renderer);
    if (renderer->document == NULL) return false;
    HPDF_SetCompressionMode(renderer->document, HPDF_COMP_ALL);
    renderer->fallback_regular = HPDF_GetFont(renderer->document, "Helvetica", NULL);
    renderer->fallback_bold = HPDF_GetFont(renderer->document, "Helvetica-Bold", NULL);
    renderer->fallback_mono = HPDF_GetFont(renderer->document, "Courier", NULL);
    (void)page_at(renderer, 0U);
    renderer->root = new_node(renderer, "#document", NULL, 0U);
    return renderer->fallback_regular != NULL && renderer->fallback_bold != NULL &&
           renderer->fallback_mono != NULL && renderer->page_count != 0U &&
           renderer->root != NULL && renderer->error == HPDF_OK;
}

void renderer_destroy(Renderer *renderer) {
    free_tree(renderer->root);
    renderer->root = NULL;
    if (renderer->document != NULL) HPDF_Free(renderer->document);
    renderer->document = NULL;
}

static const char *attribute(const GraphicNode *node, const char *name) {
    const GraphicAttribute *attr;
    for (attr = node->attributes; attr != NULL; attr = attr->next) {
        if (strcmp(attr->name, name) == 0) return attr->value;
    }
    return NULL;
}

static char *trim(char *text) {
    char *end;
    while (isspace((unsigned char)*text)) ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static bool css_color(const char *value, Color *out) {
    unsigned int r, g, b;
    char trailing;
    if (value[0] == '#' && strlen(value) == 7U &&
        sscanf(value + 1, "%2x%2x%2x%c", &r, &g, &b, &trailing) == 3) {
        *out = (Color){r / 255.0f, g / 255.0f, b / 255.0f};
        return true;
    }
    if (value[0] == '#' && strlen(value) == 4U &&
        sscanf(value + 1, "%1x%1x%1x%c", &r, &g, &b, &trailing) == 3) {
        *out = (Color){r / 15.0f, g / 15.0f, b / 15.0f};
        return true;
    }
    if (sscanf(value, "rgb( %u , %u , %u ) %c", &r, &g, &b, &trailing) == 3 &&
        r <= 255U && g <= 255U && b <= 255U) {
        *out = (Color){r / 255.0f, g / 255.0f, b / 255.0f};
        return true;
    }
    if (strcmp(value, "white") == 0) *out = (Color){1, 1, 1};
    else if (strcmp(value, "black") == 0) *out = (Color){0, 0, 0};
    else if (strcmp(value, "red") == 0) *out = (Color){1, 0, 0};
    else if (strcmp(value, "blue") == 0) *out = (Color){0, 0, 1};
    else if (strcmp(value, "green") == 0) *out = (Color){0, .5f, 0};
    else if (strcmp(value, "gray") == 0 || strcmp(value, "grey") == 0)
        *out = (Color){.5f, .5f, .5f};
    else if (strcmp(value, "navy") == 0) *out = (Color){0, 0, .5f};
    else return false;
    return true;
}

static float css_length(const char *value, float relative, float fallback) {
    char *end;
    float n = strtof(value, &end);
    if (end == value || n < 0.0f || n > 10000.0f) return fallback;
    while (isspace((unsigned char)*end)) ++end;
    if (*end == '%' && end[1] == '\0') return relative * n / 100.0f;
    if (strcmp(end, "pt") == 0) return n;
    if (*end == '\0' || strcmp(end, "px") == 0) return n * .75f;
    return fallback;
}

static void declarations(Style *style, const char *input, float relative) {
    char *buffer, *part;
    if (input == NULL || strlen(input) > 256U * 1024U) return;
    buffer = copy_text((const unsigned char *)input, strlen(input));
    if (buffer == NULL) return;
    part = buffer;
    while (part != NULL && *part != '\0') {
        char *next = strchr(part, ';');
        char *colon;
        char *key, *value;
        Color color;
        if (next != NULL) *next++ = '\0';
        colon = strchr(part, ':');
        if (colon == NULL) { part = next; continue; }
        *colon++ = '\0';
        key = trim(part);
        value = trim(colon);
        if (strcmp(key, "color") == 0 && css_color(value, &color))
            style->foreground = color;
        else if ((strcmp(key, "background") == 0 ||
                  strcmp(key, "background-color") == 0) &&
                 css_color(value, &color)) {
            style->background = color;
            style->has_background = true;
        } else if (strcmp(key, "border-color") == 0 && css_color(value, &color))
            style->border_color = color;
        else if (strcmp(key, "font-size") == 0)
            style->font_size = css_length(value, style->font_size, style->font_size);
        else if (strcmp(key, "font-weight") == 0)
            style->bold = strcmp(value, "bold") == 0 || strcmp(value, "700") == 0 ||
                          strcmp(value, "600") == 0;
        else if (strcmp(key, "font-family") == 0)
            style->mono = strstr(value, "mono") != NULL || strstr(value, "Courier") != NULL;
        else if (strcmp(key, "padding") == 0 || strcmp(key, "padding-top") == 0)
            style->padding = css_length(value, relative, style->padding);
        else if (strcmp(key, "margin") == 0 || strcmp(key, "margin-top") == 0)
            style->margin = css_length(value, relative, style->margin);
        else if (strcmp(key, "border-width") == 0)
            style->border = css_length(value, relative, style->border);
        else if (strcmp(key, "border") == 0) {
            style->border = css_length(value, relative, 1.0f);
            if (style->border == 0.0f) style->border = 1.0f;
            if (strstr(value, "red") != NULL) style->border_color = (Color){1, 0, 0};
            else if (strstr(value, "blue") != NULL) style->border_color = (Color){0, 0, 1};
        } else if (strcmp(key, "width") == 0) {
            style->width = css_length(value, relative, relative);
            style->has_width = true;
        } else if (strcmp(key, "height") == 0) {
            style->height = css_length(value, relative, 0);
            style->has_height = true;
        } else if (strcmp(key, "min-height") == 0)
            style->min_height = css_length(value, relative, 0);
        else if (strcmp(key, "display") == 0) {
            style->hidden = strcmp(value, "none") == 0;
            style->block = strcmp(value, "block") == 0 ||
                           strcmp(value, "flex") == 0 || strcmp(value, "grid") == 0;
        } else if (strcmp(key, "text-align") == 0)
            style->center = strcmp(value, "center") == 0;
        part = next;
    }
    free(buffer);
}

static bool block_tag(const char *tag) {
    return strcmp(tag, "html") == 0 || strcmp(tag, "body") == 0 ||
           strcmp(tag, "div") == 0 || strcmp(tag, "p") == 0 ||
           strcmp(tag, "main") == 0 || strcmp(tag, "section") == 0 ||
           strcmp(tag, "article") == 0 || strcmp(tag, "header") == 0 ||
           strcmp(tag, "footer") == 0 || strcmp(tag, "form") == 0 ||
           strcmp(tag, "ul") == 0 || strcmp(tag, "ol") == 0 ||
           strcmp(tag, "li") == 0 || strcmp(tag, "table") == 0 ||
           strcmp(tag, "tr") == 0 || strcmp(tag, "blockquote") == 0 ||
           strcmp(tag, "pre") == 0 || strcmp(tag, "img") == 0 ||
           (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' &&
            tag[2] == '\0');
}

static bool class_contains(const char *classes, const char *name) {
    size_t length = strlen(name);
    while (classes != NULL && *classes != '\0') {
        while (isspace((unsigned char)*classes)) ++classes;
        if (strncmp(classes, name, length) == 0 &&
            (classes[length] == '\0' || isspace((unsigned char)classes[length])))
            return true;
        while (*classes != '\0' && !isspace((unsigned char)*classes)) ++classes;
    }
    return false;
}

static bool matches_compound(const GraphicNode *node, const char *selector) {
    const char *cursor = selector;
    while (*cursor != '\0') {
        const char *end = cursor;
        char name[80];
        size_t length;
        while (*end != '\0' && *end != '#' && *end != '.') ++end;
        if (end == cursor && *cursor != '#' && *cursor != '.') return false;
        if (*cursor == '#' || *cursor == '.') {
            char marker = *cursor++;
            end = cursor;
            while (*end != '\0' && *end != '#' && *end != '.') ++end;
            length = (size_t)(end - cursor);
            if (length == 0U || length >= sizeof(name)) return false;
            memcpy(name, cursor, length);
            name[length] = '\0';
            if (marker == '#') {
                const char *id = attribute(node, "id");
                if (id == NULL || strcmp(id, name) != 0) return false;
            } else if (!class_contains(attribute(node, "class"), name)) return false;
        } else {
            length = (size_t)(end - cursor);
            if ((length != 1U || *cursor != '*') &&
                (strlen(node->tag) != length ||
                 strncmp(cursor, node->tag, length) != 0)) return false;
        }
        cursor = end;
    }
    return true;
}

static bool rule_matches(const GraphicNode *node, const char *selector) {
    char buffer[80];
    const GraphicNode *candidate = node;
    size_t length = strlen(selector);
    if (length == 0U || length >= sizeof(buffer)) return false;
    memcpy(buffer, selector, length + 1U);
    while (length > 0U) {
        char *start = buffer + length;
        while (start > buffer && !isspace((unsigned char)start[-1])) --start;
        if (!matches_compound(candidate, start)) {
            if (candidate == node) return false;
            while ((candidate = candidate->parent) != NULL &&
                   !matches_compound(candidate, start)) {}
            if (candidate == NULL) return false;
        }
        if (start == buffer) return true;
        length = (size_t)(start - buffer);
        while (length > 0U && isspace((unsigned char)buffer[length - 1U]))
            --length;
        buffer[length] = '\0';
        candidate = candidate->parent;
        if (candidate == NULL) return false;
    }
    return true;
}

static size_t collect_rules(GraphicNode *node, Rule *rules, size_t count) {
    GraphicNode *child;
    if (strcmp(node->tag, "style") == 0) {
        for (child = node->children; child != NULL; child = child->next) {
            const char *css = child->text;
            if (css == NULL || strlen(css) > 256U * 1024U) continue;
            while (*css != '\0' && count < MAX_RULES) {
                const char *open = strchr(css, '{');
                const char *close;
                char *selectors, *selector;
                size_t length;
                while (isspace((unsigned char)*css)) ++css;
                if (css[0] == '/' && css[1] == '*') {
                    const char *comment = strstr(css + 2U, "*/");
                    if (comment == NULL) break;
                    css = comment + 2U;
                    continue;
                }
                if (open == NULL) break;
                close = strchr(open + 1, '}');
                if (close == NULL) break;
                length = (size_t)(open - css);
                if (length > 0U && length < 512U &&
                    (selectors = copy_text((const unsigned char *)css, length)) != NULL) {
                    selector = selectors;
                    while (selector != NULL && count < MAX_RULES) {
                        char *next = strchr(selector, ',');
                        const char *clean;
                        if (next != NULL) *next++ = '\0';
                        clean = trim(selector);
                        if (*clean != '@' && *clean != '/' && strlen(clean) < 80U &&
                            strpbrk(clean, ">:+~[") == NULL) {
                            memcpy(rules[count].selector, clean, strlen(clean) + 1U);
                            rules[count].declarations = copy_text(
                                (const unsigned char *)open + 1U,
                                (size_t)(close - open - 1));
                            if (rules[count].declarations != NULL) ++count;
                        }
                        selector = next;
                    }
                    free(selectors);
                }
                css = close + 1;
            }
        }
    }
    for (child = node->children; child != NULL; child = child->next)
        count = collect_rules(child, rules, count);
    return count;
}

static Style compute_style(const GraphicNode *node, const Style *parent,
                           Rule *rules, size_t count, float available) {
    Style style = *parent;
    size_t i;
    const char *attr;
    style.has_background = false;
    style.has_width = false;
    style.has_height = false;
    style.padding = style.margin = style.border = 0.0f;
    style.min_height = 0.0f;
    style.hidden = parent->hidden;
    style.block = block_tag(node->tag);
    style.center = parent->center;
    if (strcmp(node->tag, "head") == 0 || strcmp(node->tag, "style") == 0 ||
        strcmp(node->tag, "script") == 0 || strcmp(node->tag, "title") == 0 ||
        strcmp(node->tag, "meta") == 0 || strcmp(node->tag, "link") == 0)
        style.hidden = true;
    if (strcmp(node->tag, "p") == 0) style.margin = 7.5f;
    if (strcmp(node->tag, "body") == 0) style.padding = 6.0f;
    if (strcmp(node->tag, "h1") == 0) { style.font_size = 24; style.bold = true; style.margin = 12; }
    if (strcmp(node->tag, "h2") == 0) { style.font_size = 19; style.bold = true; style.margin = 10; }
    if (strcmp(node->tag, "h3") == 0) { style.font_size = 15; style.bold = true; style.margin = 8; }
    if (strcmp(node->tag, "strong") == 0 || strcmp(node->tag, "b") == 0)
        style.bold = true;
    if (strcmp(node->tag, "pre") == 0 || strcmp(node->tag, "code") == 0)
        style.mono = true;
    if (strcmp(node->tag, "a") == 0) style.foreground = (Color){0, 0, .6f};
    if (strcmp(node->tag, "blockquote") == 0 || strcmp(node->tag, "ul") == 0 ||
        strcmp(node->tag, "ol") == 0) style.padding = 16;
    for (i = 0; i < count; ++i) {
        if (strchr(rules[i].selector, '#') == NULL &&
            strchr(rules[i].selector, '.') == NULL &&
            rule_matches(node, rules[i].selector))
            declarations(&style, rules[i].declarations, available);
    }
    for (i = 0; i < count; ++i) {
        if (strchr(rules[i].selector, '.') != NULL &&
            strchr(rules[i].selector, '#') == NULL &&
            rule_matches(node, rules[i].selector))
            declarations(&style, rules[i].declarations, available);
    }
    for (i = 0; i < count; ++i) {
        if (strchr(rules[i].selector, '#') != NULL &&
            rule_matches(node, rules[i].selector))
            declarations(&style, rules[i].declarations, available);
    }
    declarations(&style, attribute(node, "style"), available);
    if ((attr = attribute(node, "hidden")) != NULL) { (void)attr; style.hidden = true; }
    if ((attr = attribute(node, "width")) != NULL && !style.has_width) {
        style.width = css_length(attr, available, available);
        style.has_width = true;
    }
    if ((attr = attribute(node, "height")) != NULL && !style.has_height) {
        style.height = css_length(attr, available, 0);
        style.has_height = true;
    }
    if (style.font_size < 5.0f) style.font_size = 5.0f;
    if (style.font_size > 72.0f) style.font_size = 72.0f;
    return style;
}

static HPDF_Font select_font(Renderer *renderer, const Style *style) {
    return style->mono ? renderer->fallback_mono :
           (style->bold ? renderer->fallback_bold : renderer->fallback_regular);
}

static float word_width(Renderer *renderer, const Style *style, const char *text) {
    HPDF_Page_SetFontAndSize(renderer->pages[0], select_font(renderer, style),
                             style->font_size);
    return HPDF_Page_TextWidth(renderer->pages[0], text);
}

static void line_finish(Flow *flow) {
    if (flow->line_x != flow->x) {
        if (flow->center && flow->line_first != NULL) {
            GraphicRun *run;
            float shift = (flow->width - (flow->line_x - flow->x)) / 2.0f;
            if (shift > 0.0f) {
                for (run = flow->line_first; run != NULL; run = run->line_next)
                    run->x += shift;
            }
        }
        flow->y = flow->line_top + flow->line_height;
        flow->line_x = flow->x;
    }
    flow->line_top = flow->y;
    flow->line_height = 0.0f;
    flow->line_first = flow->line_last = NULL;
}

static void page_break_flow(Flow *flow, float height) {
    float page = (float)((size_t)(flow->line_top / PAGE_HEIGHT)) * PAGE_HEIGHT;
    if (flow->line_top + height > page + PAGE_HEIGHT - PAGE_MARGIN) {
        flow->line_top = page + PAGE_HEIGHT + PAGE_MARGIN;
        flow->y = flow->line_top;
        flow->line_x = flow->x;
    }
}

static bool layout_text(Renderer *renderer, GraphicNode *node,
                        const Style *style, Flow *flow, size_t *run_count) {
    const unsigned char *cursor = (const unsigned char *)node->text;
    while (cursor != NULL && *cursor != '\0') {
        char word[256];
        size_t length = 0U;
        float width, space;
        GraphicRun *run;
        while (*cursor != '\0' && isspace(*cursor)) ++cursor;
        if (*cursor == '\0') break;
        while (*cursor != '\0' && !isspace(*cursor) && length < sizeof(word) - 1U) {
            word[length++] = *cursor >= 32U && *cursor < 127U ? (char)*cursor : '?';
            ++cursor;
        }
        word[length] = '\0';
        width = word_width(renderer, style, word);
        space = word_width(renderer, style, " ");
        if (flow->line_x > flow->x &&
            flow->line_x + width > flow->x + flow->width) line_finish(flow);
        page_break_flow(flow, style->font_size * 1.35f);
        if (*run_count >= MAX_RUNS || (run = (GraphicRun *)calloc(1U, sizeof(*run))) == NULL)
            return false;
        ++*run_count;
        memcpy(run->text, word, length + 1U);
        run->x = flow->line_x;
        run->top = flow->line_top;
        run->size = style->font_size;
        run->color = style->foreground;
        run->bold = style->bold;
        run->mono = style->mono;
        if (node->last_run != NULL) node->last_run->next = run;
        else node->runs = run;
        node->last_run = run;
        if (flow->line_last != NULL) flow->line_last->line_next = run;
        else flow->line_first = run;
        flow->line_last = run;
        flow->line_x += width + space;
        if (flow->line_height < style->font_size * 1.35f)
            flow->line_height = style->font_size * 1.35f;
    }
    return renderer->error == HPDF_OK;
}

static bool layout_node(Renderer *renderer, GraphicNode *node, Style inherited,
                        Rule *rules, size_t rule_count, Flow *flow,
                        size_t *run_count) {
    GraphicNode *child;
    Flow inside;
    float content_width;
    if (strcmp(node->tag, "#text") == 0)
        return layout_text(renderer, node, &inherited, flow, run_count);
    node->style = compute_style(node, &inherited, rules, rule_count, flow->width);
    if (node->style.hidden) return true;
    if (!node->style.block) {
        for (child = node->children; child != NULL; child = child->next)
            if (!layout_node(renderer, child, node->style, rules, rule_count,
                             flow, run_count)) return false;
        return true;
    }
    line_finish(flow);
    flow->y += node->style.margin;
    if (flow->y - (float)((size_t)(flow->y / PAGE_HEIGHT)) * PAGE_HEIGHT >
        PAGE_HEIGHT - PAGE_MARGIN - 35.0f)
        flow->y = (float)((size_t)(flow->y / PAGE_HEIGHT) + 1U) * PAGE_HEIGHT + PAGE_MARGIN;
    node->x = flow->x + node->style.margin;
    node->top = flow->y;
    node->width = node->style.has_width ? node->style.width :
                  flow->width - 2.0f * node->style.margin;
    if (node->width > flow->width - 2.0f * node->style.margin)
        node->width = flow->width - 2.0f * node->style.margin;
    if (node->width < 10.0f) node->width = 10.0f;
    content_width = node->width - 2.0f * (node->style.padding + node->style.border);
    if (content_width < 5.0f) content_width = 5.0f;
    inside.x = node->x + node->style.padding + node->style.border;
    inside.width = content_width;
    inside.y = node->top + node->style.padding + node->style.border;
    inside.line_x = inside.x;
    inside.line_top = inside.y;
    inside.line_height = 0;
    inside.line_first = inside.line_last = NULL;
    inside.center = node->style.center;
    if (strcmp(node->tag, "img") == 0) {
        float image_height = node->style.has_height ? node->style.height : 72.0f;
        if (!node->style.has_width && node->width > 96.0f) node->width = 96.0f;
        inside.y += image_height;
    } else {
        for (child = node->children; child != NULL; child = child->next)
            if (!layout_node(renderer, child, node->style, rules, rule_count,
                             &inside, run_count)) return false;
        line_finish(&inside);
    }
    node->height = inside.y - node->top + node->style.padding + node->style.border;
    if (node->style.has_height && node->height < node->style.height)
        node->height = node->style.height;
    if (node->height < node->style.min_height) node->height = node->style.min_height;
    flow->y = node->top + node->height + node->style.margin;
    flow->line_top = flow->y;
    flow->line_x = flow->x;
    return true;
}

static bool paint_box(Renderer *renderer, const GraphicNode *node) {
    float top = node->top, end = node->top + node->height;
    const Style *style = &node->style;
    if ((!style->has_background && style->border <= 0.0f) || node->height <= 0.0f)
        return true;
    while (top < end) {
        size_t index = (size_t)(top / PAGE_HEIGHT);
        float local_top = top - (float)index * PAGE_HEIGHT;
        float part = end - top;
        HPDF_Page page = page_at(renderer, index);
        if (page == NULL) return false;
        if (part > PAGE_HEIGHT - local_top) part = PAGE_HEIGHT - local_top;
        if (style->has_background) {
            HPDF_Page_SetRGBFill(page, style->background.r, style->background.g,
                                 style->background.b);
            HPDF_Page_Rectangle(page, node->x, PAGE_HEIGHT - local_top - part,
                                node->width, part);
            HPDF_Page_Fill(page);
        }
        if (style->border > 0.0f) {
            HPDF_Page_SetRGBStroke(page, style->border_color.r,
                                   style->border_color.g, style->border_color.b);
            HPDF_Page_SetLineWidth(page, style->border);
            HPDF_Page_Rectangle(page, node->x, PAGE_HEIGHT - local_top - part,
                                node->width, part);
            HPDF_Page_Stroke(page);
        }
        top += part;
    }
    return renderer->error == HPDF_OK;
}

static int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint32_t png_u32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) |
           ((uint32_t)p[2] << 8U) | (uint32_t)p[3];
}

static uint32_t png_crc32(const unsigned char *bytes, size_t length) {
    uint32_t crc = 0xffffffffU;
    size_t i;
    for (i = 0U; i < length; ++i) {
        unsigned int bit;
        crc ^= bytes[i];
        for (bit = 0U; bit < 8U; ++bit)
            crc = crc & 1U ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
    }
    return ~crc;
}

static bool png_valid(const unsigned char *bytes, size_t length) {
    size_t pos = 8U;
    bool saw_header = false, saw_data = false;
    if (length < 45U || memcmp(bytes, "\x89PNG\r\n\x1a\n", 8U) != 0)
        return false;
    while (pos + 12U <= length) {
        uint32_t chunk = png_u32(bytes + pos);
        const unsigned char *type = bytes + pos + 4U;
        if ((size_t)chunk > length - pos - 12U ||
            png_crc32(type, (size_t)chunk + 4U) !=
                png_u32(bytes + pos + 8U + chunk)) return false;
        if (memcmp(type, "IHDR", 4U) == 0) {
            uint32_t width, height;
            if (saw_header || pos != 8U || chunk != 13U) return false;
            width = png_u32(bytes + pos + 8U);
            height = png_u32(bytes + pos + 12U);
            if (width == 0U || height == 0U || width > 4096U ||
                height > 4096U || width > 4000000U / height) return false;
            saw_header = true;
        } else if (memcmp(type, "IDAT", 4U) == 0) {
            saw_data = true;
        } else if (memcmp(type, "IEND", 4U) == 0) {
            return chunk == 0U && saw_header && saw_data &&
                   pos + 12U == length;
        }
        pos += 12U + chunk;
    }
    return false;
}

static bool paint_image(Renderer *renderer, const GraphicNode *node) {
    const char *source = attribute(node, "src");
    const char *payload;
    bool png, jpeg;
    size_t length, produced = 0U, i, index;
    unsigned char *bytes;
    HPDF_Image image;
    HPDF_Page page;
    float top;
    if (source == NULL || strncmp(source, "data:image/", 11U) != 0) return true;
    png = strncmp(source, "data:image/png;base64,", 22U) == 0;
    jpeg = strncmp(source, "data:image/jpeg;base64,", 23U) == 0;
    if (!png && !jpeg) return true;
    payload = strchr(source, ',');
    if (payload == NULL) return true;
    ++payload;
    length = strlen(payload);
    if (length < 16U || length > 2U * 1024U * 1024U || length % 4U != 0U)
        return true;
    bytes = (unsigned char *)malloc(length / 4U * 3U);
    if (bytes == NULL) return false;
    for (i = 0U; i < length; i += 4U) {
        int a = base64_digit((unsigned char)payload[i]);
        int b = base64_digit((unsigned char)payload[i + 1U]);
        int c = payload[i + 2U] == '=' ? 0 :
                base64_digit((unsigned char)payload[i + 2U]);
        int d = payload[i + 3U] == '=' ? 0 :
                base64_digit((unsigned char)payload[i + 3U]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (payload[i + 2U] == '=' && payload[i + 3U] != '=')) break;
        bytes[produced++] = (unsigned char)((a << 2) | (b >> 4));
        if (payload[i + 2U] != '=')
            bytes[produced++] = (unsigned char)((b << 4) | (c >> 2));
        if (payload[i + 3U] != '=')
            bytes[produced++] = (unsigned char)((c << 6) | d);
        if (payload[i + 2U] == '=' || payload[i + 3U] == '=') {
            i += 4U;
            break;
        }
    }
    if (i != length || produced < 8U ||
        (png && !png_valid(bytes, produced)) ||
        (jpeg && (bytes[0] != 0xffU || bytes[1] != 0xd8U))) {
        free(bytes);
        return true;
    }
    top = node->top + node->style.padding + node->style.border;
    index = (size_t)(top / PAGE_HEIGHT);
    page = page_at(renderer, index);
    if (page == NULL) { free(bytes); return false; }
    image = png ? HPDF_LoadPngImageFromMem(renderer->document, bytes,
                                            (HPDF_UINT)produced) :
                   HPDF_LoadJpegImageFromMem(renderer->document, bytes,
                                             (HPDF_UINT)produced);
    free(bytes);
    if (image == NULL) return false;
    if (HPDF_Page_DrawImage(page, image,
            node->x + node->style.padding + node->style.border,
            PAGE_HEIGHT - (top - (float)index * PAGE_HEIGHT) -
                (node->height - 2.0f * (node->style.padding + node->style.border)),
            node->width - 2.0f * (node->style.padding + node->style.border),
            node->height - 2.0f * (node->style.padding + node->style.border)) != HPDF_OK)
        return false;
    return renderer->error == HPDF_OK;
}

static bool paint_node(Renderer *renderer, const GraphicNode *node) {
    const GraphicNode *child;
    const GraphicRun *run;
    if (node->style.hidden) return true;
    if (!paint_box(renderer, node)) return false;
    if (strcmp(node->tag, "img") == 0 && !paint_image(renderer, node)) return false;
    for (run = node->runs; run != NULL; run = run->next) {
        size_t index = (size_t)(run->top / PAGE_HEIGHT);
        HPDF_Page page = page_at(renderer, index);
        HPDF_Font font = run->mono ? renderer->fallback_mono :
                         (run->bold ? renderer->fallback_bold : renderer->fallback_regular);
        float local_top = run->top - (float)index * PAGE_HEIGHT;
        if (page == NULL || HPDF_Page_BeginText(page) != HPDF_OK ||
            HPDF_Page_SetFontAndSize(page, font, run->size) != HPDF_OK ||
            HPDF_Page_SetRGBFill(page, run->color.r, run->color.g,
                                 run->color.b) != HPDF_OK ||
            HPDF_Page_TextOut(page, run->x, PAGE_HEIGHT - local_top - run->size,
                              run->text) != HPDF_OK || HPDF_Page_EndText(page) != HPDF_OK)
            return false;
    }
    for (child = node->children; child != NULL; child = child->next)
        if (!paint_node(renderer, child)) return false;
    return renderer->error == HPDF_OK;
}

bool renderer_draw(Renderer *renderer) {
    Rule rules[MAX_RULES] = {0};
    size_t count = collect_rules(renderer->root, rules, 0U);
    size_t run_count = 0U, i;
    Style style = {0};
    Flow flow = {PAGE_MARGIN, PAGE_WIDTH - 2.0f * PAGE_MARGIN,
                 PAGE_MARGIN, PAGE_MARGIN, PAGE_MARGIN, 0.0f,
                 NULL, NULL, false};
    GraphicNode *node;
    bool ok = true;
    style.foreground = (Color){0, 0, 0};
    style.border_color = (Color){0, 0, 0};
    style.font_size = 11.0f;
    for (node = renderer->root->children; node != NULL; node = node->next)
        if (!layout_node(renderer, node, style, rules, count, &flow, &run_count)) {
            ok = false;
            break;
        }
    if (ok) for (node = renderer->root->children; node != NULL; node = node->next)
        if (!paint_node(renderer, node)) { ok = false; break; }
    for (i = 0; i < count; ++i) free(rules[i].declarations);
    return ok && renderer->error == HPDF_OK;
}
