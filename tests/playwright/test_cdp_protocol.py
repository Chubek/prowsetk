"""Protocol-level smoke tests for ProwseTk's native Playwright/CDP bridge."""

import base64
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import urllib.request
from urllib.parse import urlsplit

import pytest


CLI = os.environ.get("PROWSETK_CLI")
pytestmark = pytest.mark.skipif(
    not CLI,
    reason="set PROWSETK_CLI to the built prowsetk executable",
)


def _http_json(url):
    with urllib.request.urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def _read_line(process):
    line = process.stdout.readline()
    match = re.search(r"listening on http://([^:]+):(\d+)", line)
    if match:
        return f"http://{match.group(1)}:{match.group(2)}"
    if "cannot create listening socket" in line:
        pytest.skip("loopback sockets are unavailable in this environment")
    raise RuntimeError(f"Playwright/CDP server did not start: {line!r}")


def _recv_exact(sock, size):
    payload = bytearray()
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            raise AssertionError("CDP server closed the WebSocket")
        payload.extend(chunk)
    return bytes(payload)


def _websocket_connect(url):
    parsed = urlsplit(url)
    sock = socket.create_connection((parsed.hostname, parsed.port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {parsed.path or '/'} HTTP/1.1\r\n"
        f"Host: {parsed.hostname}:{parsed.port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode("ascii"))
    headers = b""
    while b"\r\n\r\n" not in headers:
        headers += sock.recv(4096)
    if b"101 Switching Protocols" not in headers:
        sock.close()
        raise AssertionError(f"WebSocket handshake failed: {headers!r}")
    expected = base64.b64encode(
        hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
        ).digest()
    )
    assert b"Sec-WebSocket-Accept: " + expected in headers
    return sock


def _send_text(sock, payload):
    encoded = payload.encode("utf-8")
    mask = os.urandom(4)
    if len(encoded) < 126:
        header = bytes((0x81, 0x80 | len(encoded)))
    elif len(encoded) < 65536:
        header = bytes((0x81, 0x80 | 126)) + struct.pack(">H", len(encoded))
    else:
        header = bytes((0x81, 0x80 | 127)) + struct.pack(">Q", len(encoded))
    sock.sendall(header + mask + bytes(
        byte ^ mask[index % 4] for index, byte in enumerate(encoded)
    ))


def _receive_text(sock):
    while True:
        first, second = _recv_exact(sock, 2)
        opcode = first & 0x0F
        length = second & 0x7F
        if length == 126:
            length = struct.unpack(">H", _recv_exact(sock, 2))[0]
        elif length == 127:
            length = struct.unpack(">Q", _recv_exact(sock, 8))[0]
        masked = second & 0x80
        mask = _recv_exact(sock, 4) if masked else b""
        payload = bytearray(_recv_exact(sock, length))
        if masked:
            payload = bytearray(
                byte ^ mask[index % 4] for index, byte in enumerate(payload)
            )
        if opcode == 0x9:
            _send_frame(sock, 0xA, bytes(payload))
            continue
        if opcode == 0x8:
            raise AssertionError("CDP server closed the WebSocket")
        if opcode == 0x1:
            return json.loads(payload.decode("utf-8"))


def _send_frame(sock, opcode, payload):
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        header = bytes((0x80 | opcode, 0x80 | length))
    elif length < 65536:
        header = bytes((0x80 | opcode, 0x80 | 126)) + struct.pack(">H", length)
    else:
        header = bytes((0x80 | opcode, 0x80 | 127)) + struct.pack(">Q", length)
    sock.sendall(header + mask + bytes(
        byte ^ mask[index % 4] for index, byte in enumerate(payload)
    ))


def _command(sock, command_id, method, params=None):
    _send_text(sock, json.dumps({
        "id": command_id,
        "method": method,
        "params": params or {},
    }))
    while True:
        message = _receive_text(sock)
        if message.get("id") == command_id:
            assert "error" not in message, message
            return message["result"]


@pytest.fixture(scope="session")
def cdp_targets():
    process = subprocess.Popen(
        [CLI, "playwright", "--host", "127.0.0.1", "--port", "0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    base_url = _read_line(process)
    try:
        yield process, base_url, _http_json(base_url + "/json/version"), \
            _http_json(base_url + "/json/list")[0]
    finally:
        process.terminate()
        process.wait(timeout=5)


def test_cdp_browser_and_page_targets(cdp_targets):
    _, _, version, target = cdp_targets

    browser_socket = _websocket_connect(version["webSocketDebuggerUrl"])
    try:
        browser_version = _command(browser_socket, 1, "Browser.getVersion")
        assert browser_version["product"].startswith("ProwseTk/")
    finally:
        browser_socket.close()

    page_socket = _websocket_connect(target["webSocketDebuggerUrl"])
    try:
        _command(page_socket, 2, "Page.navigate", {
            "url": "data:text/html,<title>CDP</title><p id='message'>hello</p>",
        })
        result = _command(page_socket, 3, "Runtime.evaluate", {
            "expression": "document.title",
            "returnByValue": True,
        })
        assert result["result"]["value"] == "CDP"
    finally:
        page_socket.close()
