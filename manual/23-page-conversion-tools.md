# Chapter 23: Page Conversion Tools

[Manual index](README.md)

## page2pdf

page2pdf is a standalone C consumer of page IR. It accepts canonical events,
serialized VTD, and expanded IML, builds a bounded print-box layout, and paints
backgrounds, borders, text, and embedded images through libHaru.

```sh
mkdir -p build/reports
prowsetk serialize --events --stdout --no-javascript \
  --html '<h1>Report</h1><p>Generated offline.</p>' \
  | page2pdf --format events - build/reports/report.pdf
```

Usage is `page2pdf [--format auto|events|iml|vtd] INPUT OUTPUT.pdf`.
`--help`/`-h` prints usage. `-` denotes standard input. Auto detection checks
VTD magic, then event NDJSON, otherwise expects IML. All formats replay into
the same renderer sink; use `--format` explicitly when diagnosing input issues.

The tool builds when libHaru is available, at
`build/<preset>/tools/page2pdf/page2pdf`. It does not link Flatworm or fetch
remote resources. The upstream serializer can snapshot supported same-origin
resources, or offline input can contain PNG/JPEG data URLs.

### Supported print styles

Inline declarations and basic grouped/descendant tag, class, and ID selectors
from embedded style elements affect print boxes. The supported subset includes
sizes, margins/padding, colors, backgrounds/borders, text styling, and basic
block flow. Unsupported CSS is skipped. Built-in Helvetica, Helvetica-Bold,
and Courier provide fonts.

There is no full browser flex/grid layout, SVG rendering, web-font loading,
JavaScript painting, or remote image loading in the converter. A generated PDF
is a print interpretation of the IR. Input is limited to 16 MiB and output to
512 pages; conversion fails above those bounds. Invalid framing and malformed
event/tree structure also fail.

## page2latex

page2latex is a separate downstream C++ tool accepting canonical ProwseEvent
NDJSON only. It emits LaTeX for headings, paragraphs, lists, emphasis, code,
quotes, links, and image alt text. It is independent of the Flatworm C++ API.

```sh
prowsetk serialize --events --stdout --no-javascript \
  --html '<title>Report</title><h1>Overview</h1><p>Text &amp; data.</p>' \
  | page2latex --hyperref - build/reports/report.tex
```

Usage is:

```text
page2latex [--hyperref] [--template FILE] [--make-pdf] INPUT OUTPUT.{tex,ltx,pdf}
```

| Option/output | Meaning |
|---|---|
| `--hyperref` | Add hyperref and use hyperlink commands; opt-in |
| `--template FILE` | Use a caller-supplied LaTeX template |
| `.tex` / `.ltx` | Write LaTeX source |
| `.pdf` | Compile generated source to PDF |
| `--make-pdf` with TeX output | Also compile a sibling PDF |
| `-` input | Read event NDJSON from standard input |

Without hyperref, links use URL footnotes. Images become alt-text markers;
CSS and browser layout are not translated. The event parser validates
start/attribute/text/end structure and depths, with bounds of 16 MiB input,
200,000 nodes, and 512 nesting levels.

### Templates

A custom template must include `{{PAGE2LATEX_BODY}}`; it may include
`{{PAGE2LATEX_PREAMBLE}}` and `{{PAGE2LATEX_TITLE}}`. For example:

```latex
\documentclass{article}
\usepackage{url}
{{PAGE2LATEX_PREAMBLE}}
\begin{document}
{{PAGE2LATEX_BODY}}
\end{document}
```

The converter escapes ordinary page text and URL values for LaTeX and preserves
preformatted blocks with verbatim formatting. Templates are trusted LaTeX;
choose their packages and commands explicitly.

### Compiling a PDF

```sh
export PWTK_PAGE2LATEX_ENGINE=xelatex
page2latex --hyperref --make-pdf build/page.ndjson build/reports/page.tex
```

The configured engine defaults to xelatex and must be installed separately with
the packages used by the document/template. Compilation uses a temporary output
directory and propagates engine failures. A missing engine or failed compilation
does not produce a successful blank PDF. The engine setting is a trusted host
command, not a page-provided field.

## Choosing the consumer

Use page2pdf for the supported CSS/box subset and embedded images. Use
page2latex for editable typesetting source and template-based reports. Both
expect page IR, not raw HTML; an input-format mismatch is an error.

Reference: `tools/page2pdf/page2pdf.c`, `tools/page2latex/page2latex.cpp`.

**Next:** [Crawler](24-crawler.md).
