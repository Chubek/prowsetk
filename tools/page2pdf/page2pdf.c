/* page2pdf -- lay out and paint ProwseEvent, ProwseVTD, or ProwseIML as a PDF.
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
    INPUT_EVENTS,
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

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
    char error[160];
} JsonParser;

static void print_usage(FILE *stream) {
    fprintf(stream,
            "Usage: page2pdf [--format auto|events|iml|vtd] INPUT OUTPUT.pdf\n"
            "Lay out serialized ProwseEvent NDJSON, ProwseIML, or ProwseVTD as a PDF.\n"
            "Use '-' as INPUT to read standard input. OUTPUT must be a file.\n");
}

static void buffer_clear(Buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0U;
}

static bool buffer_equals(const Buffer *buffer, const char *text) {
    const size_t length = strlen(text);
    return buffer->length == length &&
           (length == 0U || memcmp(buffer->data, text, length) == 0);
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

/* --- ProwseEvent NDJSON -------------------------------------------------- */

static void json_fail(JsonParser *parser, const char *message) {
    if (parser->error[0] == '\0') {
        snprintf(parser->error, sizeof(parser->error), "%s at byte %zu", message,
                 parser->position);
    }
}

static void json_skip_space(JsonParser *parser) {
    while (parser->position < parser->length &&
           (parser->data[parser->position] == ' ' ||
            parser->data[parser->position] == '\t' ||
            parser->data[parser->position] == '\r' ||
            parser->data[parser->position] == '\n')) {
        ++parser->position;
    }
}

static bool json_take(JsonParser *parser, unsigned char expected) {
    json_skip_space(parser);
    if (parser->position >= parser->length || parser->data[parser->position] != expected) {
        json_fail(parser, "invalid JSON syntax");
        return false;
    }
    ++parser->position;
    return true;
}

static bool json_append_codepoint(Buffer *out, unsigned value) {
    unsigned char bytes[4];
    size_t count = 0U;
    if (value <= 0x7fU) {
        bytes[count++] = (unsigned char)value;
    } else if (value <= 0x7ffU) {
        bytes[count++] = (unsigned char)(0xc0U | (value >> 6U));
        bytes[count++] = (unsigned char)(0x80U | (value & 0x3fU));
    } else if (value <= 0xffffU) {
        bytes[count++] = (unsigned char)(0xe0U | (value >> 12U));
        bytes[count++] = (unsigned char)(0x80U | ((value >> 6U) & 0x3fU));
        bytes[count++] = (unsigned char)(0x80U | (value & 0x3fU));
    } else if (value <= 0x10ffffU) {
        bytes[count++] = (unsigned char)(0xf0U | (value >> 18U));
        bytes[count++] = (unsigned char)(0x80U | ((value >> 12U) & 0x3fU));
        bytes[count++] = (unsigned char)(0x80U | ((value >> 6U) & 0x3fU));
        bytes[count++] = (unsigned char)(0x80U | (value & 0x3fU));
    } else {
        return false;
    }
    return buffer_append(out, bytes, count);
}

static int json_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10U);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10U);
    return -1;
}

static bool json_read_hex4(JsonParser *parser, unsigned *out) {
    unsigned value = 0U;
    size_t i;
    if (parser->length - parser->position < 4U) return false;
    for (i = 0U; i < 4U; ++i) {
        const int digit = json_hex(parser->data[parser->position++]);
        if (digit < 0) return false;
        value = (value << 4U) | (unsigned)digit;
    }
    *out = value;
    return true;
}

static bool json_read_string(JsonParser *parser, Buffer *out) {
    buffer_clear(out);
    json_skip_space(parser);
    if (parser->position >= parser->length || parser->data[parser->position++] != '"') {
        json_fail(parser, "expected JSON string");
        return false;
    }
    while (parser->position < parser->length) {
        unsigned char c = parser->data[parser->position++];
        if (c == '"') return true;
        if (c < 0x20U) {
            json_fail(parser, "control character in JSON string");
            return false;
        }
        if (c != '\\') {
            if (!buffer_append(out, &c, 1U)) break;
            continue;
        }
        if (parser->position >= parser->length) break;
        c = parser->data[parser->position++];
        if (c == '"' || c == '\\' || c == '/') {
            if (!buffer_append(out, &c, 1U)) break;
        } else if (c == 'b') {
            c = '\b'; if (!buffer_append(out, &c, 1U)) break;
        } else if (c == 'f') {
            c = '\f'; if (!buffer_append(out, &c, 1U)) break;
        } else if (c == 'n') {
            c = '\n'; if (!buffer_append(out, &c, 1U)) break;
        } else if (c == 'r') {
            c = '\r'; if (!buffer_append(out, &c, 1U)) break;
        } else if (c == 't') {
            c = '\t'; if (!buffer_append(out, &c, 1U)) break;
        } else if (c == 'u') {
            unsigned codepoint;
            if (!json_read_hex4(parser, &codepoint)) break;
            if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                unsigned low;
                if (parser->length - parser->position < 6U ||
                    parser->data[parser->position++] != '\\' ||
                    parser->data[parser->position++] != 'u' ||
                    !json_read_hex4(parser, &low) || low < 0xdc00U || low > 0xdfffU) {
                    json_fail(parser, "invalid JSON surrogate pair");
                    return false;
                }
                codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                    (low - 0xdc00U);
            } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                json_fail(parser, "invalid JSON surrogate");
                return false;
            }
            if (!json_append_codepoint(out, codepoint)) break;
        } else {
            json_fail(parser, "invalid JSON escape");
            return false;
        }
    }
    json_fail(parser, "unterminated or oversized JSON string");
    return false;
}

static bool json_skip_value(JsonParser *parser, unsigned nesting) {
    Buffer ignored = {0};
    bool ok = true;
    json_skip_space(parser);
    if (nesting > 64U || parser->position >= parser->length) return false;
    if (parser->data[parser->position] == '"') {
        ok = json_read_string(parser, &ignored);
    } else if (parser->data[parser->position] == '{' || parser->data[parser->position] == '[') {
        const unsigned char open = parser->data[parser->position++];
        const unsigned char close = open == '{' ? '}' : ']';
        json_skip_space(parser);
        if (parser->position < parser->length && parser->data[parser->position] == close) {
            ++parser->position;
        } else {
            while (ok) {
                if (open == '{') {
                    ok = json_read_string(parser, &ignored) && json_take(parser, ':');
                }
                if (ok) ok = json_skip_value(parser, nesting + 1U);
                json_skip_space(parser);
                if (!ok || parser->position >= parser->length) break;
                if (parser->data[parser->position] == close) {
                    ++parser->position;
                    break;
                }
                if (parser->data[parser->position++] != ',') {
                    ok = false;
                    break;
                }
            }
        }
    } else {
        const size_t start = parser->position;
        while (parser->position < parser->length &&
               !isspace(parser->data[parser->position]) &&
               parser->data[parser->position] != ',' &&
               parser->data[parser->position] != '}' &&
               parser->data[parser->position] != ']') ++parser->position;
        ok = parser->position != start;
    }
    buffer_clear(&ignored);
    if (!ok) json_fail(parser, "invalid JSON value");
    return ok;
}

static bool json_read_depth(JsonParser *parser, size_t *out) {
    size_t value = 0U;
    size_t start;
    json_skip_space(parser);
    start = parser->position;
    while (parser->position < parser->length &&
           isdigit(parser->data[parser->position]) != 0) {
        const unsigned digit = (unsigned)(parser->data[parser->position++] - '0');
        if (value > (SIZE_MAX - digit) / 10U) {
            json_fail(parser, "JSON depth overflow");
            return false;
        }
        value = value * 10U + digit;
    }
    if (parser->position == start || value > PAGE2PDF_MAX_NESTING) {
        json_fail(parser, "invalid JSON depth");
        return false;
    }
    *out = value;
    return true;
}

static bool compile_event_record(Slice line, Renderer *renderer, char *error,
                                 size_t error_size) {
    JsonParser parser = {line.data, line.length, 0U, {0}};
    Buffer key = {0}, kind = {0}, tag = {0}, name = {0}, value = {0};
    bool have_kind = false, have_tag = false, have_name = false, have_value = false;
    bool have_depth = false, ok = true;
    size_t depth = 0U;
    char identifier[PAGE2PDF_MAX_TAG_BYTES + 1U];
    if (!json_take(&parser, '{')) ok = false;
    while (ok) {
        json_skip_space(&parser);
        if (parser.position >= parser.length) { json_fail(&parser, "unterminated JSON object"); break; }
        if (parser.data[parser.position] == '}') { ++parser.position; break; }
        if (!json_read_string(&parser, &key) || !json_take(&parser, ':')) { ok = false; break; }
        if (buffer_equals(&key, "kind")) { ok = json_read_string(&parser, &kind); have_kind = ok; }
        else if (buffer_equals(&key, "tag")) { ok = json_read_string(&parser, &tag); have_tag = ok; }
        else if (buffer_equals(&key, "name")) { ok = json_read_string(&parser, &name); have_name = ok; }
        else if (buffer_equals(&key, "value")) { ok = json_read_string(&parser, &value); have_value = ok; }
        else if (buffer_equals(&key, "depth")) { ok = json_read_depth(&parser, &depth); have_depth = ok; }
        else ok = json_skip_value(&parser, 0U);
        json_skip_space(&parser);
        if (!ok || parser.position >= parser.length) break;
        if (parser.data[parser.position] == '}') { ++parser.position; break; }
        if (parser.data[parser.position++] != ',') { json_fail(&parser, "expected JSON object separator"); ok = false; break; }
    }
    json_skip_space(&parser);
    if (ok && parser.position != parser.length) { json_fail(&parser, "trailing JSON data"); ok = false; }
    if (ok && (!have_kind || !have_tag || !have_name || !have_value || !have_depth)) {
        json_fail(&parser, "ProwseEvent record is missing a required field");
        ok = false;
    }
    if (ok && buffer_equals(&kind, "start")) {
        ok = copy_identifier((Slice){tag.data, tag.length}, identifier, sizeof(identifier)) &&
             renderer_start(renderer, identifier, depth);
    } else if (ok && buffer_equals(&kind, "attribute")) {
        ok = copy_identifier((Slice){name.data, name.length}, identifier, sizeof(identifier)) &&
             renderer_attribute(renderer, identifier, value.data, value.length, depth);
    } else if (ok && buffer_equals(&kind, "text")) {
        ok = copy_identifier((Slice){tag.data, tag.length}, identifier, sizeof(identifier)) &&
             renderer_text(renderer, value.data, value.length, depth, identifier);
    } else if (ok && buffer_equals(&kind, "end")) {
        ok = copy_identifier((Slice){tag.data, tag.length}, identifier, sizeof(identifier)) &&
             renderer_end(renderer, identifier, depth);
    } else if (ok) {
        json_fail(&parser, "unknown ProwseEvent kind");
        ok = false;
    }
    if (!ok) snprintf(error, error_size, "%s", parser.error[0] == '\0' ?
                      "invalid ProwseEvent record" : parser.error);
    buffer_clear(&key); buffer_clear(&kind); buffer_clear(&tag); buffer_clear(&name); buffer_clear(&value);
    return ok;
}

static bool compile_events(const Buffer *input, Renderer *renderer, char *error,
                           size_t error_size) {
    size_t position = 0U;
    size_t record = 0U;
    if (input->length >= 3U && input->data[0] == 0xefU && input->data[1] == 0xbbU &&
        input->data[2] == 0xbfU) position = 3U;
    while (position < input->length) {
        size_t end = position;
        Slice line;
        while (end < input->length && input->data[end] != '\n') ++end;
        line.data = input->data + position;
        line.length = end - position;
        while (line.length != 0U &&
               (line.data[line.length - 1U] == '\r' || line.data[line.length - 1U] == ' ' ||
                line.data[line.length - 1U] == '\t')) --line.length;
        while (line.length != 0U && (line.data[0] == ' ' || line.data[0] == '\t')) {
            ++line.data; --line.length;
        }
        if (line.length != 0U) {
            ++record;
            if (!compile_event_record(line, renderer, error, error_size)) {
                char detail[192];
                snprintf(detail, sizeof(detail), "%s", error);
                snprintf(error, error_size, "invalid ProwseEvent record %zu: %s", record, detail);
                return false;
            }
            if (renderer->error != HPDF_OK) {
                snprintf(error, error_size, "libHaru failed while rendering event %zu", record);
                return false;
            }
        }
        position = end < input->length ? end + 1U : end;
    }
    if (record == 0U || renderer->frame_count != 0U) {
        snprintf(error, error_size, "incomplete ProwseEvent stream");
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
    if (strcmp(value, "events") == 0) {
        return INPUT_EVENTS;
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
                fprintf(stderr, "page2pdf: --format requires auto, events, iml, or vtd\n");
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
        size_t first = 0U;
        if (input.length >= 3U && input.data[0] == 0xefU && input.data[1] == 0xbbU &&
            input.data[2] == 0xbfU) first = 3U;
        while (first < input.length && isspace(input.data[first]) != 0) ++first;
        format = input.length >= 5U && memcmp(input.data, "PVTD1", 5U) == 0 ? INPUT_VTD :
                 (first < input.length && input.data[first] == '{' ? INPUT_EVENTS : INPUT_IML);
    }
    if (!renderer_init(&renderer)) {
        fprintf(stderr, "page2pdf: unable to initialize libHaru (0x%04X, %u)\n",
                (unsigned int)renderer.error, (unsigned int)renderer.detail);
        renderer_destroy(&renderer);
        free(input.data);
        return EXIT_FAILURE;
    }
    ok = format == INPUT_VTD ? compile_vtd(&input, &renderer, error, sizeof(error)) :
         format == INPUT_EVENTS ? compile_events(&input, &renderer, error, sizeof(error)) :
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
