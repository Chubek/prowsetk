# Chapter 29: Prowse-TUI

[Manual index](README.md)

## Terminal browsing

prowse-tui presents Flatworm's page data in a keyboard-driven terminal interface.
It uses the same managed session, page scripts, synthetic actions, selectors,
and endpoint discovery as other hosts.

The build enables the target with `PROWSETK_BUILD_TUI=ON` when the Termlib and
Termscript sources under `third_party/termlib` are available; parser generation
also needs Perl. Its build-tree executable is
`build/<preset>/tools/prowse-tui/prowse-tui`.

```sh
prowse-tui https://example.com/
prowse-tui --html build/page.html --dump
prowse-tui --html build/page.html --dump --command 'select h1'
```

Usage is `prowse-tui [URL] [--html FILE] [--dump] [--command COMMAND]`.
`--help` prints usage and the overview topic. Dump/command mode supports
noninteractive terminal output. Offline file input is limited to 16 MiB and
uses `https://localhost/` as its base URL.

The text view is an automation-oriented projection, without graphical browser
layout. Page navigation is HTTP(S), and network access remains host-mediated.

## Keys and commands

| Key | Default action |
|---|---|
| `j` / Down, `k` / Up | Scroll |
| Space, `b` | Page down/up |
| `g`, `G` | Top/bottom |
| Enter / `l` / Right | Activate a help link or configuration row |
| `h` / Left | Back in browser/help, close configuration |
| Esc | Close help/configuration or a browser result panel |
| `q` | Quit from browser view |
| `:` | Enter a command |
| `/`, `n`, `?` | Help search/next/previous match |

Commands are entered without the colon in `--command` arguments:

| Command | Purpose |
|---|---|
| `:open URL`, `:follow N`, `:back`, `:reload` | Navigate/history; follow uses one-based link number |
| `:top`, `:bottom` | Scroll position |
| `:bookmark`, `:bookmarks` | In-process bookmarks/list |
| `:links` / `:urls` | List deduplicated redacted page/resource URLs |
| `:urls FILE` | Save that URL list |
| `:text FILE` | Save text-view lines |
| `:endpoints FILE` | Export heuristic OpenAPI including script observations |
| `:select CSS` | Show matching count and elements |
| `:click CSS` | Synthetic click on first match |
| `:set CSS VALUE` | Synthetic typing on first match |
| `:lua CODE`, `:source FILE`, `:driver FILE` | Run Lua, load file, or call its main(args) |
| `:find REGEX` | POSIX extended-regex page-line search |
| `:quit` / `:q` | Quit |

`:set` splits at the first space, so use a selector without spaces there.
`:driver` invokes main with an empty argument table and requires return 0;
the managed global session is bound. Output files need usable parent paths.
Use `:links` to inspect URLs and `:urls FILE` to save them.

## Help pager

`:help`, `:help list`, `:help open PAGE`, and `:help find REGEX` open topics,
show an index, or search them. Topics cover overview, commands, search,
configuration, and extensions. Within help, search with `/`, move matches with
`n`/`?`, activate links with the cursor controls, and return with Esc.

Help resources use bounded `.tsh` pages: `#` headings, `@key` hints, and
`[label](help:topic)` or supported link records. The pager keeps its own history.

## Configuration

Configuration lives at `$XDG_CONFIG_HOME/prowse/ProwseTUI.toml`, falling back
to `$HOME/.config/prowse/ProwseTUI.toml`. For example:

```toml
lua_extensions = ["helpers.lua"]
native_plugins = []
proxy = "http://127.0.0.1:3128"

[keys]
help = "F1"
config = "F2"
```

Omitted keybindings keep the settings model's defaults. Action names are
down/up/back/activate/page_down/page_up/home/end/
close/search/next_match/previous_match/help/config. A configured primary key
must be unique and valid (printable single key, Space/Tab, or F1–F12).

`:config` opens staged settings. Commands include `bind ACTION KEY`,
`proxy URL`, `add-lua PATH`, `remove-lua N`, `add-plugin PATH`,
`remove-plugin N`, `save`, and `reload` after `:config`.
Saving replaces the file atomically, updates the controller's settings model,
and retains extension paths for next launch. The current terminal input loop
still dispatches the hardcoded keys listed above and does not consult configured
primary bindings or F1/F2. Open help/configuration with `:help` and `:config`.
Proxy configuration is loaded on launch. Relative extension/plugin paths
resolve beside the settings file.

Unknown fields, duplicate keys/paths, invalid paths, over 64 extensions of each
kind, and configuration above 256 KiB fail validation. Startup currently falls
back quietly when initialization fails; inspect/reload settings from the
configuration view when a configured extension appears absent.

## Extensions and output policy

Lua extensions execute through the bound runtime; native plugins load through
the versioned registry and initialize at launch. They remain trusted host
automation. Link/OpenAPI exports redact their structured URL data; direct text,
selector results, and arbitrary extension output may contain page information.
Capability restrictions still apply to interactions and page scripts.

Reference: `tools/prowse-tui/main.cpp`, `controller.cpp`, `settings.cpp`, `help/`.

**Next:** [eBPF interface](30-ebpf-interface.md).
