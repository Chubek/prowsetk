# Chapter 08: PDQL

[Manual index](README.md)

## Query model and supported syntax

PDQL selects DOM elements and emits named projection rows. The implemented
slice supports bounded tag globs, core XPath, equality guards, text trimming,
numeric conversion, and count/sum/average over the selection.

```pdql
query Headings {
    from <h*>
    select { tag: node.tag_name, title: node.text.trim() }
    serialize as json
}
```

`<h*>` matches every tag beginning with `h`, including `html` and `head` when
present; it is not a heading-level range. Use `xpath("//h1 | //h2")` for an exact
set. Glob `*` matches any run of characters and `?` one character.

The compact equivalent forms are `select tag, text from <h*>` and
`from <h*> select { title: node.text }`. A selector alone, such as `<h1>`,
projects text. Guards accept equality, for example:

```pdql
select text from <p> where attr("class") = "price"
```

| Projection | Meaning |
|---|---|
| `text`, `node.text` | Subtree text |
| `tag`, `tag_name`, `node.tag_name` | Element tag |
| `attr("name")`, `node.attr("name")` | Attribute value |
| `.trim()` on a property | Remove surrounding whitespace |
| `number(node.attr("data-price"))` | Numeric-prefix conversion; nonnumeric input becomes zero |
| `count()` | Number of selected rows, repeated in each row |
| `sum()`, `avg()` | Numeric text aggregate over the selection, repeated in each row |

Aggregates do not collapse the result into a separate grouped row. An empty
selection returns no rows. The proposed marionette, RE2 `rx`, arithmetic,
`find`/pipeline, and general computational language is not implemented.
Unsupported expressions report errors.

## Lua query APIs

```lua
local pdql = require('lpdql')
pdql.validate('select text from <h1>')
local rows = pdql.rows(session, 'select tag, text from <h1>')
local json = pdql.query(session:document(), 'select text from <h1>', 'json')
```

`query` returns a serialized string; `rows` returns an array of Lua tables with
string/numeric values; `validate` returns true or raises a query error. Use
`pcall` for recoverable failures. Both a managed Document and a loaded Session
are accepted. Choose `json`, `yaml`, `xml`, or `sexpr` through the format
argument; the default is JSON. The query-block serialization annotation does
not replace the API's explicit format selection.

Live queries snapshot the installed DOM, including page-script and Lua/C++
mutations. Default redaction clears scripts/styles, private form-control values,
sensitive attributes, inline handlers, and sensitive URL components before
selection. The original DOM remains intact.

## C++ and C ownership

```cpp
#include <prowsetk/pdql.hpp>
const prowsetk::pdql::Query query("select text from <h1>");
const auto rows = query.execute(*session->document());
const auto json = prowsetk::pdql::serialize(rows, prowsetk::pdql::Format::Json);
```

The live `prowsetk::Document` overload redacts by default; its second boolean
can explicitly select raw querying. `pdql::Document::parse` constructs a
standalone document with a C-node mirror. Execution on that standalone document,
including the C API, is raw; apply an output policy for private input yourself.

The C header is `prowsetk/pdql.h`. The sequence is:

1. `pt_pdql_document_create(html, &error)`.
2. `pt_pdql_query_compile(source, &error)`.
3. `pt_pdql_query_execute(query, document, &error)`.
4. `pt_pdql_result_serialize(result, PT_PDQL_JSON, &error)`.
5. Free serialized/error strings with `pt_pdql_str_free`, and owning handles
   with `pt_pdql_result_free`, `pt_pdql_query_free`, `pt_pdql_document_free`.

Failures return NULL and an allocated diagnostic when requested. Node handles
from root/child traversal and their strings are borrowed from the document.
No C++ exception crosses the C boundary. The current symbol names are
`pt_pdql_*`, rather than the earlier design's `pt_dom_*` names.

Here is a complete C client, saved as `query.c`:

```c
#include <stdio.h>
#include <prowsetk/pdql.h>

int main(void) {
    char *error = NULL;
    char *output = NULL;
    pt_pdql_document *document = NULL;
    pt_pdql_query *query = NULL;
    pt_pdql_result *result = NULL;
    int status = 1;

    document = pt_pdql_document_create("<h1>Local report</h1>", &error);
    if (!document) goto cleanup;
    query = pt_pdql_query_compile("select text from <h1>", &error);
    if (!query) goto cleanup;
    result = pt_pdql_query_execute(query, document, &error);
    if (!result) goto cleanup;
    output = pt_pdql_result_serialize(result, PT_PDQL_JSON, &error);
    if (!output) goto cleanup;
    puts(output);
    status = 0;

cleanup:
    if (status && error) fprintf(stderr, "%s\n", error);
    pt_pdql_str_free(error);
    pt_pdql_str_free(output);
    pt_pdql_result_free(result);
    pt_pdql_query_free(query);
    pt_pdql_document_free(document);
    return status;
}
```

All matching free functions accept NULL. `pt_pdql_document_root`,
`pt_pdql_node_child_count`, and zero-based `pt_pdql_node_child` expose borrowed
tree traversal; `pt_pdql_node_attribute` returns NULL for an absent attribute.
Link the C client to the core's C++ implementation through the exported package:

```cmake
cmake_minimum_required(VERSION 3.25)
project(QueryPage LANGUAGES C CXX)
find_package(ProwseTk CONFIG REQUIRED)
add_executable(query-page query.c)
set_target_properties(query-page PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(query-page PRIVATE ProwseTk::core)
```

## Formats, bounds, and CLI use

JSON is an array of field maps; YAML is a sequence; XML uses `<results>`,
`<row>`, and `<field name="...">`; S-expressions encode rows and field/value
pairs. Values/names are escaped for their format. Query source is limited to
64 KiB, results to 10,000 rows and 16 MiB of projected/serialized data; HTML
input also obeys Flatworm bounds.

To run a query from the CLI, make a declared driver call `pdql.query` and write
or print its result. Chapter 10 supplies that recipe. Crawler and pagewatch
accept a PDQL query in their TOML configuration.

Reference: `pdql.h`, `pdql.hpp`, `src/pdql/pdql.cpp`, `tests/unit/test_pdql.cpp`.

**Next:** [Lua control API](09-lua-control-api.md).
