/* fonts.c — font discovery, TrueType/OpenType parsing, and fallback.
 *
 * Owns every decision about which face draws which character. layout.c and
 * text.c ask for a face by style and never touch a font file. The base-14
 * faces are registered first and are always present, so the tool still emits a
 * correct PDF on a machine with no fonts installed: embedded faces improve
 * fidelity, they are not a requirement.
 *
 * Supported containers: TrueType, and OpenType/CFF. Both are measured from
 * `head`, `hhea`, `hmtx` and `cmap`; TrueType-only `glyf`/`loca` add local ink
 * extents when they are present. TrueType Collections are accepted and the
 * first font in the collection is registered.
 */
#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct FontRegistry {
    FontFace *faces;
    size_t count;
    size_t capacity;
    size_t total_bytes;
    size_t files_scanned;
    bool embed;
    HPDF_Doc document;
    /* The six standard PDF faces, registered as ordinary faces. */
    const FontFace *base14[2][2][2];   /* [mono][bold][italic] */
};

/* ------------------------------------------------------------ sfnt reading */

static uint16_t read_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t read_i16(const unsigned char *p) {
    return (int16_t)read_u16(p);
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool find_table(const unsigned char *data, size_t length,
                       size_t num_tables, const char *tag, TtfTable *out) {
    if (num_tables == 0U || length < 12U) {
        return false;
    }
    for (size_t i = 0U; i < num_tables; ++i) {
        const size_t entry = 12U + i * 16U;
        uint32_t offset;
        uint32_t table_length;
        if (entry + 16U > length) {
            return false;
        }
        if (memcmp(data + entry, tag, 4) != 0) {
            continue;
        }
        offset = read_u32(data + entry + 8U);
        table_length = read_u32(data + entry + 12U);
        if (offset > length || table_length > length - offset) {
            return false;
        }
        out->data = data + offset;
        out->length = table_length;
        return true;
    }
    return false;
}

/* Chooses the best cmap subtable: full Unicode, then BMP Unicode, then any
 * Unicode platform, then the Microsoft symbol range. */
static void select_cmap_subtable(const unsigned char *cmap, size_t length,
                                 TtfFont *font) {
    const size_t count = length >= 4U ? read_u16(cmap + 2U) : 0U;
    static const struct {
        int platform;
        int encoding;
    } kPreference[] = {
        {3, 10}, {0, 6}, {0, 4}, {3, 1}, {0, 3}, {0, 2}, {0, 1}, {0, 0}, {3, 0}
    };
    if (count == 0U) {
        return;
    }
    for (size_t pass = 0U; pass < sizeof(kPreference) / sizeof(kPreference[0]); ++pass) {
        for (size_t i = 0U; i < count; ++i) {
            const size_t entry = 4U + i * 8U;
            uint16_t platform;
            uint16_t encoding;
            uint32_t offset;
            if (entry + 8U > length) {
                return;
            }
            platform = read_u16(cmap + entry);
            encoding = read_u16(cmap + entry + 2U);
            if (platform != kPreference[pass].platform ||
                encoding != kPreference[pass].encoding) {
                continue;
            }
            offset = read_u32(cmap + entry + 4U);
            if (offset + 4U > length || offset >= length) {
                continue;
            }
            font->cmap_subtable_offset = offset;
            font->cmap_subtable_format = read_u16(cmap + offset);
            return;
        }
    }
}

static bool parse_sfnt_tables(const unsigned char *data, size_t length,
                              TtfFont *font) {
    const size_t num_tables = length >= 12U ? read_u16(data + 4U) : 0U;
    TtfTable cmap_table;
    memset(font, 0, sizeof(*font));
    font->units_per_em = 1000U;
    if (num_tables == 0U || length < 12U) {
        return false;
    }
    if (find_table(data, length, num_tables, "head", &font->head) &&
        font->head.length >= 54U) {
        font->units_per_em = read_u16(font->head.data + 18U);
        font->index_to_loc_format = read_i16(font->head.data + 50U);
        if (font->units_per_em == 0U) {
            font->units_per_em = 1000U;
        }
    }
    if (find_table(data, length, num_tables, "maxp", &cmap_table) &&
        cmap_table.length >= 6U) {
        font->num_glyphs = read_u16(cmap_table.data + 4U);
    }
    if (find_table(data, length, num_tables, "hhea", &font->hhea) &&
        font->hhea.length >= 36U) {
        font->num_h_metrics = read_u16(font->hhea.data + 34U);
    }
    if (font->num_h_metrics <= 0) {
        font->num_h_metrics = font->num_glyphs > 0U ? (int)font->num_glyphs : 1;
    }
    if (find_table(data, length, num_tables, "loca", &font->loca)) {
        font->num_loca = (uint16_t)(font->loca.length /
            (font->index_to_loc_format == 0 ? 2U : 4U));
    }
    (void)find_table(data, length, num_tables, "glyf", &font->glyf);
    if (find_table(data, length, num_tables, "hmtx", &font->hmtx) == NULL) {
        /* Some CFF fonts carry advances in `hmtx` only; without it, metrics
         * fall back to a monospaced approximation. */
    }
    if (find_table(data, length, num_tables, "cmap", &cmap_table)) {
        font->cmap = (TtfTable *)p2p_calloc(1U, sizeof(TtfTable));
        if (font->cmap == NULL) {
            return false;
        }
        *font->cmap = cmap_table;
        select_cmap_subtable(cmap_table.data, cmap_table.length, font);
    }
    return true;
}

bool ttf_parse(const unsigned char *data, size_t length, TtfFont *font,
               FontHeader *header) {
    size_t offset = 0U;
    const unsigned char *sfnt = data;
    size_t sfnt_length = length;
    if (font == NULL) {
        return false;
    }
    memset(font, 0, sizeof(*font));
    if (header != NULL) {
        memset(header, 0, sizeof(*header));
        header->units_per_em = 1000U;
    }
    if (data == NULL || length < 12U) {
        return false;
    }
    /* A TrueType Collection is a header plus a list of sfnt offsets. */
    if (memcmp(data, "ttcf", 4) == 0) {
        uint32_t count = read_u32(data + 8U);
        if (count == 0U || 16U > length) {
            return false;
        }
        offset = read_u32(data + 12U);
        if (offset + 12U > length) {
            return false;
        }
        sfnt = data + offset;
        sfnt_length = length - offset;
    } else if (memcmp(data, "wOFF", 4) == 0 || memcmp(data, "wOF2", 4) == 0) {
        /* WOFF and WOFF2 are compressed wrappers libHaru cannot embed. They
         * are recognised only so they are not mistaken for garbage. */
        return false;
    }
    if (!parse_sfnt_tables(sfnt, sfnt_length, font)) {
        ttf_release(font);
        return false;
    }
    if (header != NULL) {
        TtfTable os2;
        header->units_per_em = font->units_per_em;
        if (font->head.length >= 54U) {
            header->x_min = read_i16(font->head.data + 36U);
            header->y_min = read_i16(font->head.data + 38U);
            header->x_max = read_i16(font->head.data + 40U);
            header->y_max = read_i16(font->head.data + 42U);
        }
        header->weight_class = 400U;
        header->italic = false;
        if (font->hhea.length >= 10U) {
            const double em = font->units_per_em;
            header->ascender = (double)read_i16(font->hhea.data + 4U) / em;
            header->descender = (double)read_i16(font->hhea.data + 6U) / em;
            header->line_gap = (double)read_i16(font->hhea.data + 8U) / em;
        }
        if (find_table(sfnt, sfnt_length, read_u16(sfnt + 4U), "OS/2", &os2) &&
            os2.length >= 64U) {
            const uint16_t class = read_u16(os2.data + 4U);
            if (class >= 100 && class <= 1000) {
                header->weight_class = class;
            }
            header->italic = (read_u16(os2.data + 62U) & 0x01U) != 0U;
        } else if (font->head.length >= 46U) {
            const uint16_t mac = read_u16(font->head.data + 44U);
            if ((mac & 0x01U) != 0U) {
                header->weight_class = 700U;
            }
            header->italic = (mac & 0x02U) != 0U;
        }
        if (header->ascender == 0.0) {
            /* No usable hhea: fall back to the head bounding box. */
            header->ascender = font->units_per_em != 0U
                ? (double)header->y_max / font->units_per_em : 0.8;
            header->descender = font->units_per_em != 0U
                ? (double)header->y_min / font->units_per_em : -0.2;
            header->line_gap = 0.0;
        }
    }
    return true;
}

void ttf_release(TtfFont *font) {
    if (font == NULL) {
        return;
    }
    free(font->cmap);
    free(font->hmtx);
    font->cmap = NULL;
    font->hmtx = NULL;
}

uint16_t ttf_num_glyphs(const TtfFont *font) {
    return font != NULL ? font->num_glyphs : 0U;
}

uint16_t ttf_glyph_index(const TtfFont *font, uint32_t codepoint) {
    const unsigned char *table;
    size_t length;
    uint16_t format;
    if (font == NULL || font->cmap == NULL || font->cmap->data == NULL ||
        font->cmap_subtable_offset == 0U) {
        return 0U;
    }
    table = font->cmap->data;
    length = font->cmap->length;
    if (font->cmap_subtable_offset + 4U > length) {
        return 0U;
    }
    table += font->cmap_subtable_offset;
    length -= font->cmap_subtable_offset;
    format = read_u16(table);
    if (format == 4U) {
        uint16_t segment_count;
        const unsigned char *end_codes;
        const unsigned char *start_codes;
        const unsigned char *id_deltas;
        const unsigned char *id_range_offsets;
        if (length < 14U || codepoint > 0xFFFFU) {
            return 0U;
        }
        segment_count = (uint16_t)(read_u16(table + 6U) / 2U);
        if (segment_count == 0U || length < 16U + (size_t)segment_count * 8U) {
            return 0U;
        }
        end_codes = table + 14U;
        start_codes = end_codes + (size_t)segment_count * 2U + 2U;
        id_deltas = start_codes + (size_t)segment_count * 2U;
        id_range_offsets = id_deltas + (size_t)segment_count * 2U;
        for (uint16_t segment = 0U; segment < segment_count; ++segment) {
            const uint16_t end = read_u16(end_codes + (size_t)segment * 2U);
            if (codepoint > end) {
                continue;
            }
            {
                const uint16_t start = read_u16(start_codes + (size_t)segment * 2U);
                const int16_t delta = read_i16(id_deltas + (size_t)segment * 2U);
                const uint16_t range =
                    read_u16(id_range_offsets + (size_t)segment * 2U);
                if (codepoint < start) {
                    return 0U;
                }
                if (range == 0U) {
                    return (uint16_t)(((uint32_t)codepoint + (uint32_t)(int32_t)delta) &
                                      0xFFFFU);
                }
                {
                    const size_t index =
                        (size_t)(id_range_offsets - table) + (size_t)segment * 2U +
                        range + (size_t)(codepoint - start) * 2U;
                    uint16_t glyph;
                    if (index + 2U > length) {
                        return 0U;
                    }
                    glyph = read_u16(table + index);
                    if (glyph == 0U) {
                        return 0U;
                    }
                    return (uint16_t)((glyph + (uint32_t)(int32_t)delta) & 0xFFFFU);
                }
            }
        }
        return 0U;
    }
    if (format == 12U) {
        uint32_t group_count;
        uint32_t left = 0U;
        uint32_t right;
        if (length < 16U) {
            return 0U;
        }
        group_count = read_u32(table + 12U);
        right = group_count;
        while (left < right) {
            const uint32_t mid = left + (right - left) / 2U;
            const size_t entry = 16U + (size_t)mid * 12U;
            uint32_t group_start;
            uint32_t group_end;
            if (entry + 12U > length) {
                return 0U;
            }
            group_start = read_u32(table + entry);
            group_end = read_u32(table + entry + 4U);
            if (codepoint < group_start) {
                right = mid;
            } else if (codepoint > group_end) {
                left = mid + 1U;
            } else {
                return (uint16_t)(read_u32(table + entry + 8U) +
                                  (codepoint - group_start));
            }
        }
        return 0U;
    }
    if (format == 6U) {
        uint16_t first;
        uint16_t count;
        if (length < 10U) {
            return 0U;
        }
        first = read_u16(table + 6U);
        count = read_u16(table + 8U);
        if (codepoint >= first && codepoint < (uint32_t)first + count) {
            const size_t index = 10U + (size_t)(codepoint - first) * 2U;
            if (index + 2U <= length) {
                return read_u16(table + index);
            }
        }
        return 0U;
    }
    if (format == 0U) {
        if (codepoint < 256U && length >= 262U) {
            return table[6U + codepoint];
        }
        return 0U;
    }
    return 0U;
}

uint16_t ttf_advance(const TtfFont *font, uint16_t glyph) {
    if (font == NULL || font->hmtx == NULL || font->hmtx->data == NULL ||
        font->hmtx->length < 4U) {
        return 0U;
    }
    if ((int)glyph < font->num_h_metrics) {
        const size_t index = (size_t)glyph * 4U;
        if (index + 2U <= font->hmtx->length) {
            return read_u16(font->hmtx->data + index);
        }
    }
    /* Trailing glyphs reuse the last advance, as hmtx specifies. */
    {
        const size_t index = (size_t)(font->num_h_metrics - 1) * 4U;
        if (index + 2U <= font->hmtx->length) {
            return read_u16(font->hmtx->data + index);
        }
    }
    return (uint16_t)(font->units_per_em / 2U);
}

bool ttf_glyph_bbox(const TtfFont *font, uint16_t glyph, double scale,
                    double *x0, double *y0, double *x1, double *y1) {
    uint32_t start;
    uint32_t end;
    if (font == NULL || font->loca.data == NULL || font->glyf.data == NULL) {
        return false;
    }
    if ((size_t)glyph + 1U >= font->num_loca) {
        return false;
    }
    if (font->index_to_loc_format == 0) {
        if ((size_t)(glyph + 1U) * 2U + 2U > font->loca.length) {
            return false;
        }
        start = (uint32_t)read_u16(font->loca.data + (size_t)glyph * 2U) * 2U;
        end = (uint32_t)read_u16(font->loca.data + (size_t)(glyph + 1U) * 2U) * 2U;
    } else {
        if ((size_t)(glyph + 1U) * 4U + 4U > font->loca.length) {
            return false;
        }
        start = read_u32(font->loca.data + (size_t)glyph * 4U);
        end = read_u32(font->loca.data + (size_t)(glyph + 1U) * 4U);
    }
    if (end <= start || end - start < 10U || end > font->glyf.length) {
        return false;   /* an empty glyph outline, such as a space */
    }
    *x0 = (double)read_i16(font->glyf.data + start + 2U) * scale;
    *y0 = (double)read_i16(font->glyf.data + start + 4U) * scale;
    *x1 = (double)read_i16(font->glyf.data + start + 6U) * scale;
    *y1 = (double)read_i16(font->glyf.data + start + 8U) * scale;
    return true;
}

/* ------------------------------------------------------------- base-14 faces */

static FontFace *face_push(FontRegistry *registry) {
    FontFace *face;
    if (registry->count == registry->capacity) {
        const size_t capacity = registry->capacity == 0U ? 16U
                                  : registry->capacity * 2U;
        FontFace *grown = (FontFace *)p2p_realloc(registry->faces,
                                                   capacity * sizeof(FontFace));
        if (grown == NULL) {
            return NULL;
        }
        registry->faces = grown;
        registry->capacity = capacity;
    }
    if (registry->count >= P2P_MAX_FACES) {
        return NULL;
    }
    face = &registry->faces[registry->count++];
    memset(face, 0, sizeof(*face));
    face->owner = registry;
    return face;
}

static void register_base14(FontRegistry *registry) {
    /* Ordered [sans, mono] x [regular, bold] x [upright, italic]. Haru's
     * standard font names are fixed, so the mapping is a table rather than a
     * search. */
    static const char *const kNames[2][2][2] = {
        {{"Helvetica", "Helvetica-Bold"}, {"Helvetica-Oblique", "Helvetica-BoldOblique"}},
        {{"Courier", "Courier-Bold"}, {"Courier-Oblique", "Courier-BoldOblique"}}
    };
    for (int mono = 0; mono < 2; ++mono) {
        for (int bold = 0; bold < 2; ++bold) {
            for (int italic = 0; italic < 2; ++italic) {
                FontFace *face = face_push(registry);
                if (face == NULL) {
                    continue;
                }
                face->family = p2p_strdup_c(mono ? "courier" : "helvetica");
                face->subfamily = p2p_strdup_c("");
                face->weight = bold ? 700 : 400;
                face->style = italic ? FONT_STYLE_ITALIC : FONT_STYLE_NORMAL;
                face->monospace = mono != 0;
                face->is_base14 = true;
                face->label = p2p_strdup_c("base-14");
                face->header.units_per_em = 1000U;
                face->header.ascender = 0.718;
                face->header.descender = -0.207;
                face->header.line_gap = 0.0;
                face->header.weight_class = (uint16_t)face->weight;
                if (registry->document != NULL) {
                    face->hpdf = HPDF_GetFont(registry->document,
                                              kNames[mono][bold][italic], NULL);
                }
                registry->base14[mono][bold][italic] = &registry->faces[registry->count - 1U];
            }
        }
    }
}

HPDF_Font fonts_face_handle(FontFace *face) {
    if (face == NULL) {
        return NULL;
    }
    if (face->hpdf == NULL && face->owner != NULL && face->data != NULL &&
        face->owner->document != NULL && face->owner->embed) {
        char name[64];
        snprintf(name, sizeof(name), "ProwseTkFace%zu",
                 (size_t)(face - face->owner->faces));
        face->hpdf = HPDF_LoadTypefaceFromMem(face->owner->document, face->data,
                                              (HPDF_UINT)face->data_length, name);
    }
    if (face->hpdf == NULL && face->owner != NULL) {
        /* Fall back to the closest standard face so text is never dropped. */
        const FontFace *standard = face->owner->base14[face->monospace ? 1 : 0]
                                                        [face->weight >= 600 ? 1 : 0]
                                                        [face->style == FONT_STYLE_NORMAL
                                                             ? 0 : 1];
        if (standard != NULL) {
            face->hpdf = standard->hpdf;
        }
    }
    return face->hpdf;
}

/* -------------------------------------------------------- coverage and metrics */

/* The base-14 faces are WinAnsi encoded. This is the printable WinAnsi set the
 * printer will accept, used to decide whether a standard face can draw a
 * codepoint at all. */
static bool base14_covers(uint32_t codepoint) {
    if (codepoint >= 0x20U && codepoint <= 0x7EU) {
        return true;
    }
    if (codepoint >= 0xA0U && codepoint <= 0xFFU) {
        return true;
    }
    switch (codepoint) {
        case 0x20AC: case 0x201A: case 0x0192: case 0x201E: case 0x2026:
        case 0x2020: case 0x2021: case 0x02C6: case 0x2030: case 0x0160:
        case 0x2039: case 0x0152: case 0x017D: case 0x2018: case 0x2019:
        case 0x201C: case 0x201D: case 0x2022: case 0x2013: case 0x2014:
        case 0x02DC: case 0x2122: case 0x0161: case 0x203A: case 0x0153:
        case 0x017E: case 0x0178:
            return true;
        default:
            return false;
    }
}

bool fonts_covers(const FontFace *face, uint32_t codepoint) {
    if (face == NULL) {
        return false;
    }
    if (codepoint < 0x20U) {
        return false;
    }
    if (face->is_base14) {
        return base14_covers(codepoint);
    }
    if (!face->ttf_ready) {
        return false;
    }
    return ttf_glyph_index(&face->ttf, codepoint) != 0U;
}

void fonts_face_metrics(const FontFace *face, double size, double *ascent,
                        double *descent, double *line_height) {
    double up = 0.718;
    double down = -0.207;
    double gap = 0.0;
    if (face != NULL) {
        up = face->header.ascender;
        down = face->header.descender;
        gap = face->header.line_gap;
        if (up == 0.0 && down == 0.0) {
            up = 0.718;
            down = -0.207;
        }
    }
    *ascent = up * size;
    *descent = -down * size;
    *line_height = (up - down + gap) * size;
    if (*line_height <= 0.0) {
        *line_height = size * 1.2;
    }
}

double fonts_advance(const FontFace *face, uint32_t codepoint) {    if (face == NULL) {
        return 0.5;
    }
    if (face->is_base14) {
        /* Helvetica and Courier both use a 1000-unit em; the standard widths
         * below are the classic AFM values, so line breaking matches what
         * libHaru will actually draw. */
        static const struct {
            char first, last;
            double width;
        } kWidths[] = {
            {' ', ' ', 278.0}, {'!', '"', 355.0}, {'#', '$', 556.0}, {'%', '&', 889.0},
            {'\'', '\'', 191.0}, {'(', ')', 333.0}, {'*', '*', 389.0}, {'+', '+', 584.0},
            {',', ',', 278.0}, {'-', '.', 278.0}, {'/', '/', 278.0},
            {'0', '9', 556.0}, {':', ';', 278.0}, {'<', '>', 584.0}, {'?', '?', 556.0},
            {'@', '@', 1015.0}, {'A', 'Z', 667.0}, {'[', ']', 278.0}, {'^', '^', 469.0},
            {'_', '_', 556.0}, {'`', '`', 333.0}, {'a', 'z', 556.0}, {'{', '{', 334.0},
            {'|', '|', 260.0}, {'}', '}', 334.0}, {'~', '~', 584.0}
        };
        if (face->monospace) {
            return codepoint < 0x20U ? 0.0 : 0.600;
        }
        if (codepoint >= 0xC0U && codepoint <= 0xFFU) {
            /* Latin-1 letters vary; approximate with the lowercase width. */
            return 0.556;
        }
        for (size_t i = 0U; i < sizeof(kWidths) / sizeof(kWidths[0]); ++i) {
            if (codepoint >= (uint32_t)kWidths[i].first &&
                codepoint <= (uint32_t)kWidths[i].last) {
                return kWidths[i].width / 1000.0;
            }
        }
        return 0.556;
    }
    if (!face->ttf_ready || face->ttf.units_per_em == 0U) {
        return 0.5;
    }
    {
        const uint16_t glyph = ttf_glyph_index(&face->ttf, codepoint);
        return (double)ttf_advance(&face->ttf, glyph) / face->ttf.units_per_em;
    }
}

/* ------------------------------------------------------------ family matching */

/* Family names that must not be treated as real families. */
/* True when `name` names the monospace generic specifically. */
static bool generic_wants_monospace(const char *name) {
    return strcmp(name, "monospace") == 0 || strcmp(name, "ui-monospace") == 0;
}

static bool is_generic_family(const char *name) {
    static const char *const kGeneric[] = {
        "serif", "sans-serif", "sans serif", "monospace", "cursive", "fantasy",
        "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded",
        "math", "emoji", "fangsong", "-apple-system", "blinkmacsystemfont",
        "segoe ui", "inherit", "initial", "unset", "revert"
    };
    for (size_t i = 0U; i < sizeof(kGeneric) / sizeof(kGeneric[0]); ++i) {
        if (strcmp(name, kGeneric[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Prefers a face for a generic family. Ordered by what a page is most likely
 * to want, so a document with `font-family: sans-serif` gets a text face. */
static const FontFace *generic_candidate(FontRegistry *registry, bool wants_mono,
                                         bool bold_needed, bool italic_needed) {
    const int mono_index = wants_mono ? 1 : 0;
    if (registry->base14[mono_index] != NULL) {
        if (registry->base14[mono_index][bold_needed ? 1 : 0]
                                [italic_needed ? 1 : 0] != NULL) {
            return registry->base14[mono_index][bold_needed ? 1 : 0]
                                    [italic_needed ? 1 : 0];
        }
    }
    /* No standard face available: take the best-scoring embedded face. */
    {
        const FontFace *best = NULL;
        long best_score = -1L;
        for (size_t i = 0U; i < registry->count; ++i) {
            FontFace *face = &registry->faces[i];
            long score = 0;
            if (face->is_base14) continue;
            if (wants_mono != face->monospace) score -= 40;
            score -= (long)(face->weight > bold_needed ? face->weight - bold_needed
                                                        : bold_needed - face->weight);
            if ((face->style == FONT_STYLE_ITALIC) == italic_needed) score += 10;
            if (score > best_score) {
                best_score = score;
                best = face;
            }
        }
        if (best != NULL) {
            return best;
        }
    }
    return registry->base14[0][bold_needed ? 1 : 0][italic_needed ? 1 : 0];
}

static long family_score(const FontFace *face, const char *family,
                         int wanted_weight, bool italic_needed) {
    long score;
    if (face->family == NULL || *face->family == '\0' || family == NULL ||
        *family == '\0') {
        return -1L;
    }
    if (strcmp(face->family, family) == 0) {
        score = 1000;
    } else if (face->subfamily != NULL && *face->subfamily != '\0' &&
               strstr(face->subfamily, family) != NULL) {
        score = 400;
    } else if (strstr(face->family, family) != NULL ||
               strstr(family, face->family) != NULL) {
        score = 200;
    } else {
        return -1L;
    }
    score -= (long)(face->weight > wanted_weight ? face->weight - wanted_weight
                                                 : wanted_weight - face->weight);
    if ((face->style == FONT_STYLE_ITALIC) == italic_needed) {
        score += 50;
    } else {
        score -= 50;
    }
    return score;
}

const FontFace *fonts_resolve(FontRegistry *registry, const Style *style,
                              bool bold_needed, bool italic_needed) {
    const int wanted_weight = style->font_weight;
    const char *stack = style->font_family;
    const FontFace *best = NULL;
    long best_score = -1L;
    bool settled = false;
    if (registry == NULL || registry->count == 0U) {
        return NULL;
    }
    while (stack != NULL && *stack != '\0') {
        const char *comma = strchr(stack, ',');
        const size_t length = comma != NULL ? (size_t)(comma - stack) : strlen(stack);
        char *name = p2p_strdup((const unsigned char *)stack, length);
        if (name == NULL) {
            break;
        }
        p2p_lower(p2p_trim(name));
        if (*name != '\0') {
            if (is_generic_family(name)) {
                const FontFace *generic =
                    generic_candidate(registry, generic_wants_monospace(name),
                                      bold_needed, italic_needed);
                if (generic != NULL) {
                    best = generic;
                    settled = true;
                }
            } else {
                for (size_t i = 0U; i < registry->count; ++i) {
                    FontFace *face = &registry->faces[i];
                    const long score = family_score(face, name, wanted_weight,
                                                    italic_needed);
                    if (score > best_score) {
                        best_score = score;
                        best = face;
                    }
                }
            }
        }
        free(name);
        if (settled || comma == NULL) {
            break;
        }
        stack = comma + 1;
    }
    if (best == NULL) {
        /* No family in the stack resolved. Fall back to the sans generic,
         * which is the least surprising default on paper. */
        best = generic_candidate(registry, false, bold_needed, italic_needed);
    }
    if (best == NULL) {
        best = &registry->faces[0];
    }
    /* Synthetic bold: the printer can fake weight by stroking, so a face that
     * is lighter than the request still honours it. */
    if (!best->is_base14 && best->weight < wanted_weight && wanted_weight >= 600) {
        const FontFace *heavier = NULL;
        for (size_t i = 0U; i < registry->count; ++i) {
            FontFace *face = &registry->faces[i];
            if (face->is_base14 || strcmp(face->family, best->family) != 0) {
                continue;
            }
            if ((face->style == FONT_STYLE_ITALIC) != italic_needed) {
                continue;
            }
            if (face->weight > best->weight && face->weight <= wanted_weight &&
                (heavier == NULL || face->weight < heavier->weight)) {
                heavier = face;
            }
        }
        if (heavier != NULL) {
            best = heavier;
        }
    }
    return best;
}

const FontFace *fonts_resolve_glyph(FontRegistry *registry, const Style *style,
                                    const FontFace *face, uint32_t codepoint,
                                    bool *synthetic_bold) {
    const int wanted_weight = style != NULL ? style->font_weight : 400;
    const bool italic_needed = style != NULL && style->font_style != FONT_STYLE_NORMAL;
    if (synthetic_bold != NULL) {
        *synthetic_bold = false;
    }
    if (face == NULL) {
        return NULL;
    }
    if (fonts_covers(face, codepoint)) {
        if (synthetic_bold != NULL && !face->is_base14 && face->weight < 400 &&
            wanted_weight >= 600) {
            *synthetic_bold = true;
        }
        return face;
    }
    /* Another face with the same family may cover it (an italic-only CJK face,
     * for example), then any face that does. */
    {
        const FontFace *same_family = NULL;
        const FontFace *any = NULL;
        const FontFace *any_italic = NULL;
        const FontFace *any_bold = NULL;
        for (size_t i = 0U; i < registry->count; ++i) {
            FontFace *candidate = &registry->faces[i];
            if (!fonts_covers(candidate, codepoint)) {
                continue;
            }
            if (any == NULL) any = candidate;
            if (strcmp(candidate->family, face->family) == 0) {
                if (same_family == NULL) same_family = candidate;
                continue;
            }
            if (italic_needed && (candidate->style == FONT_STYLE_ITALIC) &&
                any_italic == NULL) {
                any_italic = candidate;
            }
            if (!italic_needed && candidate->style == FONT_STYLE_NORMAL &&
                any_bold == NULL && candidate->weight >= wanted_weight) {
                any_bold = candidate;
            }
        }
        if (same_family != NULL) {
            return same_family;
        }
        if (italic_needed && any_italic != NULL) {
            return any_italic;
        }
        if (!italic_needed && any_bold != NULL) {
            return any_bold;
        }
        if (any != NULL) {
            return any;
        }
    }
    /* Nothing covers the codepoint. Fall back to the replacement character when
     * some face can draw that, so the reader sees a marked gap rather than a
     * silently missing glyph. */
    if (codepoint != 0xFFFDU) {
        for (size_t i = 0U; i < registry->count; ++i) {
            FontFace *candidate = &registry->faces[i];
            if (fonts_covers(candidate, 0xFFFDU)) {
                return candidate;
            }
        }
    }
    return face;
}

/* ---------------------------------------------------------------- data URLs */

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *decode_base64(const char *text, size_t *out_length) {
    const size_t length = strlen(text);
    unsigned char *out;
    size_t written = 0U;
    size_t i;
    if (length < 4U || length % 4U != 0U) {
        return NULL;
    }
    out = (unsigned char *)p2p_alloc(length / 4U * 3U);
    if (out == NULL) {
        return NULL;
    }
    for (i = 0U; i < length; i += 4U) {
        const int a = base64_value((unsigned char)text[i]);
        const int b = base64_value((unsigned char)text[i + 1U]);
        const int c = text[i + 2U] == '=' ? 0 : base64_value((unsigned char)text[i + 2U]);
        const int d = text[i + 3U] == '=' ? 0 : base64_value((unsigned char)text[i + 3U]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            free(out);
            return NULL;
        }
        if (text[i + 2U] == '=' && text[i + 3U] != '=') {
            free(out);
            return NULL;
        }
        out[written++] = (unsigned char)((a << 2) | (b >> 4));
        if (text[i + 2U] != '=') {
            out[written++] = (unsigned char)((b << 4) | (c >> 2));
        }
        if (text[i + 3U] != '=') {
            out[written++] = (unsigned char)((c << 6) | d);
        }
        if (text[i + 2U] == '=' || text[i + 3U] == '=') {
            break;
        }
    }
    *out_length = written;
    return out;
}

static unsigned char *read_file(const char *path, size_t *out_length) {
    FILE *input = fopen(path, "rb");
    unsigned char *data = NULL;
    size_t capacity = 0U;
    size_t length = 0U;
    if (input == NULL) {
        return NULL;
    }
    for (;;) {
        size_t count;
        if (length == capacity) {
            const size_t grown = capacity == 0U ? 65536U : capacity * 2U;
            if (grown > P2P_MAX_FONT_FILE_BYTES) {
                break;
            }
            {
                unsigned char *bigger = (unsigned char *)p2p_realloc(data, grown);
                if (bigger == NULL) {
                    free(data);
                    fclose(input);
                    return NULL;
                }
                data = bigger;
                capacity = grown;
            }
        }
        count = fread(data + length, 1U, capacity - length, input);
        length += count;
        if (count == 0U) {
            break;
        }
    }
    fclose(input);
    if (length == 0U) {
        free(data);
        return NULL;
    }
    *out_length = length;
    return data;
}

/* Reads the family name out of the font's `name` table so that `font-family`
 * lookups match what a stylesheet asks for rather than what the file is called. */
static char *read_name_table(const unsigned char *data, size_t length,
                             const char *family) {
    const size_t num_tables = length >= 12U ? read_u16(data + 4U) : 0U;
    TtfTable name_table;
    size_t count;
    size_t string_offset;
    if (!find_table(data, length, num_tables, "name", &name_table)) {
        return NULL;
    }
    if (name_table.length < 6U) {
        return NULL;
    }
    count = read_u16(name_table.data + 2U);
    string_offset = read_u16(name_table.data + 4U);
    if (string_offset > name_table.length) {
        return NULL;
    }
    for (size_t i = 0U; i < count; ++i) {
        const size_t entry = 6U + i * 12U;
        uint16_t name_id;
        uint16_t platform;
        uint16_t length;
        uint16_t offset;
        if (entry + 12U > name_table.length) {
            break;
        }
        platform = read_u16(name_table.data + entry);
        name_id = read_u16(name_table.data + entry + 6U);
        length = read_u16(name_table.data + entry + 8U);
        offset = read_u16(name_table.data + entry + 10U);
        if (name_id != (uint16_t)family) {
            continue;
        }
        if ((size_t)string_offset + offset + length > name_table.length) {
            continue;
        }
        {
            const unsigned char *text = name_table.data + string_offset + offset;
            char *out;
            if (platform == 3 || platform == 0) {
                /* UTF-16BE, re-encoded as UTF-8 into a bounded buffer. */
                const size_t characters = length / 2U;
                char *cursor;
                out = (char *)p2p_alloc(characters * 4U + 1U);
                if (out == NULL) {
                    return NULL;
                }
                cursor = out;
                {
                    size_t at2 = 0U;
                    while (at2 < characters) {
                        uint32_t unit = read_u16(text + at2 * 2U);
                        if (unit == 0U) break;
                        if (unit >= 0xD800U && unit <= 0xDBFFU && at2 + 1U < characters) {
                            const uint32_t low = read_u16(text + (at2 + 1U) * 2U);
                            if (low >= 0xDC00U && low <= 0xDFFFU) {
                                unit = 0x10000U + ((unit - 0xD800U) << 10) + (low - 0xDC00U);
                                ++at2;
                            }
                        }
                        if (unit < 0x80U) {
                            *cursor++ = (char)unit;
                        } else if (unit < 0x800U) {
                            *cursor++ = (char)(0xC0U | (unit >> 6));
                            *cursor++ = (char)(0x80U | (unit & 0x3FU));
                        } else if (unit < 0x10000U) {
                            *cursor++ = (char)(0xE0U | (unit >> 12));
                            *cursor++ = (char)(0x80U | ((unit >> 6) & 0x3FU));
                            *cursor++ = (char)(0x80U | (unit & 0x3FU));
                        } else {
                            *cursor++ = (char)(0xF0U | (unit >> 18));
                            *cursor++ = (char)(0x80U | ((unit >> 12) & 0x3FU));
                            *cursor++ = (char)(0x80U | ((unit >> 6) & 0x3FU));
                            *cursor++ = (char)(0x80U | (unit & 0x3FU));
                        }
                        ++at2;
                    }
                    *cursor = '\0';
                }
                p2p_lower(out);
                return out;
            }
            /* Mac Roman / single byte: copy verbatim. */
            out = p2p_strdup(text, length);
            if (out != NULL) {
                p2p_lower(out);
            }
            return out;
        }
    }
    return NULL;
}

static bool register_font_bytes(FontRegistry *registry, unsigned char *data,
                                size_t length, const char *label,
                                const char *forced_family) {
    TtfFont parsed;
    FontHeader header;
    FontFace *face;
    if (registry->count >= P2P_MAX_FACES ||
        registry->total_bytes + length > P2P_MAX_FONT_BYTES) {
        free(data);
        return false;
    }
    if (!ttf_parse(data, length, &parsed, &header)) {
        free(data);
        return false;
    }
    face = face_push(registry);
    if (face == NULL) {
        ttf_release(&parsed);
        free(data);
        return false;
    }
    face->data = data;
    face->data_length = length;
    face->ttf = parsed;
    face->ttf_ready = true;
    face->header = header;
    face->weight = header.weight_class >= 100 ? (int)header.weight_class : 400;
    if (face->weight < 100) face->weight = 400;
    face->style = header.italic ? FONT_STYLE_ITALIC : FONT_STYLE_NORMAL;
    face->label = p2p_strdup_c(label);
    face->family = read_name_table(data, length, "1");
    face->subfamily = read_name_table(data, length, "2");
    if (face->family == NULL || *face->family == '\0') {
        /* Fall back to the file name, which is what a user sees in a file
         * manager and usually what the stylesheet asked for. */
        const char *base = strrchr(label, '/');
        char *copy = p2p_strdup_c(base != NULL ? base + 1 : label);
        if (copy != NULL) {
            char *dot = strrchr(copy, '.');
            char *spaced = copy;
            if (dot != NULL) *dot = '\0';
            p2p_lower(spaced);
            free(face->family);
            face->family = copy;
        }
    }
    if (face->subfamily == NULL) {
        face->subfamily = p2p_strdup_c("");
    }
    if (forced_family != NULL && *forced_family != '\0') {
        free(face->family);
        face->family = p2p_strdup_c(forced_family);
        p2p_lower(face->family);
    }
    /* The classic monospace tell: every advance identical. */
    if (face->ttf.hmtx != NULL && face->ttf.hmtx->data != NULL &&
        face->ttf.num_h_metrics > 2U) {
        const uint16_t first = ttf_advance(&face->ttf, 0);
        const uint16_t third = ttf_advance(&face->ttf, 2);
        face->monospace = first != 0U && first == third;
    }
    registry->total_bytes += length;
    return true;
}

bool fonts_register_source(FontRegistry *registry, const char *family,
                           const char *source, const char *label) {
    unsigned char *data;
    size_t length = 0U;
    if (registry == NULL || source == NULL || !registry->embed) {
        return false;
    }
    if (p2p_starts_with(source, "data:")) {
        const char *comma = strchr(source, ',');
        bool base64 = p2p_istrstr(source, ";base64") != NULL;
        if (comma == NULL) {
            return false;
        }
        if (base64) {
            data = decode_base64(comma + 1, &length);
        } else {
            /* Percent-decoded text payloads are not font containers. */
            return false;
        }
        if (data == NULL) {
            return false;
        }
        {
            char *name = p2p_format("%s (%s)", label, family);
            const bool ok = register_font_bytes(registry, data, length,
                                                name != NULL ? name : "data:", family);
            free(name);
            return ok;
        }
    }
    data = read_file(source, &length);
    if (data == NULL) {
        return false;
    }
    {
        char *name = p2p_format("%s (%s)", source, label);
        const bool ok = register_font_bytes(registry, data, length,
                                            name != NULL ? name : source, family);
        free(name);
        return ok;
    }
}

/* -------------------------------------------------------------- system scan */

static bool has_font_extension(const char *name) {
    static const char *const kExtensions[] = {".ttf", ".otf", ".ttc", ".TTF",
                                              ".OTF", ".TTC"};
    for (size_t i = 0U; i < sizeof(kExtensions) / sizeof(kExtensions[0]); ++i) {
        if (p2p_ends_with(name, kExtensions[i])) {
            return true;
        }
    }
    return false;
}

/* Depth-limited directory walk. Every bound is explicit: a printer must not be
 * able to spend unbounded time or memory enumerating a font tree. */
static void scan_directory(FontRegistry *registry, const char *directory,
                           int depth) {
    static const size_t kMaxFiles = 512U;
    if (depth <= 0 || registry->files_scanned >= kMaxFiles ||
        registry->count >= P2P_MAX_FACES) {
        return;
    }
    {
        DIR *handle = opendir(directory);
        struct dirent *entry;
        if (handle == NULL) {
            return;
        }
        while ((entry = readdir(handle)) != NULL) {
            char *path;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            path = p2p_format("%s/%s", directory, entry->d_name);
            if (path == NULL) {
                continue;
            }
            if (has_font_extension(entry->d_name)) {
                if (registry->files_scanned < kMaxFiles) {
                    ++registry->files_scanned;
                    fonts_register_source(registry, "", path, path);
                }
            } else {
                scan_directory(registry, path, depth - 1);
            }
            free(path);
        }
        closedir(handle);
    }
}

void fonts_scan_system(FontRegistry *registry, const char *font_dir) {
    static const char *const kDirectories[] = {
        "/usr/share/fonts", "/usr/local/share/fonts", "/opt/share/fonts",
        "/Library/Fonts", "/System/Library/Fonts", "C:/Windows/Fonts"
    };
    if (registry == NULL) {
        return;
    }
    register_base14(registry);
    if (!registry->embed) {
        return;
    }
    if (font_dir != NULL && *font_dir != '\0') {
        scan_directory(registry, font_dir, 3);
    }
    for (size_t i = 0U; i < sizeof(kDirectories) / sizeof(kDirectories[0]); ++i) {
        scan_directory(registry, kDirectories[i], 4);
    }
}

FontRegistry *fonts_create(HPDF_Doc document, bool embed) {
    FontRegistry *registry = (FontRegistry *)p2p_calloc(1U, sizeof(FontRegistry));
    if (registry == NULL) {
        return NULL;
    }
    registry->embed = embed;
    registry->document = document;
    return registry;
}

void fonts_destroy(FontRegistry *registry) {
    if (registry == NULL) {
        return;
    }
    for (size_t i = 0U; i < registry->count; ++i) {
        FontFace *face = &registry->faces[i];
        ttf_release(&face->ttf);
        free(face->family);
        free(face->subfamily);
        free(face->label);
        free(face->data);
    }
    free(registry->faces);
    free(registry);
}

size_t fonts_face_count(const FontRegistry *registry) {
    return registry != NULL ? registry->count : 0U;
}

const FontFace *fonts_face_at(const FontRegistry *registry, size_t index) {
    if (registry == NULL || index >= registry->count) {
        return NULL;
    }
    return &registry->faces[index];
}
