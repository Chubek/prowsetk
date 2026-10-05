# prowse-gui

Desktop browser executable using `plugins/complex-gui` and the Flatworm
display-list renderer. This is independent of `ptk-basic-gui`.

```sh
cmake --preset complex-gui
cmake --build --preset complex-gui
./build/complex-gui/tools/prowse-gui/prowse-gui
./build/complex-gui/tools/prowse-gui/prowse-gui --url https://example.com/
./build/complex-gui/tools/prowse-gui/prowse-gui --file tools/prowse-gui/example.html
```

Options: `--url`, `--file`, `--base-url`, `--no-javascript`, `--images`, `--proxy`,
`--help`/`-h`. Help and argument validation work without a display. Normal use
requires an FLTK-supported display. Installation places `prowse-gui` in `bin`.
Local-file startup is offline, including subsequent navigation and page scripts.

Use the address bar or Open HTML, scroll with the wheel, and click painted controls.
The bottom editor replaces text control values without revealing existing values.
Images are opt-in. See the [plugin contract](../../plugins/complex-gui/README.md)
for the supported rendering subset, synchronous network behavior and limits.
