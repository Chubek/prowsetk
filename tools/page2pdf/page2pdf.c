/* page2pdf -- lay out and paint ProwseVTD or ProwseIML as a PDF.
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
#include "graphics.h"

#define PAGE2PDF_MAX_INPUT_BYTES (16U * 1024U * 1024U)

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
    const unsigned char *data;
    size_t length;
    size_t position;
    Renderer *renderer;
    char error[192];
} ImlParser;

static void print_usage(FILE *stream) {
    fprintf(stream,
            "Usage: page2pdf [--format auto|iml|vtd] INPUT OUTPUT.pdf\n"
            "Lay out serialized ProwseIML or ProwseVTD as a PDF.\n"
            "Use '-' as INPUT to read standard input. OUTPUT must be a file.\n");
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
        fprintf(stderr, "page2pdf: cannot open input '%s': %s\n", path,
                strerror(errno));
        return false;
    }
    while ((count = fread(chunk, 1U, sizeof(chunk), input)) != 0U) {
        if (!buffer_append(out, chunk, count)) {
            fprintf(stderr, "page2pdf: input exceeds the 16 MiB safety limit\n");
            ok = false;
            break;
        }
    }
    if (ferror(input) != 0) {
        fprintf(stderr, "page2pdf: failed to read input '%s'\n", path);
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
            if (!renderer_attribute(renderer, identifier, value.data,
                                    value.length, depth) ||
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
            !renderer_attribute(parser->renderer, attribute, value.data,
                                value.length, depth)) {
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
                fprintf(stderr, "page2pdf: --format requires auto, iml, or vtd\n");
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
        fprintf(stderr, "page2pdf: unable to initialize libHaru (0x%04X, %u)\n",
                (unsigned int)renderer.error, (unsigned int)renderer.detail);
        renderer_destroy(&renderer);
        free(input.data);
        return EXIT_FAILURE;
    }
    ok = format == INPUT_VTD ? compile_vtd(&input, &renderer, error, sizeof(error)) :
                               compile_iml(&input, &renderer, error, sizeof(error));
    if (ok && !renderer_draw(&renderer)) {
        snprintf(error, sizeof(error), "unable to lay out or draw the document");
        ok = false;
    }
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
