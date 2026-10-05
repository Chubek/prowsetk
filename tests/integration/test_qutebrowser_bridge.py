"""Real userscripts + Unix IPC + ProwseTk Lua/plugins, with a fake QUTE_FIFO."""
import contextlib
import json
import os
from pathlib import Path
import select
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

CLI, IPC, SOURCE = sys.argv[1:4]
del sys.argv[1:4]
TOOLS = Path(SOURCE) / "tools/qutebrowser-bridge"
PROJECT = Path(SOURCE) / "examples/booking-dotcom-admin-api/Prowse.toml"
sys.path.insert(0, str(TOOLS))
from protocol import BridgeError, descriptor, encode, exchange, write_file
from userscript import action_source

BASE = "https://admin.booking.com"
PAGE = ("<a href='/logout'>Account</a><button data-next>Next</button>"
        "<script>fetch('/api/first?limit=2&token=private_marker')</script>"
        "<form action='/api/first' method='post'><input name='token' value='private_marker'>"
        "<input name='count' type='number' required value='2'></form>")


def run_script(name, root, html=PAGE, **env):
    write_file(root / "page.html", html.encode())
    environment = dict(os.environ, QUTE_MODE="command", QUTE_URL=BASE + "/",
                       QUTE_TAB_INDEX="1", QUTE_HTML=str(root / "page.html"), **env)
    return subprocess.run([str(TOOLS / name), "--bridge", str(root / "bridge.json")],
                          env=environment, capture_output=True, timeout=10)


@contextlib.contextmanager
def broker():
    with tempfile.TemporaryDirectory(prefix="q-", dir=os.getcwd()) as directory:
        root = Path(directory)
        process = subprocess.Popen([str(TOOLS / "ptk-qute-bridge"), "serve", "--directory", str(root),
                                    "--url", BASE + "/", "--timeout", "30"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready, _, _ = select.select([process.stdout], [], [], 5)
            if not ready or process.stdout.readline() != b"qutebrowser bridge ready\n":
                raise AssertionError("broker did not become ready")
            yield root, descriptor(root / "bridge.json")
        finally:
            process.terminate()
            stdout, stderr = process.communicate(timeout=5)
            if process.returncode != 0 or stderr:
                raise AssertionError("broker failed: " + repr((process.returncode, stdout, stderr)))
            assert not (root / "bridge.sock").exists()
            assert not (root / "bridge.json").exists()


def driver_args(root, *extra):
    return [CLI, "run", "booking-admin-api", "--config", str(PROJECT),
            "--bridge", str(root / "bridge.json"), "--ipc_module", IPC,
            "--output", str(root / "openapi.yaml"), "--postman", str(root / "postman.json"),
            "--wait_ms", "3000", *extra]


class QutebrowserIntegration(unittest.TestCase):
    def test_one_shot_real_plugins_and_owner_only_artifacts(self):
        with broker() as (root, config):
            self.assertEqual((root / "bridge.sock").stat().st_mode & 0o777, 0o600)
            sent = run_script("ptk-qute-scrape", root)
            self.assertEqual(sent.returncode, 0, sent.stderr)
            self.assertEqual(sent.stdout, b"")
            self.assertEqual(exchange(config, {"op": "snapshot"})["snapshot"]["purpose"], "scrape")
            result = subprocess.run(driver_args(root), capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            yaml = (root / "openapi.yaml").read_text()
            postman = json.loads((root / "postman.json").read_text())
            for marker in ("/api/first", "requestBody:", "in: query", "x-prowsetk-schema:",
                           "login-evidence: true", "authentication-verified: false", "complete: false"):
                self.assertIn(marker, yaml)
            self.assertNotIn("private_marker", yaml)
            self.assertNotIn("private_marker", json.dumps(postman))
            self.assertFalse(postman["x-prowsetk-qutebrowser"]["response_probes"])
            self.assertEqual((root / "openapi.yaml").stat().st_mode & 0o777, 0o600)
            self.assertEqual((root / "postman.json").stat().st_mode & 0o777, 0o600)
            self.assertNotIn(b"private_marker", result.stdout + result.stderr)

    def test_unconfirmed_login_preserves_previous_exports(self):
        with broker() as (root, _):
            write_file(root / "openapi.yaml", b"previous export")
            sent = run_script("ptk-qute-scrape", root,
                              html="<script>var logout='Account';fetch('/api/must-not-export')</script>")
            self.assertEqual(sent.returncode, 0)
            result = subprocess.run(driver_args(root), capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((root / "openapi.yaml").read_bytes(), b"previous export")
            self.assertFalse((root / "postman.json").exists())
            self.assertNotIn(b"must-not-export", result.stdout + result.stderr)

    def test_wrong_origin_hint_mode_and_tab_rejected(self):
        with broker() as (root, config):
            write_file(root / "page.html", PAGE.encode())
            base_env = dict(os.environ, QUTE_MODE="command", QUTE_URL=BASE,
                            QUTE_TAB_INDEX="1", QUTE_HTML=str(root / "page.html"))
            for changes in ({"QUTE_URL": "https://other.test/private_marker"}, {"QUTE_MODE": "hints"}):
                result = subprocess.run([str(TOOLS / "ptk-qute-send"), "--bridge", str(root / "bridge.json")],
                                        env=dict(base_env, **changes), capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(b"private_marker", result.stdout + result.stderr)
            self.assertEqual(exchange(config, {"op": "status"})["revision"], 0)
            self.assertEqual(run_script("ptk-qute-send", root).returncode, 0)
            result = subprocess.run([str(TOOLS / "ptk-qute-send"), "--bridge", str(root / "bridge.json")],
                                    env=dict(base_env, QUTE_TAB_INDEX="2"), capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)

    def test_rejected_html_parse_finishes_the_conversation(self):
        with broker() as (root, config):
            # Within the bridge byte budget, beyond Flatworm's depth budget.
            self.assertEqual(run_script("ptk-qute-send", root, html="<div>" * 300).returncode, 0)
            result = subprocess.run(driver_args(root), capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(exchange(config, {"op": "status"})["closed"])
            self.assertFalse((root / "openapi.yaml").exists())

    def test_slow_client_does_not_block_and_live_socket_is_not_evicted(self):
        with broker() as (root, config):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as slow:
                slow.connect(config["socket"])
                slow.sendall(b'{"partial":')
                self.assertEqual(exchange(config, {"op": "status"})["revision"], 0)
            original_inode = (root / "bridge.sock").stat().st_ino
            second = subprocess.run([str(TOOLS / "ptk-qute-bridge"), "serve", "--directory", str(root),
                                     "--url", BASE], capture_output=True, timeout=5)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual((root / "bridge.sock").stat().st_ino, original_inode)
            with self.assertRaises(BridgeError):
                exchange(dict(config, token="1" * 64), {"op": "status"})

    def test_marionette_fresh_capture_roundtrip_with_real_lua(self):
        with broker() as (root, config):
            fifo = root / "fifo"
            os.mkfifo(fifo, 0o600)
            fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            write_file(root / "initial.html", PAGE.encode())
            write_file(root / "actions.json", encode({"version": 1, "actions": [
                {"type": "click", "selector": "button[data-next]"}]}))
            env = dict(os.environ, QUTE_MODE="command", QUTE_URL=BASE + "/", QUTE_TAB_INDEX="1",
                       QUTE_HTML=str(root / "initial.html"), QUTE_FIFO=str(fifo))
            controller = subprocess.Popen([str(TOOLS / "ptk-qute-marionette"), "--bridge",
                                            str(root / "bridge.json"), "--settle-ms", "0"],
                                           env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            driver = None
            try:
                # Long-poll for the initial real userscript capture, without sleeps.
                self.assertIsNotNone(exchange(config, {"op": "snapshot", "wait_ms": 3000})["snapshot"])
                driver = subprocess.Popen(driver_args(root, "--actions_file", str(root / "actions.json")),
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                pending, generated, callback = b"", None, False
                while not callback:
                    ready, _, _ = select.select([fd], [], [], 5)
                    self.assertTrue(ready, "marionette did not send FIFO command")
                    pending += os.read(fd, 16384)
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        command = line.decode()
                        self.assertNotIn("private_marker", command)
                        if command.startswith("jseval --quiet --world=main --file "):
                            path = command.split(" --file ", 1)[1]
                            self.assertEqual(Path(path).stat().st_mode & 0o777, 0o600)
                            code = Path(path).read_text()
                            generated = json.loads(code.split("const cfg = ", 1)[1].split(";\n", 1)[0])
                            self.assertEqual(generated["action"]["type"], "click")
                            self.assertEqual(generated["origin"], BASE)
                        elif command.startswith("spawn --userscript "):
                            self.assertIsNotNone(generated)
                            argv = shlex.split(command)[2:]
                            fresh = PAGE + "<script>fetch('/api/after-action')</script>" + (
                                '<meta name="prowsetk-qute-action" data-id="' + generated["id"] + '" content="ok">')
                            write_file(root / "fresh.html", fresh.encode())
                            result = subprocess.run(argv, env=dict(env, QUTE_HTML=str(root / "fresh.html")),
                                                    capture_output=True, timeout=5)
                            self.assertEqual(result.returncode, 0, result.stderr)
                            callback = True
                        else:
                            self.fail("unexpected FIFO command")
                stdout, stderr = driver.communicate(timeout=10)
                self.assertEqual(driver.returncode, 0, stderr)
                out, err = controller.communicate(timeout=5)
                self.assertEqual(controller.returncode, 0, err)
                self.assertEqual(out, b"")
                yaml = (root / "openapi.yaml").read_text()
                self.assertIn("/api/first", yaml)
                self.assertIn("/api/after-action", yaml)
                self.assertIn("actions-confirmed: 1", yaml)
                self.assertIn("snapshots: 2", yaml)
                self.assertEqual(list(root.glob(".action-*.js")), [])
            finally:
                for process in (controller, driver):
                    if process is not None and process.poll() is None:
                        process.terminate()
                        process.communicate(timeout=5)
                os.close(fd)

    def test_generated_javascript_native_setter_and_origin_guard(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js unavailable for JS simulation")
        with tempfile.TemporaryDirectory(prefix="q-", dir=os.getcwd()) as root:
            path = Path(root) / "action.js"
            write_file(path, action_source({"type": "fill", "selector": "input", "value": "'; injected=true; //"},
                                           BASE, "1" * 32))
            result = subprocess.run([node, str(Path(SOURCE) / "tests/integration/test_qutebrowser_actions.js"), str(path)],
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
