# complex-gui

A separate FLTK browser frontend for Flatworm's explicit C++ renderer. Its custom
canvas paints `render_document()` display lists and uses `hit_test()` to drive
the live Session. It does not use `Fl_Help_View` or depend on `basic-gui`.

## Build and run

```sh
cmake --preset complex-gui
cmake --build --preset complex-gui
build/complex-gui/tools/prowse-gui/prowse-gui --file tools/prowse-gui/example.html
```

`PROWSETK_BUILD_COMPLEX_GUI=ON` enables the adapter and executable; it is OFF by
default. The display-free `ProwseTk::complex_gui_model` controller and version-2
native plugin facade build in all configurations. `ProwseTk::complex_gui` is the
separate shared viewer plugin. Loading its native ABI facade opens no window and
makes no requests. It needs the existing optional FLTK dependency only.

```cpp
#include <prowsetk/plugins/complex_gui.hpp>
prowsetk::Browser browser;
auto session = browser.create_session();
prowsetk::complex_gui::Viewer viewer(*session);
viewer.controller().navigate("https://example.com/");
return viewer.exec();
```

Browser/Session outlive the Viewer. All calls belong on the desktop thread.
`available()` reports whether the graphical adapter was built; a disabled Viewer
constructor throws `Unsupported`. Headless callers can use `Controller` directly
with the renderer's approximate measurer, or supply a borrowed `TextMeasurer`.

## Supported browser workflow

- Address entry, back/forward, reload, local HTML file chooser and empty startup.
- Vertical/horizontal scrollbars, wheel/Shift-wheel and Page Up/Down.
- Backgrounds, borders, measured text, markers, image/alt-text records and form
  control outlines drawn by FLTK. The frontend supplies actual FLTK font advances.
- Link/button/checkbox/radio/disclosure actions through Session synthetic clicks.
- Click a text/password/textarea control, enter a replacement in the masked bottom
  editor, and Apply. Values are never copied from the page into the editor or
  painted unredacted. Replacement clears the DOM value, then types through
  Session's synthetic keyboard/native-setter path; no-JavaScript mode updates the
  DOM directly. Readonly/disabled controls reject edits. Select menus and native
  date/file widgets are not implemented.
- A 100 ms host checkpoint drains bounded timers/microtasks, invalidates stale
  actions, and rerenders dirty documents. Resize updates logical viewport metadata.
- Optional image loading through the owning Session; maximum 32 attempts per page,
  8 MiB decoded cache, PNG/JPEG only. FLTK receives decoded pixels, never URLs.
  External CSS and data-URL decoding are not added by this plugin. Loading failures
  leave alt-text placeholders. Turn on **Load images** or pass `--images` explicitly.
  Toolkit image scaling is capped at four million pixels per image; oversized
  destination boxes fall back to alt text, and over-budget cover scaling is omitted.

The CSS subset is the [core display-list renderer](../../README.md#explicit-headless-display-lists):
normal flow, with block fallback for tables and no flex/grid, browser stacking
contexts, or full inline decoration. Alpha is approximated against white; border
groove/ridge/double styles use solid strokes. Image boxes use CSS dimensions or
the core's alt-text-sized fallback. This is a usable small browser, not a claim of
modern-browser visual compatibility or page-JavaScript layout geometry.

Navigation and resource fetching are synchronous: the window may pause up to the
configured Session timeout; cancellation and background browsing are not provided.
The CLI uses a 10-second request timeout and a 16 MiB response bound. File startup
installs a network-free in-memory transport for the entire session, including page
scripts. Opening a local file in an existing live session retains that session's
network policy. The **Open HTML** dialog does not switch transport policies.

History holds at most 32 entries with a combined 16 MiB offline HTML budget. Live
history performs fresh navigations; credentials stay in the Session. Display
records are local page presentation and can contain page text. They are not
sanitized exports. Address/status omit URL queries/fragments/userinfo and raw
exception values. No browser activity or input values are written to logs.

Controller operations reject reentry. Hit targets carry a snapshot revision and
are rejected after DOM changes, document replacement or removal. After edits or
actions, acquire a fresh target from the new display list.
