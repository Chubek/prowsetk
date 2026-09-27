/* css_parse.c — stylesheet tokenizing, selector matching, and the cascade.
 *
 * Reads `<style>` elements and `style` attributes out of the node tree, parses
 * selectors into a form that can be matched cheaply, and produces one computed
 * Style per node. Also owns `@page` geometry and `@font-face` registration,
 * and materializes `::before` / `::after` boxes so that layout.c never has to
 * synthesize content.
 *
 * Rule selection uses rightmost-key bucketing: a rule is filed under the id,
 * each class, and the tag of its rightmost compound, and matching a node only
 * walks the buckets that node's own key can reach. Without that, a document
 * with a few thousand rules and a few thousand nodes is quadratic.
 */
#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- user-agent defaults */

/* The HTML rendering defaults. Author rules always outrank this sheet, and
 * nothing in here is reachable from the IR: it is the printer's own contract
 * for what an unstyled element looks like on paper. */
static const char kUserAgentSheet[] =
    "html, address, blockquote, body, dd, div, dl, dt, fieldset, figcaption, "
    "figure, footer, form, h1, h2, h3, h4, h5, h6, header, hgroup, hr, li, main, "
    "nav, ol, p, pre, section, table, ul, details, summary, dialog { display:block }\n"
    "head, link, meta, style, script, title, template, param, source, track, "
    "base, datalist, area, map, audio, video, canvas, svg, math, noembed, "
    "noframes { display:none }\n"
    "li { display:list-item }\n"
    "table { display:table }\n"
    "thead { display:table-header-group }\n"
    "tbody, tfoot { display:table-row-group }\n"
    "tr { display:table-row }\n"
    "td { display:table-cell; padding:1px }\n"
    "th { display:table-cell; padding:1px; font-weight:bold; text-align:center }\n"
    "caption { display:table-caption; text-align:center }\n"
    "col { display:table-column }\n"
    "colgroup { display:table-column-group }\n"
    "[hidden], dialog:not([open]) { display:none }\n"
    "body { margin:8px; font-size:16px }\n"
    "p { margin:1em 0 }\n"
    "h1 { font-size:2em; font-weight:bold; margin:0.67em 0 }\n"
    "h2 { font-size:1.5em; font-weight:bold; margin:0.83em 0 }\n"
    "h3 { font-size:1.17em; font-weight:bold; margin:1em 0 }\n"
    "h4 { font-size:1em; font-weight:bold; margin:1.33em 0 }\n"
    "h5 { font-size:0.83em; font-weight:bold; margin:1.67em 0 }\n"
    "h6 { font-size:0.67em; font-weight:bold; margin:2.33em 0 }\n"
    "blockquote, figure { margin:1em 40px }\n"
    "dl { margin:1em 0 }\n"
    "dd { margin-left:40px }\n"
    "hr { margin:0.5em 0; height:0; border-top-width:1px; "
    "border-top-style:solid; border-color:#808080 }\n"
    "ul, ol, dir, menu { margin:1em 0; padding-left:40px }\n"
    "pre, xmp, plaintext, listing { font-family:monospace; white-space:pre; "
    "margin:1em 0 }\n"
    "code, kbd, samp, tt { font-family:monospace }\n"
    "a:link, a:any-link { color:#0000ee; text-decoration:underline }\n"
    "a:visited { color:#551a8b }\n"
    "b, strong { font-weight:bold }\n"
    "i, cite, em, dfn, var, address { font-style:italic }\n"
    "small { font-size:0.83em }\n"
    "big { font-size:1.17em }\n"
    "sub { vertical-align:sub; font-size:0.83em }\n"
    "sup { vertical-align:super; font-size:0.83em }\n"
    "mark { background-color:#ffff00; color:#000000 }\n"
    "s, strike, del { text-decoration:line-through }\n"
    "u, ins { text-decoration:underline }\n"
    "center { text-align:center }\n"
    "form { margin-top:0 }\n"
    "fieldset { margin:0 2px; padding:0.35em 0.75em 0.625em; "
    "border:2px groove #d5d5d5 }\n"
    "legend { padding:0 2px }\n"
    "input, select, textarea, button { font-size:0.8125em; display:inline-block; "
    "margin:0; padding:1px 2px; border:2px inset #d5d5d5; background-color:#ffffff; "
    "color:#000000; text-align:start }\n"
    "textarea { white-space:pre-wrap; vertical-align:top }\n"
    "select { padding:0 2px }\n"
    "button, input[type=button], input[type=submit], input[type=reset] { "
    "text-align:center; padding:1px 6px; border:2px outset #d5d5d5; "
    "background-color:#efefef }\n"
    "input[type=checkbox], input[type=radio] { width:13px; height:13px; "
    "margin:3px 3px 3px 4px; padding:0; border:0; background-color:transparent }\n"
    "input[type=hidden] { display:none }\n"
    "table { border-spacing:2px; border-collapse:separate }\n"
    "table[border] { border:1px solid #808080 }\n"
    "thead, tbody, tfoot { vertical-align:middle }\n"
    "td, th { vertical-align:inherit }\n"
    "img { display:inline-block }\n"
    "progress, meter { display:inline-block; width:160px; height:16px }\n"
    "output { display:inline }\n"
    "q::before { content:\"\\201C\" }\n"
    "q::after { content:\"\\201D\" }\n";

/* ------------------------------------------------------------------ buckets */

enum {
    BUCKET_ID = 0,
    BUCKET_CLASS,
    BUCKET_TAG,
    BUCKET_UNIVERSAL
};

static size_t bucket_hash(unsigned char kind, const char *text) {
    size_t hash = (size_t)1469598103934665603ULL ^ (size_t)kind;
    for (; text != NULL && *text != '\0'; ++text) {
        hash ^= (unsigned char)*text;
        hash *= (size_t)1099511628211ULL;
    }
    return hash;
}

typedef struct {
    unsigned char kind;
    char *text;
    int *rules;
    size_t count;
    size_t capacity;
} KeyBucket;

typedef struct {
    KeyBucket *buckets;
    size_t count;
    size_t capacity;
    size_t mask;
} BucketIndex;

static void bucket_index_init(BucketIndex *index) {
    index->capacity = 2048U;
    index->mask = index->capacity - 1U;
    index->count = 0U;
    index->buckets = (KeyBucket *)p2p_calloc(index->capacity, sizeof(KeyBucket));
}

static void bucket_index_free(BucketIndex *index) {
    for (size_t i = 0U; i < index->count; ++i) {
        free(index->buckets[i].text);
        free(index->buckets[i].rules);
    }
    free(index->buckets);
    memset(index, 0, sizeof(*index));
}

/* Open-addressed lookup. The table is fixed size and simply stops accepting
 * new keys when it fills, which degrades to a slower linear probe rather than
 * to a crash or a wrong answer. */
static KeyBucket *bucket_index_find(BucketIndex *index, unsigned char kind,
                                    const char *text) {
    const size_t slot = bucket_hash(kind, text) & index->mask;
    size_t empty = index->capacity;
    /* At most `count` slots are occupied, so a linear probe cannot need to
     * look further than that. Bounding the probe is what keeps a miss on a
     * large document from becoming a full-table scan. */
    size_t limit = index->count + 8U;
    if (limit > index->capacity) {
        limit = index->capacity;
    }
    for (size_t probe = 0U; probe < limit; ++probe) {
        const size_t at = (slot + probe) & index->mask;
        KeyBucket *bucket = &index->buckets[at];
        if (bucket->text == NULL) {
            if (empty == index->capacity) {
                empty = at;
            }
            continue;
        }
        if (bucket->kind == kind && strcmp(bucket->text, text) == 0) {
            return bucket;
        }
    }
    if (empty == index->capacity) {
        return NULL;
    }
    {
        KeyBucket *bucket = &index->buckets[empty];
        bucket->text = p2p_strdup_c(text);
        if (bucket->text == NULL) {
            return NULL;
        }
        bucket->kind = kind;
        ++index->count;
        return bucket;
    }
}

static void bucket_index_add(BucketIndex *index, unsigned char kind,
                             const char *text, int rule) {
    KeyBucket *bucket = bucket_index_find(index, kind, text);
    if (bucket == NULL) {
        return;
    }
    if (bucket->count == bucket->capacity) {
        const size_t capacity = bucket->capacity == 0U ? 4U : bucket->capacity * 2U;
        int *grown = (int *)p2p_realloc(bucket->rules, capacity * sizeof(int));
        if (grown == NULL) {
            return;
        }
        bucket->rules = grown;
        bucket->capacity = capacity;
    }
    bucket->rules[bucket->count++] = rule;
}

/* Files one alternative under the keys of its rightmost compound. */
static void index_alternative(SelectorAlternative *alt, BucketIndex *index,
                              int rule) {
    const CompoundSelector *rightmost;
    if (alt->compounds == 0U) {
        bucket_index_add(index, BUCKET_UNIVERSAL, "*", rule);
        return;
    }
    rightmost = &alt->compound_list[alt->compounds - 1U];
    for (size_t i = 0U; i < rightmost->count; ++i) {
        const SimpleSelector *simple = &rightmost->simples[i];
        unsigned char kind;
        if (simple->kind == SIMPLE_ID && simple->name != NULL) {
            kind = BUCKET_ID;
        } else if (simple->kind == SIMPLE_CLASS && simple->name != NULL) {
            kind = BUCKET_CLASS;
        } else if (simple->kind == SIMPLE_TYPE && simple->name != NULL) {
            kind = BUCKET_TAG;
        } else {
            continue;
        }
        if (alt->key_count >= sizeof(alt->key_text) / sizeof(alt->key_text[0])) {
            break;
        }
        alt->key_kind[alt->key_count] = kind;
        alt->key_text[alt->key_count] = p2p_strdup_c(simple->name);
        if (alt->key_text[alt->key_count] != NULL) {
            ++alt->key_count;
            bucket_index_add(index, kind, simple->name, rule);
        }
    }
    if (alt->key_count == 0U) {
        /* Only structural pseudo-classes in the rightmost compound, such as
         * `:hover` or `:first-child` alone: reachable from every node. */
        alt->key_kind[0] = BUCKET_UNIVERSAL;
        alt->key_text[0] = p2p_strdup_c("*");
        if (alt->key_text[0] != NULL) {
            alt->key_count = 1U;
        }
        bucket_index_add(index, BUCKET_UNIVERSAL, "*", rule);
    }
}

/* ---------------------------------------------------------------- selectors */

typedef struct {
    const char *text;
    size_t length;
    size_t position;
} SelectorReader;

static void reader_skip(SelectorReader *reader) {
    while (reader->position < reader->length &&
           isspace((unsigned char)reader->text[reader->position])) {
        ++reader->position;
    }
}

/* Reads a bare name: an identifier, a class, an attribute name, or a pseudo
 * name. Stops at any character that cannot appear inside a CSS name. */
static char *reader_take_name(SelectorReader *reader) {
    size_t start;
    size_t length;
    reader_skip(reader);
    start = reader->position;
    while (reader->position < reader->length) {
        const char c = reader->text[reader->position];
        if (isspace((unsigned char)c) || c == '>' || c == '+' || c == '~' ||
            c == ',' || c == '#' || c == '.' || c == '[' || c == ']' ||
            c == ':' || c == '(' || c == ')' || c == '*' || c == '"' || c == '\'') {
            break;
        }
        ++reader->position;
    }
    length = reader->position - start;
    if (length == 0U) {
        return NULL;
    }
    {
        char *name = p2p_strdup((const unsigned char *)reader->text + start, length);
        if (name != NULL) {
            p2p_lower(name);
        }
        return name;
    }
}

static char *reader_take_string(SelectorReader *reader) {
    char quote;
    char *out;
    size_t capacity = 32U;
    size_t length = 0U;
    reader_skip(reader);
    if (reader->position >= reader->length) {
        return NULL;
    }
    if (reader->text[reader->position] != '"' &&
        reader->text[reader->position] != '\'') {
        return reader_take_name(reader);
    }
    quote = reader->text[reader->position++];
    out = (char *)p2p_alloc(capacity);
    if (out == NULL) {
        return NULL;
    }
    while (reader->position < reader->length) {
        char c = reader->text[reader->position++];
        char decoded = c;
        if (c == quote) {
            out[length] = '\0';
            return out;
        }
        if (c == '\\' && reader->position < reader->length) {
            decoded = reader->text[reader->position++];
        }
        if (length + 2U > capacity) {
            char *grown = (char *)p2p_realloc(out, capacity * 2U);
            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
            capacity *= 2U;
        }
        out[length++] = decoded;
    }
    free(out);
    return NULL;
}

/* Reads the balanced parenthesised argument of a functional selector. */
static char *reader_take_argument(SelectorReader *reader) {
    size_t start;
    int depth = 0;
    reader_skip(reader);
    if (reader->position >= reader->length ||
        reader->text[reader->position] != '(') {
        return NULL;
    }
    start = reader->position;
    while (reader->position < reader->length) {
        const char c = reader->text[reader->position];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                ++reader->position;
                return p2p_strdup((const unsigned char *)reader->text + start + 1U,
                                  reader->position - start - 2U);
            }
        }
        ++reader->position;
    }
    return NULL;
}

/* Parses the `an+b` argument of :nth-child() and friends. */
static bool parse_nth(const char *argument, int *a, int *b) {
    char *copy;
    char *cursor;
    bool found_n = false;
    int coefficient = 0;
    int offset = 0;
    int sign = 1;
    if (argument == NULL) {
        return false;
    }
    if (p2p_iequals(argument, "odd")) {
        *a = 2;
        *b = 1;
        return true;
    }
    if (p2p_iequals(argument, "even")) {
        *a = 2;
        *b = 0;
        return true;
    }
    copy = p2p_strdup_c(argument);
    if (copy == NULL) {
        return false;
    }
    cursor = p2p_skip_space(copy);
    while (*cursor != '\0') {
        if (*cursor == '+' || *cursor == '-') {
            sign = *cursor == '-' ? -1 : 1;
            ++cursor;
            cursor = p2p_skip_space(cursor);
        }
        if (*cursor >= '0' && *cursor <= '9') {
            const long number = strtol(cursor, &cursor, 10);
            cursor = p2p_skip_space(cursor);
            if (*cursor == 'n' || *cursor == 'N') {
                ++cursor;
                coefficient = (int)(number * sign);
                found_n = true;
                sign = 1;
            } else {
                offset = (int)(number * sign);
                sign = 1;
            }
        } else if (*cursor == 'n' || *cursor == 'N') {
            ++cursor;
            coefficient = sign;
            found_n = true;
            sign = 1;
        } else {
            break;
        }
        cursor = p2p_skip_space(cursor);
    }
    free(copy);
    if (!found_n && offset == 0) {
        return false;
    }
    *a = coefficient;
    *b = offset;
    return true;
}

static bool parse_selector_list(SelectorAlternative **out, size_t *out_count,
                                const char *text);

static bool simple_push(CompoundSelector *compound, SimpleKind kind, char *name,
                        char *value, AttributeOperator op) {
    SimpleSelector *grown = (SimpleSelector *)p2p_realloc(
        compound->simples, (compound->count + 1U) * sizeof(SimpleSelector));
    if (grown == NULL) {
        return false;
    }
    compound->simples = grown;
    compound->simples[compound->count].kind = kind;
    compound->simples[compound->count].name = name;
    compound->simples[compound->count].value = value;
    compound->simples[compound->count].op = op;
    ++compound->count;
    return true;
}

static void alternative_free(SelectorAlternative *alt) {
    for (size_t c = 0U; c < alt->compounds; ++c) {
        CompoundSelector *compound = &alt->compound_list[c];
        for (size_t s = 0U; s < compound->count; ++s) {
            free(compound->simples[s].name);
            free(compound->simples[s].value);
        }
        free(compound->simples);
    }
    free(alt->compound_list);
    for (size_t k = 0U; k < alt->key_count; ++k) {
        free(alt->key_text[k]);
    }
    free(alt->pseudo_element);
    memset(alt, 0, sizeof(*alt));
}

static void rule_free(CssRule *rule) {
    for (size_t a = 0U; a < rule->count_selectors; ++a) {
        alternative_free(&rule->selectors[a]);
    }
    free(rule->selectors);
    free(rule->declarations);
    free(rule->declarations_important);
    memset(rule, 0, sizeof(*rule));
}

/* Parses a single compound selector, reading simple selectors until a
 * combinator or the end of the text. */
static bool parse_compound(SelectorReader *reader, CompoundSelector *compound,
                           SelectorAlternative *alt, unsigned *spec_a,
                           unsigned *spec_b, unsigned *spec_c) {
    memset(compound, 0, sizeof(*compound));
    for (;;) {
        char c;
        reader_skip(reader);
        if (reader->position >= reader->length) {
            return true;
        }
        c = reader->text[reader->position];
        if (c == ' ' || c == '>' || c == '+' || c == '~' || c == ',') {
            return true;
        }
        if (c == '*') {
            ++reader->position;
            if (!simple_push(compound, SIMPLE_UNIVERSAL, NULL, NULL, ATTR_OP_EXISTS)) {
                return false;
            }
            continue;
        }
        if (c == '#' || c == '.') {
            const char marker = c;
            char *name;
            ++reader->position;
            name = reader_take_name(reader);
            if (name == NULL) {
                return false;
            }
            if (!simple_push(compound,
                             marker == '#' ? SIMPLE_ID : SIMPLE_CLASS, name, NULL,
                             ATTR_OP_EXISTS)) {
                free(name);
                return false;
            }
            if (marker == '#') {
                ++*spec_a;
            } else {
                ++*spec_b;
            }
            continue;
        }
        if (c == '[') {
            char *name;
            char *value = NULL;
            AttributeOperator op = ATTR_OP_EXISTS;
            ++reader->position;
            name = reader_take_name(reader);
            if (name == NULL) {
                return false;
            }
            reader_skip(reader);
            if (reader->position < reader->length) {
                const char op_char = reader->text[reader->position];
                if (op_char == '=') {
                    op = ATTR_OP_EQUALS;
                    ++reader->position;
                } else if (strchr("~|^$*", op_char) != NULL &&
                           reader->position + 1U < reader->length &&
                           reader->text[reader->position + 1U] == '=') {
                    op = op_char == '~' ? ATTR_OP_INCLUDES
                       : op_char == '|' ? ATTR_OP_DASH_MATCH
                       : op_char == '^' ? ATTR_OP_PREFIX
                       : op_char == '$' ? ATTR_OP_SUFFIX
                                        : ATTR_OP_SUBSTRING;
                    reader->position += 2U;
                    value = reader_take_string(reader);
                }
            }
            reader_skip(reader);
            if (reader->position < reader->length &&
                (reader->text[reader->position] == 'i' ||
                 reader->text[reader->position] == 'I')) {
                ++reader->position;   /* case-insensitivity flag, always honoured */
            }
            reader_skip(reader);
            if (reader->position < reader->length &&
                reader->text[reader->position] == ']') {
                ++reader->position;
            } else {
                free(name);
                free(value);
                return false;
            }
            if (!simple_push(compound, SIMPLE_ATTRIBUTE, name, value, op)) {
                free(name);
                free(value);
                return false;
            }
            ++*spec_b;
            continue;
        }
        if (c == ':') {
            const bool doubled =
                reader->position + 1U < reader->length &&
                reader->text[reader->position + 1U] == ':';
            char *name;
            char *argument = NULL;
            reader->position += doubled ? 2U : 1U;
            name = reader_take_name(reader);
            if (name == NULL) {
                return false;
            }
            reader_skip(reader);
            if (reader->position < reader->length &&
                reader->text[reader->position] == '(') {
                argument = reader_take_argument(reader);
            }
            if (doubled) {
                /* The pseudo-element is not part of the compound. It is
                 * recorded on the alternative so the cascade can file the rule
                 * against generated content instead of real nodes. */
                if (alt->pseudo_element == NULL) {
                    alt->pseudo_element = name;
                } else {
                    free(name);
                }
                free(argument);
                continue;
            }
            if (strcmp(name, "not") == 0 || strcmp(name, "is") == 0 ||
                strcmp(name, "matches") == 0 || strcmp(name, "any") == 0 ||
                strcmp(name, "where") == 0 || strcmp(name, "has") == 0) {
                /* Specificity of :not() / :is() is that of their most specific
                 * argument; :where() and :has() contribute nothing. */
                unsigned a = 0U, b = 0U, cc = 0U;
                if (argument != NULL) {
                    SelectorAlternative *nested = NULL;
                    size_t nested_count = 0U;
                    if (parse_selector_list(&nested, &nested_count, argument)) {
                        for (size_t i = 0U; i < nested_count; ++i) {
                            if (nested[i].specificity_a > a ||
                                (nested[i].specificity_a == a &&
                                 (nested[i].specificity_b > b ||
                                  (nested[i].specificity_b == b &&
                                   nested[i].specificity_c > cc)))) {
                                a = nested[i].specificity_a;
                                b = nested[i].specificity_b;
                                cc = nested[i].specificity_c;
                            }
                        }
                    }
                    free(nested);
                }
                if (strcmp(name, "where") != 0 && strcmp(name, "has") != 0) {
                    *spec_a += a;
                    *spec_b += b;
                    *spec_c += cc;
                }
                if (!simple_push(compound, SIMPLE_PSEUDO_CLASS, name, argument,
                                 ATTR_OP_EXISTS)) {
                    free(argument);
                    return false;
                }
                continue;
            }
            if (strcmp(name, "nth-child") == 0 ||
                strcmp(name, "nth-last-child") == 0 ||
                strcmp(name, "nth-of-type") == 0 ||
                strcmp(name, "nth-last-of-type") == 0) {
                int a = 0, b = 0;
                if (argument != NULL && parse_nth(argument, &a, &b)) {
                    char *encoded = p2p_format("%d %d", a, b);
                    free(argument);
                    argument = encoded;
                }
            }
            if (!simple_push(compound, SIMPLE_PSEUDO_CLASS, name, argument,
                             ATTR_OP_EXISTS)) {
                free(argument);
                return false;
            }
            ++*spec_b;
            continue;
        }
        {
            char *name = reader_take_name(reader);
            if (name == NULL) {
                return false;
            }
            if (!simple_push(compound, SIMPLE_TYPE, name, NULL, ATTR_OP_EXISTS)) {
                free(name);
                return false;
            }
            ++*spec_c;
        }
    }
}

/* Parses one comma-separated alternative: a chain of compounds joined by
 * combinators. `text` is already trimmed and free of top-level commas. */
static bool parse_alternative(SelectorAlternative *alt, const char *text) {
    SelectorReader reader = {text, strlen(text), 0U};
    CompoundSelector *compounds = NULL;
    size_t count = 0U;
    unsigned spec_a = 0U, spec_b = 0U, spec_c = 0U;
    Combinator pending = COMB_DESCENDANT;
    memset(alt, 0, sizeof(*alt));
    for (;;) {
        CompoundSelector compound;
        compound.combinator = pending;
        if (!parse_compound(&reader, &compound, alt, &spec_a, &spec_b, &spec_c)) {
            free(compound.simples);
            goto fail;
        }
        if (compound.count == 0U) {
            free(compound.simples);
            break;
        }
        {
            CompoundSelector *grown = (CompoundSelector *)p2p_realloc(
                compounds, (count + 1U) * sizeof(CompoundSelector));
            if (grown == NULL) {
                free(compound.simples);
                goto fail;
            }
            compounds = grown;
            compounds[count++] = compound;
        }
        reader_skip(&reader);
        if (reader.position >= reader.length) {
            pending = COMB_DESCENDANT;
            break;
        }
        {
            const char c = reader.text[reader.position];
            if (c == ',') {
                break;   /* cannot happen: the list is split first */
            }
            if (c == '>') {
                pending = COMB_CHILD;
                ++reader.position;
            } else if (c == '+') {
                pending = COMB_NEXT_SIBLING;
                ++reader.position;
            } else if (c == '~') {
                pending = COMB_LATER_SIBLING;
                ++reader.position;
            } else {
                pending = COMB_DESCENDANT;
            }
        }
    }
    if (count == 0U) {
        goto fail;
    }
    alt->compound_list = compounds;
    alt->compounds = count;
    alt->specificity_a = spec_a;
    alt->specificity_b = spec_b;
    alt->specificity_c = spec_c;
    return true;
fail:
    for (size_t i = 0U; i < count; ++i) {
        for (size_t s = 0U; s < compounds[i].count; ++s) {
            free(compounds[i].simples[s].name);
            free(compounds[i].simples[s].value);
        }
        free(compounds[i].simples);
    }
    free(compounds);
    free(alt->pseudo_element);
    memset(alt, 0, sizeof(*alt));
    return false;
}

/* Splits a selector list on top-level commas and parses each alternative. */
static bool parse_selector_list(SelectorAlternative **out, size_t *out_count,
                                const char *text) {
    SelectorAlternative *alternatives = NULL;
    size_t count = 0U;
    const char *cursor = text;
    while (cursor != NULL && *cursor != '\0') {
        size_t length = 0U;
        int depth = 0;
        char *piece;
        while (cursor[length] != '\0') {
            const char c = cursor[length];
            if (c == '(' || c == '[') {
                ++depth;
            } else if (c == ')' || c == ']') {
                if (depth > 0) --depth;
            } else if (c == ',' && depth == 0) {
                break;
            }
            ++length;
        }
        piece = p2p_strdup((const unsigned char *)cursor, length);
        if (piece == NULL) {
            break;
        }
        {
            char *trimmed = p2p_trim(piece);
            if (*trimmed != '\0' && strlen(trimmed) < P2P_MAX_SELECTOR_BYTES) {
                SelectorAlternative *grown = (SelectorAlternative *)p2p_realloc(
                    alternatives, (count + 1U) * sizeof(SelectorAlternative));
                if (grown != NULL) {
                    alternatives = grown;
                    memset(&alternatives[count], 0, sizeof(SelectorAlternative));
                    if (parse_alternative(&alternatives[count], trimmed)) {
                        ++count;
                    }
                }
            }
        }
        free(piece);
        cursor += length;
        if (*cursor == ',') {
            ++cursor;
        }
    }
    if (count == 0U) {
        free(alternatives);
        *out = NULL;
        *out_count = 0U;
        return false;
    }
    *out = alternatives;
    *out_count = count;
    return true;
}

/* ------------------------------------------------------------------ matching */

static bool attribute_matches(const GraphicNode *node,
                              const SimpleSelector *simple) {
    const char *value = node_attribute(node, simple->name);
    if (value == NULL) {
        return false;
    }
    if (simple->op == ATTR_OP_EXISTS || simple->value == NULL) {
        return true;
    }
    if (simple->op == ATTR_OP_EQUALS) {
        return strcmp(value, simple->value) == 0;
    }
    if (simple->op == ATTR_OP_INCLUDES) {
        const size_t needle_length = strlen(simple->value);
        const char *cursor = value;
        while (*cursor != '\0') {
            while (*cursor == ' ') ++cursor;
            if (*cursor == '\0') break;
            if (strncmp(cursor, simple->value, needle_length) == 0) {
                const char after = cursor[needle_length];
                if (after == '\0' || after == ' ') {
                    return true;
                }
            }
            while (*cursor != '\0' && *cursor != ' ') ++cursor;
        }
        return false;
    }
    if (simple->op == ATTR_OP_DASH_MATCH) {
        const size_t length = strlen(simple->value);
        return strncmp(value, simple->value, length) == 0 &&
               (value[length] == '\0' || value[length] == '-');
    }
    if (simple->op == ATTR_OP_PREFIX) {
        return strncmp(value, simple->value, strlen(simple->value)) == 0;
    }
    if (simple->op == ATTR_OP_SUFFIX) {
        const size_t value_length = strlen(value);
        const size_t needle_length = strlen(simple->value);
        return value_length >= needle_length &&
               strcmp(value + value_length - needle_length, simple->value) == 0;
    }
    if (simple->op == ATTR_OP_SUBSTRING) {
        return strstr(value, simple->value) != NULL;
    }
    return false;
}

static bool node_is_empty(const GraphicNode *node) {
    const GraphicNode *child;
    for (child = node->children; child != NULL; child = child->next) {
        if (child->is_text) {
            const unsigned char *text = (const unsigned char *)child->text;
            while (text != NULL && *text != '\0') {
                if (!isspace(*text)) {
                    return false;
                }
                ++text;
            }
            continue;
        }
        return false;
    }
    return true;
}

static bool nth_matches(long index, const char *argument) {
    int a = 0, b = 0;
    if (argument == NULL) {
        return false;
    }
    if (strcmp(argument, "odd") == 0) {
        a = 2;
        b = 1;
    } else if (strcmp(argument, "even") == 0) {
        a = 2;
        b = 0;
    } else if (!parse_nth(argument, &a, &b)) {
        return false;
    }
    if (a == 0) {
        return index == (long)b;
    }
    {
        const long difference = index - (long)b;
        if (difference % (long)a != 0) {
            return false;
        }
        return difference / (long)a >= 0;
    }
}

static bool selector_list_matches(const char *list, const GraphicNode *node) {
    SelectorAlternative *alternatives = NULL;
    size_t count = 0U;
    bool matched = false;
    if (list == NULL ||
        !parse_selector_list(&alternatives, &count, list)) {
        return false;
    }
    for (size_t i = 0U; i < count && !matched; ++i) {
        matched = css_selector_matches(&alternatives[i], node);
    }
    for (size_t i = 0U; i < count; ++i) {
        alternative_free(&alternatives[i]);
    }
    free(alternatives);
    return matched;
}

static bool pseudo_matches(const GraphicNode *node, const SimpleSelector *simple) {
    const char *name = simple->name;
    if (name == NULL) {
        return false;
    }
    if (strcmp(name, "root") == 0) {
        return node_is_root(node) ||
               (node->parent != NULL && node_is_root(node->parent));
    }
    if (strcmp(name, "empty") == 0) {
        return node_is_empty(node);
    }
    if (strcmp(name, "first-child") == 0) {
        return node_element_ordinal(node) == 1U;
    }
    if (strcmp(name, "last-child") == 0) {
        return node_element_ordinal(node) == node_element_sibling_count(node);
    }
    if (strcmp(name, "only-child") == 0) {
        return node_element_sibling_count(node) == 1U;
    }
    if (strcmp(name, "first-of-type") == 0) {
        return node_tag_ordinal(node) == 1U;
    }
    if (strcmp(name, "last-of-type") == 0) {
        return node_tag_ordinal(node) == node_type_sibling_count(node);
    }
    if (strcmp(name, "only-of-type") == 0) {
        return node_type_sibling_count(node) == 1U;
    }
    if (strcmp(name, "nth-child") == 0) {
        return nth_matches((long)node_element_ordinal(node), simple->value);
    }
    if (strcmp(name, "nth-last-child") == 0) {
        return nth_matches(
            (long)(node_element_sibling_count(node) - node_element_ordinal(node) + 1U),
            simple->value);
    }
    if (strcmp(name, "nth-of-type") == 0) {
        return nth_matches((long)node_type_index_among_siblings(node) + 1L,
                           simple->value);
    }
    if (strcmp(name, "nth-last-of-type") == 0) {
        return nth_matches(
            (long)(node_type_sibling_count(node) - node_type_index_among_siblings(node)),
            simple->value);
    }
    if (strcmp(name, "not") == 0) {
        return !selector_list_matches(simple->value, node);
    }
    if (strcmp(name, "is") == 0 || strcmp(name, "matches") == 0 ||
        strcmp(name, "any") == 0 || strcmp(name, "where") == 0) {
        return selector_list_matches(simple->value, node);
    }
    if (strcmp(name, "has") == 0) {
        if (simple->value == NULL) {
            return false;
        }
        for (const GraphicNode *child = node->children; child != NULL;
             child = child->next) {
            if (selector_list_matches(simple->value, child)) {
                return true;
            }
        }
        return false;
    }
    if (strcmp(name, "checked") == 0) {
        return node_has_attribute(node, "checked") ||
               node_has_attribute(node, "selected");
    }
    if (strcmp(name, "indeterminate") == 0) {
        return false;
    }
    if (strcmp(name, "disabled") == 0) {
        return node_has_attribute(node, "disabled");
    }
    if (strcmp(name, "enabled") == 0) {
        return !node_has_attribute(node, "disabled");
    }
    if (strcmp(name, "required") == 0) {
        return node_has_attribute(node, "required");
    }
    if (strcmp(name, "optional") == 0) {
        return !node_has_attribute(node, "required");
    }
    if (strcmp(name, "read-only") == 0) {
        return node_has_attribute(node, "readonly") ||
               node_has_attribute(node, "disabled");
    }
    if (strcmp(name, "read-write") == 0) {
        return !node_has_attribute(node, "readonly") &&
               !node_has_attribute(node, "disabled");
    }
    if (strcmp(name, "link") == 0 || strcmp(name, "any-link") == 0) {
        return strcmp(node->tag, "a") == 0 && node_has_attribute(node, "href");
    }
    if (strcmp(name, "lang") == 0) {
        const char *lang = node_attribute(node, "lang");
        if (lang == NULL && node->parent != NULL) {
            lang = node_attribute(node->parent, "lang");
        }
        return lang != NULL && simple->value != NULL &&
               p2p_starts_with(lang, simple->value);
    }
    if (strcmp(name, "visited") == 0) {
        /* A print pipeline has no history. Reporting a link as visited would
         * invent state the engine cannot observe, so nothing matches. */
        return false;
    }
    /* Interaction, focus, target and validity state all do not exist here, so
     * they deliberately never match. Matching them would make the output depend
     * on state the engine does not have. */
    return false;
}

static bool class_list_contains(const char *classes, const char *name) {
    const size_t length = strlen(name);
    const char *cursor = classes;
    if (classes == NULL) {
        return false;
    }
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (*cursor == '\0') break;
        if (strncmp(cursor, name, length) == 0 &&
            (cursor[length] == '\0' || isspace((unsigned char)cursor[length]))) {
            return true;
        }
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) ++cursor;
    }
    return false;
}

static bool compound_matches(const CompoundSelector *compound,
                             const GraphicNode *node) {
    for (size_t i = 0U; i < compound->count; ++i) {
        const SimpleSelector *simple = &compound->simples[i];
        switch (simple->kind) {
            case SIMPLE_UNIVERSAL:
                break;
            case SIMPLE_TYPE:
                if (node->is_text || strcmp(node->tag, simple->name) != 0) {
                    return false;
                }
                break;
            case SIMPLE_ID: {
                const char *id = node_attribute(node, "id");
                if (id == NULL || strcmp(id, simple->name) != 0) {
                    return false;
                }
                break;
            }
            case SIMPLE_CLASS:
                if (!class_list_contains(node_attribute(node, "class"),
                                         simple->name)) {
                    return false;
                }
                break;
            case SIMPLE_ATTRIBUTE:
                if (!attribute_matches(node, simple)) {
                    return false;
                }
                break;
            case SIMPLE_PSEUDO_CLASS:
                if (node->is_text || !pseudo_matches(node, simple)) {
                    return false;
                }
                break;
            case SIMPLE_PSEUDO_ELEMENT:
                break;
        }
    }
    return true;
}

static const GraphicNode *previous_element(const GraphicNode *node) {
    const GraphicNode *cursor = node != NULL ? node->prev : NULL;
    while (cursor != NULL &&
           (cursor->is_text || strcmp(cursor->tag, TAG_ANON) == 0 ||
            cursor->generated)) {
        cursor = cursor->prev;
    }
    return cursor;
}

bool css_selector_matches(const SelectorAlternative *alt, const GraphicNode *node) {
    size_t index;
    const GraphicNode *current = node;
    if (alt == NULL || alt->compounds == 0U || current == NULL) {
        return false;
    }
    index = alt->compounds;
    if (!compound_matches(&alt->compound_list[index - 1U], current)) {
        return false;
    }
    while (index > 1U) {
        const CompoundSelector *left = &alt->compound_list[index - 2U];
        const Combinator combinator = alt->compound_list[index - 1U].combinator;
        const GraphicNode *found = NULL;
        switch (combinator) {
            case COMB_CHILD:
                found = current->parent;
                if (found == NULL || !compound_matches(left, found)) {
                    return false;
                }
                break;
            case COMB_DESCENDANT:
                for (found = current->parent; found != NULL; found = found->parent) {
                    if (compound_matches(left, found)) {
                        break;
                    }
                }
                if (found == NULL) {
                    return false;
                }
                break;
            case COMB_NEXT_SIBLING:
            case COMB_LATER_SIBLING:
                for (found = previous_element(current); found != NULL;
                     found = previous_element(found)) {
                    if (compound_matches(left, found)) {
                        break;
                    }
                }
                if (found == NULL) {
                    return false;
                }
                break;
        }
        current = found;
        --index;
    }
    return true;
}

/* ------------------------------------------------------------------ @-rules */

/* Evaluates a media query list against the print target. page2pdf always
 * renders for print, so `print` and `all` match and `screen` does not. Feature
 * conditions are evaluated for the viewport and otherwise treated as
 * satisfied, so a print stylesheet guarded by a condition the printer cannot
 * decide still applies. */
static bool media_type_matches(const char *word) {
    return strcmp(word, "all") == 0 || strcmp(word, "print") == 0;
}

static bool feature_matches(const char *feature, const CssContext *context) {
    char *copy = p2p_strdup_c(feature);
    char *colon;
    const char *name;
    char *raw;
    char number[32];
    char *unit_end;
    double magnitude;
    bool horizontal;
    double reference;
    double points;
    bool result = true;
    if (copy == NULL) {
        return true;
    }
    colon = strchr(copy, ':');
    if (colon == NULL) {
        free(copy);
        return true;
    }
    *colon = '\0';
    name = p2p_skip_space(copy);
    raw = p2p_trim(colon + 1);
    snprintf(number, sizeof(number), "%s", raw);
    unit_end = number;
    while (*unit_end != '\0' && !isspace((unsigned char)*unit_end) &&
           *unit_end != '.') {
        ++unit_end;
    }
    *unit_end = '\0';
    magnitude = strtod(number, NULL);
    horizontal = strstr(name, "width") != NULL;
    reference = horizontal ? context->viewport_width : context->viewport_height;
    points = magnitude >= 96.0 ? magnitude * 0.75 : magnitude;
    if (p2p_starts_with(name, "min-")) {
        result = reference >= points;
    } else if (p2p_starts_with(name, "max-")) {
        result = reference <= points;
    }
    free(copy);
    return result;
}

static bool media_alternative_matches(const char *body, const CssContext *context) {
    const char *cursor = body;
    bool negate = false;
    bool type_ok = false;
    bool type_seen = false;
    bool features_ok = true;
    if (p2p_starts_with(cursor, "not ")) {
        negate = true;
        cursor = p2p_skip_space(cursor + 4);
    } else if (p2p_starts_with(cursor, "only ")) {
        cursor = p2p_skip_space(cursor + 5);
    }
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (*cursor == '\0') break;
        if (*cursor == '(') {
            const char *close = strchr(cursor, ')');
            if (close == NULL) {
                return true;
            }
            {
                char *feature = p2p_strdup((const unsigned char *)cursor + 1U,
                                           (size_t)(close - cursor - 1));
                if (feature != NULL) {
                    if (!feature_matches(feature, context)) {
                        features_ok = false;
                    }
                    free(feature);
                }
            }
            cursor = close + 1;
            continue;
        }
        if (*cursor == ',' || *cursor == ')' || *cursor == '(') {
            ++cursor;
            continue;
        }
        {
            size_t length = 0U;
            char *word;
            while (cursor[length] != '\0' && !isspace((unsigned char)cursor[length]) &&
                   cursor[length] != '(' && cursor[length] != ')') {
                ++length;
            }
            word = p2p_strdup((const unsigned char *)cursor, length);
            if (word == NULL) {
                return true;
            }
            p2p_lower(word);
            if (strcmp(word, "and") == 0 || strcmp(word, "or") == 0) {
                free(word);
                cursor += length;
                continue;
            }
            if (strcmp(word, "screen") == 0 || strcmp(word, "speech") == 0) {
                type_ok = false;
                type_seen = true;
            } else if (media_type_matches(word)) {
                type_ok = true;
                type_seen = true;
            } else {
                type_seen = true;
            }
            free(word);
            cursor += length;
        }
    }
    {
        bool result = (type_seen ? type_ok : true) && features_ok;
        return negate ? !result : result;
    }
}

static bool media_matches(const char *query, const CssContext *context) {
    char *copy;
    const char *cursor;
    bool any = false;
    if (query == NULL) {
        return true;
    }
    copy = p2p_strdup_c(query);
    if (copy == NULL) {
        return false;
    }
    cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
        char *comma = strchr(cursor, ',');
        char *piece;
        if (comma != NULL) {
            *comma = '\0';
        }
        piece = p2p_trim(cursor);
        if (*piece != '\0') {
            any = media_alternative_matches(piece, context) || any;
        }
        cursor = comma != NULL ? comma + 1 : NULL;
    }
    free(copy);
    return any;
}

static const struct {
    const char *name;
    float width, height;
} kPageSizes[] = {
    {"a3", 841.89f, 1190.55f}, {"a4", 595.28f, 841.89f},
    {"a5", 419.53f, 595.28f}, {"b4", 708.66f, 1000.63f},
    {"b5", 498.90f, 708.66f}, {"letter", 612.0f, 792.0f},
    {"legal", 612.0f, 1008.0f}, {"ledger", 1224.0f, 792.0f},
    {"tabloid", 792.0f, 1224.0f}
};

/* Applies one `@page` block. `size` is read first because margins may be
 * expressed as a percentage of the page box. */
static void apply_page_rule(const char *declarations, CssContext *context) {
    char *copy = p2p_strdup_c(declarations);
    if (copy == NULL) {
        return;
    }
    {
        char *cursor = copy;
        while (*cursor != '\0') {
            char *semicolon = strchr(cursor, ';');
            if (semicolon != NULL) {
                *semicolon = '\0';
            }
            {
                char *colon = strchr(cursor, ':');
                if (colon != NULL) {
                    char *name = cursor;
                    char *value;
                    *colon = '\0';
                    value = p2p_trim(colon + 1);
                    name = p2p_trim(name);
                    p2p_lower(name);
                    if (strcmp(name, "size") == 0) {
                        char lower[32];
                        size_t i = 0U;
                        const bool landscape = strstr(value, "landscape") != NULL;
                        while (value[i] != '\0' && !isspace((unsigned char)value[i]) &&
                               i + 1U < sizeof(lower)) {
                            lower[i] = (char)tolower((unsigned char)value[i]);
                            ++i;
                        }
                        lower[i] = '\0';
                        for (size_t s = 0U; s < sizeof(kPageSizes) / sizeof(kPageSizes[0]);
                             ++s) {
                            if (strcmp(kPageSizes[s].name, lower) == 0) {
                                context->geometry.width = kPageSizes[s].width;
                                context->geometry.height = kPageSizes[s].height;
                                break;
                            }
                        }
                        if (i == 0U) {
                            /* `size: 210mm 297mm` */
                            double first = 0.0, second = 0.0;
                            char unit[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                            if (sscanf(value, "%lf%7s %lf", &first, unit, &second) >= 2) {
                                const double factor =
                                    strcmp(unit, "mm") == 0 ? 72.0 / 25.4
                                    : strcmp(unit, "cm") == 0 ? 72.0 / 2.54
                                    : strcmp(unit, "in") == 0 ? 72.0
                                    : strcmp(unit, "pt") == 0 ? 1.0 : 1.0;
                                context->geometry.width = (float)(first * factor);
                                context->geometry.height = (float)(
                                    (second > 0.0 ? second : first * 1.41421356) * factor);
                            }
                        }
                        if (landscape) {
                            const float swap = context->geometry.width;
                            context->geometry.width = context->geometry.height;
                            context->geometry.height = swap;
                        }
                        context->geometry_set = true;
                    }
                }
            }
            if (semicolon == NULL) {
                break;
            }
            cursor = semicolon + 1;
        }
    }
    free(copy);
    /* Margins go through the normal declaration path, with the page box as
     * both the percentage reference and the viewport. */
    {
        Style page;
        css_style_initial(&page);
        page.margin[SIDE_TOP] = value_auto();
        page.margin[SIDE_RIGHT] = value_auto();
        page.margin[SIDE_BOTTOM] = value_auto();
        page.margin[SIDE_LEFT] = value_auto();
        {
            CssContext page_context = *context;
            page_context.viewport_width = context->geometry.width;
            page_context.viewport_height = context->geometry.height;
            css_apply_declarations(&page, declarations, &page_context, 16.0);
        }
        for (int i = 0; i < 4; ++i) {
            float *slot = i == SIDE_TOP ? &context->geometry.margin_top
                        : i == SIDE_RIGHT ? &context->geometry.margin_right
                        : i == SIDE_BOTTOM ? &context->geometry.margin_bottom
                                           : &context->geometry.margin_left;
            if (value_is_auto(page.margin[i])) {
                continue;   /* not declared: keep the default margin */
            }
            *slot = (float)value_resolve_length(
                page.margin[i], context->geometry.width, 16.0, 16.0,
                context->geometry.width, context->geometry.height, false);
        }
        style_release(&page);
    }
}

/* ----------------------------------------------------------------- cascading */

typedef struct {
    const CssRule *rule;
    const SelectorAlternative *alternative;
} MatchedRule;

typedef struct {
    MatchedRule *items;
    size_t count;
    size_t capacity;
} MatchSet;

static bool matchset_push(MatchSet *set, const CssRule *rule,
                          const SelectorAlternative *alternative) {
    /* The same rule can be filed under several keys that a node reaches, so
     * dedupe on the (rule, alternative) pair. The matched set is small. */
    for (size_t i = 0U; i < set->count; ++i) {
        if (set->items[i].rule == rule && set->items[i].alternative == alternative) {
            return true;
        }
    }
    if (set->count == set->capacity) {
        const size_t capacity = set->capacity == 0U ? 32U : set->capacity * 2U;
        MatchedRule *grown =
            (MatchedRule *)p2p_realloc(set->items, capacity * sizeof(MatchedRule));
        if (grown == NULL) {
            return false;
        }
        set->items = grown;
        set->capacity = capacity;
    }
    set->items[set->count].rule = rule;
    set->items[set->count].alternative = alternative;
    ++set->count;
    return true;
}

static int compare_matched(const void *left, const void *right) {
    const MatchedRule *a = (const MatchedRule *)left;
    const MatchedRule *b = (const MatchedRule *)right;
    if (a->alternative->specificity_a != b->alternative->specificity_a) {
        return a->alternative->specificity_a < b->alternative->specificity_a ? -1 : 1;
    }
    if (a->alternative->specificity_b != b->alternative->specificity_b) {
        return a->alternative->specificity_b < b->alternative->specificity_b ? -1 : 1;
    }
    if (a->alternative->specificity_c != b->alternative->specificity_c) {
        return a->alternative->specificity_c < b->alternative->specificity_c ? -1 : 1;
    }
    if (a->rule->order != b->rule->order) {
        return a->rule->order < b->rule->order ? -1 : 1;
    }
    return 0;
}

typedef struct {
    CssRuleList rules;
    CssRuleList pseudo_before;
    CssRuleList pseudo_after;
    CssRuleList pseudo_marker;
    BucketIndex index;
    BucketIndex pseudo_before_index;
    BucketIndex pseudo_after_index;
    BucketIndex pseudo_marker_index;
    CssFontFace *faces;
    size_t face_count;
    size_t face_capacity;
    CssContext context;
    Renderer *renderer;
} Cascade;

static void cascade_free(Cascade *cascade) {
    for (size_t i = 0U; i < cascade->rules.count; ++i) {
        rule_free(&cascade->rules.items[i]);
    }
    free(cascade->rules.items);
    for (size_t i = 0U; i < cascade->pseudo_before.count; ++i) {
        rule_free(&cascade->pseudo_before.items[i]);
    }
    free(cascade->pseudo_before.items);
    for (size_t i = 0U; i < cascade->pseudo_after.count; ++i) {
        rule_free(&cascade->pseudo_after.items[i]);
    }
    free(cascade->pseudo_after.items);
    for (size_t i = 0U; i < cascade->pseudo_marker.count; ++i) {
        rule_free(&cascade->pseudo_marker.items[i]);
    }
    free(cascade->pseudo_marker.items);
    bucket_index_free(&cascade->index);
    bucket_index_free(&cascade->pseudo_before_index);
    bucket_index_free(&cascade->pseudo_after_index);
    bucket_index_free(&cascade->pseudo_marker_index);
    for (size_t i = 0U; i < cascade->face_count; ++i) {
        free(cascade->faces[i].family);
        free(cascade->faces[i].source);
    }
    free(cascade->faces);
    memset(cascade, 0, sizeof(*cascade));
}

static bool rule_list_push(CssRuleList *list, CssRule *rule) {
    if (list->count >= P2P_MAX_RULES) {
        return false;
    }
    if (list->count == list->capacity) {
        const size_t capacity = list->capacity == 0U ? 64U : list->capacity * 2U;
        CssRule *grown = (CssRule *)p2p_realloc(list->items, capacity * sizeof(CssRule));
        if (grown == NULL) {
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = *rule;
    memset(rule, 0, sizeof(*rule));
    return true;
}

static bool register_rule(CssRuleList *list, const char *selectors,
                          const char *declarations) {
    CssRule rule;
    if (declarations == NULL || selectors == NULL) {
        return false;
    }
    memset(&rule, 0, sizeof(rule));
    if (!parse_selector_list(&rule.selectors, &rule.count_selectors, selectors)) {
        return false;
    }
    css_split_important(declarations, &rule.declarations, &rule.declarations_important);
    if (rule.declarations == NULL && rule.declarations_important == NULL) {
        for (size_t a = 0U; a < rule.count_selectors; ++a) {
            alternative_free(&rule.selectors[a]);
        }
        free(rule.selectors);
        return false;
    }
    rule.order = (unsigned)list->count;
    if (!rule_list_push(list, &rule)) {
        return false;
    }
    return true;
}

static void collect_matches(const CssRuleList *list, const BucketIndex *index,
                            const GraphicNode *node, MatchSet *set) {
    const char *id = node_attribute(node, "id");
    const char *classes = node_attribute(node, "class");
    const char *keys[19];
    unsigned char kinds[19];
    size_t key_count = 0U;
    if (index->buckets == NULL) {
        return;
    }
    if (id != NULL) {
        kinds[key_count] = BUCKET_ID;
        keys[key_count++] = id;
    }
    {
        const char *cursor = classes;
        while (cursor != NULL && key_count < 18U) {
            size_t length = 0U;
            while (cursor[length] != '\0' &&
                   !isspace((unsigned char)cursor[length])) {
                ++length;
            }
            if (length > 0U) {
                char *name = p2p_strdup((const unsigned char *)cursor, length);
                if (name != NULL) {
                    kinds[key_count] = BUCKET_CLASS;
                    keys[key_count++] = name;
                }
            }
            cursor += length;
            while (isspace((unsigned char)*cursor)) ++cursor;
        }
    }
    kinds[key_count] = BUCKET_TAG;
    keys[key_count++] = node->tag;
    kinds[key_count] = BUCKET_UNIVERSAL;
    keys[key_count++] = "*";
    for (size_t k = 0U; k < key_count; ++k) {
        const size_t slot = bucket_hash(kinds[k], keys[k]) & index->mask;
        size_t limit = index->count + 8U;
        if (limit > index->capacity) {
            limit = index->capacity;
        }
        for (size_t probe = 0U; probe < limit; ++probe) {
            const KeyBucket *bucket = &index->buckets[(slot + probe) & index->mask];
            if (bucket->text == NULL) {
                break;
            }
            if (bucket->kind != kinds[k] || strcmp(bucket->text, keys[k]) != 0) {
                continue;
            }
            for (size_t r = 0U; r < bucket->count; ++r) {
                const int rule_index = bucket->rules[r];
                const CssRule *rule;
                if (rule_index < 0 || (size_t)rule_index >= list->count) {
                    continue;
                }
                rule = &list->items[rule_index];
                for (size_t a = 0U; a < rule->count_selectors; ++a) {
                    if (css_selector_matches(&rule->selectors[a], node)) {
                        matchset_push(set, rule, &rule->selectors[a]);
                    }
                }
            }
            break;
        }
        /* Class keys are heap copies; free them after probing. */
        if (kinds[k] == BUCKET_CLASS) {
            free((char *)keys[k]);
        }
    }
}

/* --------------------------------------------------------- generated content */

/* Decodes a `content` value into plain text. Supports strings, `open-quote`
 * / `close-quote`, and `attr(name)`; counters and `url()` produce nothing
 * because a print pipeline has no counter state and cannot fetch. */
static char *decode_content(const char *value, const GraphicNode *subject) {
    char *out = NULL;
    const char *cursor = value;
    if (value == NULL) {
        return NULL;
    }
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor == '\0') {
        return NULL;
    }
    if (p2p_iequals(cursor, "none") || p2p_iequals(cursor, "normal")) {
        return NULL;
    }
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (*cursor == '\0') break;
        if (*cursor == '"' || *cursor == '\'') {
            const char quote = *cursor++;
            const char *start = cursor;
            char *piece;
            while (*cursor != '\0' && *cursor != quote) {
                if (*cursor == '\\' && cursor[1] != '\0') ++cursor;
                ++cursor;
            }
            piece = p2p_strdup((const unsigned char *)start,
                               (size_t)(cursor - start));
            if (*cursor == quote) ++cursor;
            if (piece != NULL) {
                if (out == NULL) {
                    out = piece;
                } else {
                    char *joined = p2p_format("%s%s", out, piece);
                    free(piece);
                    if (joined != NULL) {
                        free(out);
                        out = joined;
                    }
                }
            }
            continue;
        }
        if (*cursor == '(') {
            /* Read the balanced argument, then split it into the function name
             * and its own argument list. */
            size_t depth = 0U;
            const char *open = cursor;
            const char *close = open;
            while (*close != '\0') {
                if (*close == '(') {
                    ++depth;
                } else if (*close == ')') {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                }
                ++close;
            }
            {
                char *inner = p2p_strdup((const unsigned char *)open + 1U,
                                         (size_t)(close - open) - (close > open ? 1U : 0U));
                if (inner != NULL) {
                    char *argument = strchr(inner, '(');
                    if (argument != NULL) {
                        *argument = '\0';
                        argument = p2p_trim(argument + 1);
                    }
                    p2p_lower(inner);
                    inner = p2p_trim(inner);
                    if (strcmp(inner, "attr") == 0 && argument != NULL &&
                        subject != NULL) {
                        const char *resolved = node_attribute(subject, argument);
                        if (resolved != NULL) {
                            char *joined = out != NULL
                                ? p2p_format("%s%s", out, resolved)
                                : p2p_strdup_c(resolved);
                            if (out != NULL) {
                                free(out);
                            }
                            out = joined;
                        }
                    } else if (strcmp(inner, "open-quote") == 0 ||
                               strcmp(inner, "close-quote") == 0) {
                        const char *quote = inner[0] == 'o' ? "\xe2\x80\x9c"
                                                            : "\xe2\x80\x9d";
                        char *joined = out != NULL ? p2p_format("%s%s", out, quote)
                                                   : p2p_strdup_c(quote);
                        if (out != NULL) {
                            free(out);
                        }
                        out = joined;
                    }
                    free(inner);
                }
            }
            if (*close == '\0') {
                break;
            }
            cursor = close + 1;
            continue;
        }
        /* Any other bare keyword is inert. */
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) ++cursor;
    }
    return out;
}

/* Extracts a single declaration's value from a block without applying it. */
static char *block_value(const char *declarations, const char *property) {
    char *copy = p2p_strdup_c(declarations);
    char *result = NULL;
    if (copy == NULL) {
        return NULL;
    }
    {
        char *cursor = copy;
        while (cursor != NULL && *cursor != '\0') {
            char *semicolon = strchr(cursor, ';');
            char *block = cursor;
            if (semicolon != NULL) {
                *semicolon = '\0';
            }
            {
                char *colon = strchr(block, ':');
                if (colon != NULL) {
                    char *name = block;
                    *colon = '\0';
                    name = p2p_trim(name);
                    p2p_lower(name);
                    if (strcmp(name, property) == 0) {
                        free(result);
                        result = p2p_strdup_c(p2p_trim(colon + 1));
                    }
                }
            }
            if (semicolon == NULL) break;
            cursor = semicolon + 1;
        }
    }
    free(copy);
    return result;
}

/* Applies a generated-content style for `pseudo` to `subject`, returning the
 * decoded text or NULL when there is nothing to generate. */
static char *generate_pseudo(Cascade *cascade, const char *pseudo,
                             const GraphicNode *subject, Style *out_style) {
    const CssRuleList *list;
    const BucketIndex *index;
    MatchSet set;
    char *content_normal = NULL;
    char *content_important = NULL;
    char *text = NULL;
    double font_size;
    memset(&set, 0, sizeof(set));
    if (strcmp(pseudo, "before") == 0) {
        list = &cascade->pseudo_before;
        index = &cascade->pseudo_before_index;
    } else if (strcmp(pseudo, "after") == 0) {
        list = &cascade->pseudo_after;
        index = &cascade->pseudo_after_index;
    } else {
        list = &cascade->pseudo_marker;
        index = &cascade->pseudo_marker_index;
    }
    if (list->count == 0U) {
        return NULL;
    }
    collect_matches(list, index, subject, &set);
    if (set.count == 0U) {
        free(set.items);
        return NULL;
    }
    if (set.count > 1U) {
        qsort(set.items, set.count, sizeof(MatchedRule), compare_matched);
    }
    /* Start from the subject's own style so the generated box inherits the way
     * a real child would, then force it to be inline. */
    css_style_initial(out_style);
    css_inherit(out_style, &subject->style);
    out_style->display = DISPLAY_INLINE;
    out_style->floats = FLOAT_NONE;
    out_style->position = POS_STATIC;
    out_style->decoration = 0U;
    out_style->margin[SIDE_TOP] = px_zero();
    out_style->margin[SIDE_RIGHT] = px_zero();
    out_style->margin[SIDE_BOTTOM] = px_zero();
    out_style->margin[SIDE_LEFT] = px_zero();
    out_style->padding[SIDE_TOP] = px_zero();
    out_style->padding[SIDE_RIGHT] = px_zero();
    out_style->padding[SIDE_BOTTOM] = px_zero();
    out_style->padding[SIDE_LEFT] = px_zero();
    out_style->width = value_auto();
    out_style->height = value_auto();
    font_size = value_resolve_length(out_style->font_size, 0.0, 0.0,
                                      cascade->context.root_font_size,
                                      cascade->context.viewport_width,
                                      cascade->context.viewport_height, true);
    for (size_t i = 0U; i < set.count; ++i) {
        css_apply_cascade_block(out_style, set.items[i].rule->declarations,
                                &cascade->context, &font_size);
        if (content_normal == NULL) {
            content_normal = block_value(set.items[i].rule->declarations, "content");
        }
    }
    for (size_t i = 0U; i < set.count; ++i) {
        css_apply_cascade_block(out_style,
                                set.items[i].rule->declarations_important,
                                &cascade->context, &font_size);
        if (content_important == NULL) {
            content_important = block_value(
                set.items[i].rule->declarations_important, "content");
        }
    }
    if (content_important != NULL) {
        free(content_normal);
        content_normal = content_important;
    }
    text = decode_content(content_normal, subject);
    free(content_normal);
    free(set.items);
    if (text == NULL) {
        style_release(out_style);
    }
    return text;
}

/* Inserts a generated text node holding `text` with an already-computed style. */
static GraphicNode *insert_generated(GraphicNode *parent, const char *text,
                                     const Style *style, bool at_start) {
    GraphicNode *node = (GraphicNode *)p2p_calloc(1U, sizeof(GraphicNode));
    if (node == NULL) {
        return NULL;
    }
    node->tag = p2p_strdup_c(TAG_TEXT);
    node->text = p2p_strdup_c(text);
    if (node->tag == NULL || node->text == NULL) {
        free(node->tag);
        free(node->text);
        free(node);
        return NULL;
    }
    node->is_text = true;
    node->generated = true;
    node->style_computed = true;
    /* The style is copied by value and the caller hands over ownership of every
     * string the cascade filled in, so the node must not copy them again. */
    node->style = *style;
    if (at_start) {
        node->next = parent->children;
        node->parent = parent;
        if (parent->children != NULL) {
            parent->children->prev = node;
        } else {
            parent->last_child = node;
        }
        parent->children = node;
    } else {
        node->parent = parent;
        node->prev = parent->last_child;
        if (parent->last_child != NULL) {
            parent->last_child->next = node;
        } else {
            parent->children = node;
        }
        parent->last_child = node;
    }
    return node;
}

static void compute_node(Cascade *cascade, GraphicNode *node,
                         const Style *parent_style) {
    MatchSet set;
    const char *inline_style;
    char *inline_normal = NULL;
    char *inline_important = NULL;
    double font_size;
    if (node->style_computed) {
        return;
    }
    memset(&set, 0, sizeof(set));
    if (!node->is_text) {
        collect_matches(&cascade->rules, &cascade->index, node, &set);
        if (set.count > 1U) {
            qsort(set.items, set.count, sizeof(MatchedRule), compare_matched);
        }
    }
    css_style_initial(&node->style);
    css_inherit(&node->style, parent_style);
    if (node->is_text) {
        node->style.display = DISPLAY_INLINE;
    }
    font_size = value_resolve_length(node->style.font_size, 0.0, 0.0,
                                      cascade->context.root_font_size,
                                      cascade->context.viewport_width,
                                      cascade->context.viewport_height, true);
    /* Cascade order: author normal, inline normal, author important, inline
     * important. The user-agent sheet is the prefix of `cascade->rules`, so it
     * is outranked by document order alone. */
    for (size_t i = 0U; i < set.count; ++i) {
        css_apply_cascade_block(&node->style, set.items[i].rule->declarations,
                                &cascade->context, &font_size);
    }
    inline_style = node_attribute(node, "style");
    if (inline_style != NULL) {
        css_split_important(inline_style, &inline_normal, &inline_important);
        css_apply_cascade_block(&node->style, inline_normal, &cascade->context,
                                &font_size);
    }
    for (size_t i = 0U; i < set.count; ++i) {
        css_apply_cascade_block(&node->style,
                                set.items[i].rule->declarations_important,
                                &cascade->context, &font_size);
    }
    css_apply_cascade_block(&node->style, inline_important, &cascade->context,
                            &font_size);
    node->style_computed = true;
    free(set.items);
    free(inline_normal);
    free(inline_important);
    /* `::marker` content becomes the list marker text on the element itself. */
    if (!node->is_text && node->style.list_style_type != LIST_NONE) {
        Style marker_style;
        char *text = generate_pseudo(cascade, "marker", node, &marker_style);
        if (text != NULL) {
            free(node->style.list_marker_text);
            node->style.list_marker_text = text;
        }
        style_release(&marker_style);
    }
}

static void compute_tree(Cascade *cascade, GraphicNode *node,
                         const Style *parent_style) {
    compute_node(cascade, node, parent_style);
    if (!node->is_text) {
        if (node->style.display != DISPLAY_CONTENTS) {
            Style generated;
            char *before = generate_pseudo(cascade, "before", node, &generated);
            if (before != NULL) {
                insert_generated(node, before, &generated, true);
            } else {
                style_release(&generated);
            }
        }
        for (GraphicNode *child = node->children; child != NULL; child = child->next) {
            compute_tree(cascade, child, &node->style);
        }
        if (node->style.display != DISPLAY_CONTENTS) {
            Style generated;
            char *after = generate_pseudo(cascade, "after", node, &generated);
            if (after != NULL) {
                insert_generated(node, after, &generated, false);
            } else {
                style_release(&generated);
            }
        }
    }
}

/* ------------------------------------------------------------ stylesheet IO */

static Value px_zero(void) {
    return value_make(0.0, UNIT_PX);
}

static const char *skip_block(const char *cursor) {
    int depth = 0;
    if (*cursor != '{') {
        return cursor;
    }
    while (*cursor != '\0') {
        if (*cursor == '"' || *cursor == '\'') {
            const char quote = *cursor++;
            while (*cursor != '\0' && *cursor != quote) {
                ++cursor;
            }
            if (*cursor == '\0') {
                return cursor;
            }
        } else if (*cursor == '/' && cursor[1] == '*') {
            const char *close = strstr(cursor + 2, "*/");
            if (close == NULL) {
                return cursor + strlen(cursor);
            }
            cursor = close + 2;
            continue;
        } else if (*cursor == '{') {
            ++depth;
        } else if (*cursor == '}') {
            --depth;
            if (depth == 0) {
                return cursor + 1;
            }
        }
        ++cursor;
    }
    return cursor;
}

static void register_font_face(Cascade *cascade, const char *block) {
    CssFontFace face;
    char *normal = NULL;
    char *important = NULL;
    memset(&face, 0, sizeof(face));
    face.weight = value_number(400.0);
    css_split_important(block, &normal, &important);
    free(important);
    {
        char *cursor = normal;
        while (cursor != NULL && *cursor != '\0') {
            char *semicolon = strchr(cursor, ';');
            char *declaration = cursor;
            if (semicolon != NULL) {
                *semicolon = '\0';
                cursor = semicolon + 1;
            } else {
                cursor = declaration + strlen(declaration);
            }
            {
                char *colon = strchr(declaration, ':');
                if (colon != NULL) {
                    char *name = declaration;
                    char *value;
                    *colon = '\0';
                    value = p2p_trim(colon + 1);
                    name = p2p_trim(name);
                    p2p_lower(name);
                    if (strcmp(name, "font-family") == 0) {
                        char *copy = p2p_strdup_c(value);
                        if (copy != NULL) {
                            char *trimmed = p2p_trim(copy);
                            char *comma = strchr(trimmed, ',');
                            char *quote;
                            if (comma != NULL) *comma = '\0';
                            trimmed = p2p_trim(trimmed);
                            if (*trimmed == '"' || *trimmed == '\'') {
                                quote = trimmed + strlen(trimmed) - 1;
                                if (*quote == *trimmed) *quote = '\0';
                                trimmed = p2p_trim(trimmed + 1);
                            }
                            free(face.family);
                            face.family = p2p_strdup_c(trimmed);
                            free(copy);
                        }
                    } else if (strcmp(name, "font-weight") == 0) {
                        css_parse_length(value, &cascade->context, 16.0, &face.weight);
                    } else if (strcmp(name, "font-style") == 0) {
                        if (p2p_iequals(value, "italic")) {
                            face.style = FONT_STYLE_ITALIC;
                        } else if (p2p_iequals(value, "oblique")) {
                            face.style = FONT_STYLE_OBLIQUE;
                        }
                    } else if (strcmp(name, "src") == 0) {
                        char *copy = p2p_strdup_c(value);
                        if (copy != NULL) {
                            char *open = strstr(copy, "url(");
                            if (open != NULL) {
                                char *close = strchr(open + 4, ')');
                                if (close != NULL) {
                                    *close = '\0';
                                    free(face.source);
                                    face.source = p2p_strdup_c(p2p_trim(open + 4));
                                }
                            }
                            free(copy);
                        }
                    }
                }
            }
        }
    }
    free(normal);
    if (face.family != NULL && face.source != NULL) {
        if (cascade->face_count == cascade->face_capacity) {
            const size_t capacity = cascade->face_capacity == 0U ? 8U
                                      : cascade->face_capacity * 2U;
            CssFontFace *grown = (CssFontFace *)p2p_realloc(
                cascade->faces, capacity * sizeof(CssFontFace));
            if (grown != NULL) {
                cascade->faces = grown;
                cascade->face_capacity = capacity;
            }
        }
        if (cascade->face_count < cascade->face_capacity) {
            cascade->faces[cascade->face_count++] = face;
            if (cascade->renderer != NULL && cascade->renderer->fonts != NULL) {
                fonts_register_source(cascade->renderer->fonts, face.family, face.source,
                                      "@font-face");
            }
            return;
        }
    }
    free(face.family);
    free(face.source);
}

/* Parses one NUL-terminated stylesheet, recursing into nested at-rules. */
static void parse_stylesheet(Cascade *cascade, const char *text) {
    const char *cursor = text;
    while (cursor != NULL && *cursor != '\0') {
        if (isspace((unsigned char)*cursor)) {
            ++cursor;
            continue;
        }
        if (cursor[0] == '/' && cursor[1] == '*') {
            const char *close = strstr(cursor + 2, "*/");
            if (close == NULL) {
                return;
            }
            cursor = close + 2;
            continue;
        }
        if (*cursor == '}') {
            ++cursor;
            continue;
        }
        if (*cursor == '@') {
            char name[64];
            size_t i = 0U;
            const char *scan = cursor + 1;
            while (*scan != '\0' && *scan != ' ' && *scan != '\t' &&
                   *scan != '\n' && *scan != '\r' && *scan != '(' &&
                   *scan != '{' && *scan != ';' && i + 1U < sizeof(name)) {
                name[i] = (char)tolower((unsigned char)*scan);
                ++i;
                ++scan;
            }
            name[i] = '\0';
            {
                const char *brace = strchr(cursor, '{');
                const char *semicolon = strchr(cursor, ';');
                if (brace == NULL) {
                    return;
                }
                if (semicolon != NULL && semicolon < brace) {
                    /* A statement at-rule such as @import or @charset. Nothing
                     * is fetched: the IR already carries same-origin CSS. */
                    cursor = semicolon + 1;
                    continue;
                }
                {
                    const char *body = brace + 1;
                    const char *after = skip_block(brace);
                    const size_t body_length = (size_t)(after - body - 1);
                    if (strcmp(name, "media") == 0) {
                        /* `scan` already sits past the at-keyword. */
                        char *query = p2p_strdup((const unsigned char *)scan,
                                                 (size_t)(brace - scan));
                        const bool matched = media_matches(query, &cascade->context);
                        free(query);
                        if (matched) {
                            char *inner = p2p_strdup((const unsigned char *)body,
                                                      body_length);
                            if (inner != NULL) {
                                parse_stylesheet(cascade, inner);
                                free(inner);
                            }
                        }
                    } else if (strcmp(name, "font-face") == 0) {
                        char *inner = p2p_strdup((const unsigned char *)body,
                                                  body_length);
                        register_font_face(cascade, inner != NULL ? inner : "");
                        free(inner);
                    } else if (strcmp(name, "page") == 0) {
                        char *inner = p2p_strdup((const unsigned char *)body,
                                                  body_length);
                        if (inner != NULL) {
                            apply_page_rule(inner, &cascade->context);
                            free(inner);
                        }
                    } else if (strcmp(name, "supports") == 0 ||
                               strcmp(name, "layer") == 0) {
                        /* Feature queries and cascade layers are not evaluated.
                         * Applying the block over-applies rather than silently
                         * dropping author intent. */
                        char *inner = p2p_strdup((const unsigned char *)body,
                                                  body_length);
                        if (inner != NULL) {
                            parse_stylesheet(cascade, inner);
                            free(inner);
                        }
                    } else {
                        /* @keyframes, @property, @counter-style: the printer has
                         * no animation or counter machinery, so the block is
                         * skipped. */
                    }
                    cursor = after;
                    continue;
                }
            }
        }
        {
            const char *brace = strchr(cursor, '{');
            const char *after;
            if (brace == NULL) {
                return;
            }
            after = skip_block(brace);
            {
                char *selectors = p2p_strdup((const unsigned char *)cursor,
                                             (size_t)(brace - cursor));
                char *declarations = p2p_strdup((const unsigned char *)brace + 1,
                                                (size_t)(after - brace - 1));
                if (selectors != NULL && declarations != NULL) {
                    register_rule(&cascade->rules, selectors, declarations);
                }
                free(selectors);
                free(declarations);
            }
            cursor = after;
        }
    }
}

/* Moves every pseudo-element alternative out of the main rule list and into
 * the list for its pseudo-element. Alternatives that are not pseudo-element
 * selectors stay in place, so a rule like `p::before, p` keeps its `p` half. */
static void split_pseudo_rules(Cascade *cascade) {
    CssRuleList *targets[3];
    BucketIndex *target_indexes[3];
    targets[0] = &cascade->pseudo_before;
    targets[1] = &cascade->pseudo_after;
    targets[2] = &cascade->pseudo_marker;
    target_indexes[0] = &cascade->pseudo_before_index;
    target_indexes[1] = &cascade->pseudo_after_index;
    target_indexes[2] = &cascade->pseudo_marker_index;
    for (size_t i = 0U; i < cascade->rules.count; ++i) {
        CssRule *rule = &cascade->rules.items[i];
        size_t keep = 0U;
        for (size_t a = 0U; a < rule->count_selectors; ++a) {
            SelectorAlternative *alt = &rule->selectors[a];
            int slot = -1;
            if (alt->pseudo_element == NULL) {
                if (keep != a) {
                    rule->selectors[keep] = *alt;
                }
                ++keep;
                continue;
            }
            if (strcmp(alt->pseudo_element, "before") == 0) slot = 0;
            else if (strcmp(alt->pseudo_element, "after") == 0) slot = 1;
            else if (strcmp(alt->pseudo_element, "marker") == 0) slot = 2;
            if (slot >= 0) {
                CssRule copy;
                memset(&copy, 0, sizeof(copy));
                copy.selectors = (SelectorAlternative *)p2p_calloc(
                    1U, sizeof(SelectorAlternative));
                if (copy.selectors != NULL) {
                    copy.selectors[0] = *alt;
                    copy.count_selectors = 1U;
                    copy.declarations = p2p_strdup_c(rule->declarations);
                    copy.declarations_important =
                        p2p_strdup_c(rule->declarations_important);
                    copy.order = rule->order;
                    if (rule_list_push(targets[slot], &copy)) {
                        const int rule_index = (int)(targets[slot]->count - 1U);
                        index_alternative(&targets[slot]->items[rule_index].selectors[0],
                                          target_indexes[slot], rule_index);
                    }
                }
            }
            alternative_free(alt);
        }
        rule->count_selectors = keep;
    }
    /* Compact away rules that only carried pseudo-element alternatives. */
    {
        size_t out = 0U;
        for (size_t i = 0U; i < cascade->rules.count; ++i) {
            if (cascade->rules.items[i].count_selectors == 0U) {
                rule_free(&cascade->rules.items[i]);
                continue;
            }
            if (out != i) {
                cascade->rules.items[out] = cascade->rules.items[i];
                memset(&cascade->rules.items[i], 0, sizeof(CssRule));
            }
            ++out;
        }
        cascade->rules.count = out;
    }
    /* The main index is built only after the split, so its rule indices stay
     * valid for the lifetime of the cascade. */
    for (size_t i = 0U; i < cascade->rules.count; ++i) {
        CssRule *rule = &cascade->rules.items[i];
        for (size_t a = 0U; a < rule->count_selectors; ++a) {
            index_alternative(&rule->selectors[a], &cascade->index, (int)i);
        }
    }
}

/* ---------------------------------------------------------------- entry point */

bool css_compute_tree(GraphicNode *root, Renderer *renderer) {
    Cascade cascade;
    Style root_style;
    bool ok = true;
    memset(&cascade, 0, sizeof(cascade));
    bucket_index_init(&cascade.index);
    bucket_index_init(&cascade.pseudo_before_index);
    bucket_index_init(&cascade.pseudo_after_index);
    bucket_index_init(&cascade.pseudo_marker_index);
    cascade.context.root_font_size = 16.0;
    cascade.context.viewport_width = renderer->geometry.width;
    cascade.context.viewport_height = renderer->geometry.height;
    cascade.context.geometry = renderer->geometry;
    cascade.context.geometry_set = renderer->have_geometry;
    cascade.renderer = renderer;
    /* The user-agent sheet is registered first, so every author rule outranks
     * it on document order alone. */
    parse_stylesheet(&cascade, kUserAgentSheet);
    for (GraphicNode *child = root->children; child != NULL; child = child->next) {
        if (!child->is_text && strcmp(child->tag, "style") == 0) {
            for (GraphicNode *text = child->children; text != NULL; text = text->next) {
                if (text->is_text && text->text != NULL) {
                    parse_stylesheet(&cascade, text->text);
                }
            }
        }
    }
    split_pseudo_rules(&cascade);
    css_style_initial(&root_style);
    compute_tree(&cascade, root, &root_style);
    if (p2p_oom()) {
        p2p_report_oom();
        ok = false;
    }
    /* `@page` may have changed the paper; hand the result back to the caller. */
    if (cascade.context.geometry_set) {
        renderer->geometry = cascade.context.geometry;
        renderer->have_geometry = true;
    }
    cascade_free(&cascade);
    return ok;
}
