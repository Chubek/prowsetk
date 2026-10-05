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

### Launcher script

`tools/launch-gui.sh` locates the executable across presets, sets the matching
runtime library path, validates the display, and forwards page options:

```sh
tools/launch-gui.sh --file plugins/basic-gui/example.html
tools/launch-gui.sh --url https://example.com
tools/launch-gui.sh --check            # is the GUI build usable here?
tools/launch-gui.sh --print-bin        # which executable would run?
```

It accepts `--url`, `--file`, `--base-url`, `--proxy`, `--no-javascript`,
`--preset`, `--bin`, `--print-bin`, `--check` and `--verbose`. Unrecognized
arguments and everything after `--` pass through to `ptk-basic-gui`, which owns
their diagnostics. `PROWSETK_GUI_BIN` overrides discovery. A missing display is
reported before the process starts (use `xvfb-run -a` for a headless check).
A `--proxy` URL carrying userinfo is rejected so credentials never reach a
process listing; use the `HTTPS_PROXY`/`HTTP_PROXY` variables instead. Verbose
output reports stage names and whether proxy variables are set, never proxy or
page values.

The `gui` preset enables `PROWSETK_BUILD_BASIC_GUI` (default **OFF**). FLTK is
found as a CMake config package or built from `third_party/fltk`. The vendored
build disables demos, FLUID, OpenGL and documentation; Linux defaults to X11.
Desktop development headers and a running display are required only for this
frontend. The core, controller, native metadata facade and ordinary tests remain
display-free. `--help` also runs without a display. FLTK's LGPL license includes
its static-linking exception; see `third_party/fltk/COPYING`.

Options: `--url URL` or `--file FILE`, `--base-url URL` for offline relative
references, `--no-javascript`, `--proxy URL`, `--marionette LUA-FILE`, `--goal TEXT`, `--help`. File startup uses a memory
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

## OpenCode Lua marionette

The **Marionette** tab contains a Lua editor, **Load Lua**, **Run marionette**,
and an **OpenCode goal** field. Load `plugins/basic-gui/marionette.lua`, edit its
permitted actions to match your page, load or navigate the page, then run it.
The initial editor has no allowed actions; load/edit a policy to permit browsing.
A nonempty goal field overrides the policy's goal. Goals go to OpenCode, so keep
credentials and other secrets out of them.

Start an OpenCode API server separately (`opencode serve`). The GUI reads
`OPENCODE_BASE_URL` (default `http://127.0.0.1:4096`),
`OPENCODE_SERVER_USERNAME`, and `OPENCODE_SERVER_PASSWORD`. It uses the existing
bridge's V2 `/api` protocol, deny-all agent tool permissions, separate host
transport, redirect rejection, loopback-only plain HTTP and verified HTTPS.
It never sends page authentication to OpenCode. No server is started implicitly.

```sh
export OPENCODE_SERVER_USERNAME=opencode
# Set OPENCODE_SERVER_PASSWORD securely in the launching environment.
tools/launch-gui.sh --url https://your-site.example/ \
  --marionette plugins/basic-gui/marionette.lua --goal 'Explore the next pages'
```

These are **trusted host Lua scripts**, with the normal `lprowse`, `lprowsext`
and `lprowseir` modules and a global managed `session` referring to the displayed
session. A fresh Lua runtime is used for each run; subscriptions and Lua state
are released when it ends. Lua may prepare that session before returning a
policy. GUI marionettes have a distinct entrypoint contract from CLI drivers:
`main(args)` returns a **version-1 decisions JSON string**, not an integer exit
code. `args.goal` is the goal field/`--goal` value, or an empty string.

```lua
function main(args)
    assert(session:document(), "Load a page first")
    return [[{
      "version": 1, "goal": "Visit the permitted next page",
      "max_steps": 4, "max_page_requests": 32, "max_get_probes": 0,
      "actions": [{"id": "next", "kind": "click", "selector": "a[rel=next]"}]
    }]]
end
```

After Lua preparation, the host runs the existing
[OpenCode marionette controller](../opencode-marionette/README.md). OpenCode sees
the goal, redacted URL, structural target availability and endpoint counts;
it chooses only caller-defined click/type/navigation action IDs or `stop`.
Selectors, typing values, page text, scripts, cookies and headers are omitted
from agent prompts. Model output is validated as a choice and never executed as
Lua, JavaScript or shell code. The policy's same-origin page request bounds,
action/use limits and optional GET-probe limits apply during the host loop.
Use `max_get_probes: 0` to avoid automatic schema GET requests. Extraction runs
internally, but this GUI returns only action count and stop reason; it does not
write exports or claim complete discovery.

Lua source/files are capped at 64 KiB and goals at 4096 bytes. The OpenCode
client allows at most 512 requests including polls, with a 30-second request
timeout and a 120-second wait per reply. GUI execution is synchronous on the
owning desktop thread: preparation and agent/network waits block controls;
there is no in-window cancellation. Lua preparation is trusted and has no
execution deadline or sandbox; it is outside the subsequent action policy.
Actions are immediate and survive a later failure. Both completion and failure
refresh the displayed DOM and navigation history. Routine Lua `print` is
suppressed and UI/CLI errors show error codes rather than script/model values;
trusted scripts still have ordinary Lua IO access and must handle secrets
responsibly. Lua-disabled builds report Unsupported and disable Run.

C++ embedders can call `Controller::run_marionette(source, goal, agent_transport)`
with an optional borrowed, separate `NetworkClient` for testing, or
`Controller::run_marionette_file(path, goal)`. These methods are implemented by
`prowsetk_basic_gui` and run without a display; link it along with
`ProwseTk::basic_gui_model`. `Viewer` exposes corresponding source/file methods
and refreshes its widgets. This reuses existing dependencies and leaves the
native plugin ABI unchanged; loading its facade remains network-free.

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
builds/tests the CLI, the FLTK adapter and the launcher script. Its window smoke
test uses `xvfb-run` when available, otherwise a supplied display; it skips only
when neither exists. The launcher script additionally runs under
`ctest --preset default`, since its help and preflight checks need no display.
No live website is used.
