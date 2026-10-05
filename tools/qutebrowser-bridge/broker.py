"""One bounded assistant-browser conversation; page data is never logged."""

import argparse
import asyncio
import contextlib
import os
from pathlib import Path
import secrets
import signal
import socket
import struct
import subprocess
import sys

from protocol import (BridgeError, MAX_ACTIONS, MAX_HTML, MAX_MESSAGE, MAX_WAIT_MS,
                      VERSION, action, authorized, decode, descriptor, encode,
                      exchange, integer, keys, origin, private_directory, string,
                      write_file)


class Conversation:
    def __init__(self, allowed_origin, token, max_actions=MAX_ACTIONS):
        self.origin = allowed_origin
        self.token = token
        self.max_actions = integer(max_actions, 0, MAX_ACTIONS)
        self.changed = asyncio.Condition()
        self.closed = False
        self.revision = 0
        self.latest = None
        self.tab = None
        self.controller = None
        self.pending = None
        self.sent = False
        self.action_count = 0
        self.result = None

    async def wait_for(self, predicate, wait_ms):
        integer(wait_ms, 0, MAX_WAIT_MS)
        if not predicate() and wait_ms:
            try:
                await asyncio.wait_for(self.changed.wait_for(predicate), wait_ms / 1000)
            except asyncio.TimeoutError:
                pass
        return predicate()

    async def close(self):
        async with self.changed:
            self.closed = True
            self.changed.notify_all()

    async def handle(self, request):
        keys(request, ("version", "token", "op"),
             ("snapshot", "controller", "tab", "after", "wait_ms", "action"))
        if type(request["version"]) is not int or request["version"] != VERSION:
            raise BridgeError("unsupported protocol version")
        if not authorized(request["token"], self.token):
            raise BridgeError("bridge authentication failed")
        common = ("version", "token", "op")
        op = request["op"]
        async with self.changed:
            if op == "status":
                keys(request, common)
                return {"ok": True, "closed": self.closed, "revision": self.revision,
                        "connected": self.controller is not None,
                        "actions_used": self.action_count, "max_actions": self.max_actions}
            if op == "finish":
                keys(request, common)
                self.closed = True
                self.changed.notify_all()
                return {"ok": True}
            if self.closed and op == "next" and request.get("controller") == self.controller:
                keys(request, common + ("controller",), ("wait_ms",))
                return {"ok": True, "closed": True, "action": None}
            if self.closed:
                raise BridgeError("bridge conversation is closed")
            if op == "attach":
                keys(request, common + ("controller", "tab"))
                controller = string(request["controller"], 64)
                tab = integer(request["tab"], 0, 100000)
                if self.controller is not None or (self.tab is not None and self.tab != tab):
                    raise BridgeError("assistant tab is already connected")
                self.controller, self.tab = controller, tab
                self.changed.notify_all()
                return {"ok": True}
            if op == "detach":
                keys(request, common + ("controller",))
                if request["controller"] != self.controller:
                    raise BridgeError("invalid assistant controller")
                self.controller = None
                if self.pending is not None:
                    self.closed = True
                self.changed.notify_all()
                return {"ok": True}
            if op == "publish":
                keys(request, common + ("snapshot",))
                snap = request["snapshot"]
                keys(snap, ("url", "html", "tab", "purpose"), ("action_id", "action_status"))
                if origin(snap["url"]) != self.origin:
                    raise BridgeError("snapshot is outside the approved origin")
                string(snap["html"], MAX_HTML, empty=True)
                tab = integer(snap["tab"], 0, 100000)
                if self.tab is not None and self.tab != tab:
                    raise BridgeError("snapshot is from another tab index")
                if snap["purpose"] not in ("page", "scrape", "action"):
                    raise BridgeError("invalid snapshot purpose")
                if snap["purpose"] == "action":
                    keys(snap, ("url", "html", "tab", "purpose", "action_id", "action_status"))
                    if (not self.pending or not self.sent or
                            snap["action_id"] != self.pending["id"]):
                        raise BridgeError("unexpected action snapshot")
                    if snap["action_status"] not in ("ok", "error", "unconfirmed"):
                        raise BridgeError("invalid action result")
                elif "action_id" in snap or "action_status" in snap:
                    raise BridgeError("unexpected action fields")
                self.tab = tab
                self.revision += 1
                self.latest = dict(snap, revision=self.revision)
                if snap["purpose"] == "action":
                    self.result = self.latest
                self.changed.notify_all()
                return {"ok": True, "revision": self.revision}
            if op == "snapshot":
                keys(request, common, ("after", "wait_ms"))
                after = integer(request.get("after", 0), 0, 2**53 - 1)
                await self.wait_for(lambda: self.closed or self.revision > after,
                                    request.get("wait_ms", 0))
                if self.closed:
                    raise BridgeError("bridge conversation is closed")
                return {"ok": True, "snapshot": self.latest if self.revision > after else None}
            if op == "next":
                keys(request, common + ("controller",), ("wait_ms",))
                if request["controller"] != self.controller:
                    raise BridgeError("invalid assistant controller")
                await self.wait_for(lambda: self.closed or (self.pending is not None and not self.sent),
                                    request.get("wait_ms", 0))
                result = None
                if not self.closed and self.pending is not None and not self.sent:
                    result, self.sent = self.pending, True
                return {"ok": True, "closed": self.closed, "action": result}
            if op == "act":
                keys(request, common + ("action",), ("wait_ms",))
                approved = action(request["action"], self.origin)
                if self.controller is None or self.latest is None:
                    raise BridgeError("assistant marionette is not connected")
                if self.pending is not None:
                    raise BridgeError("an assistant action is already active")
                if self.action_count >= self.max_actions:
                    raise BridgeError("assistant action limit reached")
                wait_ms = integer(request.get("wait_ms", MAX_WAIT_MS), 1, MAX_WAIT_MS)
                self.action_count += 1
                self.pending = dict(approved, id=secrets.token_hex(16))
                self.sent, self.result = False, None
                self.changed.notify_all()
                await self.wait_for(lambda: self.closed or self.result is not None, wait_ms)
                result = self.result
                self.pending, self.result = None, None
                if result is None or self.closed:
                    # A dispatched action can have side effects even if its reply is
                    # lost. Stop the conversation instead of silently retrying it.
                    self.closed = True
                    self.changed.notify_all()
                    raise BridgeError("assistant action did not return a fresh snapshot")
                return {"ok": True, "snapshot": result}
            raise BridgeError("unsupported bridge operation")


async def serve(directory, url, timeout, max_actions, launch=False, browser="qutebrowser"):
    directory = private_directory(directory, create=True)
    socket_path, descriptor_path = directory / "bridge.sock", directory / "bridge.json"
    # Refuse an occupied path, including a stale socket. The owner may explicitly
    # remove stale runtime state; a second server must never evict a live one.
    if socket_path.exists() or socket_path.is_symlink() or descriptor_path.exists():
        raise BridgeError("bridge runtime is already occupied")
    token = secrets.token_hex(32)
    conversation = Conversation(origin(url), token, max_actions)
    clients = set()
    stop = asyncio.Event()

    async def client(reader, writer):
        task = asyncio.current_task()
        accepted = len(clients) < 8
        clients.add(task)
        try:
            if not accepted:
                return
            peer = writer.get_extra_info("socket")
            if hasattr(socket, "SO_PEERCRED"):
                _, uid, _ = struct.unpack("3i", peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                if uid != os.geteuid():
                    return
            try:
                # One request and reply per connection, including bounded long polls.
                line = await asyncio.wait_for(reader.readline(), 5)
                if not line.endswith(b"\n") or len(line) > MAX_MESSAGE + 1:
                    raise BridgeError("invalid bridge frame")
                reply = await conversation.handle(decode(line[:-1]))
            except (BridgeError, ValueError, TypeError, UnicodeError, asyncio.TimeoutError):
                reply = {"ok": False, "error": "bridge request rejected"}
            writer.write(encode(reply) + b"\n")
            await asyncio.wait_for(writer.drain(), 5)
        except (OSError, ConnectionError, asyncio.TimeoutError):
            pass
        finally:
            clients.discard(task)
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), 1)
            except (OSError, asyncio.TimeoutError):
                writer.transport.abort()

    old_mask = os.umask(0o077)
    try:
        server = await asyncio.start_unix_server(client, path=str(socket_path), limit=MAX_MESSAGE + 1)
    finally:
        os.umask(old_mask)
    os.chmod(socket_path, 0o600)
    socket_inode = socket_path.lstat().st_ino
    descriptor_inode = None
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    try:
        write_file(descriptor_path, encode({"version": VERSION, "socket": str(socket_path),
                                          "token": token, "origin": conversation.origin}))
        descriptor_inode = descriptor_path.lstat().st_ino
        if launch:
            # Commands/URL are distinct argv, and userinfo has already been rejected.
            # Discard browser diagnostics which could contain page or account values.
            subprocess.Popen([browser, "--untrusted-args", url], stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)
        print("qutebrowser bridge ready", flush=True)
        async with server:
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(stop.wait(), timeout)
    finally:
        server.close()
        await server.wait_closed()
        await conversation.close()
        for task in list(clients):
            task.cancel()
        if clients:
            await asyncio.gather(*list(clients), return_exceptions=True)
        for path, inode in ((socket_path, socket_inode), (descriptor_path, descriptor_inode)):
            with contextlib.suppress(FileNotFoundError):
                if inode is not None and path.lstat().st_ino == inode:
                    path.unlink()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.remove_signal_handler(sig)


def main():
    parser = argparse.ArgumentParser(description="Local Qutebrowser assistant-browser bridge")
    sub = parser.add_subparsers(dest="command", required=True)
    server = sub.add_parser("serve", help="start a bounded owner-only bridge")
    server.add_argument("--directory", required=True)
    server.add_argument("--url", required=True)
    server.add_argument("--timeout", type=int, default=300, help="lifetime in seconds (1..3600)")
    server.add_argument("--max-actions", type=int, default=MAX_ACTIONS)
    server.add_argument("--launch", action="store_true", help="open the URL in Qutebrowser")
    server.add_argument("--browser", default="qutebrowser", help="Qutebrowser executable")
    for command in ("status", "finish"):
        control = sub.add_parser(command)
        control.add_argument("--bridge", required=True, help="private bridge.json descriptor")
    args = parser.parse_args()
    try:
        if args.command == "serve":
            integer(args.timeout, 1, 3600)
            if args.launch:
                sys.stderr.write("Open the configured page in Qutebrowser? [y/N] ")
                if sys.stdin.readline().strip().lower() not in ("y", "yes"):
                    raise BridgeError("assistant-browser handoff cancelled")
            asyncio.run(serve(args.directory, args.url, args.timeout, args.max_actions,
                              args.launch, args.browser))
        else:
            reply = exchange(descriptor(args.bridge), {"op": args.command})
            print(encode(reply).decode("ascii"))
        return 0
    except (BridgeError, OSError, ValueError, UnicodeError):
        sys.stderr.write("qutebrowser bridge: operation failed\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
