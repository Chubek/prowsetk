/* page2pdf -- compile ProwseVTD or ProwseIML into a semantic PDF.
 *
 * This program intentionally consumes only serialized ProwseTk IR.  It never
 * links against Flatworm or reaches into the C++ DOM, which keeps the C tool
 * on the lowering/encoding side of the IR boundary.
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hpdf.h>

#define PAGE2PDF_MAX_INPUT_BYTES (16U * 1024U * 1024U)
#define PAGE2PDF_MAX_NESTING 256U
#define PAGE2PDF_MAX_TAG_BYTES 127U
#define PAGE2PDF_MARGIN 54.0f
#define PAGE2PDF_PAGE_WIDTH 612.0f
#define PAGE2PDF_PAGE_HEIGHT 792.0f

typedef enum {
    INPUT_AUTO,
    INPUT_IML,
    INPUT_VTD
} InputFormat;

typedef struct {
    unsigned char *data;
    size_t length;
} Buffer;

typedef struct {
    const unsigned char *data;
    size_t length;
} Slice;

typedef struct {
    char tag[PAGE2PDF_MAX_TAG_BYTES + 1U];
} ElementFrame;

typedef struct {
    HPDF_Doc document;
    HPDF_Page page;
    HPDF_Font regular;
    HPDF_Font bold;
    HPDF_Font mono;
    ElementFrame frames[PAGE2PDF_MAX_NESTING];
    size_t frame_count;
    float y;
    HPDF_STATUS error;
    HPDF_STATUS detail;
} Renderer;

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
    Renderer *renderer;
    char error[192];
} ImlParser;

static void hpdf_error_handler(HPDF_STATUS error, HPDF_STATUS detail,
                               void *user_data) {
    Renderer *renderer = (Renderer *)user_data;
    if (renderer != NULL && renderer->error == HPDF_OK) {
        renderer->error = error;
        renderer->detail = detail;
    }
}

static void print_usage(FILE *stream) {
    fprintf(stream,
            "Usage: page2pdf [--format auto|iml|vtd] INPUT OUTPUT.pdf\\n"
            "Compile serialized ProwseIML or ProwseVTD to a semantic PDF.\\n"
            "Use '-' as INPUT to read standard input. OUTPUT must be a file.\\n");
}

static bool buffer_append(Buffer *buffer, const unsigned char *data,
                          size_t count) {
    unsigned char *grown;
    if (count > PAGE2PDF_MAX_INPUT_BYTES - buffer->length) {
        return false;
    }
    grown = (unsigned char *)realloc(buffer->data, buffer->length + count + 1U);
    if (grown == NULL) {
        return false;
    }
    buffer->data = grown;
    if (count != 0U) {
        memcpy(buffer->data + buffer->length, data, count);
    }
    buffer->length += count;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool read_input(const char *path, Buffer *out) {
    FILE *input = NULL;
    unsigned char chunk[8192];
    size_t count;
    bool ok = true;

    if (strcmp(path, "-") == 0) {
        input = stdin;
    } else {
        input = fopen(path, "rb");
    }
    if (input == NULL) {
        fprintf(stderr, "page2pdf: cannot open input '%s': %s\\n", path,
                strerror(errno));
        return false;
    }
    while ((count = fread(chunk, 1U, sizeof(chunk), input)) != 0U) {
        if (!buffer_append(out, chunk, count)) {
            fprintf(stderr, "page2pdf: input exceeds the 16 MiB safety limit\\n");
            ok = false;
            break;
        }
    }
    if (ferror(input) != 0) {
        fprintf(stderr, "page2pdf: failed to read input '%s'\\n", path);
        ok = false;
    }
    if (input != stdin) {
        fclose(input);
    }
    return ok;
}

static bool copy_identifier(Slice value, char *destination,
                            size_t destination_size) {
    size_t i;
    if (value.length == 0U || value.length >= destination_size) {
        return false;
    }
    for (i = 0U; i < value.length; ++i) {
        const unsigned char c = value.data[i];
        if (!(isalnum(c) != 0 || c == '-' || c == '_' || c == ':')) {
            return false;
        }
    }
    memcpy(destination, value.data, value.length);
    destination[value.length] = '\0';
    return true;
}

static bool tag_is(const Renderer *renderer, const char *tag) {
    return renderer->frame_count != 0U &&
           strcmp(renderer->frames[renderer->frame_count - 1U].tag, tag) == 0;
}

static bool tag_is_heading(const Renderer *renderer) {
    return tag_is(renderer, "h1") || tag_is(renderer, "h2") ||
           tag_is(renderer, "h3") || tag_is(renderer, "h4") ||
           tag_is(renderer, "h5") || tag_is(renderer, "h6");
}

static bool tag_is_block(const char *tag) {
    return strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 ||
           strcmp(tag, "section") == 0 || strcmp(tag, "article") == 0 ||
           strcmp(tag, "header") == 0 || strcmp(tag, "footer") == 0 ||
           strcmp(tag, "main") == 0 || strcmp(tag, "blockquote") == 0 ||
           strcmp(tag, "pre") == 0 || strcmp(tag, "li") == 0 ||
           tag[0] == 'h';
}

static bool renderer_new_page(Renderer *renderer) {
    renderer->page = HPDF_AddPage(renderer->document);
    if (renderer->page == NULL || renderer->error != HPDF_OK) {
        return false;
    }
    if (HPDF_Page_SetSize(renderer->page, HPDF_PAGE_SIZE_LETTER,
                          HPDF_PAGE_PORTRAIT) != HPDF_OK) {
        return false;
    }
    renderer->y = PAGE2PDF_PAGE_HEIGHT - PAGE2PDF_MARGIN;
    return renderer->error == HPDF_OK;
}

static HPDF_Font renderer_font(const Renderer *renderer, float *size) {
    if (tag_is(renderer, "h1")) {
        *size = 22.0f;
        return renderer->bold;
    }
    if (tag_is(renderer, "h2")) {
        *size = 18.0f;
        return renderer->bold;
    }
    if (tag_is(renderer, "h3")) {
        *size = 15.0f;
        return renderer->bold;
    }
    if (tag_is_heading(renderer)) {
        *size = 13.0f;
        return renderer->bold;
    }
    if (tag_is(renderer, "pre") || tag_is(renderer, "code")) {
        *size = 9.0f;
        return renderer->mono;
    }
    *size = 11.0f;
    return renderer->regular;
}

static size_t renderer_indent(const Renderer *renderer) {
    size_t indent = 0U;
    size_t i;
    for (i = 0U; i < renderer->frame_count; ++i) {
        const char *tag = renderer->frames[i].tag;
        if (strcmp(tag, "ul") == 0 || strcmp(tag, "ol") == 0 ||
            strcmp(tag, "blockquote") == 0) {
            indent += 18U;
        }
    }
    return indent > 108U ? 108U : indent;
}

static bool renderer_write_line(Renderer *renderer, const char *line,
                                size_t indent, bool list_prefix) {
    HPDF_Font font;
    float font_size;
    float line_height;
    float x;
    char prefixed[1100];
    const char *text = line;

    font = renderer_font(renderer, &font_size);
    line_height = font_size * 1.35f;
    if (renderer->y - line_height < PAGE2PDF_MARGIN) {
        if (!renderer_new_page(renderer)) {
            return false;
        }
    }
    if (list_prefix) {
        const int written = snprintf(prefixed, sizeof(prefixed), "* %s", line);
        if (written < 0 || (size_t)written >= sizeof(prefixed)) {
            return false;
        }
        text = prefixed;
        if (indent >= 12U) {
            indent -= 12U;
        }
    }
    x = PAGE2PDF_MARGIN + (float)indent;
    if (HPDF_Page_BeginText(renderer->page) != HPDF_OK ||
        HPDF_Page_SetFontAndSize(renderer->page, font, font_size) != HPDF_OK ||
        HPDF_Page_SetRGBFill(renderer->page, 0.0f, 0.0f, 0.0f) != HPDF_OK ||
        HPDF_Page_TextOut(renderer->page, x, renderer->y, text) != HPDF_OK ||
        HPDF_Page_EndText(renderer->page) != HPDF_OK) {
        return false;
    }
    renderer->y -= line_height;
    return renderer->error == HPDF_OK;
}

static bool renderer_write_text(Renderer *renderer, const unsigned char *text,
                                size_t length) {
    char line[1024];
    char word[512];
    size_t line_length = 0U;
    size_t word_length = 0U;
    size_t i;
    bool emitted = false;
    bool first_line = true;
    const size_t indent = renderer_indent(renderer);
    float font_size;
    HPDF_Font font = renderer_font(renderer, &font_size);
    const float available_width = PAGE2PDF_PAGE_WIDTH - PAGE2PDF_MARGIN * 2.0f -
                                  (float)indent;

    for (i = 0U; i <= length; ++i) {
        const unsigned char original = i == length ? ' ' : text[i];
        const bool whitespace = isspace(original) != 0;
        const char c = original >= 32U && original <= 126U ? (char)original : '?';
        if (!whitespace && word_length + 1U < sizeof(word)) {
            word[word_length++] = c;
            continue;
        }
        if (!whitespace && word_length + 1U == sizeof(word)) {
            /* A pathological token is split deterministically instead of
             * allocating from untrusted input. */
            word[word_length] = '\0';
        }
        if (word_length == 0U) {
            continue;
        }
        word[word_length] = '\0';
        if (line_length != 0U) {
            const size_t candidate_length = line_length + 1U + word_length;
            if (candidate_length >= sizeof(line)) {
                if (!renderer_write_line(renderer, line, indent,
                                         first_line && tag_is(renderer, "li"))) {
                    return false;
                }
                emitted = true;
                first_line = false;
                line_length = 0U;
            } else {
                char candidate[1024];
                memcpy(candidate, line, line_length);
                candidate[line_length] = ' ';
                memcpy(candidate + line_length + 1U, word, word_length + 1U);
                HPDF_Page_SetFontAndSize(renderer->page, font, font_size);
                if (HPDF_Page_TextWidth(renderer->page, candidate) > available_width) {
                    if (!renderer_write_line(renderer, line, indent,
                                             first_line && tag_is(renderer, "li"))) {
                        return false;
                    }
                    emitted = true;
                    first_line = false;
                    line_length = 0U;
                } else {
                    memcpy(line, candidate, candidate_length + 1U);
                    line_length = candidate_length;
                    word_length = 0U;
                    continue;
                }
            }
        }
        if (word_length >= sizeof(line)) {
            return false;
        }
        memcpy(line, word, word_length + 1U);
        line_length = word_length;
        word_length = 0U;
    }
    if (line_length != 0U) {
        if (!renderer_write_line(renderer, line, indent,
                                 first_line && tag_is(renderer, "li"))) {
            return false;
        }
        emitted = true;
    }
    if (emitted && tag_is_heading(renderer)) {
        renderer->y -= 5.0f;
    }
    return renderer->error == HPDF_OK;
}

static bool renderer_start(Renderer *renderer, const char *tag, size_t depth) {
    if (depth != renderer->frame_count || renderer->frame_count == PAGE2PDF_MAX_NESTING) {
        return false;
    }
    if (tag_is_block(tag) && renderer->frame_count != 0U &&
        renderer->y < PAGE2PDF_PAGE_HEIGHT - PAGE2PDF_MARGIN - 1.0f) {
        renderer->y -= 4.0f;
    }
    memcpy(renderer->frames[renderer->frame_count].tag, tag, strlen(tag) + 1U);
    ++renderer->frame_count;
    return true;
}

static bool renderer_attribute(Renderer *renderer, size_t depth) {
    return renderer->frame_count != 0U && depth + 1U == renderer->frame_count;
}

static bool renderer_text(Renderer *renderer, const unsigned char *text,
                          size_t length, size_t depth, const char *tag) {
    if (renderer->frame_count == 0U || depth != renderer->frame_count ||
        (tag != NULL && strcmp(tag, renderer->frames[renderer->frame_count - 1U].tag) != 0)) {
        return false;
    }
    return renderer_write_text(renderer, text, length);
}

static bool renderer_end(Renderer *renderer, const char *tag, size_t depth) {
    if (renderer->frame_count == 0U || depth + 1U != renderer->frame_count ||
        strcmp(tag, renderer->frames[renderer->frame_count - 1U].tag) != 0) {
        return false;
    }
    --renderer->frame_count;
    if (tag_is_block(tag)) {
        renderer->y -= 2.0f;
    }
    return true;
}

static bool renderer_init(Renderer *renderer) {
    memset(renderer, 0, sizeof(*renderer));
    renderer->error = HPDF_OK;
    renderer->document = HPDF_New(hpdf_error_handler, renderer);
    if (renderer->document == NULL) {
        return false;
    }
    HPDF_SetCompressionMode(renderer->document, HPDF_COMP_ALL);
    renderer->regular = HPDF_GetFont(renderer->document, "Helvetica", NULL);
    renderer->bold = HPDF_GetFont(renderer->document, "Helvetica-Bold", NULL);
    renderer->mono = HPDF_GetFont(renderer->document, "Courier", NULL);
    if (renderer->regular == NULL || renderer->bold == NULL || renderer->mono == NULL ||
        renderer->error != HPDF_OK) {
        return false;
    }
    return renderer_new_page(renderer);
}

static void renderer_destroy(Renderer *renderer) {
    if (renderer->document != NULL) {
        HPDF_Free(renderer->document);
    }
    renderer->document = NULL;
}

static bool vtd_read_u32(const Buffer *input, size_t *position, uint32_t *out) {
    if (*position > input->length || input->length - *position < 4U) {
        return false;
    }
    *out = (uint32_t)input->data[*position] |
           ((uint32_t)input->data[*position + 1U] << 8U) |
           ((uint32_t)input->data[*position + 2U] << 16U) |
           ((uint32_t)input->data[*position + 3U] << 24U);
    *position += 4U;
    return true;
}

static bool vtd_read_slice(const Buffer *input, size_t *position, Slice *out) {
    uint32_t length;
    if (!vtd_read_u32(input, position, &length) || *position > input->length ||
        (size_t)length > input->length - *position) {
        return false;
    }
    out->data = input->data + *position;
    out->length = (size_t)length;
    *position += out->length;
    return true;
}

static bool vtd_path_matches_tag(Slice path, const char *tag) {
    size_t start = path.length;
    size_t end = path.length;
    while (start != 0U && path.data[start - 1U] != '/') {
        --start;
    }
    while (end > start && path.data[end - 1U] != '[') {
        --end;
    }
    if (end == start || end == 0U || path.data[end - 1U] != '[') {
        return false;
    }
    --end;
    return strlen(tag) == end - start && memcmp(path.data + start, tag, end - start) == 0;
}

static bool compile_vtd(const Buffer *input, Renderer *renderer, char *error,
                        size_t error_size) {
    size_t position = 5U;
    uint32_t count;
    uint32_t index;
    if (input->length < 9U || memcmp(input->data, "PVTD1", 5U) != 0 ||
        !vtd_read_u32(input, &position, &count) || count > input->length / 17U) {
        snprintf(error, error_size, "invalid ProwseVTD header");
        return false;
    }
    for (index = 0U; index < count; ++index) {
        uint32_t depth;
        const unsigned char type = position < input->length ? input->data[position++] : 0U;
        Slice path;
        Slice name;
        Slice value;
        char identifier[PAGE2PDF_MAX_TAG_BYTES + 1U];
        if (position > input->length || !vtd_read_u32(input, &position, &depth) ||
            !vtd_read_slice(input, &position, &path) ||
            !vtd_read_slice(input, &position, &name) ||
            !vtd_read_slice(input, &position, &value) || depth > PAGE2PDF_MAX_NESTING) {
            snprintf(error, error_size, "truncated or invalid ProwseVTD token %u", index);
            return false;
        }
        if ((type == 1U || type == 3U || type == 4U || type == 2U) &&
            !copy_identifier(name, identifier, sizeof(identifier))) {
            snprintf(error, error_size, "invalid ProwseVTD identifier at token %u", index);
            return false;
        }
        if (type == 1U) {
            if (!renderer_start(renderer, identifier, depth)) {
                snprintf(error, error_size, "invalid ProwseVTD start token %u", index);
                return false;
            }
        } else if (type == 2U) {
            if (!renderer_attribute(renderer, depth) ||
                !vtd_path_matches_tag(path, renderer->frames[renderer->frame_count - 1U].tag)) {
                snprintf(error, error_size, "invalid ProwseVTD attribute token %u", index);
                return false;
            }
        } else if (type == 3U) {
            if (!renderer_text(renderer, value.data, value.length, depth, identifier)) {
                snprintf(error, error_size, "invalid ProwseVTD text token %u", index);
                return false;
            }
        } else if (type == 4U) {
            if (!renderer_end(renderer, identifier, depth)) {
                snprintf(error, error_size, "invalid ProwseVTD end token %u", index);
                return false;
            }
        } else {
            snprintf(error, error_size, "unknown ProwseVTD token type at token %u", index);
            return false;
        }
        if (renderer->error != HPDF_OK) {
            snprintf(error, error_size, "libHaru failed while rendering token %u", index);
            return false;
        }
    }
    if (position != input->length || renderer->frame_count != 0U) {
        snprintf(error, error_size, "incomplete ProwseVTD document");
        return false;
    }
    return true;
}

static void iml_fail(ImlParser *parser, const char *message) {
    if (parser->error[0] == '\0') {
        snprintf(parser->error, sizeof(parser->error), "%s at byte %zu", message,
                 parser->position);
    }
}

static void iml_skip_space(ImlParser *parser) {
    while (parser->position < parser->length &&
           isspace(parser->data[parser->position]) != 0) {
        ++parser->position;
    }
}

static bool iml_take(ImlParser *parser, unsigned char expected) {
    iml_skip_space(parser);
    if (parser->position >= parser->length || parser->data[parser->position] != expected) {
        iml_fail(parser, "unexpected ProwseIML syntax");
        return false;
    }
    ++parser->position;
    return true;
}

static bool iml_identifier(ImlParser *parser, char *out, size_t out_size) {
    size_t begin;
    size_t length;
    iml_skip_space(parser);
    begin = parser->position;
    while (parser->position < parser->length) {
        const unsigned char c = parser->data[parser->position];
        if (isspace(c) != 0 || c == '(' || c == ')' || c == '"') {
            break;
        }
        ++parser->position;
    }
    length = parser->position - begin;
    if (length == 0U || length >= out_size ||
        !copy_identifier((Slice){parser->data + begin, length}, out, out_size)) {
        iml_fail(parser, "invalid ProwseIML identifier");
        return false;
    }
    return true;
}

static bool iml_string(ImlParser *parser, Buffer *out) {
    if (!iml_take(parser, '"')) {
        return false;
    }
    while (parser->position < parser->length) {
        unsigned char c = parser->data[parser->position++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (parser->position >= parser->length) {
                iml_fail(parser, "unterminated ProwseIML escape");
                return false;
            }
            c = parser->data[parser->position++];
            if (c == 'n') {
                c = '\n';
            } else if (c == 'r') {
                c = '\r';
            } else if (c == 't') {
                c = '\t';
            } else if (c != '\\' && c != '"') {
                iml_fail(parser, "invalid ProwseIML escape");
                return false;
            }
        }
        if (!buffer_append(out, &c, 1U)) {
            iml_fail(parser, "ProwseIML string exceeds safety limit");
            return false;
        }
    }
    iml_fail(parser, "unterminated ProwseIML string");
    return false;
}

static bool parse_iml_node(ImlParser *parser, size_t depth);

static bool parse_iml_attributes(ImlParser *parser, size_t depth) {
    if (!iml_take(parser, '(')) {
        return false;
    }
    iml_skip_space(parser);
    if (parser->position >= parser->length || parser->data[parser->position] != '@') {
        iml_fail(parser, "expected ProwseIML attribute list");
        return false;
    }
    ++parser->position;
    for (;;) {
        char attribute[PAGE2PDF_MAX_TAG_BYTES + 1U];
        Buffer value = {0};
        iml_skip_space(parser);
        if (parser->position < parser->length && parser->data[parser->position] == ')') {
            ++parser->position;
            return true;
        }
        if (!iml_take(parser, '(') || !iml_identifier(parser, attribute, sizeof(attribute)) ||
            !iml_string(parser, &value) || !iml_take(parser, ')') ||
            !renderer_attribute(parser->renderer, depth)) {
            free(value.data);
            iml_fail(parser, "invalid ProwseIML attribute");
            return false;
        }
        free(value.data);
    }
}

static bool parse_iml_element(ImlParser *parser, size_t depth) {
    char tag[PAGE2PDF_MAX_TAG_BYTES + 1U];
    if (!iml_identifier(parser, tag, sizeof(tag)) ||
        !renderer_start(parser->renderer, tag, depth)) {
        iml_fail(parser, "invalid ProwseIML element");
        return false;
    }
    iml_skip_space(parser);
    if (parser->position + 2U <= parser->length && parser->data[parser->position] == '(' &&
        parser->data[parser->position + 1U] == '@') {
        if (!parse_iml_attributes(parser, depth)) {
            return false;
        }
    }
    for (;;) {
        iml_skip_space(parser);
        if (parser->position >= parser->length) {
            iml_fail(parser, "unterminated ProwseIML element");
            return false;
        }
        if (parser->data[parser->position] == ')') {
            ++parser->position;
            if (!renderer_end(parser->renderer, tag, depth)) {
                iml_fail(parser, "mismatched ProwseIML element");
                return false;
            }
            return true;
        }
        if (!parse_iml_node(parser, depth + 1U)) {
            return false;
        }
    }
}

static bool parse_iml_text(ImlParser *parser, size_t depth) {
    Buffer text = {0};
    bool ok = iml_string(parser, &text) && iml_take(parser, ')') &&
              renderer_text(parser->renderer, text.data, text.length, depth, NULL);
    free(text.data);
    if (!ok) {
        iml_fail(parser, "invalid ProwseIML text");
    }
    return ok;
}

static bool parse_iml_node(ImlParser *parser, size_t depth) {
    char kind[16];
    if (!iml_take(parser, '(') || !iml_identifier(parser, kind, sizeof(kind))) {
        return false;
    }
    if (strcmp(kind, "element") == 0) {
        return parse_iml_element(parser, depth);
    }
    if (strcmp(kind, "text") == 0) {
        return parse_iml_text(parser, depth);
    }
    iml_fail(parser, "unsupported ProwseIML form");
    return false;
}

static bool compile_iml(const Buffer *input, Renderer *renderer, char *error,
                        size_t error_size) {
    ImlParser parser = {input->data, input->length, 0U, renderer, {0}};
    char root[16];
    bool have_child = false;
    if (!iml_take(&parser, '(') || !iml_identifier(&parser, root, sizeof(root)) ||
        strcmp(root, "document") != 0) {
        snprintf(error, error_size, "%s", parser.error[0] == '\0' ?
                 "expected a ProwseIML document" : parser.error);
        return false;
    }
    for (;;) {
        iml_skip_space(&parser);
        if (parser.position >= parser.length) {
            iml_fail(&parser, "unterminated ProwseIML document");
            break;
        }
        if (parser.data[parser.position] == ')') {
            ++parser.position;
            break;
        }
        if (!parse_iml_node(&parser, 0U)) {
            break;
        }
        have_child = true;
        if (renderer->error != HPDF_OK) {
            iml_fail(&parser, "libHaru rendering error");
            break;
        }
    }
    iml_skip_space(&parser);
    if (parser.error[0] != '\0' || parser.position != parser.length ||
        renderer->frame_count != 0U) {
        snprintf(error, error_size, "%s", parser.error[0] == '\0' ?
                 "invalid trailing ProwseIML data" : parser.error);
        return false;
    }
    (void)have_child;
    return true;
}

static InputFormat parse_format(const char *value) {
    if (strcmp(value, "auto") == 0) {
        return INPUT_AUTO;
    }
    if (strcmp(value, "iml") == 0) {
        return INPUT_IML;
    }
    if (strcmp(value, "vtd") == 0) {
        return INPUT_VTD;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *output_path = NULL;
    InputFormat format = INPUT_AUTO;
    Buffer input = {0};
    Renderer renderer;
    char error[256] = {0};
    int i;
    bool ok;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--format") == 0) {
            if (++i >= argc || (format = parse_format(argv[i])) == (InputFormat)-1) {
                fprintf(stderr, "page2pdf: --format requires auto, iml, or vtd\\n");
                return EXIT_FAILURE;
            }
            continue;
        }
        if (input_path == NULL) {
            input_path = argv[i];
        } else if (output_path == NULL) {
            output_path = argv[i];
        } else {
            print_usage(stderr);
            return EXIT_FAILURE;
        }
    }
    if (input_path == NULL || output_path == NULL || strcmp(output_path, "-") == 0) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }
    if (!read_input(input_path, &input)) {
        free(input.data);
        return EXIT_FAILURE;
    }
    if (format == INPUT_AUTO) {
        format = input.length >= 5U && memcmp(input.data, "PVTD1", 5U) == 0 ?
                 INPUT_VTD : INPUT_IML;
    }
    if (!renderer_init(&renderer)) {
        fprintf(stderr, "page2pdf: unable to initialize libHaru (0x%04X, %u)\\n",
                (unsigned int)renderer.error, (unsigned int)renderer.detail);
        renderer_destroy(&renderer);
        free(input.data);
        return EXIT_FAILURE;
    }
    ok = format == INPUT_VTD ? compile_vtd(&input, &renderer, error, sizeof(error)) :
                               compile_iml(&input, &renderer, error, sizeof(error));
    if (ok && HPDF_SaveToFile(renderer.document, output_path) != HPDF_OK) {
        snprintf(error, sizeof(error), "unable to write '%s'", output_path);
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "page2pdf: %s", error[0] == '\0' ? "PDF compilation failed" : error);
        if (renderer.error != HPDF_OK) {
            fprintf(stderr, " (libHaru 0x%04X, %u)", (unsigned int)renderer.error,
                    (unsigned int)renderer.detail);
        }
        fputc('\n', stderr);
    }
    renderer_destroy(&renderer);
    free(input.data);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
