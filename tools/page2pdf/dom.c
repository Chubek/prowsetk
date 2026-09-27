/* dom.c — the IR boundary and the node tree.
 *
 * Replays the ProwseTk event stream (start/attribute/text/end) into a node
 * tree. This is the only place that knows the wire order of the IR; nothing
 * above it re-reads the stream. The tree is inert: no CSS, no geometry, no
 * libHaru. Layout and paint consume it read-only.
 */
#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ tree building */

static GraphicNode *node_new(GraphicNode *parent, const char *tag,
                             const unsigned char *text, size_t length) {
    GraphicNode *node = (GraphicNode *)p2p_calloc(1U, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    node->tag = p2p_strdup_c(tag);
    if (node->tag == NULL) {
        free(node);
        return NULL;
    }
    if (text != NULL) {
        node->text = p2p_strdup(text, length);
        if (node->text == NULL) {
            free(node->tag);
            free(node);
            return NULL;
        }
    }
    node->is_text = strcmp(tag, TAG_TEXT) == 0;
    node->parent = parent;
    return node;
}

static void node_append(GraphicNode *parent, GraphicNode *child) {
    child->parent = parent;
    child->prev = parent->last_child;
    if (parent->last_child != NULL) {
        parent->last_child->next = child;
    } else {
        parent->children = child;
    }
    parent->last_child = child;
}

/* Walks the tree depth-first, releasing every node. */
static void tree_free(GraphicNode *node) {
    while (node != NULL) {
        GraphicNode *next = node->next;
        GraphicAttribute *attribute = node->attributes;
        GraphicNode *child = node->children;
        while (child != NULL) {
            GraphicNode *after = child->next;
            tree_free(child);
            child = after;
        }
        while (attribute != NULL) {
            GraphicAttribute *after = attribute->next;
            free(attribute->name);
            free(attribute->value);
            free(attribute);
            attribute = after;
        }
        free(node->tag);
        free(node->text);
        style_release(&node->style);
        free(node);
        node = next;
    }
}

static bool attach_attribute(GraphicNode *node, const char *name,
                             const unsigned char *value, size_t length) {
    GraphicAttribute *attribute =
        (GraphicAttribute *)p2p_calloc(1U, sizeof(*attribute));
    if (attribute == NULL) {
        return false;
    }
    attribute->name = p2p_strdup_c(name);
    attribute->value = p2p_strdup(value, length);
    if (attribute->name == NULL || attribute->value == NULL) {
        free(attribute->name);
        free(attribute->value);
        free(attribute);
        return false;
    }
    /* Preserve document order: the cascade is sensitive to duplicate
     * attribute precedence, and inspect output should read like the IR. */
    if (node->attributes == NULL) {
        node->attributes = attribute;
    } else {
        GraphicAttribute *tail = node->attributes;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = attribute;
    }
    return true;
}

bool renderer_start(Renderer *renderer, const char *tag, size_t depth) {
    GraphicNode *node;
    GraphicNode *parent;
    const char *safe = tag;
    if (depth != renderer->frame_count || depth >= PAGE2PDF_MAX_NESTING) {
        return false;
    }
    if (safe == NULL) {
        safe = "unknown";
    }
    if (strlen(safe) > PAGE2PDF_MAX_TAG_BYTES) {
        return false;
    }
    if (renderer->node_count >= (renderer->max_nodes != 0U ? renderer->max_nodes
                                                           : P2P_MAX_NODES)) {
        return false;
    }
    parent = depth == 0U ? renderer->root : renderer->frames[depth - 1U].node;
    if (parent == NULL) {
        return false;
    }
    node = node_new(parent, safe, NULL, 0U);
    if (node == NULL) {
        return false;
    }
    node->depth = (unsigned)depth;
    node_append(parent, node);
    ++renderer->node_count;
    memcpy(renderer->frames[depth].tag, safe, strlen(safe) + 1U);
    renderer->frames[depth].node = node;
    ++renderer->frame_count;
    return true;
}

bool renderer_attribute(Renderer *renderer, const char *name,
                        const unsigned char *value, size_t length, size_t depth) {
    if (renderer->frame_count == 0U || depth + 1U != renderer->frame_count) {
        return false;
    }
    return attach_attribute(renderer->frames[depth].node, name, value, length);
}

bool renderer_text(Renderer *renderer, const unsigned char *text,
                   size_t length, size_t depth, const char *tag) {
    GraphicNode *node;
    GraphicNode *parent;
    /* A leading UTF-8 byte order mark belongs to the stream framing, not to
     * the document text. */
    if (length >= 3U && text[0] == 0xEFU && text[1] == 0xBBU && text[2] == 0xBFU) {
        text += 3U;
        length -= 3U;
    }
    if (depth != renderer->frame_count) {
        return false;
    }
    if (tag != NULL && (depth == 0U ||
        strcmp(tag, renderer->frames[depth - 1U].tag) != 0)) {
        return false;
    }
    if (renderer->node_count >= (renderer->max_nodes != 0U ? renderer->max_nodes
                                                           : P2P_MAX_NODES)) {
        return false;
    }
    parent = depth == 0U ? renderer->root : renderer->frames[depth - 1U].node;
    if (parent == NULL) {
        return false;
    }
    node = node_new(parent, TAG_TEXT, text, length);
    if (node == NULL) {
        return false;
    }
    node->depth = (unsigned)depth;
    node_append(parent, node);
    ++renderer->node_count;
    return true;
}

bool renderer_end(Renderer *renderer, const char *tag, size_t depth) {
    if (renderer->frame_count == 0U || depth + 1U != renderer->frame_count ||
        strcmp(tag, renderer->frames[depth].tag) != 0) {
        return false;
    }
    --renderer->frame_count;
    return true;
}

/* ------------------------------------------------------------ node queries */

const char *node_attribute(const GraphicNode *node, const char *name) {
    const GraphicAttribute *attribute;
    for (attribute = node->attributes; attribute != NULL;
         attribute = attribute->next) {
        if (strcmp(attribute->name, name) == 0) {
            return attribute->value;
        }
    }
    return NULL;
}

bool node_has_attribute(const GraphicNode *node, const char *name) {
    return node_attribute(node, name) != NULL;
}

size_t node_index_among_siblings(const GraphicNode *node) {
    size_t index = 0U;
    const GraphicNode *cursor;
    for (cursor = node->parent != NULL ? node->parent->children : NULL;
         cursor != NULL; cursor = cursor->next) {
        if (cursor == node) {
            return index;
        }
        ++index;
    }
    return index;
}

size_t node_element_ordinal(const GraphicNode *node) {
    size_t ordinal = 1U;
    const GraphicNode *cursor;
    for (cursor = node->parent != NULL ? node->parent->children : NULL;
         cursor != NULL && cursor != node; cursor = cursor->next) {
        if (!cursor->is_text && strcmp(cursor->tag, TAG_ANON) != 0 &&
            strcmp(cursor->tag, TAG_ANON_BLOCK) != 0) {
            ++ordinal;
        }
    }
    return ordinal;
}

size_t node_element_sibling_count(const GraphicNode *node) {
    size_t count = 0U;
    const GraphicNode *cursor;
    for (cursor = node->parent != NULL ? node->parent->children : NULL;
         cursor != NULL; cursor = cursor->next) {
        if (!cursor->is_text && strcmp(cursor->tag, TAG_ANON) != 0 &&
            strcmp(cursor->tag, TAG_ANON_BLOCK) != 0) {
            ++count;
        }
    }
    return count;
}

size_t node_tag_ordinal(const GraphicNode *node) {
    size_t ordinal = 1U;
    const GraphicNode *cursor;
    for (cursor = node->parent != NULL ? node->parent->children : NULL;
         cursor != NULL && cursor != node; cursor = cursor->next) {
        if (!cursor->is_text && strcmp(cursor->tag, node->tag) == 0) {
            ++ordinal;
        }
    }
    return ordinal;
}

size_t node_type_index_among_siblings(const GraphicNode *node) {
    size_t index = 0U;
    const GraphicNode *cursor;
    for (cursor = node->parent != NULL ? node->parent->children : NULL;
         cursor != NULL; cursor = cursor->next) {
        if (cursor == node) {
            return index;
        }
        if (!cursor->is_text) {
            ++index;
        }
    }
    return index;
}

size_t node_type_sibling_count(const GraphicNode *node) {
    size_t count = 0U;
    const GraphicNode *cursor;
    for (cursor = node->parent != NULL ? node->parent->children : NULL;
         cursor != NULL; cursor = cursor->next) {
        if (!cursor->is_text) {
            ++count;
        }
    }
    return count;
}

bool node_is_root(const GraphicNode *node) {
    return node != NULL && node->parent == NULL;
}

/* Maps a tag plus a `type` attribute onto a form control kind. `tag` is
 * returned unchanged when it does not select a control. */
const char *node_form_control_kind(const GraphicNode *node) {
    const char *type;
    if (node == NULL) {
        return "";
    }
    if (strcmp(node->tag, "textarea") == 0) {
        return "textarea";
    }
    if (strcmp(node->tag, "select") == 0) {
        return "select";
    }
    if (strcmp(node->tag, "option") == 0) {
        return "option";
    }
    if (strcmp(node->tag, "button") != 0 && strcmp(node->tag, "input") != 0 &&
        strcmp(node->tag, "fieldset") != 0 && strcmp(node->tag, "output") != 0 &&
        strcmp(node->tag, "progress") != 0 && strcmp(node->tag, "meter") != 0) {
        return node->tag;
    }
    if (strcmp(node->tag, "button") == 0) {
        const char *button_type = node_attribute(node, "type");
        return button_type != NULL && p2p_iequals(button_type, "reset") ? "reset" : "button";
    }
    type = node_attribute(node, "type");
    if (type == NULL || *type == '\0') {
        return strcmp(node->tag, "input") == 0 ? "text" : node->tag;
    }
    return type;
}

bool node_is_form_control(const GraphicNode *node) {
    const char *kind;
    static const char *const kinds[] = {
        "button", "reset", "submit", "text", "password", "search", "email",
        "url", "tel", "number", "range", "checkbox", "radio", "file",
        "hidden", "color", "date", "datetime-local", "month", "time", "week",
        "image", "textarea", "select", "progress", "meter"
    };
    size_t i;
    if (node == NULL) {
        return false;
    }
    kind = node_form_control_kind(node);
    for (i = 0U; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        if (p2p_iequals(kind, kinds[i])) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ renderer */

static void haru_error_handler(HPDF_STATUS error, HPDF_STATUS detail,
                               void *user_data) {
    Renderer *renderer = (Renderer *)user_data;
    if (renderer != NULL && renderer->error == HPDF_OK) {
        renderer->error = error;
        renderer->detail = detail;
    }
}

bool renderer_failed(const Renderer *renderer) {
    return renderer->error != HPDF_OK;
}

bool renderer_init(Renderer *renderer) {
    static const PageGeometry kDefault = {612.0f, 792.0f, 36.0f, 36.0f, 36.0f, 36.0f};
    memset(renderer, 0, sizeof(*renderer));
    renderer->error = HPDF_OK;
    renderer->geometry = kDefault;
    renderer->max_nodes = P2P_MAX_NODES;
    renderer->root = (GraphicNode *)p2p_calloc(1U, sizeof(GraphicNode));
    if (renderer->root == NULL) {
        return false;
    }
    renderer->root->tag = p2p_strdup_c(TAG_ROOT);
    if (renderer->root->tag == NULL) {
        p2p_report_oom();
        return false;
    }
    renderer->document = HPDF_New(haru_error_handler, renderer);
    if (renderer->document == NULL) {
        return false;
    }
    HPDF_SetCompressionMode(renderer->document, HPDF_COMP_ALL);
    renderer->fonts = fonts_create(!renderer->no_embed_fonts);
    if (renderer->fonts == NULL) {
        return false;
    }
    if (!renderer->no_embed_fonts) {
        fonts_scan_system(renderer->fonts, renderer->font_dir);
    }
    renderer->fallback_regular = fonts_base14(renderer->fonts, false, false, false);
    renderer->fallback_bold = fonts_base14(renderer->fonts, true, false, false);
    renderer->fallback_italic = fonts_base14(renderer->fonts, false, true, false);
    renderer->fallback_bold_italic = fonts_base14(renderer->fonts, true, true, false);
    renderer->fallback_mono = fonts_base14(renderer->fonts, false, false, true);
    return renderer->fallback_regular != NULL && renderer->fallback_bold != NULL &&
           renderer->fallback_italic != NULL && renderer->fallback_mono != NULL;
}

void renderer_destroy(Renderer *renderer) {
    if (renderer->root != NULL) {
        tree_free(renderer->root);
        renderer->root = NULL;
    }
    if (renderer->fonts != NULL) {
        fonts_destroy(renderer->fonts);
        renderer->fonts = NULL;
    }
    if (renderer->document != NULL) {
        HPDF_Free(renderer->document);
        renderer->document = NULL;
    }
}
