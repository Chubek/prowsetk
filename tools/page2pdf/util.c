/* util.c — allocation accounting, ASCII string helpers, CSS units, colors.
 *
 * Nothing here knows about layout or painting. The tool is a bounded consumer
 * of untrusted IR, so allocation failure is tracked rather than propagated
 * through every return value: p2p_oom() latches, every subsequent allocation
 * fails, and the CLI reports a single diagnostic.
 */
#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool out_of_memory = false;

bool p2p_oom(void) {
    return out_of_memory;
}

void p2p_report_oom(void) {
    fputs("page2pdf: out of memory\n", stderr);
}

void *p2p_alloc(size_t size) {
    void *pointer;
    if (out_of_memory) {
        return NULL;
    }
    pointer = malloc(size != 0U ? size : 1U);
    if (pointer == NULL) {
        out_of_memory = true;
    }
    return pointer;
}

void *p2p_calloc(size_t count, size_t size) {
    void *pointer;
    if (out_of_memory) {
        return NULL;
    }
    pointer = calloc(count != 0U ? count : 1U, size != 0U ? size : 1U);
    if (pointer == NULL) {
        out_of_memory = true;
    }
    return pointer;
}

void *p2p_realloc(void *pointer, size_t size) {
    void *grown;
    if (out_of_memory) {
        return NULL;
    }
    grown = realloc(pointer, size != 0U ? size : 1U);
    if (grown == NULL) {
        out_of_memory = true;
    }
    return grown;
}

char *p2p_strdup(const unsigned char *bytes, size_t length) {
    char *copy = (char *)p2p_alloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }
    if (length != 0U) {
        memcpy(copy, bytes, length);
    }
    copy[length] = '\0';
    return copy;
}

char *p2p_strdup_c(const char *text) {
    return text == NULL ? NULL : p2p_strdup((const unsigned char *)text, strlen(text));
}

char *p2p_format(const char *format, ...) {
    va_list args;
    va_list retry;
    int needed;
    char *out;
    va_start(args, format);
    va_copy(retry, args);
    needed = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (needed < 0) {
        va_end(retry);
        return NULL;
    }
    out = (char *)p2p_alloc((size_t)needed + 1U);
    if (out != NULL) {
        vsnprintf(out, (size_t)needed + 1U, format, retry);
    }
    va_end(retry);
    return out;
}

/* ------------------------------------------------------------------ strings */

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool p2p_iequals(const char *a, const char *b) {
    size_t i;
    if (a == NULL || b == NULL) {
        return false;
    }
    for (i = 0U; a[i] != '\0' && b[i] != '\0'; ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return a[i] == '\0' && b[i] == '\0';
}

bool p2p_starts_with(const char *text, const char *prefix) {
    size_t i;
    if (text == NULL || prefix == NULL) {
        return false;
    }
    for (i = 0U; prefix[i] != '\0'; ++i) {
        if (text[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

bool p2p_ends_with(const char *text, const char *suffix) {
    size_t text_length, suffix_length;
    if (text == NULL || suffix == NULL) {
        return false;
    }
    text_length = strlen(text);
    suffix_length = strlen(suffix);
    if (suffix_length > text_length) {
        return false;
    }
    return memcmp(text + text_length - suffix_length, suffix, suffix_length) == 0;
}

const char *p2p_istrstr(const char *haystack, const char *needle) {
    size_t needle_length;
    if (haystack == NULL || needle == NULL) {
        return NULL;
    }
    needle_length = strlen(needle);
    if (needle_length == 0U) {
        return haystack;
    }
    for (; *haystack != '\0'; ++haystack) {
        size_t i;
        for (i = 0U; i < needle_length; ++i) {
            if (lower_ascii(haystack[i]) != lower_ascii(needle[i])) {
                break;
            }
        }
        if (i == needle_length) {
            return haystack;
        }
    }
    return NULL;
}

static bool is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

char *p2p_skip_space(char *text) {
    while (text != NULL && is_space_char(*text)) {
        ++text;
    }
    return text;
}

char *p2p_trim(char *text) {
    char *end;
    if (text == NULL) {
        return NULL;
    }
    while (is_space_char(*text)) {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && is_space_char(end[-1])) {
        *--end = '\0';
    }
    return text;
}

void p2p_lower(char *text) {
    for (; text != NULL && *text != '\0'; ++text) {
        *text = lower_ascii(*text);
    }
}

float p2p_clampf(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

double p2p_clamp(double value, double low, double high) {
    return value < low ? low : (value > high ? high : value);
}

/* -------------------------------------------------------------------- units */

Value value_make(double number, Unit unit) {
    Value value;
    value.number = number;
    value.unit = (signed char)unit;
    return value;
}

Value value_auto(void) {
    return value_make(0.0, UNIT_AUTO);
}

Value value_number(double number) {
    return value_make(number, UNIT_NUMBER);
}

bool value_is_auto(Value value) {
    return value.unit == (signed char)UNIT_AUTO;
}

bool value_is_length(Value value) {
    return value.unit >= (signed char)UNIT_PX &&
           value.unit <= (signed char)UNIT_VMAX;
}

bool value_is_number(Value value) {
    return value.unit == (signed char)UNIT_NUMBER;
}

bool value_is_percentage(Value value) {
    return value.unit == (signed char)UNIT_PCT;
}

bool value_is_zero(Value value) {
    return value.number == 0.0 && value.unit != (signed char)UNIT_AUTO;
}

double value_resolve_length(Value value, double reference, double font_size,
                            double root_font_size, double viewport_width,
                            double viewport_height, bool zero_for_auto) {
    double scale = 1.0;
    switch ((Unit)value.unit) {
        case UNIT_AUTO:
            return zero_for_auto ? 0.0 : 0.0;
        case UNIT_NUMBER:
        case UNIT_PX:
            return value.number;
        case UNIT_PT:
            return value.number;
        case UNIT_PC:
            return value.number * 12.0;
        case UNIT_IN:
            return value.number * 72.0;
        case UNIT_CM:
            return value.number * 72.0 / 2.54;
        case UNIT_MM:
            return value.number * 72.0 / 25.4;
        case UNIT_Q:
            return value.number * 72.0 / 101.6;
        case UNIT_EM:
            return value.number * font_size;
        case UNIT_REM:
            return value.number * root_font_size;
        case UNIT_EX:
            return value.number * font_size * 0.5;
        case UNIT_CH:
            return value.number * font_size * 0.5;
        case UNIT_PCT:
            return reference * value.number / 100.0;
        case UNIT_VW:
            return viewport_width * value.number / 100.0;
        case UNIT_VH:
            return viewport_height * value.number / 100.0;
        case UNIT_VMIN:
            scale = viewport_width < viewport_height ? viewport_width : viewport_height;
            return scale * value.number / 100.0;
        case UNIT_VMAX:
            scale = viewport_width > viewport_height ? viewport_width : viewport_height;
            return scale * value.number / 100.0;
    }
    return 0.0;
}

/* ------------------------------------------------------------------- colors */

typedef struct {
    const char *name;
    unsigned char r, g, b;
} NamedColor;

/* The CSS Color Module Level 4 named colors, plus `transparent`. */
static const NamedColor kNamedColors[] = {
    {"transparent", 0, 0, 0},
    {"aliceblue", 240, 248, 255}, {"antiquewhite", 250, 235, 215},
    {"aqua", 0, 255, 255}, {"aquamarine", 127, 255, 212},
    {"azure", 240, 255, 255}, {"beige", 245, 245, 220},
    {"bisque", 255, 228, 196}, {"black", 0, 0, 0},
    {"blanchedalmond", 255, 235, 205}, {"blue", 0, 0, 255},
    {"blueviolet", 138, 43, 226}, {"brown", 165, 42, 42},
    {"burlywood", 222, 184, 135}, {"cadetblue", 95, 158, 160},
    {"chartreuse", 127, 255, 0}, {"chocolate", 210, 105, 30},
    {"coral", 255, 127, 80}, {"cornflowerblue", 100, 149, 237},
    {"cornsilk", 255, 248, 220}, {"crimson", 220, 20, 60},
    {"cyan", 0, 255, 255}, {"darkblue", 0, 0, 139},
    {"darkcyan", 0, 139, 139}, {"darkgoldenrod", 184, 134, 11},
    {"darkgray", 169, 169, 169}, {"darkgreen", 0, 100, 0},
    {"darkgrey", 169, 169, 169}, {"darkkhaki", 189, 183, 107},
    {"darkmagenta", 139, 0, 139}, {"darkolivegreen", 85, 107, 47},
    {"darkorange", 255, 140, 0}, {"darkorchid", 153, 50, 204},
    {"darkred", 139, 0, 0}, {"darksalmon", 233, 150, 122},
    {"darkseagreen", 143, 188, 143}, {"darkslateblue", 72, 61, 139},
    {"darkslategray", 47, 79, 79}, {"darkslategrey", 47, 79, 79},
    {"darkturquoise", 0, 206, 209}, {"darkviolet", 148, 0, 211},
    {"deeppink", 255, 20, 147}, {"deepskyblue", 0, 191, 255},
    {"dimgray", 105, 105, 105}, {"dimgrey", 105, 105, 105},
    {"dodgerblue", 30, 144, 255}, {"firebrick", 178, 34, 34},
    {"floralwhite", 255, 250, 240}, {"forestgreen", 34, 139, 34},
    {"fuchsia", 255, 0, 255}, {"gainsboro", 220, 220, 220},
    {"ghostwhite", 248, 248, 255}, {"gold", 255, 215, 0},
    {"goldenrod", 218, 165, 32}, {"gray", 128, 128, 128},
    {"green", 0, 128, 0}, {"greenyellow", 173, 255, 47},
    {"grey", 128, 128, 128}, {"honeydew", 240, 255, 240},
    {"hotpink", 255, 105, 180}, {"indianred", 205, 92, 92},
    {"indigo", 75, 0, 130}, {"ivory", 255, 255, 240},
    {"khaki", 240, 230, 140}, {"lavender", 230, 230, 250},
    {"lavenderblush", 255, 240, 245}, {"lawngreen", 124, 252, 0},
    {"lemonchiffon", 255, 250, 205}, {"lightblue", 173, 216, 230},
    {"lightcoral", 240, 128, 128}, {"lightcyan", 224, 255, 255},
    {"lightgoldenrodyellow", 250, 250, 210}, {"lightgray", 211, 211, 211},
    {"lightgreen", 144, 238, 144}, {"lightgrey", 211, 211, 211},
    {"lightpink", 255, 182, 193}, {"lightsalmon", 255, 160, 122},
    {"lightseagreen", 32, 178, 170}, {"lightskyblue", 135, 206, 250},
    {"lightslategray", 119, 136, 153}, {"lightslategrey", 119, 136, 153},
    {"lightsteelblue", 176, 196, 222}, {"lightyellow", 255, 255, 224},
    {"lime", 0, 255, 0}, {"limegreen", 50, 205, 50},
    {"linen", 250, 240, 230}, {"magenta", 255, 0, 255},
    {"maroon", 128, 0, 0}, {"mediumaquamarine", 102, 205, 170},
    {"mediumblue", 0, 0, 205}, {"mediumorchid", 186, 85, 211},
    {"mediumpurple", 147, 112, 219}, {"mediumseagreen", 60, 179, 113},
    {"mediumslateblue", 123, 104, 238}, {"mediumspringgreen", 0, 250, 154},
    {"mediumturquoise", 72, 209, 204}, {"mediumvioletred", 199, 21, 133},
    {"midnightblue", 25, 25, 112}, {"mintcream", 245, 255, 250},
    {"mistyrose", 255, 228, 225}, {"moccasin", 255, 228, 181},
    {"navajowhite", 255, 222, 173}, {"navy", 0, 0, 128},
    {"oldlace", 253, 245, 230}, {"olive", 128, 128, 0},
    {"olivedrab", 107, 142, 35}, {"orange", 255, 165, 0},
    {"orangered", 255, 69, 0}, {"orchid", 218, 112, 214},
    {"palegoldenrod", 238, 232, 170}, {"palegreen", 152, 251, 152},
    {"paleturquoise", 175, 238, 238}, {"palevioletred", 219, 112, 147},
    {"papayawhip", 255, 239, 213}, {"peachpuff", 255, 218, 185},
    {"peru", 205, 133, 63}, {"pink", 255, 192, 203},
    {"plum", 221, 160, 221}, {"powderblue", 176, 224, 230},
    {"purple", 128, 0, 128}, {"rebeccapurple", 102, 51, 153},
    {"red", 255, 0, 0}, {"rosybrown", 188, 143, 143},
    {"royalblue", 65, 105, 225}, {"saddlebrown", 139, 69, 19},
    {"salmon", 250, 128, 114}, {"sandybrown", 244, 164, 96},
    {"seagreen", 46, 139, 87}, {"seashell", 255, 245, 238},
    {"sienna", 160, 82, 45}, {"silver", 192, 192, 192},
    {"skyblue", 135, 206, 235}, {"slateblue", 106, 90, 205},
    {"slategray", 112, 128, 144}, {"slategrey", 112, 128, 144},
    {"snow", 255, 250, 250}, {"springgreen", 0, 255, 127},
    {"steelblue", 70, 130, 180}, {"tan", 210, 180, 140},
    {"teal", 0, 128, 128}, {"thistle", 216, 191, 216},
    {"tomato", 255, 99, 71}, {"turquoise", 64, 224, 208},
    {"violet", 238, 130, 238}, {"wheat", 245, 222, 179},
    {"white", 255, 255, 255}, {"whitesmoke", 245, 245, 245},
    {"yellow", 255, 255, 0}, {"yellowgreen", 154, 205, 50}
};

const char *color_name_lookup(const char *name, Color *out) {
    size_t low = 0U, high = sizeof(kNamedColors) / sizeof(kNamedColors[0]);
    char lowered[64];
    size_t length;
    if (name == NULL || out == NULL) {
        return NULL;
    }
    length = strlen(name);
    if (length == 0U || length >= sizeof(lowered)) {
        return NULL;
    }
    for (size_t i = 0U; i < length; ++i) {
        lowered[i] = lower_ascii(name[i]);
    }
    lowered[length] = '\0';
    while (low < high) {
        size_t mid = low + (high - low) / 2U;
        const int order = strcmp(kNamedColors[mid].name, lowered);
        if (order == 0) {
            out->r = kNamedColors[mid].r / 255.0f;
            out->g = kNamedColors[mid].g / 255.0f;
            out->b = kNamedColors[mid].b / 255.0f;
            /* The table stores `transparent` as a zero rgb triple. */
            out->a = strcmp(lowered, "transparent") == 0 ? 0.0f : 1.0f;
            return kNamedColors[mid].name;
        }
        if (order < 0) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    return NULL;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex_digits(const char *text, size_t count, unsigned *out) {
    unsigned value = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const int digit = hex_value(text[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (unsigned)digit;
    }
    *out = value;
    return true;
}

static double clamp_unit(double number) {
    return p2p_clamp(number, 0.0, 1.0);
}

static double hue_to_channel(double p, double q, double t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

static bool parse_hsl(double hue, double saturation, double lightness, Color *out) {
    double r, g, b;
    hue = hue - 360.0 * floor(hue / 360.0);
    if (lightness <= 0.0) {
        r = g = b = 0.0;
    } else if (lightness >= 1.0) {
        r = g = b = 1.0;
    } else {
        const double q = lightness < 0.5
            ? lightness * (1.0 + saturation)
            : lightness + saturation - lightness * saturation;
        const double p = 2.0 * lightness - q;
        r = hue_to_channel(p, q, hue / 360.0 + 1.0 / 3.0);
        g = hue_to_channel(p, q, hue / 360.0);
        b = hue_to_channel(p, q, hue / 360.0 - 1.0 / 3.0);
    }
    out->r = (float)clamp_unit(r);
    out->g = (float)clamp_unit(g);
    out->b = (float)clamp_unit(b);
    out->a = 1.0f;
    return true;
}

/* Parses the comma or space separated arguments of rgb()/rgba()/hsl()/hsla().
 * Returns the number of components written to `components` (1..4). */
static int parse_color_components(const char *text, double *components,
                                  bool *has_alpha) {
    const char *cursor = text;
    int count = 0;
    *has_alpha = false;
    while (count < 4) {
        char *end = NULL;
        double number;
        while (*cursor == ' ' || *cursor == '/' || *cursor == ',') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if ((*cursor == 'n' || *cursor == 'N') &&
            p2p_iequals(cursor, "none")) {
            number = 0.0;
            while (*cursor != '\0' && *cursor != ')' && *cursor != ',' && *cursor != ' ') {
                ++cursor;
            }
        } else {
            number = strtod(cursor, &end);
            if (end == cursor) {
                break;
            }
            cursor = end;
            if (p2p_iequals(cursor, "deg")) {
                cursor += 3;
            } else if (p2p_iequals(cursor, "grad")) {
                cursor += 4;
            } else if (p2p_iequals(cursor, "rad")) {
                cursor += 3;
            } else if (p2p_iequals(cursor, "turn")) {
                number *= 360.0;
                cursor += 4;
            }
        }
        if (*cursor == '%') {
            number = count == 0 ? number * 100.0 : number;
            if (count == 0 && number > 360.0) {
                number /= 100.0;   /* bare hue in hsla() */
            }
            ++cursor;
        } else if (count > 0) {
            number /= 255.0;
        }
        components[count] = number;
        ++count;
        while (*cursor == ' ') {
            ++cursor;
        }
    }
    if (count >= 4) {
        *has_alpha = true;
    }
    return count;
}

bool color_parse(const char *value, Color current, Color *out) {
    char lowered[64];
    char buffer[256];
    char *text;
    size_t length;
    if (value == NULL || out == NULL) {
        return false;
    }
    length = strlen(value);
    if (length == 0U || length >= sizeof(buffer)) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        buffer[i] = value[i];
    }
    buffer[length] = '\0';
    text = p2p_trim(buffer);
    if (*text == '\0') {
        return false;
    }
    if (p2p_iequals(text, "currentcolor") || p2p_iequals(text, "currentColor")) {
        *out = current;
        return true;
    }
    if (color_name_lookup(text, out) != NULL) {
        return true;
    }
    if (*text == '#') {
        unsigned components[4] = {0U, 0U, 0U, 0xFFU};
        const size_t digits = length - 1U;
        if (digits == 3U || digits == 4U) {
            unsigned packed;
            if (!parse_hex_digits(text + 1, 4U, &packed)) {
                return false;
            }
            out->r = (float)(((packed >> 12) & 0xFU) * 17U) / 255.0f;
            out->g = (float)(((packed >> 8) & 0xFU) * 17U) / 255.0f;
            out->b = (float)(((packed >> 4) & 0xFU) * 17U) / 255.0f;
            out->a = (float)((packed & 0xFU) * 17U) / 255.0f;
            return true;
        }
        if (digits == 6U || digits == 8U) {
            if (!parse_hex_digits(text + 1, digits, components)) {
                return false;
            }
            out->r = components[0] / 255.0f;
            out->g = components[1] / 255.0f;
            out->b = components[2] / 255.0f;
            out->a = digits == 8U ? components[3] / 255.0f : 1.0f;
            return true;
        }
        return false;
    }
    for (size_t i = 0U; i < strlen(text) && i + 1U < sizeof(lowered); ++i) {
        lowered[i] = (text[i] >= 'A' && text[i] <= 'Z') ? (char)(text[i] - 'A' + 'a')
                                                         : text[i];
    }
    lowered[strlen(text) < sizeof(lowered) ? strlen(text) : sizeof(lowered) - 1U] = '\0';
    if (p2p_starts_with(lowered, "rgb(") || p2p_starts_with(lowered, "rgba(") ||
        p2p_starts_with(lowered, "hsl(") || p2p_starts_with(lowered, "hsla(")) {
        const char *open = strchr(text, '(');
        const char *close = strrchr(text, ')');
        char inner[128];
        double components[4] = {0.0, 0.0, 0.0, 1.0};
        bool has_alpha = false;
        int count;
        if (open == NULL || close == NULL || close < open ||
            (size_t)(close - open - 1) >= sizeof(inner)) {
            return false;
        }
        memcpy(inner, open + 1, (size_t)(close - open - 1));
        inner[close - open - 1] = '\0';
        count = parse_color_components(inner, components, &has_alpha);
        if (count < 1) {
            return false;
        }
        if (p2p_starts_with(lowered, "hsl")) {
            if (!parse_hsl(components[0], count > 1 ? clamp_unit(components[1]) : 0.0,
                           count > 2 ? clamp_unit(components[2]) : 0.5, out)) {
                return false;
            }
        } else {
            out->r = (float)clamp_unit(count > 0 ? components[0] : 0.0);
            out->g = (float)clamp_unit(count > 1 ? components[1] : 0.0);
            out->b = (float)clamp_unit(count > 2 ? components[2] : 0.0);
            out->a = has_alpha ? (float)clamp_unit(components[3]) : 1.0f;
        }
        return true;
    }
    return false;
}
