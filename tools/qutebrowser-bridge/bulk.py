"""Private, length-framed FIFO bodies; control messages remain small JSON."""
import contextlib
import errno
import os
from pathlib import Path
import secrets
import select
import stat
import struct
import time
from concurrent.futures import ThreadPoolExecutor

from protocol import BridgeError, MAX_HTML, exchange, integer, private_directory

MAGIC = b"QHTML1\n"
HEADER_SIZE = len(MAGIC) + 8
TRANSFER_SECONDS = 10


def fifo_path(directory, name):
    root = private_directory(directory)
    if (not isinstance(name, str) or not name.startswith("bulk-") or not name.endswith(".fifo") or
            len(name) != 42 or any(c not in "0123456789abcdef" for c in name[5:-5])):
        raise BridgeError("invalid FIFO transfer")
    return root / name


def create_fifo(directory):
    path = fifo_path(directory, "bulk-" + secrets.token_hex(16) + ".fifo")
    os.mkfifo(path, 0o600)
    return path


def _open(path, flags):
    fd = os.open(path, flags | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if not stat.S_ISFIFO(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise BridgeError("invalid private FIFO")
        return fd
    except BaseException:
        os.close(fd)
        raise


def _ready(fd, event, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise BridgeError("FIFO transfer timed out")
    poll = select.poll()
    poll.register(fd, event)
    if not poll.poll(max(1, int(remaining * 1000))):
        raise BridgeError("FIFO transfer timed out")


def receive_fifo(path, size, timeout=TRANSFER_SECONDS):
    integer(size, 0, MAX_HTML)
    path = fifo_path(Path(path).parent, Path(path).name)
    deadline = time.monotonic() + timeout
    fd = _open(path, os.O_RDONLY)
    try:
        data = bytearray()
        total = HEADER_SIZE + size
        while True:
            _ready(fd, select.POLLIN, deadline)
            try:
                chunk = os.read(fd, min(65536, total + 1 - len(data)))
            except BlockingIOError:
                continue
            if not chunk:
                if not data:
                    time.sleep(min(0.01, max(0, deadline - time.monotonic())))
                    continue
                break
            data.extend(chunk)
            if len(data) > total:
                raise BridgeError("FIFO frame exceeds length")
            if len(data) >= HEADER_SIZE:
                if data[:len(MAGIC)] != MAGIC or struct.unpack("!Q", data[len(MAGIC):HEADER_SIZE])[0] != size:
                    raise BridgeError("invalid FIFO frame")
        if len(data) != total:
            raise BridgeError("truncated FIFO frame")
        return bytes(data[HEADER_SIZE:])
    finally:
        os.close(fd)


def send_fifo(path, data, timeout=TRANSFER_SECONDS):
    integer(len(data), 0, MAX_HTML)
    path = fifo_path(Path(path).parent, Path(path).name)
    deadline = time.monotonic() + timeout
    while True:
        try:
            fd = _open(path, os.O_WRONLY)
            break
        except OSError as error:
            if error.errno != errno.ENXIO or time.monotonic() >= deadline:
                raise BridgeError("FIFO reader unavailable") from error
            time.sleep(0.01)
    try:
        frame = memoryview(MAGIC + struct.pack("!Q", len(data)) + data)
        at = 0
        while at < len(frame):
            _ready(fd, select.POLLOUT, deadline)
            try:
                count = os.write(fd, frame[at:at + 65536])
            except BlockingIOError:
                continue
            if count <= 0:
                raise BridgeError("FIFO write failed")
            at += count
    finally:
        os.close(fd)


def publish_snapshot(config, snapshot):
    if config.get("bulk") != "fifo-v1":
        return exchange(config, {"op": "publish", "snapshot": snapshot})
    body = snapshot["html"].encode("utf-8")
    path = create_fifo(Path(config["socket"]).parent)
    metadata = dict(snapshot)
    del metadata["html"]
    metadata.update(html_fifo=path.name, html_bytes=len(body))
    try:
        with ThreadPoolExecutor(max_workers=1) as pool:
            reply = pool.submit(exchange, config, {"op": "publish", "snapshot": metadata})
            send_fifo(path, body)
            return reply.result()
    finally:
        with contextlib.suppress(FileNotFoundError):
            path.unlink()
