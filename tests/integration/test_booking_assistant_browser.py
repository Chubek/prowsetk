"""Booking handoff resumes while a terminal-independent browser stays open."""
import json
import os
from pathlib import Path
import selectors
import shlex
import signal
import subprocess
import sys
import tempfile
import time

CLI, SOURCE = sys.argv[1:]
SOURCE = Path(SOURCE)


def await_handoff(process, browser_events):
    """Wait for both the real terminal prompt and the fake browser's startup."""
    stderr = b""
    events = b""
    deadline = time.monotonic() + 10
    with selectors.DefaultSelector() as selector:
        selector.register(process.stderr, selectors.EVENT_READ, "stderr")
        selector.register(browser_events, selectors.EVENT_READ, "browser")
        while b"press Enter after browser interaction is complete." not in stderr or b"\n" not in events:
            remaining = deadline - time.monotonic()
            assert remaining > 0, ("browser launch blocked the handoff prompt", stderr)
            ready = selector.select(remaining)
            assert ready, ("browser launch blocked the handoff prompt", stderr)
            for key, _ in ready:
                chunk = os.read(key.fd, 4096)
                assert chunk, ("handoff ended before the prompt/browser startup", stderr)
                if key.data == "stderr":
                    stderr += chunk
                else:
                    events += chunk
    return stderr, json.loads(events.splitlines()[0])


with tempfile.TemporaryDirectory(prefix="booking-handoff-", dir=os.getcwd()) as tmp:
    root = Path(tmp)
    fake_browser = root / "browser.py"
    fake_browser.write_text("""import json
import os
import sys
events, release = map(int, sys.argv[1:3])
print('browser stdout fixture', flush=True)
print('browser stderr fixture', file=sys.stderr, flush=True)
os.write(events, (json.dumps({'pid': os.getpid(), 'url': sys.argv[3]}) + '\\n').encode())
# This stays open even after the CLI finishes; no sleeps or GUI are needed.
os.read(release, 1)
os.write(events, b'stopped\\n')
""")

    for confirm in (True, False):
        case = root / ("confirmed" if confirm else "eof")
        case.mkdir()
        output = case / "booking.yaml"
        entrypoint = case / "driver.lua"
        entrypoint.write_text(f"""function main(args)
    scenario = 'cookie-session'
    test_directory = [==[{case}]==]
    output_file = [==[{output}]==]
    postman_file = [==[{output}.postman.json]==]
    dotenv_file = [==[{case / 'fixture.env'}]==]
    driver_file = [==[{SOURCE / 'examples/booking-dotcom-admin-scrape/scrape-booking-dotcom-admin.lua'}]==]
    driver_arguments = {{assistant_browser_force=true}}
    dofile([==[{SOURCE / 'tests/integration/booking_driver_fixture.lua'}]==])
    return 0
end
""")
        config = case / "Prowse.toml"
        config.write_text("[project]\nroot='.'\n[[drivers]]\nname='handoff'\nscript='driver.lua'\n")
        event_read, event_write = os.pipe()
        release_read, release_write = os.pipe()
        browser_pid = None
        process = None
        try:
            browser_command = shlex.join([sys.executable, str(fake_browser),
                                          str(event_write), str(release_read)])
            env = dict(os.environ, PROWSETK_ASSISTANT_BROWSER=browser_command)
            process = subprocess.Popen([CLI, "run", "handoff", "--config", str(config)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       env=env, pass_fds=(event_write, release_read), start_new_session=True)
            stderr, started = await_handoff(process, event_read)
            browser_pid = started["pid"]
            assert started["url"] == "https://admin.booking.com/"
            os.kill(browser_pid, 0)
            stdout, remainder = process.communicate(input=b"\n" if confirm else b"", timeout=15)
            stderr += remainder
            assert (process.returncode == 0) == confirm, (stdout, stderr)
            assert output.exists() == confirm, (stdout, stderr)
            if confirm:
                assert "authenticated: true" in output.read_text()
            else:
                assert b"browser interaction was not confirmed" in stderr, stderr
            assert b"browser stdout fixture" not in stdout, stdout
            assert b"browser stderr fixture" not in stderr, stderr
            # Completing or cancelling the driver must not wait for browser exit.
            os.kill(browser_pid, 0)
            os.write(release_write, b"x")
            with selectors.DefaultSelector() as selector:
                selector.register(event_read, selectors.EVENT_READ)
                assert selector.select(5), "browser did not finish after release"
                assert os.read(event_read, 4096) == b"stopped\n"
            print("PASS:", "Enter resumes with browser open" if confirm else "EOF cancels the handoff")
        finally:
            if browser_pid is not None:
                try:
                    os.kill(browser_pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            if process is not None:
                # Includes the test browser on failure, even before its PID arrives.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.communicate(timeout=5)
            for fd in (event_read, event_write, release_read, release_write):
                os.close(fd)
