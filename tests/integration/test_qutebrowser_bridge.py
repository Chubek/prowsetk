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
import time
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
            self.assertIn(b"login evidence failed", result.stderr)
            self.assertIn(b"--success-selector", result.stderr)

    def test_already_logged_in_semantic_controls_need_no_selector_override(self):
        controls = [
            "<a href='/hotel/hoteladmin/sign_out.html?token=private_marker'>Exit</a>",
            "<a href='/account/LogOut'>Exit</a>",
            "<button><span> Log out </span></button>",
            "<button aria-label='Sign out'><svg></svg></button>",
            "<form action='/account/logoff.php'><button>Exit</button></form>",
            "<input type='submit' value='Sign out'>",
        ]
        for control in controls:
            with self.subTest(control=control), broker() as (root, _):
                self.assertEqual(run_script("ptk-qute-scrape", root, html=control +
                    "<script>fetch('/api/already-logged-in?token=private_marker')</script>").returncode, 0)
                result = subprocess.run(driver_args(root), capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                yaml = (root / "openapi.yaml").read_text()
                self.assertIn("login-evidence: true", yaml)
                self.assertIn("authentication-verified: false", yaml)
                self.assertIn("/api/already-logged-in", yaml)
                self.assertNotIn("private_marker", yaml)
                self.assertNotIn(b"private_marker", result.stdout + result.stderr)

    def test_implicit_login_evidence_excludes_scripts_templates_and_query_values(self):
        pages = [
            "<button><script>Log out</script></button>",
            "<template><button>Sign out</button></template>",
            "<p>Log out</p><h1>Account</h1>",
            "<a href='/help?redirect=logout'>Help</a>",
            "<input type='hidden' value='Sign out' href='/logout'>",
        ]
        for page in pages:
            with self.subTest(page=page), broker() as (root, _):
                write_file(root / "openapi.yaml", b"previous export")
                self.assertEqual(run_script("ptk-qute-scrape", root, html=page).returncode, 0)
                result = subprocess.run(driver_args(root), capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"login evidence failed", result.stderr)
                self.assertEqual((root / "openapi.yaml").read_bytes(), b"previous export")

    def test_explicit_login_beacons_do_not_fall_back_to_implicit_evidence(self):
        for flag, beacon in (("--success_selector", "#missing"), ("--success_xpath", "//h1[@id='missing']")):
            with self.subTest(flag=flag), broker() as (root, _):
                self.assertEqual(run_script("ptk-qute-scrape", root).returncode, 0)
                result = subprocess.run(driver_args(root, flag, beacon), capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"login evidence failed", result.stderr)

    def test_diagnostics_distinguish_module_beacon_and_output_failures_without_values(self):
        cases = [
            (["--ipc_module", "/missing/private_marker.so"], b"IPC module loading failed"),
            (["--success_selector", ":private_marker"], b"login beacon validation failed"),
            (["--output", None], b"OpenAPI export failed"),
        ]
        for extra, marker in cases:
            with self.subTest(marker=marker), broker() as (root, _):
                self.assertEqual(run_script("ptk-qute-scrape", root).returncode, 0)
                options = [str(root / "page.html/private_marker.yaml") if value is None else value for value in extra]
                result = subprocess.run(driver_args(root, *options), capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(marker, result.stderr)
                self.assertNotIn(b"private_marker", result.stdout + result.stderr)

    def test_missing_evidence_after_a_dispatched_action_cannot_be_retried(self):
        with broker() as (root, config):
            self.assertEqual(run_script("ptk-qute-scrape", root).returncode, 0)
            controller = "fixture-controller"
            exchange(config, {"op": "attach", "controller": controller, "tab": 1})
            write_file(root / "actions.json", encode({"version": 1, "actions": [
                {"type": "click", "selector": "button[data-next]"}]}))
            process = subprocess.Popen(driver_args(root, "--retry_login_evidence", "true",
                "--actions_file", str(root / "actions.json")), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                action = exchange(config, {"op": "next", "controller": controller, "wait_ms": 3000})["action"]
                self.assertIsNotNone(action)
                exchange(config, {"op": "publish", "snapshot": {
                    "url": BASE + "/", "html": "<p>private_marker</p>", "tab": 1,
                    "purpose": "action", "action_id": action["id"], "action_status": "ok"}})
                stdout, stderr = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 1, stderr)
                self.assertIn(b"login evidence failed", stderr)
                self.assertNotIn(b"private_marker", stdout + stderr)
                status = exchange(config, {"op": "status"})
                self.assertTrue(status["closed"])
                self.assertEqual(status["actions_used"], 1)
                self.assertFalse((root / "openapi.yaml").exists())
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=5)

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
            self.assertIn(b"snapshot parsing failed", result.stderr)
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
            for kind in ('fill', 'select', 'check', 'focus', 'scroll', 'submit', 'click', 'capture'):
                order = {'type': kind}
                if kind != 'capture': order['selector'] = 'input'
                if kind in ('fill', 'select'): order['value'] = "'; injected=true; //"
                if kind == 'check': order['checked'] = True
                write_file(path, action_source(order, BASE, '1' * 32))
                result = subprocess.run([node, str(Path(SOURCE) / 'tests/integration/test_qutebrowser_actions.js'),
                    str(path), kind], capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_expect_waits_for_a_corrected_authenticated_snapshot_and_reports_final_status(self):
        expect = shutil.which("expect")
        if not expect:
            self.skipTest("Expect unavailable")
        for two_way in (False, True):
            with self.subTest(two_way=two_way):
                self.expect_corrected_snapshot(expect, two_way)

    def test_launcher_forwards_settle_delay_and_api_only_to_its_downstream(self):
        expect = shutil.which("expect")
        if not expect:
            self.skipTest("Expect unavailable")
        script = Path(SOURCE) / "examples/booking-dotcom-admin-api/qute-assist.exp"
        for api_only, keeps_noise in ((None, False), ("false", True)):
            with self.subTest(api_only=api_only):
                with tempfile.TemporaryDirectory(prefix="q-", dir=os.getcwd()) as directory:
                    root = Path(directory)
                    command = [expect, str(script), "--one-shot", "--directory", str(root),
                               "--timeout", "120", "--cli-bin", CLI,
                               "--output", str(root / "openapi.yaml"),
                               "--postman", str(root / "postman.json")]
                    if api_only:
                        command.extend(["--api-only", api_only])
                    process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
                    captured = bytearray()

                    def until(marker):
                        deadline = time.monotonic() + 20
                        while marker not in captured:
                            ready, _, _ = select.select([process.stdout], [], [],
                                                        max(0, deadline - time.monotonic()))
                            self.assertTrue(ready, "launcher stalled: " + repr(bytes(captured)))
                            data = os.read(process.stdout.fileno(), 16384)
                            self.assertTrue(data, "launcher exited early: " + repr(bytes(captured)))
                            captured.extend(data)

                    try:
                        until(b"take your time logging in")
                        page = ("<a href='/logout'>Sign out</a>"
                                "<script>fetch('/api/first?limit=2');"
                                "fetch('/js_errors', {method:'POST'});</script>")
                        self.assertEqual(run_script("ptk-qute-scrape", root, html=page).returncode, 0)
                        stdout, stderr = process.communicate(timeout=30)
                        captured.extend(stdout)
                        self.assertEqual(process.returncode, 0, (bytes(captured), stderr))
                        self.assertIn(b"specs exported", captured)
                        self.assertNotIn(b"private_marker", captured + stderr)
                        yaml = (root / "openapi.yaml").read_text()
                        self.assertIn("/api/first", yaml)
                        self.assertEqual("/js_errors" in yaml, keeps_noise)
                    finally:
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.communicate(timeout=5)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.communicate(timeout=5)

    def test_launcher_prints_the_requested_marionette_settle_delay(self):
        expect = shutil.which("expect")
        if not expect:
            self.skipTest("Expect unavailable")
        with tempfile.TemporaryDirectory(prefix="q-", dir=os.getcwd()) as root:
            root = Path(root)
            write_file(root / "actions.json", encode({"version": 1, "actions": []}))
            script = Path(SOURCE) / "examples/booking-dotcom-admin-api/qute-assist.exp"
            process = subprocess.Popen([expect, str(script), "--directory", str(root),
                                        "--timeout", "120", "--cli-bin", CLI,
                                        "--settle-ms", "1500",
                                        "--actions-file", str(root / "actions.json")],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, bufsize=0)
            captured = bytearray()
            try:
                deadline = time.monotonic() + 20
                while b"--settle-ms 1500" not in captured and time.monotonic() < deadline:
                    ready, _, _ = select.select([process.stdout], [], [], 1)
                    if not ready:
                        continue
                    data = os.read(process.stdout.fileno(), 16384)
                    if not data:
                        break
                    captured.extend(data)
                self.assertIn(b"--settle-ms 1500", captured)
            finally:
                process.terminate()
                try:
                    process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)

    def expect_corrected_snapshot(self, expect, two_way):
        with tempfile.TemporaryDirectory(prefix="q-", dir=os.getcwd()) as directory:
            root = Path(directory)
            script = Path(SOURCE) / "examples/booking-dotcom-admin-api/qute-assist.exp"
            command = [expect, str(script), "--one-shot", "--directory", str(root), "--timeout", "120",
                "--cli-bin", CLI, "--output", str(root / "openapi.yaml"),
                "--postman", str(root / "postman.json")]
            if two_way:
                write_file(root / "actions.json", encode({"version": 1, "actions": []}))
                command.extend(["--actions-file", str(root / "actions.json")])
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            captured = bytearray()

            def until(marker):
                deadline = time.monotonic() + 15
                while marker not in captured:
                    ready, _, _ = select.select([process.stdout], [], [], max(0, deadline - time.monotonic()))
                    self.assertTrue(ready, "Expect did not report its stage: " + repr(bytes(captured)))
                    data = os.read(process.stdout.fileno(), 16384)
                    self.assertTrue(data, "Expect exited before the expected stage: " + repr(bytes(captured)))
                    captured.extend(data)

            try:
                until(b"take your time logging in")
                config = descriptor(root / "bridge.json")
                if two_way:
                    exchange(config, {"op": "attach", "controller": "fixture-controller", "tab": 1})
                self.assertEqual(run_script("ptk-qute-scrape", root, html="<p>private_marker</p>").returncode, 0)
                until(b"keeping the bridge open")
                self.assertIn(b"login evidence failed", captured)
                self.assertFalse(exchange(config, {"op": "status"})["closed"])
                if two_way:
                    self.assertTrue(exchange(config, {"op": "status"})["connected"])
                self.assertFalse((root / "openapi.yaml").exists())
                page = "<button><span>Sign out</span></button>" + PAGE
                self.assertEqual(run_script("ptk-qute-scrape", root, html=page).returncode, 0)
                stdout, stderr = process.communicate(timeout=15)
                captured.extend(stdout)
                self.assertEqual(process.returncode, 0, (bytes(captured), stderr))
                self.assertIn(b"specs exported", captured)
                self.assertNotIn(b"private_marker", captured + stderr)
                self.assertIn("login-evidence: true", (root / "openapi.yaml").read_text())
                self.assertFalse((root / "bridge.sock").exists())
                self.assertFalse((root / "bridge.json").exists())
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main()
