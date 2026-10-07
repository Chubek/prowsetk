# prowse-gui

An event-stream consumer of the reusable `ProwseTk::gfx` backend layer:

```text
Document → event.hpp ProwseEvent → src/core/event_stream.hpp
         → gfx_ir.hpp PGFX1 bytecode → GFX-Backend.h → selected adapter
```

`-T NAME` (or `--backend NAME`) selects the adapter. `--list-backends` reports
availability without opening a display. The default build provides `headless`;
enable `PROWSETK_GFX_X11` and/or `PROWSETK_GFX_FLTK` for desktop surfaces. The
default preference is FLTK, X11, then headless. BGFX, ImGui and direct Wayland
are reserved names and currently report unavailable; they have no implementation.

```sh
cmake --preset default -DPROWSETK_GFX_X11=ON
cmake --build --preset default
build/default/tools/prowse-gui/prowse-gui -T x11 --file tools/prowse-gui/example.html
build/default/tools/prowse-gui/prowse-gui -T headless --file tools/prowse-gui/example.html --dump-text
```

The tool owns its EmbeddedBrowser. URL navigation and optional page JavaScript
use its Session and NetworkClient; adapters receive no engine handles. JavaScript
is off by default. `--check` submits and validates without opening a window.
`--dump-text` explicitly prints the preview; normal runs print no page values.
Failures expose error codes only.

This first PGFX1 format is a fixed-cell, semantic text preview, with block
breaks and bounded UTF-8 wrapping. It omits head/script/style/template content,
hidden subtrees and private input/textarea/select values. Other page text remains
caller data; it is not a general secret scrubber. There is no CSS layout, image
rasterization or page-action hit testing in this preview. The independent
`render_document` API remains the explicit CSS display-list renderer.

X11 currently uses the server's core font and does not shape UTF-8 glyphs; FLTK
uses its Courier drawing API. Wheel scrolling is supported; Escape or `q` closes
the window. Resize clips the submitted snapshot and does not reflow it. Hosts
must produce a new frame after document/viewport changes.

The C-compatible graphics ABI is version 1 and independent of the native plugin
and Flatworm module ABIs. Definitions are supplied explicitly by trusted hosts
through `GfxBackend(definition)`; automatic shared-library discovery is not
implemented. Backends retain a validated frame copy and destroy their own opaque
state. Calls run synchronously on the creating thread; exceptions must not cross
the ABI. No backend opens page-network sockets.

Bounds: 400,000 source events / 32 MiB aggregate event strings, 256 nested
elements, 4 MiB PGFX1 bytes, 50,000 text commands, 4,096 bytes per command,
1..16,384 surface pixels and 1,000,000 content pixels vertically. Decoding rejects
unknown opcodes, invalid coordinates/UTF-8, truncation and trailing bytes.
