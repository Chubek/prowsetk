"""Headless protocol/policy tests with an injected asyncio conversation."""
import asyncio
import os
from pathlib import Path
import sys
import tempfile
import struct
import unittest
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, sys.argv.pop(1))
from broker import Conversation
from protocol import (BridgeError, MAX_HTML, action, decode, descriptor, encode,
                      origin, private_directory, read_file, write_file)
from userscript import action_source, command_path
from bulk import MAGIC, create_fifo, fifo_path, receive_fifo, send_fifo

TOKEN = "0" * 64
ORIGIN = "https://example.test"


class Protocol(unittest.TestCase):
    def test_fifo_stream_above_old_limit_and_private_path_policy(self):
        with tempfile.TemporaryDirectory(dir=os.getcwd()) as directory:
            path = create_fifo(directory)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            body = b'"\\\n\x00' * (1024 * 1024 + 1)
            with ThreadPoolExecutor(max_workers=1) as pool:
                writer = pool.submit(send_fifo, path, body)
                self.assertEqual(receive_fifo(path, len(body)), body)
                writer.result(timeout=2)
            for name in ('../escape.fifo', 'bulk-wrong.fifo', '/absolute.fifo'):
                with self.assertRaises(BridgeError):
                    fifo_path(directory, name)
            path.unlink()
            path.symlink_to('/dev/null')
            with self.assertRaises(OSError):
                receive_fifo(path, 1, timeout=0.1)

    def test_fifo_truncation_extra_bytes_framing_and_stalled_peer(self):
        def raw_write(path, frame):
            fd = os.open(path, os.O_WRONLY)
            try:
                os.write(fd, frame)
            finally:
                os.close(fd)
        frames = [b'bad frame', MAGIC + struct.pack('!Q', 3) + b'ab',
                  MAGIC + struct.pack('!Q', 3) + b'abcd', MAGIC + struct.pack('!Q', 4) + b'abc']
        for frame in frames:
            with tempfile.TemporaryDirectory(dir=os.getcwd()) as directory:
                path = create_fifo(directory)
                with ThreadPoolExecutor(max_workers=1) as pool:
                    sender = pool.submit(raw_write, path, frame)
                    with self.assertRaises(BridgeError):
                        receive_fifo(path, 3, timeout=1)
                    sender.result(timeout=2)
        with tempfile.TemporaryDirectory(dir=os.getcwd()) as directory:
            path = create_fifo(directory)
            with self.assertRaises(BridgeError):
                receive_fifo(path, 3, timeout=0.01)
            with self.assertRaises(BridgeError):
                send_fifo(path, b'abc', timeout=0.01)
            os.chmod(path, 0o644)
            with self.assertRaises(BridgeError):
                receive_fifo(path, 3, timeout=0.01)

    def test_strict_json_and_bounds(self):
        for value in (b'{"op":"next","op":"act"}', b'{"x":NaN}', b'{"x":1e999}', b'{"x":1}junk',
                      b'"\xff"', b'[' * 33 + b']' * 33):
            with self.assertRaises(BridgeError):
                decode(value)
        self.assertEqual(decode(encode({"text": "\u00e9\U0001f600\n", "value": False})),
                         {"text": "\u00e9\U0001f600\n", "value": False})

    def test_origin_and_fixed_action_policy(self):
        self.assertEqual(origin("https://EXAMPLE.test:443/path?q=1"), ORIGIN)
        self.assertEqual(origin("http://[::1]:8123/"), "http://[::1]:8123")
        for url in ("file:///tmp/a", "https://user:password@example.test/", "https://x:0/",
                    "https://x:65536/", "https://x/\n", "https://x\\@example.test/"):
            with self.assertRaises(BridgeError):
                origin(url)
        for value in ({"type": "evaluate", "value": "alert(1)"},
                      {"type": "click", "selector": "a", "command": "spawn"},
                      {"type": "navigate", "url": "https://other.test/"},
                      {"type": "fill", "selector": "input", "value": "x" * 4097}):
            with self.assertRaises(BridgeError):
                action(value, ORIGIN)
        for value in ({'type': 'check', 'selector': 'input', 'checked': 'true'},
                      {'type': 'capture', 'selector': 'body'}, {'type': 'scroll', 'selector': ''}):
            with self.assertRaises(BridgeError):
                action(value, ORIGIN)
        for kind in ('focus', 'scroll', 'submit'):
            self.assertEqual(action({'type': kind, 'selector': 'button'}, ORIGIN)['type'], kind)
        self.assertEqual(action({'type': 'capture'}, ORIGIN), {'type': 'capture'})

    def test_private_files_and_descriptor(self):
        with tempfile.TemporaryDirectory(prefix="q-", dir=os.getcwd()) as root:
            path = Path(root)
            private_directory(path)
            value = {"version": 1, "socket": str(path / "bridge.sock"), "token": TOKEN, "origin": ORIGIN}
            write_file(path / "bridge.json", encode(value))
            self.assertEqual(descriptor(path / "bridge.json"), value)
            self.assertEqual((path / "bridge.json").stat().st_mode & 0o777, 0o600)
            # A checkout may be reached through a symlinked parent. Neither the
            # descriptor nor its socket must depend on the caller's spelling.
            with tempfile.TemporaryDirectory(prefix="qa-", dir=os.getcwd()) as aliases:
                alias = Path(aliases) / "checkout"
                alias.symlink_to(path.parent, target_is_directory=True)
                self.assertEqual(descriptor(alias / path.name / "bridge.json"), value)
                wrong = dict(value, socket=str(path.parent / "foreign.sock"))
                write_file(path / "wrong.json", encode(wrong))
                with self.assertRaises(BridgeError):
                    descriptor(path / "wrong.json")
            os.chmod(path / "bridge.json", 0o644)
            with self.assertRaises(BridgeError):
                descriptor(path / "bridge.json")
            (path / "link").symlink_to(path / "bridge.json")
            with self.assertRaises(OSError):
                read_file(path / "link")
            os.chmod(path, 0o755)
            with self.assertRaises(BridgeError):
                private_directory(path)

    def test_action_data_never_becomes_command_syntax(self):
        data = "'; globalThis.injected=true; //\n;; spawn evil"
        source = action_source({"type": "fill", "selector": "input", "value": data}, ORIGIN, "1" * 32)
        self.assertIn(b"\\n;; spawn evil", source)
        self.assertIn(b"location.origin !== cfg.origin", source)
        for path in ("/x/;;spawn", "/x/{url}", "/x/\nspawn"):
            with self.assertRaises(BridgeError):
                command_path(path)


class Session(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.session = Conversation(ORIGIN, TOKEN, max_actions=1)

    async def call(self, **request):
        return await self.session.handle(dict(version=1, token=TOKEN, **request))

    async def publish(self, **fields):
        return await self.call(op="publish", snapshot=dict(url=ORIGIN + "/", html="<h1>page</h1>",
                                                           tab=1, purpose="page", **fields))

    async def test_authentication_schema_and_capture_scope(self):
        for token in ("wrong", "\u2603" * 64, None):
            with self.assertRaises(BridgeError):
                await self.session.handle({"version": 1, "token": token, "op": "status"})
        with self.assertRaises(BridgeError):
            await self.call(op="status", snapshot={})
        for snap in ({"url": "https://other.test", "html": "x", "tab": 1, "purpose": "page"},
                     {"url": ORIGIN, "html": "x" * (MAX_HTML + 1), "tab": 1, "purpose": "page"}):
            with self.assertRaises(BridgeError):
                await self.call(op="publish", snapshot=snap)
        await self.publish()
        with self.assertRaises(BridgeError):
            await self.call(op="publish", snapshot={"url": ORIGIN, "html": "x", "tab": 2, "purpose": "page"})
        self.assertEqual((await self.call(op="snapshot", after=1, wait_ms=0))["snapshot"], None)

    async def test_fresh_action_roundtrip_and_budget(self):
        await self.call(op="attach", controller="test", tab=1)
        await self.publish()
        with self.assertRaises(BridgeError):
            await self.call(op="attach", controller="second", tab=1)
        turn = asyncio.create_task(self.call(op="act", action={"type": "click", "selector": "button"}, wait_ms=1000))
        delivery = await self.call(op="next", controller="test", wait_ms=1000)
        self.assertIsNotNone(delivery["action"])
        action_id = delivery["action"]["id"]
        # A normal capture cannot acknowledge an action.
        await self.publish()
        self.assertFalse(turn.done())
        await self.call(op="publish", snapshot={"url": ORIGIN + "/new", "html": "<h2>new</h2>", "tab": 1,
                                                "purpose": "action", "action_id": action_id, "action_status": "ok"})
        result = (await turn)["snapshot"]
        self.assertEqual(result["revision"], 3)
        self.assertEqual(result["html"], "<h2>new</h2>")
        with self.assertRaises(BridgeError):
            await self.call(op="act", action={"type": "reload"}, wait_ms=1)
        await self.call(op="finish")
        self.assertTrue((await self.call(op="next", controller="test"))["closed"])

    async def test_timeout_closes_conversation_without_retry(self):
        await self.call(op="attach", controller="test", tab=1)
        await self.publish()
        with self.assertRaises(BridgeError):
            await self.call(op="act", action={"type": "reload"}, wait_ms=1)
        self.assertTrue((await self.call(op="status"))["closed"])
        with self.assertRaises(BridgeError):
            await self.publish()


if __name__ == "__main__":
    unittest.main()
