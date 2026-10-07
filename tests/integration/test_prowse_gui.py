import os
import pathlib
import subprocess
import sys

cli = sys.argv[1]
page = pathlib.Path("gfx-preview.html")
page.write_text("<h1>Visible preview</h1><input value='SECRET_INPUT'>"
                "<textarea>SECRET_TEXTAREA</textarea><script>SECRET_SCRIPT</script>"
                "<div hidden>SECRET_HIDDEN</div>", encoding="utf-8")
env = dict(os.environ)
env.pop("DISPLAY", None)
env.pop("WAYLAND_DISPLAY", None)

def run(*args):
    return subprocess.run([cli, *args], env=env, text=True, capture_output=True, timeout=10)

try:
    result = run("--help")
    assert result.returncode == 0 and "-T" in result.stdout
    result = run("--list-backends")
    assert result.returncode == 0 and "headless\tavailable" in result.stdout
    backends = result.stdout
    for backend in ("x11", "fltk"):
        if backend + "\tavailable" in backends:
            result = run("-T", backend, "--file", str(page), "--check")
            assert result.returncode == 0, result.stderr
            result = run("-T", backend, "--file", str(page))
            assert result.returncode != 0 and "unsupported" in result.stderr, result.stderr
    result = run("-T", "headless", "--file", str(page), "--dump-text")
    assert result.returncode == 0, result.stderr
    assert "Visible preview" in result.stdout and "SECRET" not in result.stdout + result.stderr
    result = run("-T", "headless", "--file", str(page), "--check")
    assert result.returncode == 0 and not result.stdout
    for args in [("-T",), ("--backend", "unknown", "--file", str(page)),
                 ("-T", "bgfx", "--file", str(page)), ("--invalid",),
                 ("--file", str(page), "--url", "https://example.test"),
                 ("--file", str(page), "--width", "0"), ("--file", "SECRET_MISSING")]:
        result = run(*args)
        assert result.returncode != 0 and "SECRET" not in result.stdout + result.stderr
finally:
    page.unlink(missing_ok=True)
