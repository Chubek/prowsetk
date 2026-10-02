"""Standalone crawler CLI, offline examples and approved assistant projects."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

CRAWLER, SOURCE = sys.argv[1:]


def run(*args, success=True, stdin=None):
    result = subprocess.run([CRAWLER, *map(str, args)], input=stdin, text=True,
                            capture_output=True, timeout=15)
    assert (result.returncode == 0) == success, (result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="crawler-", dir=os.getcwd()) as tmp:
    root = Path(tmp)
    config = root / "Crawler.toml"
    config.write_text("""
[crawler]
url = "https://example.test/"
query = "select text, attr('value'), attr('data-token') from <*>"
output = "out/pages.jsonl"
openapi = "out/api.yaml"
postman = "out/postman.json"
[login]
mode = "form"
""")
    html = "<h1>Offline</h1><input value='private-input'><script data-token='private-token'>fetch('/api/items?token=private-query')</script><a href='/next'>next</a>"
    result = run("--config", config, "--html", html)
    assert json.loads(result.stdout)["offline"] is True
    rows = [json.loads(line) for line in (root / "out/pages.jsonl").read_text().splitlines()]
    assert len(rows) == 1 and "Offline" in json.dumps(rows)
    for output in (root / "out").iterdir():
        assert "private-" not in output.read_text(), output.name
        assert output.stat().st_mode & 0o777 == 0o600
    assert "/api/items" in (root / "out/api.yaml").read_text()
    json.loads((root / "out/postman.json").read_text())
    # The shipped Booking.com configuration can extract offline data without
    # looking up credentials or claiming an authenticated live session.
    booking = root / "booking.jsonl"
    booking_config = root / "Booking.toml"
    booking_config.write_text((Path(SOURCE) / "tools/crawler/booking-dotcom-admin/Crawler.toml").read_text())
    run("--config", booking_config,
        "--html", "<main><h1>Booking offline</h1></main>", "--output", booking)
    assert "Booking offline" in booking.read_text()
    config.write_text("[crawler]\nurl='https://example.test/'\nmax_pages=0\n")
    run("--config", config, "--html", "<h1>Bad</h1>", success=False)
    config.write_text("[crawler]\nurl='https://example.test/'\nquery='select typo from <h1>'\n")
    run("--config", config, "--html", "<h1>Bad</h1>", success=False)
    # Missing bearer credentials requests an assistant before any HTTP request.
    # A trusted assistant project is launched only after affirmative approval.
    assist_script = root / "assist.lua"
    marker = root / "approved.txt"
    assist_script.write_text("function main(args) local f=assert(io.open(args.marker,'wb')); f:write('approved'); f:close(); return 0 end")
    assist_project = root / "Prowse.toml"
    assist_project.write_text("[project]\nroot='.'\n[[drivers]]\nname='assist'\nscript='assist.lua'\narguments=[{name='url',type='url'}, {name='marker',type='path',default=" + json.dumps(str(marker)) + "}]\n")
    config.write_text("[crawler]\nurl='https://example.test/'\n[login]\nmode='bearer'\ntoken_env='PWTK_CRAWLER_MISSING_TOKEN'\n[assistant]\nenabled=true\nproject='Prowse.toml'\n")
    os.environ.pop("PWTK_CRAWLER_MISSING_TOKEN", None)
    run("--config", config, success=False, stdin="n\n")
    assert not marker.exists()
    run("--config", config, success=False, stdin="y\n")
    assert marker.read_text() == "approved"
    # Relocated tools invoked through PATH find the installed CLI instead of
    # depending on the build-tree fallback. This helper requires no network.
    bindir = root / "bin"
    bindir.mkdir()
    (bindir / "crawler").symlink_to(Path(CRAWLER).resolve())
    cli = bindir / "prowsetk"
    cli.write_text(f"#!{sys.executable}\nfrom pathlib import Path\nPath({str(marker)!r}).write_text('installed')\n")
    cli.chmod(0o700)
    marker.unlink()
    env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ.get("PATH", ""))
    result = subprocess.run(["crawler", "--config", str(config)], input="y\n",
                            text=True, capture_output=True, cwd=root, env=env, timeout=15)
    assert result.returncode != 0, (result.stdout, result.stderr)
    assert marker.read_text() == "installed"
