# Chapter 03: Quick Start and CLI

[Manual index](README.md)

## An offline first run

The serializer works without a project file. This command installs a supplied
document, disables page scripts, and emits canonical event NDJSON:

```sh
prowsetk version
prowsetk serialize --events --stdout --no-javascript \
  --url https://example.test/ \
  --html '<main><h1>Hello ProwseTk</h1><p>Offline document</p></main>'
```

When both `--html` and `--url` are supplied, the URL is an offline base URL. It
does not initiate the navigation. `--no-javascript` makes this example entirely
static. Save output with `--output build/page.ndjson` instead of `--stdout`.

## Implemented commands

| Command | Purpose | Main options |
|---|---|---|
| `version` | Print toolkit version | None required |
| `serve` | JSON REST API and static web UI | `--host`, `--port`, `--web-root`, `--no-javascript` |
| `webdriver` | WebDriver HTTP endpoint | `--host`, `--port`, `--no-javascript` |
| `cdp` / `playwright` | CDP endpoint for Playwright clients | `--host`, `--port`, `--no-javascript` |
| `endpoints` | Navigate once and emit heuristic OpenAPI | `--url`, `--output`, `--javascript`, `--no-javascript`, `--proxy` |
| `serialize` | Encode a freshly loaded document | Format, source, destination, script policy, `--proxy` |
| `run NAME` | Execute a declared Lua driver | `--config`, declared `--name VALUE` arguments, host overrides |

The current CLI uses a usage message for missing/unknown commands; it does not
provide a global `--help` command. `Prowse.toml` files are authored directly.
PDQL runs through its C/C++/Lua API or a driver, as shown in Chapter 8.

## Serialization reference

Select exactly one of `--events`, `--iml`, or `--vtd`, and exactly one of
`--stdout` or `--output FILE`. Supply `--url URL`, `--html HTML`, or both with
the offline semantics above. VTD is binary; standard output contains the bytes
without a status message.

```sh
prowsetk serialize --vtd --stdout --no-javascript --html '<h1>Report</h1>' \
  | page2pdf --format vtd - build/report.pdf
```

Live URL serialization can snapshot bounded same-origin CSS and PNG/JPEG
resources into the emitted page. The standalone converters read the IR rather
than fetching page resources themselves. See Chapters 22 and 23.

## Endpoint extraction and services

```sh
prowsetk endpoints --url https://example.com/ --output build/openapi.yaml
prowsetk serve --host 127.0.0.1 --port 8080
```

Run the service in one terminal and visit its printed URL from another client.
The CLI `endpoints` command uses extraction from the resulting `Document`.
For session-observed page requests and richer resolution/enrichment, use
`lprowsext.endpoints`, scrape-endpoints, or the crawler workflow.

The service implementation's argument structure defaults to port 8080; use an
explicit port for WebDriver/CDP examples. `--port 0` requests an available port
and the listener prints its actual address.

## Driver invocation and exit status

```sh
prowsetk run crawl-site --config Prowse.toml \
  --url https://example.test/ --html '<h1>Offline</h1>' \
  --output build/pages.jsonl
```

The project must declare that driver and those arguments. `--config` defaults
to `Prowse.toml` in the caller's directory. Values are passed as named Lua
arguments after type coercion. Boolean arguments use an explicit value, such
as `--enabled true`.

Successful CLI operations normally return 0. Usage/configuration errors
commonly return 2; runtime failures return 1. `run` propagates the driver's
integer exit code. Write drivers to return an integer deliberately.

Reference: `src/cli/prowsetk_main.cpp`, `man/man1/prowsetk.1`.

**Next:** [Project configuration](04-project-configuration.md).
