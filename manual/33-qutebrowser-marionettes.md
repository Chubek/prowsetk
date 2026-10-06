# Chapter 33: Two-Way Qutebrowser Marionettes

[Manual index](README.md)

## Inspect, act, capture again

The Qutebrowser marionette is a two-way userscript workflow. Lua inspects an
assistant snapshot, sends one fixed DOM action, and receives a fresh capture
before choosing its next step. This is browser interaction through the bridge;
PDQL's proposed `marionette { ... }` query syntax remains unsupported, as
documented in Chapter 8.

Start the broker from Chapter 31, open the approved-origin page, and connect
with the target tab active:

```text
:spawn --userscript /absolute/prowsetk/tools/qutebrowser-bridge/ptk-qute-marionette --bridge /absolute/prowsetk/build/qute-demo/bridge.json --settle-ms 500
```

Only one marionette can attach to a conversation. It stays alive until the
driver finishes, the broker expires, or the connection/action fails. The
browser itself remains open and user-owned.

## The fresh-capture sequence

```text
1. Marionette attaches and publishes the initial QUTE_HTML snapshot.
2. Lua sends client:act({type=..., ...}, wait_ms).
3. Broker validates the action, reserves its budget, and assigns an action ID.
4. Marionette writes a private file containing a fixed action implementation.
5. QUTE_FIFO receives jseval --quiet --world=main --file <private-file>.
6. After the settling interval, QUTE_FIFO spawns a new ptk-qute-send userscript.
7. That sender reads its new QUTE_HTML and publishes the matching action capture.
8. Broker returns the fresh snapshot to the waiting Lua call.
```

`QUTE_HTML` is captured at userscript startup. Rereading the original file after
an action would return old data. The sender invoked in step 6 is essential.
The original userscript's temporary page files also disappear when it exits.

`--settle-ms` accepts 0–5000 ms, default 500. This is a settling delay, not a
load-complete or network-idle guarantee. Inspect the returned DOM for the
application state you need. `client:snapshot` alone does not trigger recapture.

## Supported actions

| Action table | Behavior |
|---|---|
| `{type='click', selector='button[data-next]'}` | Select the first matching element and invoke DOM `click()` |
| `{type='fill', selector='input[name=filter]', value='available'}` | Focus a supported control, use its prototype value setter, dispatch input/change |
| `{type='navigate', url='https://example.test/catalog'}` | Assign a validated same-origin HTTP(S) URL |
| `{type='reload'}` | Reload the current approved-origin page |
| `{type='capture'}` | Request a fresh DOM without changing the page |
| `{type='focus', selector='input'}` | Focus an interactable target |
| `{type='scroll', selector='section'}` | Scroll an interactable target into view |
| `{type='select', selector='select', value='available'}` | Set an existing option and emit input/change |
| `{type='check', selector='input[type=checkbox]', checked=true}` | Activate a checkbox/radio to reach the requested state |
| `{type='submit', selector='form'}` | `requestSubmit` on a form with a same-origin action |

Only the fields shown for each action are accepted. Selectors and fill values
are limited to 4096 UTF-8 bytes each. Unknown actions and extra fields fail
validation. URLs carrying userinfo or belonging to another origin are rejected.

For element actions, the target must exist and be enabled. `disabled` and
`aria-disabled="true"` block it; inline/computed hiding on the target or an
ancestor also blocks it. The visibility checks cover `hidden`, `display: none`,
and `visibility: hidden`.

`fill` accepts input, textarea, and select elements, rejects readonly controls,
and rejects password/file/hidden/submit/button/checkbox/radio input types. It
walks the control's prototype chain for the value descriptor, invokes the
setter, then emits bubbling `input` and `change` events. It does not implement
a hardware keyboard stream. DOM `click()` similarly does not generate a full
hardware pointer cascade.

These operations execute fixed shipped code. Caller values are JSON-encoded
into an owner-only JavaScript file; they never become Qutebrowser command text.
There is no bridge action for arbitrary JavaScript, shell execution, browser
command strings, or model-generated code.

## A Lua action with a DOM postcondition

The following example assumes the approved page contains `button[data-next]`
and exposes `[data-step='2']` after that action. Replace these application
selectors with the actual page contract:

```lua
local qute = dofile('tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua')
local client = qute.connect{descriptor = 'build/qute-demo/bridge.json'}
local snapshot = assert(client:snapshot(0, 30000), 'No assistant snapshot')
local session, browser = qute.load_snapshot(snapshot)

local ok = pcall(function()
    local before = qute.scrape(session)
    local next_snapshot = client:act({
        type = 'click', selector = 'button[data-next]'
    }, 30000)
    assert(next_snapshot.revision > snapshot.revision, 'Stale assistant snapshot')
    assert(next_snapshot.action_status ~= 'error', 'Assistant action failed')
    session:load_html(next_snapshot.html, next_snapshot.url)
    local document = session:document()
    assert(document:query_selector("[data-step='2']"), 'Next step not confirmed')
    local after = qute.scrape(session)
    -- Merge before/after endpoint records in trusted Lua, then enrich/export.
end)

session:close()
browser = nil
pcall(function() client:finish() end)
if not ok then error('Assistant workflow failed', 0) end
```

Acquire new Document/Element handles after each HTML installation. To preserve
discovery across changes, accumulate endpoints in a bounded map keyed by
method and URL, then pass the merged array to `qute.enrich`. The shipped Booking
driver implements this pattern and caps its accumulated seeds at 10,000.
The Replxx console in Chapter 34 exposes these actions as terminal orders and
Lua methods. It enriches each capture before replacing it, retaining sanitized
request schemas across pages and strengthening them when later forms reveal
additional fields. Numbered targets are invalidated on every capture.

## Interpreting action replies

The reply includes a fresh snapshot, matching `action_id`, and `action_status`:

| Status | Meaning |
|---|---|
| `ok` | The fixed action completed and its DOM marker survived until capture |
| `error` | The fixed action reported failure and its marker survived |
| `unconfirmed` | No matching marker survived in the captured HTML |

Navigation/reload may replace the document and remove the marker. Pages can
also mutate markers. A marker indicates the fixed operation's reported result,
not a successful application workflow. Use a DOM postcondition even for `ok`.
Treat `unconfirmed` as requiring application evidence rather than as an
automatic success or failure.

Waits are finite, with 30,000 ms as the default and maximum action wait. The
broker accepts at most `--max-actions` total actions and one active action at
a time. A normal one-shot page capture cannot acknowledge an action; only its
matching fresh action capture can complete the waiting call.

If the action times out or the attached controller disappears while an action
is pending, the conversation closes. An action may already have had side
effects even when its reply is lost. The bridge does not automatically retry
it. Inspect the browser and begin a new conversation if further work is needed.

## Trusted action files

The Booking example accepts a trusted JSON action sequence:

```json
{
  "version": 1,
  "actions": [
    {"type": "fill", "selector": "input[name=filter]", "value": "available"},
    {"type": "click", "selector": "button[data-next]"}
  ]
}
```

That driver bounds the policy to 256 KiB and 32 actions. Its
`--actions_file` argument selects the file. Broker validation still applies to
every action; the policy does not increase the broker's configured budget.
Selectors in the shipped `actions.example.json` are illustrative and must
match the actual account UI.

## Origin, tab scope, and troubleshooting

The broker validates explicit navigation URLs and accepts captures only from
the approved origin/tab index. Generated code checks `location.origin` again
before executing. Keep the connected tab active in the same window and avoid
tab reordering; the index-based identity limitation from Chapter 31 applies.

A click can cause the page to navigate or issue requests according to its own
handlers. The bridge is not a network firewall for Qutebrowser. A resulting
cross-origin capture is rejected, and the waiting action cannot complete
normally. Explicit browser redirects and requests do not become Flatworm
Session observations.

For a failure, check broker readiness, the active origin/index, selector
existence, control visibility/disabled state, the action budget, and the
settling interval. Native/IPC failures use generic errors; browser page values
and raw script errors are not bridge diagnostics. Headless protocol and
FIFO/JavaScript simulations run without a desktop or public network:

```sh
ctest --preset default -R 'qutebrowser|Qutebrowser'
```

Reference: `tools/qutebrowser-bridge/userscript.py`,
`tests/integration/test_qutebrowser_bridge.py`,
`tests/integration/test_qutebrowser_actions.js`.

**Next:** [Booking admin API snapshots](34-booking-admin-api-snapshots.md).
