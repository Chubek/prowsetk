"""Actual Replxx/Lua host, FIFO bulk transfer and a display-free assistant peer."""
import contextlib
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import termios
import time
import unittest
from concurrent.futures import ThreadPoolExecutor

REPL, IPC, SOURCE = sys.argv[1:4]
del sys.argv[1:4]
TOOLS = Path(SOURCE) / 'tools/qutebrowser-bridge'
sys.path.insert(0, str(TOOLS))
from protocol import descriptor, exchange, write_file
from bulk import create_fifo, publish_snapshot, send_fifo

BASE = 'https://admin.booking.com'
PAGE = """<a href='/logout'>Log out</a><button data-next>Next</button>
<form action='/api/first' method='post'><input name='customer' value='private_marker'>
<input name='count' type='number' required value='2'></form>
<script>fetch('/api/first?limit=2&token=private_marker'); fetch('/js_errors', {method:'POST'});</script>"""


@contextlib.contextmanager
def broker():
    with tempfile.TemporaryDirectory(prefix='qr-', dir=os.getcwd()) as directory:
        root = Path(directory)
        process = subprocess.Popen([str(TOOLS / 'ptk-qute-bridge'), 'serve', '--directory', str(root),
            '--url', BASE + '/', '--timeout', '60'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready, _, _ = select.select([process.stdout], [], [], 5)
            if not ready or process.stdout.readline() != b'qutebrowser bridge ready\n':
                raise AssertionError('broker not ready')
            yield root, descriptor(root / 'bridge.json')
        finally:
            process.terminate()
            _, stderr = process.communicate(timeout=15)
            assert process.returncode == 0 and not stderr, stderr
            assert not list(root.glob('bulk-*.fifo'))


def publish(config, html=PAGE, **extra):
    snapshot = dict(url=BASE + '/', html=html, tab=1, purpose='page')
    snapshot.update(extra)
    return publish_snapshot(config, snapshot)


def command(root):
    return [REPL, '--bridge', str(root / 'bridge.json'), '--ipc-module', IPC, '--require-login',
        '--output', str(root / 'openapi.yaml'), '--postman', str(root / 'postman.json'), '--wait-ms', '3000']


class Repl(unittest.TestCase):
    def test_native_fifo_rejects_mismatched_frames_without_peer_values(self):
        with broker() as (root, _):
            path = create_fifo(root)
            lua = root / 'check.lua'
            write_file(lua, ("local value, err = qute.client.ipc.read_fifo('" + str(path) + "', 3)\n"
                "assert(value == nil and err == 'qutebrowser bridge operation failed')\n").encode())
            with ThreadPoolExecutor(max_workers=1) as pool:
                peer = pool.submit(send_fifo, path, b'private_marker')
                result = subprocess.run(command(root) + ['--script', str(lua)], capture_output=True, timeout=15)
                # A rejected header may close the pipe while its writer finishes.
                try:
                    peer.result(timeout=5)
                except BrokenPipeError:
                    pass
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(b'private_marker', result.stdout + result.stderr)
            path.unlink()

    def test_large_escaping_heavy_snapshot_over_both_fifo_hops_and_persistent_lua(self):
        with broker() as (root, config):
            page = '<!--' + '"\\' * (3 * 1024 * 1024) + '-->' + PAGE
            write_file(root / 'page.html', page.encode())
            env = dict(os.environ, QUTE_MODE='command', QUTE_URL=BASE + '/', QUTE_TAB_INDEX='1',
                QUTE_HTML=str(root / 'page.html'))
            sent = subprocess.run([str(TOOLS / 'ptk-qute-scrape'), '--bridge', str(root / 'bridge.json')],
                env=env, capture_output=True, timeout=15)
            self.assertEqual(sent.returncode, 0, sent.stderr)
            script = '''retained = 7
function answer(x)
  return x + retained
end
assert(answer(3) == 10)
:targets
:endpoints
:export
:quit
'''
            result = subprocess.run(command(root) + ['--batch'], input=script, text=True,
                capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
            self.assertIn('specs exported', result.stdout)
            self.assertNotIn('private_marker', result.stdout + result.stderr)
            yaml = (root / 'openapi.yaml').read_text()
            self.assertIn('/api/first', yaml)
            self.assertNotIn('/js_errors', yaml)
            self.assertNotIn('private_marker', yaml)
            self.assertTrue(exchange(config, {'op': 'status'})['closed'])

    def test_actions_accumulate_schema_and_reject_stale_numbered_targets(self):
        with broker() as (root, config):
            exchange(config, {'op': 'attach', 'controller': 'fixture', 'tab': 1})
            publish(config)

            def assistant():
                orders = []
                for _ in range(8):
                    action = exchange(config, {'op': 'next', 'controller': 'fixture', 'wait_ms': 3000})['action']
                    self.assertIsNotNone(action)
                    orders.append(action['type'])
                    page = "<a href='/logout'>Exit</a><form action='/api/second' method='post'>" \
                        "<input name='arrival' type='date' required value='private_marker'></form>" \
                        "<form action='/api/first' method='post'><input name='locale'></form>" \
                        "<script>fetch('/api/first?offset=3')</script>"
                    publish_snapshot(config, dict(url=BASE + '/second', html=page, tab=1, purpose='action',
                        action_id=action['id'], action_status='ok'))
                return orders

            script = '''targets = qute:targets()
assert(#targets > 0)
qute:click(2)
assert(not pcall(function() qute:click(2) end))
qute:fill("input", "date")
qute:select("select", "b")
qute:check("input", true)
qute:focus("input")
qute:scroll("form")
qute:submit("form")
qute:capture()
:export
:quit
'''
            with ThreadPoolExecutor(max_workers=1) as pool:
                peer = pool.submit(assistant)
                result = subprocess.run(command(root) + ['--batch'], input=script, text=True,
                    capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
                self.assertEqual(peer.result(timeout=5), ['click', 'fill', 'select', 'check', 'focus', 'scroll', 'submit', 'capture'])
            yaml = (root / 'openapi.yaml').read_text()
            self.assertIn('/api/first', yaml)
            self.assertIn('/api/second', yaml)
            self.assertIn('customer', yaml)
            self.assertIn('arrival', yaml)
            self.assertIn('locale', yaml)
            self.assertIn("name: 'offset'", yaml)
            self.assertEqual(yaml.count('\n    get:'), 1)
            self.assertEqual(len(json.loads((root / 'postman.json').read_text())['item']), 3)
            self.assertNotIn('private_marker', yaml + result.stdout + result.stderr)

    def test_empty_noise_only_export_preserves_previous_artifacts_and_errors_hide_values(self):
        with broker() as (root, config):
            publish(config, "<button>Log out</button><script>fetch('/js_errors', {method:'POST'})</script>")
            write_file(root / 'openapi.yaml', b'previous export')
            result = subprocess.run(command(root) + ['--batch'], input=":export\nerror('private_marker')\n:quit\n",
                text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertIn('export validation failed', result.stderr)
            self.assertNotIn('private_marker', result.stdout + result.stderr)
            self.assertEqual((root / 'openapi.yaml').read_bytes(), b'previous export')

    def test_rejected_action_capture_invalidates_targets_and_can_recover_with_fresh_html(self):
        with broker() as (root, config):
            exchange(config, {'op': 'attach', 'controller': 'fixture', 'tab': 1})
            publish(config)
            write_file(root / 'openapi.yaml', b'previous export')

            def assistant():
                for expected, page in (
                    ('click', '<div>' * 257 + '<a href="/logout">Log out</a>' + '</div>' * 257),
                    ('capture', PAGE),
                ):
                    order = exchange(config, {'op': 'next', 'controller': 'fixture', 'wait_ms': 3000})['action']
                    self.assertEqual(order['type'], expected)
                    publish(config, page, purpose='action', action_id=order['id'], action_status='ok')

            script = '''qute:targets()
assert(not pcall(function() qute:click(2) end))
assert(qute.current == false and qute.confirmed == false)
assert(not pcall(function() qute:click(2) end))
assert(qute:beacon() == false)
assert(not pcall(function() qute:export() end))
qute:capture()
assert(qute.current and qute.confirmed)
:export
:quit
'''
            with ThreadPoolExecutor(max_workers=1) as pool:
                peer = pool.submit(assistant)
                result = subprocess.run(command(root) + ['--batch'], input=script, text=True,
                    capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
                peer.result(timeout=5)
            self.assertIn('/api/first', (root / 'openapi.yaml').read_text())
            self.assertNotIn('private_marker', result.stdout + result.stderr)

    def test_replxx_terminal_completion_and_multiline_editing(self):
        with broker() as (root, config):
            publish(config)
            master, slave = pty.openpty()
            process = subprocess.Popen(command(root), stdin=slave, stdout=slave, stderr=slave,
                env=dict(os.environ, TERM='xterm'))
            os.close(slave)
            output = bytearray()

            def until(marker):
                deadline = time.monotonic() + 10
                while marker not in output:
                    ready, _, _ = select.select([master], [], [], max(0, deadline - time.monotonic()))
                    self.assertTrue(ready, repr(bytes(output)))
                    chunk = os.read(master, 65536)
                    self.assertTrue(chunk)
                    output.extend(chunk)
                    # Replxx asks the terminal for its cursor position.
                    if b'\x1b[6n' in chunk:
                        os.write(master, b'\x1b[1;1R')
            try:
                until(b'qute> ')
                output.clear()
                os.write(master, b':sta\t\r')
                until(b'actions_used')
                output.clear()
                os.write(master, b'function value()\r')
                until(b' ...> ')
                os.write(master, b'return 42\rend\r')
                output.clear()
                os.write(master, b'assert(value() == 42)\r:quit\r')
                process.wait(timeout=10)
                self.assertEqual(process.returncode, 0)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                os.close(master)

    def test_expect_launcher_enters_the_repl_and_reaps_its_broker(self):
        expect = shutil.which('expect')
        if not expect:
            self.skipTest('Expect unavailable')
        with tempfile.TemporaryDirectory(prefix='qr-', dir=os.getcwd()) as directory:
            root = Path(directory)
            master, slave = pty.openpty()
            launcher = Path(SOURCE) / 'examples/booking-dotcom-admin-api/qute-assist.exp'
            def controlling_terminal():
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            process = subprocess.Popen([expect, str(launcher), '--directory', str(root), '--timeout', '60',
                '--repl-bin', REPL, '--output', str(root / 'openapi.yaml'),
                '--postman', str(root / 'postman.json')], stdin=slave, stdout=slave, stderr=slave,
                env=dict(os.environ, TERM='xterm'), preexec_fn=controlling_terminal)
            os.close(slave)
            output = bytearray()

            def until(marker):
                deadline = time.monotonic() + 15
                while marker not in output:
                    ready, _, _ = select.select([master], [], [], max(0, deadline - time.monotonic()))
                    self.assertTrue(ready, repr(bytes(output)))
                    data = os.read(master, 65536)
                    self.assertTrue(data)
                    output.extend(data)
                    if b'\x1b[6n' in data:
                        os.write(master, b'\x1b[1;1R')
            try:
                until(b'qute> ')
                self.assertIn(b'ptk-qute-marionette', output)
                config = descriptor(root / 'bridge.json')
                publish(config)
                output.clear()
                os.write(master, b':capture\r')
                until(b'login-evidence=true')
                output.clear()
                os.write(master, b':export\r')
                until(b'specs exported')
                os.write(master, b':quit\r')
                process.wait(timeout=10)
                self.assertEqual(process.returncode, 0)
                self.assertFalse((root / 'bridge.json').exists())
                self.assertFalse((root / 'bridge.sock').exists())
                self.assertFalse(list(root.glob('bulk-*.fifo')))
                self.assertFalse(list(root.glob('*history*')))
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
                os.close(master)


if __name__ == '__main__':
    unittest.main()
