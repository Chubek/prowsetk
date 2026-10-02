#ifndef PROWSETK_PDQL_H
#define PROWSETK_PDQL_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct pt_pdql_document pt_pdql_document;
typedef struct pt_pdql_node pt_pdql_node;
typedef struct pt_pdql_query pt_pdql_query;
typedef struct pt_pdql_result pt_pdql_result;
typedef enum pt_pdql_format { PT_PDQL_JSON=0, PT_PDQL_YAML=1, PT_PDQL_XML=2, PT_PDQL_SEXPR=3 } pt_pdql_format;
/* Owning document/query/result handles are released by their matching free
 * functions. Node handles and returned node strings are borrowed from the
 * document. Serialized strings and error_out strings must be released with
 * pt_pdql_str_free. Failure returns NULL, with an allocated diagnostic when
 * error_out is supplied; no C++ exception crosses this C boundary.
 * HTML parsing uses the bounded Flatworm parser. PDQL supports bounded tag
 * globs, core XPath, projections, equality guards and numeric aggregates;
 * unsupported syntax fails explicitly. */
pt_pdql_document *pt_pdql_document_create(const char *html, char **error_out);
void pt_pdql_document_free(pt_pdql_document *document);
pt_pdql_node *pt_pdql_document_root(pt_pdql_document *document);
size_t pt_pdql_node_child_count(const pt_pdql_node *node);
pt_pdql_node *pt_pdql_node_child(const pt_pdql_node *node, size_t index);
const char *pt_pdql_node_tag(const pt_pdql_node *node);
const char *pt_pdql_node_text(const pt_pdql_node *node);
const char *pt_pdql_node_attribute(const pt_pdql_node *node, const char *name);
pt_pdql_query *pt_pdql_query_compile(const char *source, char **error_out);
void pt_pdql_query_free(pt_pdql_query *query);
pt_pdql_result *pt_pdql_query_execute(const pt_pdql_query *query, const pt_pdql_document *document, char **error_out);
char *pt_pdql_result_serialize(const pt_pdql_result *result, pt_pdql_format format, char **error_out);
void pt_pdql_result_free(pt_pdql_result *result);
void pt_pdql_str_free(char *value);
#ifdef __cplusplus
}
#endif
#endif
