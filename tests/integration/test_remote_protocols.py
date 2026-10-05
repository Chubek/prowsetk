"""Hermetic RFC6455 peer and CLI credential-isolation tests (standard library)."""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading


def exact(peer, count):
    result = b""
    while len(result) < count:
        chunk = peer.recv(count - len(result))
        if not chunk:
            raise AssertionError("truncated client frame")
        result += chunk
    return result


def client_frame(peer):
    first, second = exact(peer, 2)
    assert second & 128, "client frames must be masked"
    size = second & 127
    if size == 126:
        size = struct.unpack("!H", exact(peer, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", exact(peer, 8))[0]
    assert size < 4096
    mask = exact(peer, 4)
    data = exact(peer, size)
    return first & 15, bytes(c ^ mask[i % 4] for i, c in enumerate(data))


def websocket_case(probe, mode):
    errors = []
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(5)

    def serve():
        try:
            with listener.accept()[0] as peer:
                peer.settimeout(5)
                headers = b""
                while not headers.endswith(b"\r\n\r\n"):
                    headers += exact(peer, 1)
                    assert len(headers) < 8192
                fields = dict(line.split(b": ", 1) for line in headers.split(b"\r\n")[1:] if line)
                key = fields[b"Sec-WebSocket-Key"]
                assert len(base64.b64decode(key)) == 16
                accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
                if mode == "bad-accept":
                    accept = b"invalid"
                peer.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
                if mode == "bad-accept":
                    return
                opcode, command = client_frame(peer)
                assert opcode == 1
                command = json.loads(command)
                assert command["method"] == "Browser.getVersion"
                if mode == "masked-server":
                    peer.sendall(b"\x81\x80\x00\x00\x00\x00")
                    return
                if mode == "oversize":
                    peer.sendall(b"\x81\x7e\x20\x00")
                    return
                if mode == "bad-utf8":
                    peer.sendall(b"\x81\x01\xff")
                    return
                event = b'{"method":"Page.loadEventFired","params":{}}'
                peer.sendall(bytes([129, len(event)]) + event)
                reply = json.dumps({"id": command["id"], "result": {"product": "fixture"}}, separators=(",", ":")).encode()
                half = len(reply) // 2
                peer.sendall(bytes([1, half]) + reply[:half])
                peer.sendall(b"\x89\x01p")
                assert client_frame(peer) == (10, b"p")
                peer.sendall(bytes([128, len(reply) - half]) + reply[half:])
        except BaseException as error:
            errors.append(error)
        finally:
            listener.close()

    port = listener.getsockname()[1]
    worker = threading.Thread(target=serve)
    worker.start()
    env = dict(os.environ, NO_PROXY="*", no_proxy="*")
    result = subprocess.run([probe, f"ws://127.0.0.1:{port}/devtools/browser"], env=env, capture_output=True, timeout=10)
    worker.join(6)
    assert not worker.is_alive()
    assert not errors, errors
    assert result.returncode == (0 if mode == "success" else 1), result.stderr


def cli_cases(oauth, browser):
    for binary in (oauth, browser):
        assert subprocess.run([binary, "--help"], capture_output=True, timeout=10).returncode == 0
    with tempfile.TemporaryDirectory(dir=".") as tmp:
        home = Path(tmp).resolve()
        cache = home / ".cache/ProwseTk/OAuth"
        cache.mkdir(parents=True, mode=0o700)
        config = home / "Prowse.toml"
        config.write_text('[oauth]\nclient_id="fixture"\nscopes="fixture"\n')
        record = {"issuer": "https://dash.cloudflare.com/oauth2/token", "client": "fixture", "scopes": "fixture",
                  "redirect": "http://localhost:8976/oauth/callback", "access_token": "private-fixture", "refresh_token": "private-refresh", "expires_at": 4102444800}
        token_file = cache / "token.json"
        token_file.write_text(json.dumps(record))
        token_file.chmod(0o600)
        env = dict(os.environ, HOME=str(home), PROWSETK_OAUTH_CLIENT_ID="", PROWSETK_OAUTH_SCOPES="",
                   CLOUDFLARE_ACCOUNT_ID="bad", CLOUDFLARE_API_TOKEN="private-fixture")
        result = subprocess.run([oauth, "status", "--config", str(config)], env=env, capture_output=True, timeout=10)
        assert result.returncode == 0 and result.stdout == b"authenticated\n", result.stderr
        result = subprocess.run([browser, "cdp", "--method", "Browser.getVersion", "--config", str(config)], env=env, capture_output=True, timeout=10)
        assert result.returncode != 0 and b"private-fixture" not in result.stdout + result.stderr
        result = subprocess.run([oauth, "logout", "--config", str(config)], env=env, capture_output=True, timeout=10)
        assert result.returncode == 0 and not token_file.exists()


if __name__ == "__main__":
    for case in ("success", "bad-accept", "masked-server", "oversize", "bad-utf8"):
        websocket_case(sys.argv[1], case)
    cli_cases(sys.argv[2], sys.argv[3])
