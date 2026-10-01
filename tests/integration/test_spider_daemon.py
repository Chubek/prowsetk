"""Hermetic daemon/controller/driver tests; files stay in the CTest directory."""
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

DAEMON, CTL = sys.argv[1:]


def main():
    with tempfile.TemporaryDirectory(prefix="spider-", dir=os.getcwd()) as tmp:
        root = Path(tmp)
        sock = str(root / "ipc")
        state = str(root / "state")
        process = None

        def start():
            proc = subprocess.Popen(
                [DAEMON, "--socket", sock, "--directory", state,
                 "--job-timeout-ms", "3000", "--max-spiders", "3"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            with selectors.DefaultSelector() as selector:
                selector.register(proc.stdout, selectors.EVENT_READ)
                assert selector.select(10), "daemon startup timed out"
                assert proc.stdout.readline().strip() == "ptkspiderd ready", proc.stderr.read()
            return proc

        def ctl(command, name=None, success=True):
            args = [CTL, "--socket", sock]
            if name is not None:
                args += ["--name", name]
            result = subprocess.run(args + ["--"] + command, capture_output=True, text=True, timeout=15)
            assert (result.returncode == 0) == success, (command[0], result.stdout, result.stderr)
            return json.loads(result.stdout)

        def shutdown(proc, sig=None):
            if sig is None:
                ctl(["shutdown"])
            else:
                proc.send_signal(sig)
            proc.wait(timeout=10)
            proc.stdout.close()
            proc.stderr.close()

        try:
            process = start()
            assert os.stat(sock).st_mode & 0o777 == 0o600
            assert ctl(["ping"])["protocol"] == 1
            ctl(["create", "https://example.test/", "10", "3", "0"], "booking.com")
            assert ctl(["create", "https://example.test/"], "booking.com", False)["error"] == "already_exists"
            assert ctl(["status"], "../invalid", False)["error"] == "invalid_argument"
            html = """<title>Spider test</title><button>Go</button><h1>Before</h1>
                <input value="private-form"><script>
                document.querySelector('button').addEventListener('click', function() {
                    document.querySelector('h1').textContent = 'After';
                });</script>"""
            ctl(["load-html", html], "booking.com")
            ctl(["click", "button", "1"], "booking.com")
            assert "After" in ctl(["query", "h1"], "booking.com")[0]
            assert ctl(["click", "button", "0"], "booking.com", False)["error"] == "not_found"
            assert ctl(["navigate", "https://other.test/"], "booking.com", False)["error"] == "security_violation"
            records = ctl(["cache", "query", "page/"], "booking.com")
            assert records and "private-form" not in json.dumps(records)
            ctl(["cache", "put", "token", "private-token"], "booking.com")
            assert ctl(["cache", "get", "record/token"], "booking.com") == "[REDACTED]"

            driver = root / "driver.lua"
            driver.write_text("""
                local spider = require('lspider')
                function main(args)
                    if args.html then spider.load_html(args.html, 'https://example.test/') end
                    spider.cache.put('driver-title', spider.document():title())
                    spider.enqueue('https://example.test/next', 1)
                    return 0
                end
            """)
            ctl(["driver", str(driver), "html", "<title>Driver page</title>"], "booking.com")
            assert ctl(["cache", "get", "record/driver-title"], "booking.com") == "Driver page"
            assert ctl(["status"], "booking.com")["pending"] == 2
            ctl(["create", "https://example.test/"], "second")
            ctl(["load-html", "<title>Isolated</title>"], "second")
            assert "Driver page" not in json.dumps(ctl(["cache", "query", "page/"], "second"))

            # An incomplete and oversized client must not block other clients.
            with socket.socket(socket.AF_UNIX) as incomplete:
                incomplete.connect(sock)
                incomplete.sendall(struct.pack(">I", 100))
                assert ctl(["ping"])["protocol"] == 1
            with socket.socket(socket.AF_UNIX) as malformed:
                malformed.connect(sock)
                malformed.sendall(struct.pack(">I", 10 * 1024 * 1024))
                assert ctl(["ping"])["protocol"] == 1

            # Coroutine loops do not inherit the main Lua state's hook. The
            # supervisor's wall deadline must terminate the isolated worker.
            driver.write_text("function main(args) local c=coroutine.create(function() while true do end end); coroutine.resume(c); return 0 end")
            hung = subprocess.Popen([CTL, "--socket", sock, "--name", "booking.com", "--", "driver", str(driver)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            assert ctl(["status"], "second")["name"] == "second"
            stdout, stderr = hung.communicate(timeout=12)
            assert hung.returncode != 0 and json.loads(stdout)["error"] == "timeout", (stdout, stderr)
            assert ctl(["status"], "booking.com")["state"] == "paused"
            assert ctl(["cache", "get", "record/driver-title"], "booking.com") == "Driver page"
            ctl(["stop"], "second")
            assert not next(item for item in ctl(["list"]) if item["name"] == "second")["alive"]
            assert ctl(["status"], "second")["state"] == "paused"

            shutdown(process, signal.SIGTERM)
            process = start()
            assert ctl(["status"], "booking.com")["state"] == "paused"
            assert ctl(["status"], "booking.com")["pending"] == 2
            assert ctl(["cache", "get", "record/driver-title"], "booking.com") == "Driver page"
            ctl(["remove"], "second")
            assert not (Path(state) / "second").exists()
            shutdown(process)
            process = None
            assert not Path(sock).exists()
        finally:
            if process is not None and process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
