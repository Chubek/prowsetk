"""Bounded, local-only protocol shared by the Qutebrowser bridge scripts."""

import hmac
import json
import math
import os
from pathlib import Path
import socket
import stat
import tempfile
import time
from urllib.parse import urlsplit

VERSION = 1
MAX_HTML = 16 * 1024 * 1024
MAX_MESSAGE = 8 * 1024 * 1024
MAX_FILE = 16 * 1024 * 1024
MAX_ACTIONS = 256
MAX_WAIT_MS = 30000


class BridgeError(Exception):
    """A value-free error which may be shown to the user."""


def integer(value, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise BridgeError("invalid integer or limit")
    return value


def string(value, maximum, empty=False):
    if not isinstance(value, str) or (not empty and not value):
        raise BridgeError("invalid string")
    if len(value.encode("utf-8")) > maximum or "\x00" in value:
        raise BridgeError("string exceeds limit")
    return value


def keys(value, required, optional=()):
    if not isinstance(value, dict) or set(value) - set(required) - set(optional):
        raise BridgeError("invalid message fields")
    if set(required) - set(value):
        raise BridgeError("missing message fields")


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise BridgeError("duplicate JSON member")
        result[key] = value
    return result


def _float(value):
    number = float(value)
    if not math.isfinite(number):
        raise BridgeError("invalid JSON number")
    return number


def decode(data):
    if len(data) > MAX_MESSAGE:
        raise BridgeError("message exceeds limit")
    # Bound parser recursion before handing the bytes to json.loads.
    depth, quoted, escape = 0, False, False
    for byte in data:
        if quoted:
            if escape:
                escape = False
            elif byte == 92:
                escape = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (123, 91):
            depth += 1
            if depth > 32:
                raise BridgeError("JSON depth exceeds limit")
        elif byte in (125, 93):
            depth -= 1
    try:
        return json.loads(data, object_pairs_hook=_object, parse_float=_float,
                          parse_constant=lambda _: (_ for _ in ()).throw(
                              BridgeError("invalid JSON number")))
    except (ValueError, UnicodeError, RecursionError) as error:
        raise BridgeError("invalid JSON") from error


def encode(value):
    try:
        data = json.dumps(value, ensure_ascii=True, separators=(",", ":"),
                          allow_nan=False).encode("utf-8")
    except (ValueError, UnicodeError, RecursionError) as error:
        raise BridgeError("invalid JSON") from error
    if len(data) > MAX_MESSAGE:
        raise BridgeError("message exceeds limit")
    return data


def origin(url):
    string(url, 8192)
    if any(ord(char) <= 32 or ord(char) == 127 for char in url) or "\\" in url:
        raise BridgeError("invalid HTTP(S) URL")
    try:
        parts = urlsplit(url)
        if parts.scheme not in ("http", "https") or not parts.hostname:
            raise BridgeError("invalid HTTP(S) URL")
        if parts.username is not None or parts.password is not None:
            raise BridgeError("URL userinfo is not supported")
        host = parts.hostname.encode("idna").decode("ascii").lower()
        port = parts.port if parts.port is not None else (443 if parts.scheme == "https" else 80)
        if not 1 <= port <= 65535:
            raise BridgeError("invalid HTTP(S) URL")
        host = "[" + host + "]" if ":" in host else host
        suffix = "" if port == (443 if parts.scheme == "https" else 80) else ":" + str(port)
        return parts.scheme + "://" + host + suffix
    except (ValueError, UnicodeError) as error:
        raise BridgeError("invalid HTTP(S) URL") from error


def action(value, allowed_origin):
    keys(value, ("type",), ("selector", "value", "url", "checked"))
    kind = value["type"]
    if kind in ("click", "focus", "scroll", "submit"):
        keys(value, ("type", "selector"))
        string(value["selector"], 4096)
    elif kind in ("fill", "select"):
        keys(value, ("type", "selector", "value"))
        string(value["selector"], 4096)
        string(value["value"], 4096, empty=True)
    elif kind == "check":
        keys(value, ("type", "selector", "checked"))
        string(value["selector"], 4096)
        if type(value["checked"]) is not bool:
            raise BridgeError("invalid checkbox state")
    elif kind == "navigate":
        keys(value, ("type", "url"))
        if origin(value["url"]) != allowed_origin:
            raise BridgeError("action is outside the approved origin")
    elif kind in ("reload", "capture"):
        keys(value, ("type",))
    else:
        raise BridgeError("unsupported action")
    return dict(value)


def private_directory(path, create=False):
    path = Path(path).absolute()
    if create:
        path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise BridgeError("bridge directory must be owner-only")
    return path.resolve()


def read_file(path, limit=MAX_FILE, private=False):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
            raise BridgeError("invalid file or file exceeds limit")
        if private and (info.st_uid != os.geteuid() or info.st_mode & 0o077):
            raise BridgeError("file must be owner-only")
        with os.fdopen(fd, "rb") as stream:
            fd = -1
            result = stream.read(limit + 1)
        if len(result) > limit:
            raise BridgeError("file exceeds limit")
        return result
    finally:
        if fd >= 0:
            os.close(fd)


def write_file(path, data):
    path = Path(path)
    if len(data) > MAX_FILE:
        raise BridgeError("file exceeds limit")
    fd, temporary = tempfile.mkstemp(prefix=".qute-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def descriptor(path):
    path = Path(path).absolute()
    directory = private_directory(path.parent)
    result = decode(read_file(path, 16384, private=True))
    keys(result, ("version", "socket", "token", "origin"), ("bulk",))
    if "bulk" in result and result["bulk"] != "fifo-v1":
        raise BridgeError("unsupported bulk transport")
    if type(result["version"]) is not int or result["version"] != VERSION or origin(result["origin"]) != result["origin"]:
        raise BridgeError("unsupported bridge descriptor")
    socket_path = Path(string(result["socket"], 4096))
    if not socket_path.is_absolute() or socket_path.name != "bridge.sock" or socket_path.parent.resolve() != directory:
        raise BridgeError("invalid bridge socket")
    result["socket"] = str(directory / "bridge.sock")
    string(result["token"], 64)
    if len(result["token"]) != 64 or any(c not in "0123456789abcdef" for c in result["token"]):
        raise BridgeError("invalid bridge token")
    return result


def authorized(token, expected):
    return (isinstance(token, str) and len(token) == 64 and token.isascii() and
            hmac.compare_digest(token, expected))


def exchange(config, request, timeout_ms=MAX_WAIT_MS + 2000):
    integer(timeout_ms, 1, MAX_WAIT_MS + 2000)
    request = dict(request, token=config["token"], version=VERSION)
    if config.get("bulk") == "fifo-v1" and request.get("op") in ("snapshot", "act"):
        request["transport"] = "fifo"
    deadline = time.monotonic() + timeout_ms / 1000
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(timeout_ms / 1000)
        connection.connect(config["socket"])
        connection.sendall(encode(request) + b"\n")
        result = bytearray()
        while b"\n" not in result:
            connection.settimeout(max(0.001, deadline - time.monotonic()))
            chunk = connection.recv(min(65536, MAX_MESSAGE + 1 - len(result)))
            if not chunk or len(result) + len(chunk) > MAX_MESSAGE + 1:
                raise BridgeError("invalid bridge reply")
            result.extend(chunk)
        line, extra = bytes(result).split(b"\n", 1)
        if extra:
            raise BridgeError("invalid bridge reply")
        reply = decode(line)
        if not isinstance(reply, dict) or type(reply.get("ok")) is not bool:
            raise BridgeError("invalid bridge reply")
        if not reply["ok"]:
            # Peer errors are deliberately not displayed verbatim.
            raise BridgeError("bridge operation failed")
        snap = reply.get("snapshot")
        if isinstance(snap, dict) and "html_fifo" in snap:
            from bulk import fifo_path, receive_fifo
            path = fifo_path(Path(config["socket"]).parent, snap.pop("html_fifo"))
            snap["html"] = receive_fifo(path, snap.pop("html_bytes")).decode("utf-8")
        return reply
