"""Hermetic watcher deployment, DOM changes, IPC, actions and restart recovery."""
import json
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import sys
import tempfile
import time

DAEMON, CTL = sys.argv[1:]


def main():
    with tempfile.TemporaryDirectory(prefix="pw-", dir=os.getcwd()) as tmp:
        root = Path(tmp)
        runtime = root / "r"
        action = root / "action.lua"
        action.write_text("function main(args) local f=assert(io.open('action.json','wb')); f:write(args.data); f:close(); return 0 end")
        daemon_config = root / "Pagewatch.toml"
        daemon_config.write_text("[pagewatch]\naction_script='action.lua'\naction_timeout_ms=500\n")
        script = root / "watch.lua"
        script.write_text("""
local watch = require('lpgwatch')
function main(args)
    watch.watch { session=session, query=args.query,
        before_sample=function(s)
            local h=s:document():query_selector('h1')
            if h then h:set_text(h:text() .. ' via Lua') end
        end }
    return 0
end
""")
        page = root / "page.html"

        def content(title, outside="outside"):
            temporary = root / "page.new"
            temporary.write_text("<main><h1 data-token='private-token'>" + title + "</h1><p>" + outside + "</p><input value='private-input'><textarea>private-textarea</textarea><script>var x='private-script';</script></main>")
            temporary.replace(page)

        content("First")
        watch_config = root / "Watcher.toml"
        watch_config.write_text("[watcher]\nurl='https://example.test/'\nhtml_file='page.html'\nquery=\"select text, attr('value'), attr('data-token') from xpath('//h1 | //input | //textarea | //script')\"\ninterval_ms=50\nworker_timeout_ms=5000\n[engine]\njavascript=false\n")

        def ctl(*args, success=True):
            result = subprocess.run([CTL, "--directory", str(runtime), *map(str, args)],
                                    capture_output=True, text=True, timeout=5)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return json.loads(result.stdout) if result.stdout else None

        def wait(predicate, timeout=10):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                result = predicate()
                if result:
                    return result
            raise AssertionError("condition timed out")

        def start():
            proc = subprocess.Popen([DAEMON, "--directory", str(runtime), "--config", str(daemon_config)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            with selectors.DefaultSelector() as selector:
                selector.register(proc.stdout, selectors.EVENT_READ)
                assert selector.select(10), "daemon startup timed out"
                assert proc.stdout.readline().strip() == "pgwatchd ready"
            return proc

        def shutdown(proc):
            ctl("shutdown")
            assert proc.wait(timeout=10) == 0
            proc.stdout.close()
            proc.stderr.close()

        process = None
        worker_pid = 0
        try:
            process = start()
            assert (runtime / "pgwatch.sock").stat().st_mode & 0o777 == 0o600
            assert runtime.stat().st_mode & 0o777 == 0o700
            ctl("deploy", "headings", script, "--config", watch_config)
            wait(lambda: ctl("status", "headings")["state"] == "running")
            first = ctl("data", "headings")
            assert first["sequence"] == 1
            assert "First via Lua" in first["data"] and "private-" not in first["data"]
            assert (runtime / "watchers/headings/tab.json").stat().st_mode & 0o777 == 0o600
            assert not (runtime / "watchers/headings/action.json").exists()
            content("First", "unselected change")
            ctl("check", "headings")
            assert ctl("data", "headings")["sequence"] == first["sequence"]
            content("Second")
            ctl("check", "headings")
            wait(lambda: ctl("data", "headings")["sequence"] >= 2)
            wait(lambda: (runtime / "watchers/headings/action.json").exists())
            events = ctl("updates", "headings", "--since", 1)
            assert len(events["updates"]) == 1 and "Second via Lua" in events["updates"][0]["data"]
            assert "private-" not in json.dumps(events)
            ctl("pause", "headings")
            paused = ctl("data", "headings")["sequence"]
            content("Third")
            assert ctl("status", "headings")["state"] == "paused"
            assert ctl("data", "headings")["sequence"] == paused
            ctl("resume", "headings")
            wait(lambda: "Third via Lua" in ctl("data", "headings")["data"])
            # Slow or malformed IPC clients cannot monopolize the daemon loop.
            with socket.socket(socket.AF_UNIX) as client:
                client.connect(str(runtime / "pgwatch.sock"))
                client.sendall(struct.pack(">I", 100))
                assert ctl("ping")["ready"]
            with socket.socket(socket.AF_UNIX) as client:
                client.connect(str(runtime / "pgwatch.sock"))
                client.sendall(struct.pack(">I", 4 * 1024 * 1024))
                assert ctl("ping")["ready"]
            ctl("deploy", "../invalid", script, "--config", watch_config, success=False)
            ctl("deploy", "headings", script, "--config", watch_config, success=False)
            # A stuck trusted Lua watcher is killed by its process watchdog.
            hung = root / "hung.lua"
            hung.write_text("function main(args) local c=coroutine.create(function() while true do end end); coroutine.resume(c); return 0 end")
            hung_config = root / "Hung.toml"
            hung_config.write_text("[watcher]\nurl='https://example.test/'\nhtml_file='page.html'\nworker_timeout_ms=2000\n")
            ctl("deploy", "hung", hung, "--config", hung_config)
            assert ctl("ping")["ready"]
            wait(lambda: ctl("status", "hung")["state"] == "timed_out")
            ctl("remove", "hung")
            saved = ctl("data", "headings")
            shutdown(process)
            process = start()
            assert ctl("status", "headings")["state"] == "paused"
            assert ctl("data", "headings") == saved
            assert ctl("updates", "headings", "--since", 0)["history_lost"]
            ctl("resume", "headings")
            wait(lambda: ctl("status", "headings")["state"] == "running")
            assert ctl("data", "headings")["sequence"] == saved["sequence"]
            # Abrupt daemon death must terminate even a runaway coroutine;
            # deployments still recover paused with the last saved snapshot.
            started = root / "started"
            hung.write_text("function main(args) local f=assert(io.open(" + json.dumps(str(started)) + ", 'wb')); f:write('started'); f:close(); local c=coroutine.create(function() while true do end end); coroutine.resume(c); return 0 end")
            hung_config.write_text("[watcher]\nurl='https://example.test/'\nhtml_file='page.html'\nworker_timeout_ms=10000\n")
            ctl("deploy", "crash", hung, "--config", hung_config)
            wait(lambda: started.exists())
            worker_pid = ctl("status", "crash")["pid"]
            assert worker_pid > 0
            process.kill()
            process.wait(timeout=10)
            process.stdout.close()
            process.stderr.close()

            def worker_ended():
                try:
                    return Path(f"/proc/{worker_pid}/stat").read_text().split(")", 1)[1].split()[0] == "Z"
                except (FileNotFoundError, ProcessLookupError):
                    # procfs may report ESRCH if the worker exits after open
                    # but before read, as well as ENOENT before open.
                    return True

            wait(worker_ended, timeout=5)
            process = start()
            assert ctl("status", "crash")["state"] == "paused"
            assert ctl("status", "headings")["state"] == "paused"
            assert ctl("data", "headings") == saved
            ctl("remove", "crash")
            # Update replaces the deployed script/config, keeping the previous
            # snapshot until the replacement has sampled successfully.
            replacement = root / "replacement.lua"
            replacement.write_text("function main(args) require('lpgwatch').watch {session=session,query='<p>'}; return 0 end")
            ctl("update", "headings", replacement, "--config", watch_config)
            wait(lambda: 'outside' in ctl("data", "headings")["data"])
            # A stalled configured action must leave control and sampling live.
            shutdown(process)
            action.write_text("function main(args) while true do end end")
            process = start()
            ctl("resume", "headings")
            wait(lambda: ctl("status", "headings")["state"] == "running")
            content("Third", "action timeout change")
            ctl("check", "headings")
            wait(lambda: ctl("status", "headings")["action_failures"] >= 1)
            assert ctl("ping")["ready"]
            ctl("remove", "headings")
            assert ctl("list") == []
            shutdown(process)
            process = None
        finally:
            if process and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            if worker_pid:
                try:
                    os.killpg(worker_pid, 9)
                except ProcessLookupError:
                    pass


main()
