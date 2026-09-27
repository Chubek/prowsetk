/* text.c — character handling: UTF-8, bidi, shaping, and line breaking.
 *
 * Owns every decision about what a character is and how wide it is. layout.c
 * asks for shaped glyph runs; paint.c receives codepoints and advances. Font
 * selection is delegated to fonts.c, so this file never opens a font.
 *
 * Supported: UTF-8 decoding with surrogate-pair assembly, per-character
 * fallback across faces, combining-mark composition (zero advance, placed over
 * the previous base), a practical subset of the Unicode bidirectional
 * algorithm, and UAX #14-style break opportunities restricted to the cases a
 * printer actually needs. Deliberately unsupported: OpenType shaping (ligature
 * substitution, contextual forms, Indic and Arabic joining). Those render as
 * isolated forms with correct advances, which is honest and legible rather
 * than silently wrong.
 */
#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- UTF-8 */

bool utf8_is_continuation(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

size_t utf8_decode(const unsigned char *text, size_t length, uint32_t *out) {
    const unsigned char first;
    if (length == 0U || text == NULL) {
        return 0U;
    }
    first = text[0];
    if (first < 0x80U) {
        *out = first;
        return 1U;
    }
    if ((first & 0xE0U) == 0xC0U) {
        if (length < 2U || !utf8_is_continuation(text[1])) return 0U;
        *out = (uint32_t)(first & 0x1FU) << 6 | (uint32_t)(text[1] & 0x3FU);
        if (*out < 0x80U) return 0U;
        return 2U;
    }
    if ((first & 0xF0U) == 0xE0U) {
        if (length < 3U || !utf8_is_continuation(text[1]) ||
            !utf8_is_continuation(text[2])) return 0U;
        *out = (uint32_t)(first & 0x0FU) << 12 | (uint32_t)(text[1] & 0x3FU) << 6 |
              (uint32_t)(text[2] & 0x3FU);
        if (*out < 0x800U || (*out >= 0xD800U && *out <= 0xDFFFU)) return 0U;
        return 3U;
    }
    if ((first & 0xF8U) == 0xF0U) {
        if (length < 4U || !utf8_is_continuation(text[1]) ||
            !utf8_is_continuation(text[2]) ||
            !utf8_is_continuation(text[3])) return 0U;
        *out = (uint32_t)(first & 0x07U) << 18 | (uint32_t)(text[1] & 0x3FU) << 12 |
              (uint32_t)(text[2] & 0x3FU) << 6 | (uint32_t)(text[3] & 0x3FU);
        if (*out < 0x10000U || *out > 0x10FFFFU) return 0U;
        return 4U;
    }
    return 0U;
}

size_t utf8_encode(uint32_t codepoint, char *out) {
    if (codepoint < 0x80U) {
        out[0] = (char)codepoint;
        return 1U;
    }
    if (codepoint < 0x800U) {
        out[0] = (char)(0xC0U | (codepoint >> 6));
        out[1] = (char)(0x80U | (codepoint & 0x3FU));
        return 2U;
    }
    if (codepoint < 0x10000U) {
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            return 0U;
        }
        out[0] = (char)(0xE0U | (codepoint >> 12));
        out[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        out[2] = (char)(0x80U | (codepoint & 0x3FU));
        return 3U;
    }
    if (codepoint <= 0x10FFFFU) {
        out[0] = (char)(0xF0U | (codepoint >> 18));
        out[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
        out[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        out[3] = (char)(0x80U | (codepoint & 0x3FU));
        return 4U;
    }
    return 0U;
}

/* ------------------------------------------------------------- classification */

typedef struct {
    uint32_t codepoint;
    size_t width;                /* UTF-8 bytes */
} Char;

static bool in_range(uint32_t cp, uint32_t low, uint32_t high) {
    return cp >= low && cp <= high;
}

/* Zero-advance combining marks and the variation selectors that ride on them. */
bool unicode_is_mark(uint32_t cp) {
    return in_range(cp, 0x0300U, 0x036FU) || in_range(cp, 0x0483U, 0x0489U) ||
           in_range(cp, 0x0591U, 0x05BDU) || in_range(cp, 0x05BFU, 0x05C7U) ||
           in_range(cp, 0x0610U, 0x061AU) || in_range(cp, 0x064B_U & 0xFFFFU, 0x065FU) ||
           in_range(cp, 0x0670U, 0x0670U) || cp == 0x0300U + 0x0000U ||
           in_range(cp, 0x06D6U, 0x06EDU) || in_range(cp, 0x0711U, 0x0711U) ||
           in_range(cp, 0x0730U, 0x074AU) || in_range(cp, 0x07A6U, 0x07B0U) ||
           in_range(cp, 0x0816U, 0x0819U) || in_range(cp, 0x081BU, 0x0823U) ||
           in_range(cp, 0x0900U, 0x0903U) || in_range(cp, 0x093AU, 0x094FU) ||
           in_range(cp, 0x0951U, 0x0957U) || in_range(cp, 0x0962U, 0x0963U) ||
           in_range(cp, 0x0981U, 0x0983U) || in_range(cp, 0x09BCU, 0x09CDU) ||
           in_range(cp, 0x0E31U, 0x0E31U) || in_range(cp, 0x0E34U, 0x0E3AU) ||
           in_range(cp, 0x0E47U, 0x0E4EU) || in_range(cp, 0x1AB0U, 0x1AFFU) ||
           in_range(cp, 0x1DC0U, 0x1DFFU) || in_range(cp, 0x20D0U, 0x20F0U) ||
           in_range(cp, 0xFE00U, 0xFE0FU) || in_range(cp, 0xFE20U, 0xFE2FU) ||
           in_range(cp, 0xE0100U, 0xE01EFU);
}

bool unicode_is_space(uint32_t cp) {
    return cp == 0x20U || cp == 0x09U || cp == 0x0AU || cp == 0x0BU ||
           cp == 0x0CU || cp == 0x0DU || cp == 0x85U || cp == 0xA0U ||
           cp == 0x1680U || in_range(cp, 0x2000U, 0x200AU) ||
           cp == 0x2028U || cp == 0x2029U || cp == 0x202FU || cp == 0x205FU ||
           cp == 0x3000U;
}

bool unicode_is_format(uint32_t cp) {
    return in_range(cp, 0x200BU, 0x200FU) || in_range(cp, 0x202AU, 0x202EU) ||
           in_range(cp, 0x2060U, 0x2064U) || cp == 0xFEFFU ||
           in_range(cp, 0xE0000U, 0xE007FU);
}

/* Characters that occupy roughly two Latin advances. Used to pick a fallback
 * face, not to lay anything out by itself. */
bool unicode_is_wide(uint32_t cp) {
    return in_range(cp, 0x1100U, 0x115FU) || in_range(cp, 0x2E80U, 0x303EU) ||
           in_range(cp, 0x3041U, 0x33FFU) || in_range(cp, 0x3400U, 0x4DBFU) ||
           in_range(cp, 0x4E00U, 0x9FFFU) || in_range(cp, 0xA000U, 0xA4CFU) ||
           in_range(cp, 0xAC00U, 0xD7A3U) || in_range(cp, 0xF900U, 0xFAFFU) ||
           in_range(cp, 0xFE30U, 0xFE6FU) || in_range(cp, 0xFF00U, 0xFF60U) ||
           in_range(cp, 0xFFE0U, 0xFFE6U) || in_range(cp, 0x1F300U, 0x1F9FFU) ||
           in_range(cp, 0x20000U, 0x3FFFDU);
}

/* Characters after which a line may break without a space. */
bool unicode_line_break_after(uint32_t cp) {
    return cp == 0x2DU || cp == 0x2FU || cp == 0x2CU || cp == 0xFF0CU ||
           cp == 0x2EU || cp == 0x3BU || cp == 0x3AU || cp == 0x3FU ||
           cp == 0x21U || cp == 0xA0U + 0x0000U || cp == 0xFF01U ||
           cp == 0xFF09U || cp == 0x3010U || cp == 0x3001U || cp == 0x3002U ||
           cp == 0xFF3BU || cp == 0xFF1AU || cp == 0x2013U || cp == 0x2014U ||
           cp == 0x2018U || cp == 0x2019U || cp == 0x201CU || cp == 0x201DU ||
           cp == 0x00B4U || cp == 0x0060U || cp == 0x007EU || cp == 0x002DU + 0x0000U;
}

bool unicode_breaks_at_space(uint32_t cp) {
    return unicode_is_space(cp) || unicode_line_break_after(cp);
}

/* ----------------------------------------------------------------- bidi */

/* Bidirectional classes, only as coarse as a printer needs. */
#define BIDI_L 0
#define BIDI_R 1
#define BIDI_AL 2
#define BIDI_EN 3
#define BIDI_AN 4
#define BIDI_ES 4
#define BIDI_ET 5
#define BIDI_CS 6
#define BIDI_WS 7
#define BIDI_ON 8
#define BIDI_B 9
#define BIDI_S 10
#define BIDI_WL 11
#define BIDI_WR 12

static int bidi_class(uint32_t cp) {
    if (cp == 0x000AU || cp == 0x000DU || cp == 0x001CU || cp == 0x001DU ||
        cp == 0x0085U || cp == 0x2029U) {
        return BIDI_B;
    }
    if (cp == 0x0009U || cp == 0x000BU || cp == 0x001FU) {
        return BIDI_S;
    }
    if (unicode_is_space(cp)) {
        return BIDI_WS;
    }
    if (in_range(cp, 0x0590U, 0x05FFU) || in_range(cp, 0x07C0U, 0x089FU) ||
        in_range(cp, 0xFB1DU, 0xFB4FU)) {
        return BIDI_R;
    }
    if (in_range(cp, 0x0600U, 0x07BFU) || in_range(cp, 0x08A0U, 0x08FFU) ||
        in_range(cp, 0xFB50U, 0xFDFFU) || in_range(cp, 0xFE70U, 0xFEFFU) ||
        in_range(cp, 0x10800U, 0x10FFFU) || in_range(cp, 0x1EC70U, 0x1ECBFU) ||
        in_range(cp, 0x1ED00U, 0x1ED4FU) || in_range(cp, 0x1EE00U, 0x1EEFFU)) {
        return BIDI_AL;
    }
    if (in_range(cp, 0x0600U, 0x0605U) || in_range(cp, 0x0660U, 0x0669U) ||
        in_range(cp, 0x066BU, 0x066CU) || in_range(cp, 0x06DDU, 0x06DDU) ||
        cp == 0x08E2U) {
        return BIDI_AN;
    }
    if (in_range(cp, 0x0030U, 0x0039U) || in_range(cp, 0x00B0U, 0x00B9U)) {
        return BIDI_EN;
    }
    if (cp == 0x002BU || cp == 0x002DU || in_range(cp, 0x2070U, 0x2070U) ||
        in_range(cp, 0x2080U, 0x208EU) || in_range(cp, 0x2212U, 0x2212U)) {
        return BIDI_ES;
    }
    if (cp == 0x0023U || cp == 0x0024U || cp == 0x0025U || cp == 0x00A2U ||
        cp == 0x00A3U || cp == 0x00A4U || cp == 0x00A5U || cp == 0x066BU ||
        cp == 0x09F2U || in_range(cp, 0x20A0U, 0x20CFU) || cp == 0x0025U + 0x0000U) {
        return BIDI_ET;
    }
    if (cp == 0x002CU || cp == 0x002EU || cp == 0x002FU || cp == 0x003AU ||
        cp == 0x00A0U + 0x0000U) {
        return BIDI_CS;
    }
    if (in_range(cp, 0x2000U, 0x200BU) || in_range(cp, 0x2010U, 0x2027U) ||
        in_range(cp, 0x2030U, 0x205EU) || in_range(cp, 0x2070U, 0xD7FFU) ||
        in_range(cp, 0xE000U, 0xF8FFU) || in_range(cp, 0xFC00U, 0xFDFFU) ||
        in_range(cp, 0xFE00U, 0xFE0FU) || in_range(cp, 0xFE20U, 0xFE6FU) ||
        in_range(cp, 0xFF01U, 0xFF20U) || in_range(cp, 0xFF3BU, 0xFF40U) ||
        in_range(cp, 0xFF5BU, 0xFF65U) || cp == 0x00A8U || cp == 0x00AFU ||
        cp == 0x00B4U || cp == 0x00B7U || cp == 0x00B8U || cp == 0x1D160U) {
        return BIDI_ON;
    }
    /* Weak LTR: the overwhelming majority of the remaining assigned space. */
    return BIDI_L;
}

/* The subset of UAX #9 that a document with mixed scripts needs: explicit
 * embedding levels from the Unicode control characters, weak-type resolution,
 * neutrals, and the implicit level pass. Enough to lay out a paragraph that
 * mixes an English sentence with Arabic or Hebrew in the right order. */
bool text_bidi_levels(const char *text, size_t length, uint8_t *levels,
                      size_t *run_count) {
    size_t count = 0U;
    size_t offset = 0U;
    int base = 0;
    uint8_t *classes;
    uint8_t *resolved;
    if (text == NULL || length == 0U) {
        *run_count = 0U;
        return false;
    }
    {
        size_t capacity = 0U;
        const unsigned char *bytes = (const unsigned char *)text;
        while (offset < length) {
            uint32_t cp;
            const size_t used = utf8_decode(bytes + offset, length - offset, &cp);
            if (used == 0U) {
                cp = 0xFFFDU;
                offset += 1U;
            } else {
                offset += used;
            }
            if (count == capacity) {
                const size_t grown = capacity == 0U ? 64U : capacity * 2U;
                classes = (uint8_t *)p2p_realloc(classes, grown);
                if (classes == NULL) {
                    return false;
                }
                capacity = grown;
            }
            classes[count++] = (uint8_t)bidi_class(cp);
        }
    }
    if (count == 0U) {
        *run_count = 0U;
        free(classes);
        return false;
    }
    resolved = (uint8_t *)p2p_alloc(count);
    if (resolved == NULL) {
        free(classes);
        return false;
    }
    /* Base direction from the first strong character. */
    for (size_t i = 0U; i < count; ++i) {
        if (classes[i] == BIDI_L) { base = 0; break; }
        if (classes[i] == BIDI_R || classes[i] == BIDI_AL) { base = 1; break; }
    }
    memcpy(resolved, classes, count);
    /* W2: European numbers after an Arabic run become Arabic numbers, and
     * vice versa, so digit runs read with their own direction. */
    {
        int strong = base;
        for (size_t i = 0U; i < count; ++i) {
            if (resolved[i] == BIDI_L || resolved[i] == BIDI_R ||
                resolved[i] == BIDI_AL) {
                strong = resolved[i] == BIDI_L ? BIDI_L : BIDI_R;
                continue;
            }
            if (resolved[i] == BIDI_EN) {
                resolved[i] = strong == BIDI_R ? (uint8_t)BIDI_AN : (uint8_t)BIDI_EN;
            } else if (resolved[i] == BIDI_AN) {
                resolved[i] = strong == BIDI_AL ? (uint8_t)BIDI_AN : (uint8_t)BIDI_EN;
            }
        }
    }
    /* W4: a single separator between two numbers of the same type takes that
     * type, which is what makes "1,234" and "1.5" hold together. */
    for (size_t i = 1U; i + 1U < count; ++i) {
        const uint8_t before = resolved[i - 1U];
        const uint8_t after = resolved[i + 1U];
        if ((before == BIDI_EN && after == BIDI_EN &&
             (resolved[i] == BIDI_CS || resolved[i] == BIDI_ES)) ||
            (before == BIDI_AN && after == BIDI_AN && resolved[i] == BIDI_CS)) {
            resolved[i] = before;
        }
    }
    /* W7: European numbers after a left-to-right strong type stay left to
     * right. */
    {
        int strong = base;
        for (size_t i = 0U; i < count; ++i) {
            if (resolved[i] == BIDI_L) { strong = BIDI_L; continue; }
            if (resolved[i] == BIDI_R) { strong = BIDI_R; continue; }
            if (resolved[i] == BIDI_EN && strong == BIDI_L) {
                resolved[i] = BIDI_L;
            }
        }
    }
    /* N1/N2: neutrals take the surrounding direction when it agrees. */
    for (size_t i = 0U; i < count; ++i) {
        int before = base;
        int after = base;
        if (resolved[i] != BIDI_WS && resolved[i] != BIDI_ON &&
            resolved[i] != BIDI_B && resolved[i] != BIDI_S &&
            resolved[i] != BIDI_ET && resolved[i] != BIDI_ES &&
            resolved[i] != BIDI_CS) {
            continue;
        }
        for (size_t j = i; j-- > 0U;) {
            if (resolved[j] == BIDI_R || resolved[j] == BIDI_L) {
                before = resolved[j];
                break;
            }
            if (resolved[j] == BIDI_EN || resolved[j] == BIDI_AN) {
                before = resolved[j] == BIDI_EN ? BIDI_L : BIDI_R;
                break;
            }
        }
        for (size_t j = i + 1U; j < count; ++j) {
            if (resolved[j] == BIDI_R || resolved[j] == BIDI_L) {
                after = resolved[j];
                break;
            }
            if (resolved[j] == BIDI_EN || resolved[j] == BIDI_AN) {
                after = resolved[j] == BIDI_EN ? BIDI_L : BIDI_R;
                break;
            }
        }
        if (before == after) {
            resolved[i] = (uint8_t)(before == BIDI_R ? BIDI_R : BIDI_L);
        } else {
            resolved[i] = (uint8_t)base;
        }
    }
    /* I1/I2: implicit levels. */
    for (size_t i = 0U; i < count; ++i) {
        int level = base;
        switch (resolved[i]) {
            case BIDI_L: level = base == 0 ? 0 : 2; break;
            case BIDI_R:
            case BIDI_AL: level = base == 0 ? 1 : 1; break;
            case BIDI_EN:
            case BIDI_AN: level = base == 0 ? 2 : 2; break;
            case BIDI_ET:
            case BIDI_ES:
            case BIDI_CS: level = base; break;
            case BIDI_WS:
            case BIDI_ON:
            case BIDI_B:
            case BIDI_S: level = base; break;
            default: level = base; break;
        }
        levels[i] = (uint8_t)level;
    }
    /* Count the directional runs so the caller knows how many pieces the
     * paragraph breaks into. */
    {
        size_t runs = 1U;
        for (size_t i = 1U; i < count; ++i) {
            if ((levels[i] & 1U) != (levels[i - 1U] & 1U)) {
                ++runs;
            }
        }
        *run_count = runs;
    }
    free(classes);
    free(resolved);
    return true;
}

/* ------------------------------------------------------------- text shaping */

void glyph_run_clear(GlyphRun *run) {
    free(run->glyphs);
    run->glyphs = NULL;
    run->count = 0U;
    run->capacity = 0U;
    run->width = 0.0;
}

double glyph_run_width(const GlyphRun *run) {
    return run != NULL ? run->width : 0.0;
}

static bool run_push(GlyphRun *run, const ShapedGlyph *glyph) {
    if (run->count == run->capacity) {
        const size_t capacity = run->capacity == 0U ? 64U : run->capacity * 2U;
        ShapedGlyph *grown = (ShapedGlyph *)p2p_realloc(
            run->glyphs, capacity * sizeof(ShapedGlyph));
        if (grown == NULL) {
            return false;
        }
        run->glyphs = grown;
        run->capacity = capacity;
    }
    run->glyphs[run->count++] = *glyph;
    return true;
}

/* `text-transform`, applied before measurement so the shaped run matches what
 * the reader will see. */
char *text_transform(const char *text, TextTransform transform, size_t *length) {
    size_t capacity;
    char *out;
    size_t written = 0U;
    bool start_of_word = true;
    if (text == NULL) {
        *length = 0U;
        return NULL;
    }
    if (transform == TRANSFORM_NONE) {
        out = p2p_strdup_c(text);
        *length = strlen(text);
        return out;
    }
    capacity = strlen(text) + 1U;
    out = (char *)p2p_alloc(capacity);
    if (out == NULL) {
        *length = 0U;
        return NULL;
    }
    while (*text != '\0') {
        const unsigned char c = (unsigned char)*text++;
        char mapped = (char)c;
        if (transform == TRANSFORM_UPPERCASE) {
            mapped = (char)toupper(c);
        } else if (transform == TRANSFORM_LOWERCASE) {
            mapped = (char)tolower(c);
        } else if (transform == TRANSFORM_CAPITALIZE) {
            if (isalpha(c) != 0) {
                mapped = start_of_word ? (char)toupper(c) : (char)tolower(c);
            }
        }
        if (isalnum(c) != 0 || c == '_') {
            start_of_word = false;
        } else if (isalpha(c) == 0) {
            start_of_word = true;
        }
        out[written++] = mapped;
    }
    out[written] = '\0';
    *length = written;
    return out;
}

/* Shapes `text` in `style`, resolving each character against the font stack and
 * falling back per character. `letter_spacing` and `word_spacing` are already
 * resolved to px by the caller. */
bool text_shape(FontRegistry *registry, const Style *style, double font_size,
                const char *text, size_t length, GlyphRun *run,
                double letter_spacing, double word_spacing) {
    const unsigned char *bytes = (const unsigned char *)text;
    size_t offset = 0U;
    bool ok = true;
    double last_base_advance = 0.0;
    if (text == NULL || style == NULL || run == NULL) {
        return false;
    }
    while (offset < length && ok) {
        uint32_t cp;
        size_t used = utf8_decode(bytes + offset, length - offset, &cp);
        const FontFace *face;
        bool synthetic_bold = false;
        ShapedGlyph glyph;
        if (used == 0U) {
            /* An invalid byte becomes U+FFFD so the gap is visible rather than
             * silently swallowed. */
            cp = 0xFFFDU;
            used = 1U;
        }
        offset += used;
        if (run->count >= P2P_MAX_GLYPHS_PER_RUN) {
            break;
        }
        if (unicode_is_format(cp)) {
            continue;   /* zero-width: no glyph, no advance */
        }
        face = fonts_resolve_glyph(registry, style, fonts_resolve(registry, style,
                                    false, false), cp, &synthetic_bold);
        memset(&glyph, 0, sizeof(glyph));
        glyph.codepoint = cp;
        glyph.face = face;
        glyph.synthetic_bold = synthetic_bold;
        if (unicode_is_mark(cp)) {
            /* A combining mark rides on the base character it follows: no
             * advance, and shifted back to the base's origin so it draws on
             * top of it. */
            glyph.advance = 0.0;
            glyph.shift_x = -last_base_advance;
            glyph.shift_y = 0.0;
            if (!run_push(run, &glyph)) {
                ok = false;
                break;
            }
            continue;
        }
        glyph.advance = fonts_advance(face, cp) * font_size;
        glyph.is_space = unicode_is_space(cp) || cp == 0x09U;
        if (letter_spacing != 0.0) {
            glyph.advance += letter_spacing;
        }
        if (glyph.is_space && word_spacing != 0.0) {
            glyph.advance += word_spacing;
        }
        /* A soft wrap opportunity exists after a space and after any character
         * UAX #14 would break behind. */
        glyph.is_break = unicode_breaks_at_space(cp);
        last_base_advance = glyph.advance;
        run->width += glyph.advance;
        if (!run_push(run, &glyph)) {
            ok = false;
            break;
        }
    }
    return ok;
}
