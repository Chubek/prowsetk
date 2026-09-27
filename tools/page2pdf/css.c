/* css.c — the cascade stratum.
 *
 * Owns CSS tokenizing, selector parsing and matching, the cascade, computed
 * values, and the user-agent stylesheet. Knows nothing about geometry: every
 * length stays a Value with its original unit here and is resolved by
 * layout.c, which is the only place that knows containing blocks and font
 * sizes in context. Also materializes `::before` / `::after` generated
 * content so that layout never has to invent boxes.
 */
#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================== initial values
 *
 * Kept in one table-shaped function so that "which properties inherit" is
 * answerable by reading css_inherit(), and every other property's initial
 * value is answerable by reading css_style_initial().
 */
static Value px(double number) {
    return value_make(number, UNIT_PX);
}

void css_style_initial(Style *style) {
    /* Every field is a scalar except the owned strings, which must be NULL
     * rather than whatever the caller's previous value was. Callers either
     * pass a fresh zeroed struct or release the old strings first. */
    memset(style, 0, sizeof(*style));
    style->display = DISPLAY_INLINE;
    style->position = POS_STATIC;
    style->floats = FLOAT_NONE;
    style->clear = CLEAR_NONE;
    style->overflow = OVERFLOW_VISIBLE;
    style->overflow_x = OVERFLOW_VISIBLE;
    style->overflow_y = OVERFLOW_VISIBLE;

    style->width = value_auto();
    style->height = value_auto();
    style->min_width = value_auto();
    style->max_width = value_auto();
    style->min_height = value_auto();
    style->max_height = value_auto();
    for (int i = 0; i < 4; ++i) {
        style->margin[i] = value_auto();
        style->padding[i] = px(0.0);
        style->border_width[i] = px(0.0);
        style->border_color[i] = COLOR_BLACK;
        style->border_style[i] = BORDER_NONE;
    }
    for (int i = 0; i < 4; ++i) {
        style->border_radius[i] = px(0.0);
    }

    style->foreground = COLOR_BLACK;
    style->background = COLOR_TRANSPARENT;
    style->background_set = false;
    style->opacity = 1.0f;

    style->font_size = px(16.0);
    style->font_weight = 400;
    style->font_style = FONT_STYLE_NORMAL;
    style->font_family = NULL;
    style->line_height = value_number(0.0);
    style->line_height_unit = (signed char)UNIT_NUMBER;
    style->letter_spacing = px(0.0);
    style->word_spacing = px(0.0);

    style->text_align = ALIGN_START;
    style->text_align_last = ALIGN_AUTO;
    style->text_indent = px(0.0);
    style->decoration = 0U;
    style->decoration_color = COLOR_BLACK;
    style->decoration_color_set = false;
    style->text_transform = TRANSFORM_NONE;
    style->white_space = WS_NORMAL;
    style->word_break = value_auto();
    style->overflow_wrap = value_auto();
    style->vertical_align = VA_BASELINE;
    style->tab_size = px(8.0);

    style->list_style_type = LIST_DISC;
    style->list_position = LIST_POSITION_OUTSIDE;

    style->table_layout = TABLE_AUTO;
    style->border_collapse = BORDER_COLLAPSE_SEPARATE;
    style->border_spacing_set = false;
    style->border_spacing[0] = style->border_spacing[1] = 2.0f;
    style->caption_side = value_auto();

    style->flex_direction = FLEX_ROW;
    style->flex_wrap = FLEX_WRAP_NOWRAP;
    style->justify_content = FLEX_START;
    style->align_items = FLEX_STRETCH;
    style->align_self = FLEX_AUTO;
    style->align_content = FLEX_STRETCH;
    style->row_gap = value_auto();
    style->column_gap = value_auto();
    style->flex_grow = 0.0;
    style->flex_shrink = 1.0;
    style->flex_basis = value_auto();
    style->order = 0;

    style->grid_template_columns = value_auto();
    style->grid_template_rows = value_auto();
    style->grid_auto_columns = value_auto();
    style->grid_auto_rows = value_auto();
    style->grid_column_gap = value_auto();
    style->grid_row_gap = value_auto();
    style->grid_auto_flow = 0;

    style->column_count = 1;
    style->column_width = value_auto();
    style->column_gap = value_auto();
    style->column_rule_set = false;
    style->column_rule_color = COLOR_BLACK;
    style->column_rule_width = 0.0f;

    style->break_inside = BREAK_INSIDE_AUTO;
    style->break_before = BREAK_AUTO;
    style->break_after = BREAK_AUTO;
    style->orphans = 2;
    style->widows = 2;
    style->page_break_inside = 0;
    style->keep_with_next = false;

    style->z_index = 0;
    style->visibility_collapse = false;
    style->object_fit = OBJECT_FIT_FILL;
}

void style_release(Style *style) {
    free(style->font_family);
    free(style->list_marker_text);
    free(style->background_image);
    free(style->background_position);
    free(style->background_size);
    free(style->grid_template_columns);
    free(style->grid_template_rows);
    free(style->grid_auto_columns);
    free(style->grid_auto_rows);
    for (int i = 0; i < 4; ++i) {
        free(style->grid_column_rules[i]);
        free(style->grid_row_rules[i]);
    }
    memset(style, 0, sizeof(*style));
}

void css_inherit(Style *style, const Style *parent) {
    style->foreground = parent->foreground;
    style->font_size = parent->font_size;
    style->font_weight = parent->font_weight;
    style->font_style = parent->font_style;
    style->font_family = parent->font_family;
    style->line_height = parent->line_height;
    style->line_height_unit = parent->line_height_unit;
    style->letter_spacing = parent->letter_spacing;
    style->word_spacing = parent->word_spacing;
    style->text_align = parent->text_align;
    style->text_align_last = parent->text_align_last;
    style->text_indent = parent->text_indent;
    style->text_transform = parent->text_transform;
    style->white_space = parent->white_space;
    style->word_break = parent->word_break;
    style->overflow_wrap = parent->overflow_wrap;
    style->tab_size = parent->tab_size;
    style->list_style_type = parent->list_style_type;
    style->list_position = parent->list_position;
    style->border_collapse = parent->border_collapse;
    style->border_spacing_set = parent->border_spacing_set;
    style->border_spacing[0] = parent->border_spacing[0];
    style->border_spacing[1] = parent->border_spacing[1];
    style->caption_side = parent->caption_side;
    style->orphans = parent->orphans;
    style->widows = parent->widows;
    style->visibility_collapse = parent->visibility_collapse;
    /* `direction` is not modelled; RTL text order comes from the bidi pass in
     * text.c, which is stronger evidence than a declaration we do not track. */
}

/* ================================================================== lengths */

static const struct {
    const char *name;
    Unit unit;
} kUnits[] = {
    {"px", UNIT_PX}, {"pt", UNIT_PT}, {"pc", UNIT_PC}, {"in", UNIT_IN},
    {"cm", UNIT_CM}, {"mm", UNIT_MM}, {"q", UNIT_Q}, {"em", UNIT_EM},
    {"rem", UNIT_REM}, {"ex", UNIT_EX}, {"ch", UNIT_CH}, {"%", UNIT_PCT},
    {"vw", UNIT_VW}, {"vh", UNIT_VH}, {"vmin", UNIT_VMIN}, {"vmax", UNIT_VMAX}
};

bool css_parse_length(const char *text, const CssContext *context,
                      double font_size, Value *out) {
    char *end = NULL;
    double number;
    char unit_name[8];
    size_t i = 0U;
    if (text == NULL || out == NULL) {
        return false;
    }
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    if (p2p_iequals(text, "auto")) {
        *out = value_auto();
        return true;
    }
    if (p2p_iequals(text, "none") || p2p_iequals(text, "normal")) {
        *out = value_auto();
        return true;
    }
    number = strtod(text, &end);
    if (end == text) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        ++end;
    }
    while (end[i] != '\0' && !isspace((unsigned char)end[i]) &&
           i + 1U < sizeof(unit_name)) {
        unit_name[i] = end[i];
        ++i;
    }
    unit_name[i] = '\0';
    if (i == 0U) {
        if (p2p_ends_with(text, "0") || number == 0.0) {
            *out = px(0.0);
            return true;
        }
        *out = value_number(number);
        return true;
    }
    for (i = 0U; i < sizeof(kUnits) / sizeof(kUnits[0]); ++i) {
        if (p2p_iequals(unit_name, kUnits[i].name)) {
            /* A percentage font-size resolves against the parent's font size;
             * layout handles the general case for other properties. */
            (void)context;
            *out = value_make(number, kUnits[i].unit);
            return true;
        }
    }
    return false;
}

/* Resolves a calc() expression. Supports + - * / over lengths and plain
 * numbers with parenthesis nesting; any other token aborts the expression and
 * the declaration is dropped, which matches the CSS rule for invalid calc(). */
typedef struct {
    const char *cursor;
    const CssContext *context;
    double font_size;
    bool ok;
} CalcParser;

static double calc_sum(CalcParser *parser);

static void calc_skip_space(CalcParser *parser) {
    while (isspace((unsigned char)*parser->cursor)) {
        ++parser->cursor;
    }
}

static double calc_primary(CalcParser *parser) {
    double result = 0.0;
    Value value;
    char token[64];
    size_t i = 0U;
    calc_skip_space(parser);
    if (*parser->cursor == '(') {
        ++parser->cursor;
        result = calc_sum(parser);
        calc_skip_space(parser);
        if (*parser->cursor == ')') {
            ++parser->cursor;
        } else {
            parser->ok = false;
        }
        return result;
    }
    while (parser->cursor[i] != '\0' && !isspace((unsigned char)parser->cursor[i]) &&
           strchr("+-*/)", parser->cursor[i]) == NULL &&
           i + 1U < sizeof(token)) {
        token[i] = parser->cursor[i];
        ++i;
    }
    token[i] = '\0';
    if (i == 0U) {
        parser->ok = false;
        return 0.0;
    }
    parser->cursor += i;
    if (!css_parse_length(token, parser->context, parser->font_size, &value)) {
        parser->ok = false;
        return 0.0;
    }
    return value_resolve_length(value, 0.0, parser->font_size,
                                parser->context->root_font_size,
                                parser->context->viewport_width,
                                parser->context->viewport_height, true);
}

static double calc_product(CalcParser *parser) {
    double result = calc_primary(parser);
    for (;;) {
        char op;
        calc_skip_space(parser);
        op = *parser->cursor;
        if (op != '*' && op != '/') {
            return result;
        }
        ++parser->cursor;
        {
            const double right = calc_primary(parser);
            if (op == '*') {
                result *= right;
            } else if (right != 0.0) {
                result /= right;
            } else {
                parser->ok = false;
            }
        }
    }
}

static double calc_sum(CalcParser *parser) {
    double result = calc_product(parser);
    for (;;) {
        char op;
        calc_skip_space(parser);
        op = *parser->cursor;
        if (op != '+' && op != '-') {
            return result;
        }
        ++parser->cursor;
        {
            const double right = calc_product(parser);
            result = op == '+' ? result + right : result - right;
        }
    }
}

static bool parse_value(const char *text, const CssContext *context,
                        double font_size, Value *out) {
    char buffer[256];
    char *open;
    char *close;
    size_t length;
    if (text == NULL || out == NULL) {
        return false;
    }
    length = strlen(text);
    if (length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text, length + 1U);
    if (p2p_iequals(buffer, "auto") || p2p_iequals(buffer, "none") ||
        p2p_iequals(buffer, "normal")) {
        *out = value_auto();
        return true;
    }
    open = strchr(buffer, '(');
    close = strrchr(buffer, ')');
    if (open != NULL && close != NULL && close > open &&
        p2p_starts_with(p2p_skip_space(buffer), "calc")) {
        CalcParser parser;
        char *inner = p2p_strdup((const unsigned char *)open + 1U,
                                 (size_t)(close - open - 1));
        double result;
        if (inner == NULL) {
            return false;
        }
        parser.cursor = inner;
        parser.context = context;
        parser.font_size = font_size;
        parser.ok = true;
        result = calc_sum(&parser);
        calc_skip_space(&parser);
        if (parser.ok && *parser.cursor == '\0' && isfinite(result)) {
            *out = px(result);
            free(inner);
            return true;
        }
        free(inner);
        return false;
    }
    return css_parse_length(buffer, context, font_size, out);
}

/* ==================================================================== colors */

/* =========================================================== enum dispatchers */

typedef struct {
    const char *name;
    int value;
} NameValue;

static bool lookup_enum(const NameValue *table, size_t count, const char *name,
                        int *out) {
    size_t low = 0U, high = count;
    while (low < high) {
        const size_t mid = low + (high - low) / 2U;
        const int order = strcmp(table[mid].name, name);
        if (order == 0) {
            *out = table[mid].value;
            return true;
        }
        if (order < 0) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    return false;
}

#define NAME_VALUE_COUNT(table) (sizeof(table) / sizeof((table)[0]))

/* `display` is intentionally not a table: it also accepts the two-value
 * legacy form and the blockification rules live in layout.c. */
static const NameValue kDisplay[] = {
    {"none", DISPLAY_NONE}, {"block", DISPLAY_BLOCK}, {"inline", DISPLAY_INLINE},
    {"inline-block", DISPLAY_INLINE_BLOCK}, {"flex", DISPLAY_FLEX},
    {"inline-flex", DISPLAY_INLINE_FLEX}, {"grid", DISPLAY_GRID},
    {"inline-grid", DISPLAY_INLINE_GRID}, {"table", DISPLAY_TABLE},
    {"inline-table", DISPLAY_INLINE_TABLE},
    {"table-row-group", DISPLAY_TABLE_ROW_GROUP},
    {"table-header-group", DISPLAY_TABLE_HEADER_GROUP},
    {"table-footer-group", DISPLAY_TABLE_ROW_GROUP},
    {"table-row", DISPLAY_TABLE_ROW}, {"table-cell", DISPLAY_TABLE_CELL},
    {"table-column-group", DISPLAY_TABLE_COLUMN_GROUP},
    {"table-column", DISPLAY_TABLE_COLUMN},
    {"table-caption", DISPLAY_TABLE_CAPTION}, {"list-item", DISPLAY_LIST_ITEM},
    {"contents", DISPLAY_CONTENTS}, {"flow-root", DISPLAY_BLOCK},
    {"inline flow-root", DISPLAY_INLINE_BLOCK}, {"run-in", DISPLAY_BLOCK}
};

static const NameValue kPosition[] = {
    {"static", POS_STATIC}, {"relative", POS_RELATIVE},
    {"absolute", POS_ABSOLUTE}, {"fixed", POS_FIXED}, {"sticky", POS_STICKY}
};

static const NameValue kFloat[] = {
    {"none", FLOAT_NONE}, {"left", FLOAT_LEFT}, {"right", FLOAT_RIGHT},
    {"inline-start", FLOAT_LEFT}, {"inline-end", FLOAT_RIGHT}
};

static const NameValue kClear[] = {
    {"none", CLEAR_NONE}, {"left", CLEAR_LEFT}, {"right", CLEAR_RIGHT},
    {"both", CLEAR_BOTH}, {"inline-start", CLEAR_LEFT},
    {"inline-end", CLEAR_RIGHT}
};

static const NameValue kOverflow[] = {
    {"visible", OVERFLOW_VISIBLE}, {"hidden", OVERFLOW_HIDDEN},
    {"scroll", OVERFLOW_SCROLL}, {"auto", OVERFLOW_AUTO}, {"clip", OVERFLOW_HIDDEN}
};

static const NameValue kTextAlign[] = {
    {"start", ALIGN_START}, {"end", ALIGN_END}, {"left", ALIGN_LEFT},
    {"right", ALIGN_RIGHT}, {"center", ALIGN_CENTER}, {"justify", ALIGN_JUSTIFY},
    {"match-parent", ALIGN_START}, {"justify-all", ALIGN_JUSTIFY},
    {"-webkit-center", ALIGN_CENTER}
};

static const NameValue kVerticalAlign[] = {
    {"baseline", VA_BASELINE}, {"top", VA_TOP}, {"text-top", VA_TEXT_TOP},
    {"middle", VA_MIDDLE}, {"bottom", VA_BOTTOM}, {"text-bottom", VA_TEXT_BOTTOM},
    {"sub", VA_SUB}, {"super", VA_SUPER}
};

static const NameValue kBorderStyle[] = {
    {"none", BORDER_NONE}, {"hidden", BORDER_NONE}, {"solid", BORDER_SOLID},
    {"dashed", BORDER_DASHED}, {"dotted", BORDER_DOTTED},
    {"double", BORDER_DOUBLE}, {"groove", BORDER_GROOVE},
    {"ridge", BORDER_RIDGE}, {"inset", BORDER_INSET}, {"outset", BORDER_OUTSET}
};

static const NameValue kWhiteSpace[] = {
    {"normal", WS_NORMAL}, {"nowrap", WS_NOWRAP}, {"pre", WS_PRE},
    {"pre-wrap", WS_PRE_WRAP}, {"pre-line", WS_PRE_LINE},
    {"break-spaces", WS_PRE_WRAP}
};

static const NameValue kTextTransform[] = {
    {"none", TRANSFORM_NONE}, {"uppercase", TRANSFORM_UPPERCASE},
    {"lowercase", TRANSFORM_LOWERCASE}, {"capitalize", TRANSFORM_CAPITALIZE}
};

static const NameValue kListStyleType[] = {
    {"none", LIST_NONE}, {"disc", LIST_DISC}, {"circle", LIST_CIRCLE},
    {"square", LIST_SQUARE}, {"decimal", LIST_DECIMAL},
    {"decimal-leading-zero", LIST_DECIMAL_LEADING_ZERO},
    {"lower-alpha", LIST_LOWER_ALPHA}, {"upper-alpha", LIST_UPPER_ALPHA},
    {"lower-latin", LIST_LOWER_ALPHA}, {"upper-latin", LIST_UPPER_ALPHA},
    {"lower-roman", LIST_LOWER_ROMAN}, {"upper-roman", LIST_UPPER_ROMAN},
    {"lower-greek", LIST_LOWER_GREEK}, {"arabic-indic", LIST_ARABIC_INDIAN},
    {"hebrew", LIST_HEBREW}, {"cjk-decimal", LIST_CJK_DECIMAL},
    {"cjk-circle", LIST_CJK_CIRCLE}, {"disclosure-open", LIST_DISC},
    {"disclosure-closed", LIST_CIRCLE}
};

static const NameValue kListPosition[] = {
    {"outside", LIST_POSITION_OUTSIDE}, {"inside", LIST_POSITION_INSIDE}
};

static const NameValue kFontStyle[] = {
    {"normal", FONT_STYLE_NORMAL}, {"italic", FONT_STYLE_ITALIC},
    {"oblique", FONT_STYLE_OBLIQUE}
};

static const NameValue kObjectFit[] = {
    {"fill", OBJECT_FIT_FILL}, {"contain", OBJECT_FIT_CONTAIN},
    {"cover", OBJECT_FIT_COVER}, {"none", OBJECT_FIT_NONE},
    {"scale-down", OBJECT_FIT_SCALE_DOWN}
};

static const NameValue kFlexDirection[] = {
    {"row", FLEX_ROW}, {"row-reverse", FLEX_ROW_REVERSE},
    {"column", FLEX_COLUMN}, {"column-reverse", FLEX_COLUMN_REVERSE}
};

static const NameValue kFlexWrap[] = {
    {"nowrap", FLEX_WRAP_NOWRAP}, {"wrap", FLEX_WRAP_WRAP},
    {"wrap-reverse", FLEX_WRAP_WRAP_REVERSE}
};

static const NameValue kFlexAlign[] = {
    {"auto", FLEX_AUTO}, {"flex-start", FLEX_START}, {"start", FLEX_START},
    {"flex-end", FLEX_END}, {"end", FLEX_END}, {"center", FLEX_CENTER},
    {"space-between", FLEX_SPACE_BETWEEN}, {"space-around", FLEX_SPACE_AROUND},
    {"space-evenly", FLEX_SPACE_EVENLY}, {"stretch", FLEX_STRETCH},
    {"normal", FLEX_STRETCH}, {"baseline", FLEX_BASELINE},
    {"first baseline", FLEX_BASELINE}, {"last baseline", FLEX_BASELINE}
};

static const NameValue kTableLayout[] = {
    {"auto", TABLE_AUTO}, {"fixed", TABLE_FIXED}
};

static const NameValue kBorderCollapse[] = {
    {"separate", BORDER_COLLAPSE_SEPARATE}, {"collapse", BORDER_COLLAPSE_COLLAPSE}
};

static const NameValue kBreakInside[] = {
    {"auto", BREAK_INSIDE_AUTO}, {"avoid", BREAK_INSIDE_AVOID},
    {"avoid-page", BREAK_INSIDE_AVOID}, {"avoid-column", BREAK_INSIDE_AVOID},
    {"forbid", BREAK_INSIDE_FORBIDDEN}
};

static const NameValue kBreakBefore[] = {
    {"auto", BREAK_AUTO}, {"avoid", BREAK_AVOID}, {"avoid-page", BREAK_AVOID},
    {"left", BREAK_LEFT}, {"right", BREAK_RIGHT}, {"always", BREAK_ALWAYS},
    {"page", BREAK_PAGE}
};

static const NameValue kWordBreak[] = {
    {"normal", 0}, {"break-all", 1}, {"keep-all", 2}, {"break-word", 3}
};

static const NameValue kOverflowWrap[] = {
    {"normal", 0}, {"break-word", 1}, {"anywhere", 1}, {"inherit", 0}
};

static const NameValue kVisibility[] = {
    {"visible", 0}, {"hidden", 1}, {"collapse", 1}, {"inherit", 0}
};

static const NameValue kBoxSizing[] = {
    {"content-box", 0}, {"border-box", 1}
};

static const NameValue kFontWeightNamed[] = {
    {"normal", 400}, {"bold", 700}, {"lighter", 100}, {"bolder", 700}
};

static const NameValue kBackgroundRepeat[] = {
    {"repeat", 0}, {"repeat-x", 1}, {"repeat-y", 2}, {"no-repeat", 3}
};

/* ---------------------------------------------------------------- shorthands */

static int side_from_keyword(const char *name, int fallback) {
    if (strcmp(name, "top") == 0) return SIDE_TOP;
    if (strcmp(name, "right") == 0) return SIDE_RIGHT;
    if (strcmp(name, "bottom") == 0) return SIDE_BOTTOM;
    if (strcmp(name, "left") == 0) return SIDE_LEFT;
    return fallback;
}

/* Applies a 1-to-4 value box shorthand, honouring top/right/bottom/left order. */
static void apply_box_shorthand(Value *slots, const char *value,
                                const CssContext *context, double font_size,
                                bool allow_negative) {
    Value parts[4];
    size_t count = 0U;
    char buffer[128];
    char *cursor;
    size_t length = strlen(value);
    if (length >= sizeof(buffer)) {
        return;
    }
    memcpy(buffer, value, length + 1U);
    cursor = buffer;
    while (cursor != NULL && count < 4U) {
        char *end;
        char *token;
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (*cursor == '\0') break;
        token = cursor;
        end = strchr(cursor, ' ');
        if (end == NULL) end = cursor + strlen(cursor);
        {
            char saved = *end;
            *end = '\0';
            if (parse_value(token, context, font_size, &parts[count])) {
                if (allow_negative || parts[count].number >= 0.0) {
                    ++count;
                }
            }
            *end = saved;
        }
        cursor = *end == '\0' ? NULL : end + 1;
    }
    if (count == 0U) {
        return;
    }
    slots[SIDE_TOP] = parts[0];
    slots[SIDE_RIGHT] = count > 1U ? parts[1] : parts[0];
    slots[SIDE_BOTTOM] = count > 2U ? parts[2] : parts[0];
    slots[SIDE_LEFT] = count > 3U ? parts[3] : count > 1U ? parts[1] : parts[0];
}

/* ============================================================== declarations */

/* Applies one declaration. `!important` is stripped by the caller, which
 * routes the declaration to the important bucket instead. */
static void apply_declaration(Style *style, const char *name, const char *value,
                              const CssContext *context, double font_size,
                              const char *vendor, const char *property) {
    int number = 0;
    Value parsed;
    (void)vendor;
    if (p2p_starts_with(property, "-webkit-") || p2p_starts_with(property, "-moz-") ||
        p2p_starts_with(property, "-o-") || p2p_starts_with(property, "-ms-")) {
        /* Vendor prefixes are accepted for the handful of properties that have
         * no unprefixed equivalent still in use, and ignored otherwise. */
        if (strcmp(property, "-webkit-text-size-adjust") != 0 &&
            strcmp(property, "-webkit-box-orient") != 0 &&
            strcmp(property, "-webkit-line-clamp") != 0) {
            return;
        }
        return;
    }
    if (strcmp(name, "display") == 0) {
        if (lookup_enum(kDisplay, sizeof(kDisplay) / sizeof(kDisplay[0]), value, &number))
            style->display = (Display)number;
        return;
    }
    if (strcmp(name, "position") == 0) {
        if (lookup_enum(kPosition, sizeof(kPosition) / sizeof(kPosition[0]), value, &number))
            style->position = (Position)number;
        return;
    }
    if (strcmp(name, "float") == 0) {
        if (lookup_enum(kFloat, sizeof(kFloat) / sizeof(kFloat[0]), value, &number))
            style->floats = (FloatSide)number;
        return;
    }
    if (strcmp(name, "clear") == 0) {
        if (lookup_enum(kClear, sizeof(kClear) / sizeof(kClear[0]), value, &number))
            style->clear = (ClearSide)number;
        return;
    }
    if (strcmp(name, "overflow") == 0) {
        if (lookup_enum(kOverflow, sizeof(kOverflow) / sizeof(kOverflow[0]), value, &number)) {
            style->overflow = (Overflow)number;
            style->overflow_x = style->overflow;
            style->overflow_y = style->overflow;
        }
        return;
    }
    if (strcmp(name, "overflow-x") == 0) {
        if (lookup_enum(kOverflow, sizeof(kOverflow) / sizeof(kOverflow[0]), value, &number))
            style->overflow_x = (Overflow)number;
        return;
    }
    if (strcmp(name, "overflow-y") == 0) {
        if (lookup_enum(kOverflow, sizeof(kOverflow) / sizeof(kOverflow[0]), value, &number))
            style->overflow_y = (Overflow)number;
        return;
    }
    if (strcmp(name, "visibility") == 0) {
        if (lookup_enum(kVisibility, sizeof(kVisibility) / sizeof(kVisibility[0]), value, &number))
            style->visibility_collapse = number != 0;
        return;
    }
    if (strcmp(name, "box-sizing") == 0) {
        if (lookup_enum(kBoxSizing, sizeof(kBoxSizing) / sizeof(kBoxSizing[0]), value, &number))
            style->box_sizing_border_box = number != 0;
        return;
    }
    if (strcmp(name, "vertical-align") == 0) {
        if (lookup_enum(kVerticalAlign, sizeof(kVerticalAlign) / sizeof(kVerticalAlign[0]), value, &number))
            style->vertical_align = (VerticalAlign)number;
        return;
    }
    if (strcmp(name, "white-space") == 0) {
        if (lookup_enum(kWhiteSpace, sizeof(kWhiteSpace) / sizeof(kWhiteSpace[0]), value, &number))
            style->white_space = (WhiteSpace)number;
        return;
    }
    if (strcmp(name, "text-transform") == 0) {
        if (lookup_enum(kTextTransform, sizeof(kTextTransform) / sizeof(kTextTransform[0]), value, &number))
            style->text_transform = (TextTransform)number;
        return;
    }
    if (strcmp(name, "text-align") == 0) {
        if (lookup_enum(kTextAlign, sizeof(kTextAlign) / sizeof(kTextAlign[0]), value, &number))
            style->text_align = (TextAlign)number;
        return;
    }
    if (strcmp(name, "text-align-last") == 0) {
        if (lookup_enum(kTextAlign, sizeof(kTextAlign) / sizeof(kTextAlign[0]), value, &number))
            style->text_align_last = (TextAlign)number;
        return;
    }
    if (strcmp(name, "word-break") == 0) {
        if (lookup_enum(kWordBreak, sizeof(kWordBreak) / sizeof(kWordBreak[0]), value, &number))
            style->word_break = value_number((double)number);
        return;
    }
    if (strcmp(name, "overflow-wrap") == 0 || strcmp(name, "word-wrap") == 0) {
        if (lookup_enum(kOverflowWrap, sizeof(kOverflowWrap) / sizeof(kOverflowWrap[0]), value, &number))
            style->overflow_wrap = value_number((double)number);
        return;
    }
    if (strcmp(name, "list-style-type") == 0) {
        if (lookup_enum(kListStyleType, sizeof(kListStyleType) / sizeof(kListStyleType[0]), value, &number))
            style->list_style_type = (ListStyle)number;
        return;
    }
    if (strcmp(name, "list-style-position") == 0) {
        if (lookup_enum(kListPosition, sizeof(kListPosition) / sizeof(kListPosition[0]), value, &number))
            style->list_position = (ListPosition)number;
        return;
    }
    if (strcmp(name, "font-style") == 0) {
        if (lookup_enum(kFontStyle, sizeof(kFontStyle) / sizeof(kFontStyle[0]), value, &number))
            style->font_style = (FontStyle)number;
        return;
    }
    if (strcmp(name, "font-weight") == 0) {
        if (lookup_enum(kFontWeightNamed, sizeof(kFontWeightNamed) / sizeof(kFontWeightNamed[0]), value, &number)) {
            style->font_weight = number;
        } else if (parse_value(value, context, font_size, &parsed) &&
                   value_is_number(parsed)) {
            style->font_weight = (int)(parsed.number + 0.5);
        }
        if (style->font_weight < 1) style->font_weight = 1;
        if (style->font_weight > 1000) style->font_weight = 1000;
        return;
    }
    if (strcmp(name, "font-size") == 0) {
        static const struct {
            const char *name;
            double size;
        } kKeywords[] = {
            {"xx-small", 9.0}, {"x-small", 10.0}, {"small", 13.0},
            {"medium", 16.0}, {"large", 18.0}, {"x-large", 24.0},
            {"xx-large", 32.0}, {"xxx-large", 48.0}
        };
        size_t i;
        for (i = 0U; i < sizeof(kKeywords) / sizeof(kKeywords[0]); ++i) {
            if (p2p_iequals(value, kKeywords[i].name)) {
                style->font_size = px(kKeywords[i].size);
                return;
            }
        }
        if (parse_value(value, context, font_size, &parsed) && !value_is_auto(parsed)) {
            double resolved = value_resolve_length(parsed, font_size, font_size,
                                                   context->root_font_size,
                                                   context->viewport_width,
                                                   context->viewport_height, true);
            /* A percentage font size resolves against the parent, which is the
             * `font_size` reference here. */
            if (value_is_percentage(parsed)) {
                resolved = font_size * parsed.number / 100.0;
            }
            if (resolved >= 1.0 && resolved <= 400.0) {
                style->font_size = px(resolved);
            }
        }
        return;
    }
    if (strcmp(name, "font-family") == 0) {
        /* Store the stack verbatim, minus quotes and whitespace, as a
         * comma-separated list. fonts.c does the family matching. */
        char *copy = p2p_strdup_c(value);
        char *out;
        size_t written = 0U;
        const char *cursor;
        if (copy == NULL) {
            return;
        }
        out = p2p_alloc(strlen(copy) + 1U);
        if (out == NULL) {
            free(copy);
            return;
        }
        for (cursor = copy; *cursor != '\0';) {
            const char *start;
            const char *end;
            while (isspace((unsigned char)*cursor) || *cursor == ',') ++cursor;
            if (*cursor == '\0') break;
            if (*cursor == '"' || *cursor == '\'') {
                const char quote = *cursor++;
                start = cursor;
                while (*cursor != '\0' && *cursor != quote) ++cursor;
                end = cursor;
                if (*cursor == quote) ++cursor;
            } else {
                start = cursor;
                while (*cursor != '\0' && *cursor != ',') ++cursor;
                end = cursor;
            }
            while (start < end && isspace((unsigned char)*start)) ++start;
            while (end > start && isspace((unsigned char)end[-1])) --end;
            if (end > start) {
                if (written != 0U) {
                    out[written++] = ',';
                }
                memcpy(out + written, start, (size_t)(end - start));
                written += (size_t)(end - start);
            }
        }
        out[written] = '\0';
        if (written != 0U) {
            free(style->font_family);
            style->font_family = p2p_strdup_c(out);
        }
        free(out);
        free(copy);
        return;
    }
    if (strcmp(name, "font") == 0) {
        /* `font: [style] [weight] size[/line-height] family`. Only the size
         * and line-height components are kept, which is all a print pipeline
         * can honour from the shorthand. */
        char copy[192];
        char *cursor;
        if (strlen(value) >= sizeof(copy)) {
            return;
        }
        strcpy(copy, value);
        cursor = copy;
        while (*cursor != '\0') {
            char *token;
            char *end;
            while (isspace((unsigned char)*cursor)) ++cursor;
            if (*cursor == '\0') break;
            token = cursor;
            end = token;
            while (*end != '\0' && *end != ' ' && *end != '/') ++end;
            {
                const char saved = *end;
                *end = '\0';
                if (parse_value(token, context, font_size, &parsed) &&
                    value_is_length(parsed) && !value_is_auto(parsed)) {
                    double resolved = value_resolve_length(
                        parsed, font_size, font_size, context->root_font_size,
                        context->viewport_width, context->viewport_height, true);
                    if (value_is_percentage(parsed)) {
                        resolved = font_size * parsed.number / 100.0;
                    }
                    if (resolved >= 1.0 && resolved <= 400.0) {
                        style->font_size = px(resolved);
                    }
                }
                *end = saved;
            }
            cursor = end;
            if (*cursor == '/') {
                /* The token after the slash is the line-height. */
                char *line_token;
                ++cursor;
                while (isspace((unsigned char)*cursor)) ++cursor;
                line_token = cursor;
                end = line_token;
                while (*end != '\0' && *end != ' ') ++end;
                {
                    const char saved = *end;
                    *end = '\0';
                    if (p2p_iequals(line_token, "normal")) {
                        style->line_height = value_number(0.0);
                        style->line_height_unit = (signed char)UNIT_NUMBER;
                    } else if (parse_value(line_token, context, font_size, &parsed)) {
                        if (value_is_number(parsed) && parsed.number >= 0.0) {
                            style->line_height = value_number(parsed.number);
                            style->line_height_unit = (signed char)UNIT_NUMBER;
                        } else if (value_is_length(parsed)) {
                            style->line_height = parsed;
                            style->line_height_unit = (signed char)UNIT_PX;
                        }
                    }
                    *end = saved;
                }
                cursor = end;
            }
        }
        return;
    }
    if (strcmp(name, "line-height") == 0) {
        if (p2p_iequals(value, "normal")) {
            style->line_height = value_number(0.0);
            style->line_height_unit = (signed char)UNIT_NUMBER;
        } else if (parse_value(value, context, font_size, &parsed)) {
            if (value_is_number(parsed) && parsed.number >= 0.0) {
                style->line_height = value_number(parsed.number);
                style->line_height_unit = (signed char)UNIT_NUMBER;
            } else if (value_is_auto(parsed)) {
                style->line_height = value_number(0.0);
                style->line_height_unit = (signed char)UNIT_NUMBER;
            } else {
                style->line_height = parsed;
                style->line_height_unit = (signed char)UNIT_PX;
            }
        }
        return;
    }
    if (strcmp(name, "letter-spacing") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->letter_spacing = parsed;
        return;
    }
    if (strcmp(name, "word-spacing") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->word_spacing = parsed;
        return;
    }
    if (strcmp(name, "text-indent") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->text_indent = parsed;
        return;
    }
    if (strcmp(name, "tab-size") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->tab_size = parsed;
        return;
    }
    if (strcmp(name, "color") == 0) {
        Color parsed_color;
        if (color_parse(value, style->foreground, &parsed_color))
            style->foreground = parsed_color;
        return;
    }
    if (strcmp(name, "opacity") == 0) {
        if (parse_value(value, context, font_size, &parsed) && value_is_number(parsed)) {
            style->opacity = (float)p2p_clamp(parsed.number, 0.0, 1.0);
        }
        return;
    }
    if (strcmp(name, "background-color") == 0) {
        Color parsed_color;
        if (color_parse(value, style->foreground, &parsed_color)) {
            style->background = parsed_color;
            style->background_set = true;
        }
        return;
    }
    if (strcmp(name, "background-image") == 0) {
        free(style->background_image);
        style->background_image = p2p_iequals(value, "none") ? NULL : p2p_strdup_c(value);
        return;
    }
    if (strcmp(name, "background") == 0) {
        /* `background: <color> <image> <position> / <size> <repeat>` */
        char copy[256];
        char *cursor;
        if (strlen(value) >= sizeof(copy)) return;
        strcpy(copy, value);
        if (p2p_iequals(copy, "none")) {
            style->background_set = false;
            style->background = COLOR_TRANSPARENT;
            free(style->background_image);
            style->background_image = NULL;
            return;
        }
        cursor = copy;
        while (*cursor != '\0') {
            char *token;
            char *end;
            while (*cursor == ' ' || *cursor == '/') ++cursor;
            if (*cursor == '\0') break;
            token = cursor;
            end = token;
            while (*end != '\0' && *end != ' ' && *end != '/') ++end;
            {
                char saved = *end;
                *end = '\0';
                {
                    Color parsed_color;
                    if (color_parse(token, style->foreground, &parsed_color)) {
                        style->background = parsed_color;
                        style->background_set = true;
                    } else if (p2p_starts_with(token, "url(") ||
                               p2p_starts_with(token, "linear-gradient(") ||
                               p2p_starts_with(token, "radial-gradient(") ||
                               p2p_starts_with(token, "repeating-linear-gradient(") ||
                               p2p_starts_with(token, "repeating-radial-gradient(")) {
                        free(style->background_image);
                        style->background_image = p2p_strdup_c(token);
                    }
                }
                *end = saved;
            }
            cursor = end;
        }
        return;
    }
    if (strcmp(name, "width") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->width = parsed;
        return;
    }
    if (strcmp(name, "height") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->height = parsed;
        return;
    }
    if (strcmp(name, "min-width") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->min_width = parsed;
        return;
    }
    if (strcmp(name, "max-width") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->max_width = parsed;
        return;
    }
    if (strcmp(name, "min-height") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->min_height = parsed;
        return;
    }
    if (strcmp(name, "max-height") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->max_height = parsed;
        return;
    }
    if (strcmp(name, "margin") == 0) {
        apply_box_shorthand(style->margin, value, context, font_size, true);
        return;
    }
    if (strcmp(name, "padding") == 0) {
        apply_box_shorthand(style->padding, value, context, font_size, false);
        return;
    }
    if (strncmp(name, "margin-", 7) == 0 && side_from_keyword(name + 7, -1) >= 0) {
        if (parse_value(value, context, font_size, &parsed))
            style->margin[side_from_keyword(name + 7, SIDE_TOP)] = parsed;
        return;
    }
    if (strncmp(name, "padding-", 8) == 0 && side_from_keyword(name + 8, -1) >= 0) {
        if (parse_value(value, context, font_size, &parsed))
            style->padding[side_from_keyword(name + 8, SIDE_TOP)] = parsed;
        return;
    }
    if (strcmp(name, "border") == 0) {
        /* `border: <width> <style> <color>` on every side. */
        char copy[128];
        char *cursor;
        if (strlen(value) >= sizeof(copy)) return;
        strcpy(copy, value);
        cursor = copy;
        while (*cursor != '\0') {
            char *token = cursor;
            char *end = token;
            while (*end != '\0' && *end != ' ') ++end;
            {
                char saved = *end;
                char lower[64];
                size_t length;
                *end = '\0';
                length = strlen(token);
                if (length >= sizeof(lower)) length = sizeof(lower) - 1U;
                memcpy(lower, token, length);
                lower[length] = '\0';
                p2p_lower(lower);
                if (lookup_enum(kBorderStyle, sizeof(kBorderStyle) / sizeof(kBorderStyle[0]),
                                lower, &number)) {
                    for (int i = 0; i < 4; ++i) {
                        style->border_style[i] = (BorderStyle)number;
                        style->border_width_set[i] = true;
                    }
                } else {
                    Color parsed_color;
                    Value length_value;
                    if (parse_value(token, context, font_size, &length_value) &&
                        value_is_length(length_value)) {
                        for (int i = 0; i < 4; ++i) {
                            style->border_width[i] = length_value;
                            style->border_width_set[i] = true;
                            if (style->border_style[i] == BORDER_NONE)
                                style->border_style[i] = BORDER_SOLID;
                        }
                    } else if (color_parse(token, style->foreground, &parsed_color)) {
                        for (int i = 0; i < 4; ++i) {
                            style->border_color[i] = parsed_color;
                        }
                    }
                }
                *end = saved;
            }
            cursor = end;
            while (*cursor == ' ') ++cursor;
        }
        return;
    }
    if (strncmp(name, "border-", 7) == 0) {
        const char *rest = name + 7;
        int side = side_from_keyword(rest, -1);
        if (side >= 0) {
            /* border-<side> shorthand; handled with the other side shorthands
             * further down so the token scan lives in one place. */
            return;
        }
        if (strcmp(rest, "width") == 0) {
            if (parse_value(value, context, font_size, &parsed))
                for (int i = 0; i < 4; ++i) {
                    style->border_width[i] = parsed;
                    style->border_width_set[i] = true;
                }
            return;
        }
        if (strcmp(rest, "style") == 0) {
            if (lookup_enum(kBorderStyle, sizeof(kBorderStyle) / sizeof(kBorderStyle[0]), value, &number))
                for (int i = 0; i < 4; ++i) style->border_style[i] = (BorderStyle)number;
            return;
        }
        if (strcmp(rest, "color") == 0) {
            Color parsed_color;
            if (color_parse(value, style->foreground, &parsed_color))
                for (int i = 0; i < 4; ++i) style->border_color[i] = parsed_color;
            return;
        }
        if (strcmp(rest, "radius") == 0) {
            /* `border-radius: a / b` is horizontal / vertical. The printer
             * draws elliptical corners, so keep the larger of the two radii
             * per corner rather than dropping the vertical component. */
            const char *slash = strchr(value, '/');
            const size_t horizontal_length =
                slash != NULL ? (size_t)(slash - value) : strlen(value);
            size_t count = 0U;
            Value parts[4];
            char *cursor = p2p_strdup((const unsigned char *)value, horizontal_length);
            if (cursor == NULL) {
                return;
            }
            {
                char *token = cursor;
                while (token != NULL && count < 4U) {
                    char *end;
                    while (*token == ' ') ++token;
                    if (*token == '\0') break;
                    end = strchr(token, ' ');
                    if (end == NULL) end = token + strlen(token);
                    {
                        char saved = *end;
                        *end = '\0';
                        if (parse_value(token, context, font_size, &parts[count]) &&
                            value_is_length(parts[count]))
                            ++count;
                        *end = saved;
                    }
                    token = *end == '\0' ? NULL : end + 1;
                }
            }
            free(cursor);
            if (count == 0U) return;
            style->border_radius[CORNER_TOP_LEFT] = parts[0];
            style->border_radius[CORNER_TOP_RIGHT] = count > 1U ? parts[1] : parts[0];
            style->border_radius[CORNER_BOTTOM_RIGHT] = count > 2U ? parts[2] : parts[0];
            style->border_radius[CORNER_BOTTOM_LEFT] = count > 3U ? parts[3] : count > 1U ? parts[1] : parts[0];
            if (slash != NULL) {
                Value vertical[4];
                size_t vertical_count = 0U;
                char *cursor_v = p2p_strdup_c(slash + 1);
                if (cursor_v != NULL) {
                    char *token = cursor_v;
                    while (token != NULL && vertical_count < 4U) {
                        char *end;
                        while (*token == ' ') ++token;
                        if (*token == '\0') break;
                        end = strchr(token, ' ');
                        if (end == NULL) end = token + strlen(token);
                        {
                            char saved = *end;
                            *end = '\0';
                            if (parse_value(token, context, font_size, &vertical[vertical_count]) &&
                                value_is_length(vertical[vertical_count]))
                                ++vertical_count;
                            *end = saved;
                        }
                        token = *end == '\0' ? NULL : end + 1;
                    }
                    free(cursor_v);
                }
                if (vertical_count != 0U) {
                    /* The vertical shorthand has the same corner shape as the
                     * horizontal one, so index directly. */
                    for (int i = 0; i < 4; ++i) {
                        const Value *candidate = i < (int)vertical_count
                            ? &vertical[i] : &vertical[vertical_count - 1U];
                        if (candidate->number > style->border_radius[i].number) {
                            style->border_radius[i] = *candidate;
                        }
                    }
                }
            }
            style->border_radius_set = true;
            return;
        }
        return;
    }
    /* border-top / border-right / border-bottom / border-left shorthands. */
    {
        int side = -1;
        if (strcmp(name, "border-top") == 0) side = SIDE_TOP;
        else if (strcmp(name, "border-right") == 0) side = SIDE_RIGHT;
        else if (strcmp(name, "border-bottom") == 0) side = SIDE_BOTTOM;
        else if (strcmp(name, "border-left") == 0) side = SIDE_LEFT;
        if (side >= 0) {
            char copy[128];
            char *cursor;
            if (strlen(value) >= sizeof(copy)) return;
            strcpy(copy, value);
            cursor = copy;
            while (*cursor != '\0') {
                char *token = cursor;
                char *end = token;
                while (*end != '\0' && *end != ' ') ++end;
                {
                    char saved = *end;
                    char lower[64];
                    size_t length;
                    *end = '\0';
                    length = strlen(token);
                    if (length >= sizeof(lower)) length = sizeof(lower) - 1U;
                    memcpy(lower, token, length);
                    lower[length] = '\0';
                    p2p_lower(lower);
                    if (lookup_enum(kBorderStyle, sizeof(kBorderStyle) / sizeof(kBorderStyle[0]),
                                    lower, &number)) {
                        style->border_style[side] = (BorderStyle)number;
                        style->border_width_set[side] = true;
                    } else {
                        Color parsed_color;
                        Value length_value;
                        if (parse_value(token, context, font_size, &length_value) &&
                            value_is_length(length_value)) {
                            style->border_width[side] = length_value;
                            style->border_width_set[side] = true;
                            if (style->border_style[side] == BORDER_NONE)
                                style->border_style[side] = BORDER_SOLID;
                        } else if (color_parse(token, style->foreground, &parsed_color)) {
                            style->border_color[side] = parsed_color;
                        }
                    }
                    *end = saved;
                }
                cursor = end;
                while (*cursor == ' ') ++cursor;
            }
            return;
        }
    }
    {
        int side = -1;
        if (strcmp(name, "border-top-width") == 0) side = SIDE_TOP;
        else if (strcmp(name, "border-right-width") == 0) side = SIDE_RIGHT;
        else if (strcmp(name, "border-bottom-width") == 0) side = SIDE_BOTTOM;
        else if (strcmp(name, "border-left-width") == 0) side = SIDE_LEFT;
        if (side >= 0) {
            if (parse_value(value, context, font_size, &parsed)) {
                style->border_width[side] = parsed;
                style->border_width_set[side] = true;
                if (style->border_style[side] == BORDER_NONE)
                    style->border_style[side] = BORDER_SOLID;
            }
            return;
        }
    }
    {
        int side = -1;
        if (strcmp(name, "border-top-style") == 0) side = SIDE_TOP;
        else if (strcmp(name, "border-right-style") == 0) side = SIDE_RIGHT;
        else if (strcmp(name, "border-bottom-style") == 0) side = SIDE_BOTTOM;
        else if (strcmp(name, "border-left-style") == 0) side = SIDE_LEFT;
        if (side >= 0) {
            if (lookup_enum(kBorderStyle, sizeof(kBorderStyle) / sizeof(kBorderStyle[0]), value, &number))
                style->border_style[side] = (BorderStyle)number;
            return;
        }
    }
    {
        int side = -1;
        if (strcmp(name, "border-top-color") == 0) side = SIDE_TOP;
        else if (strcmp(name, "border-right-color") == 0) side = SIDE_RIGHT;
        else if (strcmp(name, "border-bottom-color") == 0) side = SIDE_BOTTOM;
        else if (strcmp(name, "border-left-color") == 0) side = SIDE_LEFT;
        if (side >= 0) {
            Color parsed_color;
            if (color_parse(value, style->foreground, &parsed_color))
                style->border_color[side] = parsed_color;
            return;
        }
    }
    {
        int corner = -1;
        if (strcmp(name, "border-top-left-radius") == 0) corner = CORNER_TOP_LEFT;
        else if (strcmp(name, "border-top-right-radius") == 0) corner = CORNER_TOP_RIGHT;
        else if (strcmp(name, "border-bottom-right-radius") == 0) corner = CORNER_BOTTOM_RIGHT;
        else if (strcmp(name, "border-bottom-left-radius") == 0) corner = CORNER_BOTTOM_LEFT;
        if (corner >= 0) {
            if (parse_value(value, context, font_size, &parsed) && value_is_length(parsed)) {
                style->border_radius[corner] = parsed;
                style->border_radius_set = true;
            }
            return;
        }
    }
    if (strcmp(name, "list-style") == 0) {
        if (p2p_iequals(value, "none")) {
            style->list_style_type = LIST_NONE;
        } else {
            char copy[128];
            char *cursor;
            if (strlen(value) >= sizeof(copy)) return;
            strcpy(copy, value);
            cursor = copy;
            while (*cursor != '\0') {
                char *token = cursor;
                char *end = token;
                while (*end != '\0' && *end != ' ') ++end;
                {
                    char saved = *end;
                    char lower[64];
                    size_t length;
                    *end = '\0';
                    length = strlen(token);
                    if (length >= sizeof(lower)) length = sizeof(lower) - 1U;
                    memcpy(lower, token, length);
                    lower[length] = '\0';
                    p2p_lower(lower);
                    if (lookup_enum(kListStyleType,
                                    sizeof(kListStyleType) / sizeof(kListStyleType[0]),
                                    lower, &number))
                        style->list_style_type = (ListStyle)number;
                    else if (lookup_enum(kListPosition,
                                         sizeof(kListPosition) / sizeof(kListPosition[0]),
                                         lower, &number))
                        style->list_position = (ListPosition)number;
                    *end = saved;
                }
                cursor = end;
                while (*cursor == ' ') ++cursor;
            }
        }
        return;
    }
    if (strcmp(name, "list-style-image") == 0) {
        return;
    }
    if (strcmp(name, "text-decoration") == 0 || strcmp(name, "text-decoration-line") == 0) {
        if (p2p_iequals(value, "none")) {
            style->decoration = 0U;
        } else {
            style->decoration = 0U;
            if (strstr(value, "underline") != NULL) style->decoration |= DECO_UNDERLINE;
            if (strstr(value, "overline") != NULL) style->decoration |= DECO_OVERLINE;
            if (strstr(value, "line-through") != NULL) style->decoration |= DECO_LINE_THROUGH;
        }
        return;
    }
    if (strcmp(name, "text-decoration-color") == 0) {
        Color parsed_color;
        if (color_parse(value, style->foreground, &parsed_color)) {
            style->decoration_color = parsed_color;
            style->decoration_color_set = true;
        }
        return;
    }
    if (strcmp(name, "object-fit") == 0) {
        if (lookup_enum(kObjectFit, sizeof(kObjectFit) / sizeof(kObjectFit[0]), value, &number))
            style->object_fit = (ObjectFit)number;
        return;
    }
    if (strcmp(name, "table-layout") == 0) {
        if (lookup_enum(kTableLayout, sizeof(kTableLayout) / sizeof(kTableLayout[0]), value, &number))
            style->table_layout = (TableLayout)number;
        return;
    }
    if (strcmp(name, "border-collapse") == 0) {
        if (lookup_enum(kBorderCollapse, sizeof(kBorderCollapse) / sizeof(kBorderCollapse[0]), value, &number))
            style->border_collapse = (BorderCollapse)number;
        return;
    }
    if (strcmp(name, "border-spacing") == 0) {
        Value parts[2];
        size_t count = 0U;
        char copy[64];
        char *cursor;
        if (strlen(value) >= sizeof(copy)) return;
        strcpy(copy, value);
        cursor = copy;
        while (cursor != NULL && count < 2U) {
            char *end;
            char *token;
            while (isspace((unsigned char)*cursor)) ++cursor;
            if (*cursor == '\0') break;
            token = cursor;
            end = strchr(cursor, ' ');
            if (end == NULL) end = cursor + strlen(cursor);
            {
                char saved = *end;
                *end = '\0';
                if (parse_value(token, context, font_size, &parts[count]) && value_is_length(parts[count]))
                    ++count;
                *end = saved;
            }
            cursor = *end == '\0' ? NULL : end + 1;
        }
        if (count == 0U) return;
        style->border_spacing[0] = parts[0].number;
        style->border_spacing[1] = count > 1U ? parts[1].number : parts[0].number;
        style->border_spacing_set = true;
        return;
    }
    if (strcmp(name, "caption-side") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->caption_side = parsed;
        return;
    }
    if (strcmp(name, "flex-direction") == 0) {
        if (lookup_enum(kFlexDirection, sizeof(kFlexDirection) / sizeof(kFlexDirection[0]), value, &number))
            style->flex_direction = (FlexDirection)number;
        return;
    }
    if (strcmp(name, "flex-wrap") == 0) {
        if (lookup_enum(kFlexWrap, sizeof(kFlexWrap) / sizeof(kFlexWrap[0]), value, &number))
            style->flex_wrap = (FlexWrap)number;
        return;
    }
    if (strcmp(name, "flex-flow") == 0) {
        char copy[64];
        char *cursor;
        if (strlen(value) >= sizeof(copy)) return;
        strcpy(copy, value);
        cursor = copy;
        while (*cursor != '\0') {
            char *token = cursor;
            char *end = token;
            while (*end != '\0' && *end != ' ') ++end;
            {
                char saved = *end;
                *end = '\0';
                if (lookup_enum(kFlexDirection, sizeof(kFlexDirection) / sizeof(kFlexDirection[0]), token, &number))
                    style->flex_direction = (FlexDirection)number;
                else if (lookup_enum(kFlexWrap, sizeof(kFlexWrap) / sizeof(kFlexWrap[0]), token, &number))
                    style->flex_wrap = (FlexWrap)number;
                *end = saved;
            }
            cursor = end;
            while (*cursor == ' ') ++cursor;
        }
        return;
    }
    if (strcmp(name, "justify-content") == 0) {
        if (lookup_enum(kFlexAlign, sizeof(kFlexAlign) / sizeof(kFlexAlign[0]), value, &number))
            style->justify_content = (FlexAlign)number;
        return;
    }
    if (strcmp(name, "align-items") == 0) {
        if (lookup_enum(kFlexAlign, sizeof(kFlexAlign) / sizeof(kFlexAlign[0]), value, &number))
            style->align_items = number == FLEX_AUTO ? FLEX_STRETCH : (FlexAlign)number;
        return;
    }
    if (strcmp(name, "align-self") == 0) {
        if (lookup_enum(kFlexAlign, sizeof(kFlexAlign) / sizeof(kFlexAlign[0]), value, &number))
            style->align_self = (FlexAlign)number;
        return;
    }
    if (strcmp(name, "align-content") == 0) {
        if (lookup_enum(kFlexAlign, sizeof(kFlexAlign) / sizeof(kFlexAlign[0]), value, &number))
            style->align_content = number == FLEX_AUTO ? FLEX_STRETCH : (FlexAlign)number;
        return;
    }
    if (strcmp(name, "row-gap") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->row_gap = parsed;
        return;
    }
    if (strcmp(name, "column-gap") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->column_gap = parsed;
        return;
    }
    if (strcmp(name, "gap") == 0) {
        Value parts[2];
        size_t count = 0U;
        char copy[64];
        char *cursor;
        if (strlen(value) >= sizeof(copy)) return;
        strcpy(copy, value);
        cursor = copy;
        while (cursor != NULL && count < 2U) {
            char *end;
            char *token;
            while (isspace((unsigned char)*cursor)) ++cursor;
            if (*cursor == '\0') break;
            token = cursor;
            end = strchr(cursor, ' ');
            if (end == NULL) end = cursor + strlen(cursor);
            {
                char saved = *end;
                *end = '\0';
                if (parse_value(token, context, font_size, &parts[count]) &&
                    value_is_length(parts[count]))
                    ++count;
                *end = saved;
            }
            cursor = *end == '\0' ? NULL : end + 1;
        }
        if (count == 0U) return;
        style->row_gap = parts[0];
        style->column_gap = count > 1U ? parts[1] : parts[0];
        return;
    }
    if (strcmp(name, "flex") == 0) {
        char copy[96];
        char *cursor;
        if (strlen(value) >= sizeof(copy)) return;
        strcpy(copy, value);
        cursor = copy;
        while (*cursor != '\0') {
            char *token = cursor;
            char *end = token;
            while (*end != '\0' && *end != ' ') ++end;
            {
                char saved = *end;
                *end = '\0';
                if (p2p_iequals(token, "none")) {
                    style->flex_grow = 0.0;
                    style->flex_shrink = 0.0;
                    style->flex_basis = value_auto();
                } else if (p2p_iequals(token, "auto")) {
                    style->flex_grow = 1.0;
                    style->flex_shrink = 1.0;
                    style->flex_basis = value_auto();
                } else if (p2p_iequals(token, "initial")) {
                    style->flex_grow = 0.0;
                    style->flex_shrink = 1.0;
                    style->flex_basis = value_auto();
                } else {
                    const double number = strtod(token, NULL);
                    const char *cursor2 = token;
                    while (*cursor2 != '\0' && *cursor2 != ' ') ++cursor2;
                    if (strcmp(cursor2, "") == 0) {
                        style->flex_grow = number;
                    } else {
                        char basis_copy[64];
                        char *basis_end;
                        snprintf(basis_copy, sizeof(basis_copy), "%s", cursor2);
                        basis_end = basis_copy;
                        while (*basis_end != '\0' && *basis_end != ' ') ++basis_end;
                        *basis_end = '\0';
                        if (parse_value(basis_copy, context, font_size, &parsed) &&
                            !value_is_auto(parsed))
                            style->flex_basis = parsed;
                    }
                }
                *end = saved;
            }
            cursor = end;
            while (*cursor == ' ') ++cursor;
        }
        return;
    }
    if (strcmp(name, "flex-grow") == 0) {
        if (parse_value(value, context, font_size, &parsed) && value_is_number(parsed))
            style->flex_grow = p2p_clamp(parsed.number, 0.0, 1000.0);
        return;
    }
    if (strcmp(name, "flex-shrink") == 0) {
        if (parse_value(value, context, font_size, &parsed) && value_is_number(parsed))
            style->flex_shrink = p2p_clamp(parsed.number, 0.0, 1000.0);
        return;
    }
    if (strcmp(name, "flex-basis") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->flex_basis = parsed;
        return;
    }
    if (strcmp(name, "order") == 0) {
        if (parse_value(value, context, font_size, &parsed) && value_is_number(parsed))
            style->order = (int)parsed.number;
        return;
    }
    if (strcmp(name, "grid-template-columns") == 0) {
        free(style->grid_template_columns);
        style->grid_template_columns = p2p_strdup_c(value);
        return;
    }
    if (strcmp(name, "grid-template-rows") == 0) {
        free(style->grid_template_rows);
        style->grid_template_rows = p2p_strdup_c(value);
        return;
    }
    if (strcmp(name, "grid-auto-columns") == 0) {
        free(style->grid_auto_columns);
        style->grid_auto_columns = p2p_strdup_c(value);
        return;
    }
    if (strcmp(name, "grid-auto-rows") == 0) {
        free(style->grid_auto_rows);
        style->grid_auto_rows = p2p_strdup_c(value);
        return;
    }
    if (strcmp(name, "grid-column-gap") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->grid_column_gap = parsed;
        return;
    }
    if (strcmp(name, "grid-row-gap") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->grid_row_gap = parsed;
        return;
    }
    if (strcmp(name, "grid-auto-flow") == 0) {
        if (strstr(value, "column") != NULL)
            style->grid_auto_flow |= 1;
        if (strstr(value, "dense") != NULL)
            style->grid_auto_flow |= 2;
        return;
    }
    if (strcmp(name, "column-count") == 0) {
        if (parse_value(value, context, font_size, &parsed) && value_is_number(parsed))
            style->column_count = (int)p2p_clamp(parsed.number, 1.0, 64.0);
        if (p2p_iequals(value, "auto")) style->column_count = 1;
        return;
    }
    if (strcmp(name, "column-width") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->column_width = parsed;
        return;
    }
    if (strcmp(name, "column-gap") == 0 || strcmp(name, "columns-gap") == 0) {
        if (parse_value(value, context, font_size, &parsed)) style->column_gap = parsed;
        return;
    }
    if (strcmp(name, "columns") == 0) {
        if (parse_value(value, context, font_size, &parsed)) {
            if (value_is_number(parsed))
                style->column_count = (int)p2p_clamp(parsed.number, 1.0, 64.0);
            else
                style->column_width = parsed;
        }
        return;
    }
    if (strcmp(name, "break-inside") == 0) {
        if (lookup_enum(kBreakInside, sizeof(kBreakInside) / sizeof(kBreakInside[0]), value, &number))
            style->break_inside = (BreakInside)number;
        return;
    }
    if (strcmp(name, "break-before") == 0) {
        if (lookup_enum(kBreakBefore, sizeof(kBreakBefore) / sizeof(kBreakBefore[0]), value, &number))
            style->break_before = (BreakMode)number;
        return;
    }
    if (strcmp(name, "break-after") == 0) {
        if (lookup_enum(kBreakBefore, sizeof(kBreakBefore) / sizeof(kBreakBefore[0]), value, &number))
            style->break_after = (BreakMode)number;
        return;
    }
    if (strcmp(name, "page-break-before") == 0 || strcmp(name, "page-break-after") == 0) {
        const bool before = name[11] == 'b';
        int mode = BREAK_AUTO;
        if (p2p_iequals(value, "always") || p2p_iequals(value, "page") ||
            p2p_iequals(value, "left") || p2p_iequals(value, "right")) {
            mode = BREAK_PAGE;
        } else if (p2p_iequals(value, "avoid")) {
            mode = BREAK_AVOID;
        }
        if (before) style->break_before = (BreakMode)mode;
        else style->break_after = (BreakMode)mode;
        return;
    }
    if (strcmp(name, "page-break-inside") == 0) {
        style->page_break_inside = p2p_iequals(value, "avoid") ? 1 : 0;
        return;
    }
    if (strcmp(name, "orphans") == 0) {
        if (parse_value(value, context, font_size, &parsed) && value_is_number(parsed))
            style->orphans = (int)p2p_clamp(parsed.number, 1.0, 100.0);
        return;
    }
    if (strcmp(name, "widows") == 0) {
        if (parse_value(value, context, font_size, &parsed) && value_is_number(parsed))
            style->widows = (int)p2p_clamp(parsed.number, 1.0, 100.0);
        return;
    }
    if (strcmp(name, "z-index") == 0) {
        if (parse_value(value, context, font_size, &parsed) && value_is_number(parsed))
            style->z_index = (int)parsed.number;
        return;
    }
    if (strcmp(name, "background-repeat") == 0) {
        if (lookup_enum(kBackgroundRepeat, NAME_VALUE_COUNT(kBackgroundRepeat),
                        value, &number))
            style->background_repeat = number;
        return;
    }
    if (strcmp(name, "background-attachment") == 0) {
        return;
    }
    if (strcmp(name, "background-position") == 0) {
        free(style->background_position);
        style->background_position = p2p_strdup_c(value);
        return;
    }
    if (strcmp(name, "background-size") == 0) {
        free(style->background_size);
        style->background_size = p2p_strdup_c(value);
        return;
    }
    if (strcmp(name, "content") == 0) {
        /* `content` is only meaningful on ::before / ::after / ::marker, which
         * css_parse.c reads straight off the pseudo-element rule. On an
         * ordinary element it is inert, as the spec requires. */
        return;
    }
    if (strcmp(name, "print-color-adjust") == 0 ||
        strcmp(name, "-webkit-print-color-adjust") == 0) {
        return;
    }
}

/* Splits a declaration block and applies every declaration it contains.
 *
 * The block is copied to one heap buffer because a single declaration can be
 * arbitrarily long (a base64 `background-image` is the realistic worst case)
 * and the engine must never place an attacker-sized value on the stack. */
void css_apply_declarations(Style *style, const char *declarations,
                            const CssContext *context, double font_size) {
    css_apply_cascade_block(style, declarations, context, &font_size);
}

void css_apply_cascade_block(Style *style, const char *declarations,
                             const CssContext *context, double *font_size) {
    char *buffer;
    char *cursor;
    if (declarations == NULL || context == NULL) {
        return;
    }
    if (strlen(declarations) > P2P_MAX_DECLARATION_BYTES) {
        return;
    }
    buffer = p2p_strdup_c(declarations);
    if (buffer == NULL) {
        return;
    }
    cursor = buffer;
    while (*cursor != '\0') {
        char *start = cursor;
        char *end;
        char *colon;
        char quote = '\0';
        int depth = 0;
        /* Find the end of this declaration without disturbing the scan, so the
         * cursor keeps pointing into the original text. */
        while (*cursor != '\0') {
            const char c = *cursor;
            if (quote != '\0') {
                if (c == '\\' && cursor[1] != '\0') {
                    ++cursor;
                } else if (c == quote) {
                    quote = '\0';
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth > 0) --depth;
            } else if (c == ';' && depth == 0) {
                break;
            }
            ++cursor;
        }
        end = cursor;
        if (*end == ';') {
            ++end;
        }
        colon = strchr(start, ':');
        if (colon != NULL && colon < end) {
            char property[96];
            char vendor[96];
            char *name;
            char *value;
            char *name_end;
            *colon = '\0';
            name = p2p_trim(start);
            value = p2p_trim(colon + 1);
            /* Strip `!important`. The cascade splits it into a separate block
             * before calling, so the flag only has to be removed here. */
            {
                const size_t value_length = strlen(value);
                if (value_length >= 10U &&
                    strcmp(value + value_length - 10U, "!important") == 0) {
                    value[value_length - 10U] = '\0';
                    value = p2p_trim(value);
                }
            }
            /* Split a vendor prefix off the property name. */
            name_end = name;
            if (strncmp(name, "-webkit-", 8) == 0 ||
                strncmp(name, "-moz-", 5) == 0 ||
                strncmp(name, "-o-", 3) == 0 ||
                strncmp(name, "-ms-", 4) == 0) {
                const char *dash = strchr(name + 1, '-');
                if (dash != NULL) {
                    name_end = dash + 1;
                }
            }
            {
                size_t property_length = (size_t)(name_end - name);
                size_t vendor_length = (size_t)(name_end - name);
                if (property_length >= sizeof(property)) {
                    property_length = sizeof(property) - 1U;
                }
                if (vendor_length >= sizeof(vendor)) {
                    vendor_length = sizeof(vendor) - 1U;
                }
                memcpy(property, name, property_length);
                property[property_length] = '\0';
                p2p_lower(property);
                memcpy(vendor, name, vendor_length);
                vendor[vendor_length] = '\0';
                p2p_lower(vendor);
            }
            if (*name != '\0' && *value != '\0') {
                const double before = style->font_size.number;
                apply_declaration(style, property, value, context, *font_size,
                                  vendor, property);
                if (style->font_size.number != before) {
                    *font_size = style->font_size.number;
                }
            }
        }
        cursor = end;
    }
    free(buffer);
}

/* Splits a declaration block into its normal and `!important` halves. The two
 * halves are applied in separate cascade passes, which is what makes
 * `!important` outrank specificity rather than merely win a tie. */
void css_split_important(const char *declarations, char **normal,
                         char **important) {
    char *buffer;
    char *cursor;
    if (normal != NULL) *normal = NULL;
    if (important != NULL) *important = NULL;
    if (declarations == NULL) {
        return;
    }
    if (strlen(declarations) > P2P_MAX_DECLARATION_BYTES) {
        return;
    }
    buffer = p2p_strdup_c(declarations);
    if (buffer == NULL) {
        return;
    }
    cursor = buffer;
    while (*cursor != '\0') {
        char *start = cursor;
        char *end;
        char *body;
        char quote = '\0';
        int depth = 0;
        size_t body_length;
        while (*cursor != '\0') {
            const char c = *cursor;
            if (quote != '\0') {
                if (c == '\\' && cursor[1] != '\0') {
                    ++cursor;
                } else if (c == quote) {
                    quote = '\0';
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth > 0) --depth;
            } else if (c == ';' && depth == 0) {
                break;
            }
            ++cursor;
        }
        end = cursor;
        if (*end == ';') {
            ++end;
        }
        body = start;
        body_length = (size_t)(end - start);
        while (body_length > 0U &&
               (body[body_length - 1U] == ';' || body[body_length - 1U] == ' ')) {
            --body_length;
        }
        if (body_length > 0U) {
            char *bang = memchr(body, '!', body_length);
            bool is_important = false;
            if (bang != NULL) {
                const char *tail = p2p_skip_space((char *)bang + 1);
                if (p2p_istrstr(tail, "important") == tail) {
                    is_important = true;
                    body_length = (size_t)(bang - body);
                    while (body_length > 0U && body[body_length - 1U] == ' ') {
                        --body_length;
                    }
                }
            }
            {
                char **target = is_important ? important : normal;
                if (target != NULL) {
                    char *addition = p2p_strdup((const unsigned char *)body, body_length);
                    if (addition != NULL) {
                        if (*target == NULL) {
                            *target = addition;
                        } else {
                            const size_t existing = strlen(*target);
                            char *joined = p2p_format("%s;%s", *target, addition);
                            free(addition);
                            if (joined != NULL) {
                                free(*target);
                                *target = joined;
                            } else {
                                (void)existing;
                            }
                        }
                    }
                }
            }
        }
        cursor = end;
    }
    free(buffer);
}
