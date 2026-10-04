# Basic GUI — Flatworm desktop inspector

`ptk-basic-gui` is an optional FLTK browser GUI for seeing the current Flatworm
DOM and page activity. It uses **ProwseTk's own Session and QuickJS**, with
`third_party/fltk` supplying the desktop widgets and basic HTML preview.

## Build and run

```sh
cmake --preset gui
cmake --build --preset gui -j4
build/gui/plugins/basic-gui/ptk-basic-gui --file plugins/basic-gui/example.html
build/gui/plugins/basic-gui/ptk-basic-gui --url https://example.com
```

The `gui` preset enables `PROWSETK_BUILD_BASIC_GUI` (default **OFF**). FLTK is
found as a CMake config package or built from `third_party/fltk`. The vendored
build disables demos, FLUID, OpenGL and documentation; Linux defaults to X11.
Desktop development headers and a running display are required only for this
frontend. The core, controller, native metadata facade and ordinary tests remain
display-free. `--help` also runs without a display. FLTK's LGPL license includes
its static-linking exception; see `third_party/fltk/COPYING`.

Options: `--url URL` or `--file FILE`, `--base-url URL` for offline relative
references, `--no-javascript`, `--proxy URL`, `--help`. File startup uses a memory
transport that rejects all page requests, including script-initiated navigation;
it can be used for deterministic inspection. Launch without `--file` for live
browsing. HTML files are bounded to 16 MiB.

## Controls

- Address bar, Go, Back, Forward, Reload and Open HTML.
- **Page:** live basic HTML projection; click links, buttons and summaries to
  execute the owning Session's synthetic event cascade. Click a form field to
  select it in the DOM tab, enter masked text and use **Type / append**. This
  appends text through the native prototype setter and keyboard/input/change
  events. Checkboxes/radios and submit controls activate directly.
  Select options can be assigned through the JavaScript entry, for example
  `document.querySelector('select').value = 'option-value'`.
- **DOM:** document-order node list, canonical paths, sanitized attributes,
  hidden/disabled flags. A CSS selector plus **Inspect**, **Click**, or
  **Type / append** provides direct interaction. Empty selectors use the
  selected node. Refreshes clear node selection; stale/detached targets fail.
- **Source:** the current DOM after page scripting, serialized with script,
  style, private form content and sensitive attributes omitted/redacted.
- **Console / events** and **Network:** bounded activity from this session,
  including page-script requests, redirects, DOM mutations and script failures.
  Network activity omits queries, credentials, headers and bodies. Console
  messages and evaluation results hide their values by default; **Console
  values** explicitly enables their local display for subsequent messages and
  results. It can expose page secrets. No page values are written to host logs.
- JavaScript entry evaluates explicitly supplied page code, then pumps lifecycle
  work and refreshes the inspector. Errors display error codes, without raw
  exception values. Clear events clears the activity ring.

Due timers, animation frames and microtasks are pumped every 100 ms while the
window is open. Calls use bounded ScriptOptions (100-ms script checkpoints,
500 microtask jobs, at most 16 passes). Networking retains Session timeouts,
cookies, proxies, TLS verification, request/response hooks and cancellation.
Calls are synchronous on the GUI thread; network requests can temporarily block
the controls. Close releases the timer and event subscription.

## Preview and bounds

The preview consumes canonical ProwseEvent records and uses an allowlisted FLTK
HTML subset: headings, text, emphasis, paragraphs, lists and simple tables.
Buttons/links become synthetic action IDs, controls become editable inspection
targets, and images show alt text. Preview markup has **no image, stylesheet,
file or external-URI loaders**. Hidden attributes/inline hiding, closed dialogs,
and collapsed details are recognized. Stylesheets, flex/grid, canvas pixels,
embedded frames, SVG, fonts and browser hit-testing are outside this preview.
Source includes hidden structure for debugging; form values stay redacted.
This preview is an inspection projection rather than browser-equivalent layout.

Snapshots are bounded to 1 MiB of DOM HTML, 4096 elements (including preflight
root), 64 element levels, and 1 MiB each of projected source/preview. Exceeding
a bound shows an explicit limited snapshot while retaining the loaded session.
Activity retains 512 entries with values capped at 1024 bytes and reports lost
older entries. URL history retains at most 64 navigations; Back/Forward refetch
GET documents, so it is distinct from a browser's back/forward cache or POST
replay. Offline Reload replays the supplied HTML. The address display redacts
known secret parameters; Go on the unchanged address reloads the real URL.

## C++ embedding and native plugin

```cpp
#include <prowsetk/plugins/basic_gui.hpp>

prowsetk::Browser browser;
auto session = browser.create_session();
prowsetk::basic_gui::Viewer viewer(*session);
viewer.load_html("<h1>Inspection</h1>");
return viewer.exec();
```

Link the optional `prowsetk_basic_gui` shared library; its header contains no
FLTK types. `Viewer::show/refresh/close` support an existing FLTK event loop;
`exec()` runs that loop. Create/use/destroy it on the desktop/main thread and
keep the Session and Browser alive longer than the viewer. Serialize external
automation with GUI calls. `Controller` and `inspect_document` are separately
available as `ProwseTk::basic_gui_model`, without a display. GUI-disabled
`available()` returns false and Viewer construction reports Unsupported.

The native library exports the unchanged ABI-v2 `prowsetk_plugin_entry`.
Loading it is display/network-free and exposes capability metadata; opening the
viewer is an explicit C++ operation, since the C plugin ABI supplies document
snapshots rather than an owning live Session. It exposes no page-JavaScript or
Lua extension entry point and creates no graphical requirement for Flatworm.

## Web API additions

`Session::set_viewport(ViewportInfo)` provides logical CSS-pixel dimensions
(1..16384, finite pixel ratio `(0,8]`) before or after a page load.
`window.innerWidth/innerHeight`, `outerWidth/outerHeight`, `devicePixelRatio`,
`screen` and `visualViewport` read that host metadata. Resize dispatches
window/visualViewport events and updates MediaQueryList change listeners.
`matchMedia` supports screen/all/print, comma alternatives, `not`/`only`, `and`,
width/height/min/max in px, resolution/min/max in dppx, orientation, light color
scheme and reduced-motion no-preference. Unknown expressions return false;
queries are capped at 4096 characters and 256 live lists. This supplies metadata,
without CSS layout or nonzero element rectangles.

Details/summary toggles update `open` and emit coalesced `toggle` events.
Dialogs implement `show`, `showModal`, `close`, `requestClose`, `returnValue`
and cancel/close events; `method="dialog"` closes without a network request.
They have semantic state without graphical top-layer/focus-trapping semantics.
Checkbox/radio activation and select-value assignment update form-control state;
disabled fields are omitted from ordinary form submission. Alerts/prompts,
popups and unsupported graphical APIs retain their documented restricted stubs.

## Tests

Model, redaction, identity, navigation and script/network integration tests run
without a display under `ctest --preset default`. `ctest --preset gui` also
builds/tests the CLI and FLTK adapter. Its window smoke test uses `xvfb-run` when
available, otherwise a supplied display; it skips only when neither exists.
No live website is used.
