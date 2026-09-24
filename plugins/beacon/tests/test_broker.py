"""Hermetic end-to-end broker, native-host framing and tab-scope test."""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

DAEMON, HOST, CLIENT = sys.argv[1:4]
FLASH = "11111111-1111-4111-8111-111111111111"


def exchange(path, message):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
        conn.settimeout(2)
        conn.connect(path)
        conn.sendall(json.dumps(message).encode() + b"\n")
        data = bytearray()
        while b"\n" not in data:
            part = conn.recv(4096)
            assert part and len(data) < 9 * 1024 * 1024
            data.extend(part)
        return json.loads(data.split(b"\n", 1)[0])


with tempfile.TemporaryDirectory(dir=os.getcwd()) as directory:
    path = os.path.join(directory, "broker.sock")
    daemon = subprocess.Popen([DAEMON, "--socket", path], stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            if os.path.exists(path):
                break
            if daemon.poll() is not None:
                raise RuntimeError("beacond exited before binding")
            time.sleep(.01)
        assert os.stat(path).st_mode & 0o777 == 0o600
        assert exchange(path, {"type": "ping"})["type"] == "pong"
        assert json.loads(subprocess.check_output([CLIENT, "--socket", path, "ping"]))["type"] == "pong"
        request = {"type": "flash_request", "flash_id": FLASH,
                   "request_type": "page_dom", "filters": {"url_pattern": "https://example.test/*"}}
        assert exchange(path, request)["type"] == "flash_registered"
        assert exchange(path, {"type": "flash_poll", "flash_id": FLASH})["type"] == "flash_empty"
        assert json.loads(subprocess.check_output([CLIENT, "--socket", path, "poll", FLASH]))["type"] == "flash_empty"
        bad = {"type": "flash_data", "flash_id": FLASH, "tab_id": 9,
               "data_type": "page_dom", "payload": {"html": "private"}}
        assert exchange(path, bad)["type"] == "error"
        assert exchange(path, {"type": "flash_connect", "flash_id": FLASH,
                               "tab_id": 7})["type"] == "flash_connected"
        assert exchange(path, bad)["type"] == "error"
        bad["tab_id"] = 7
        frame = json.dumps(bad).encode()
        host = subprocess.Popen([HOST, "--socket", path], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out, err = host.communicate(struct.pack("<I", len(frame)) + frame, timeout=5)
        assert host.returncode == 0, err
        size, = struct.unpack("<I", out[:4])
        assert json.loads(out[4:4 + size])["type"] == "flash_queued"
        assert exchange(path, {"type": "flash_poll", "flash_id": FLASH})["payload"]["html"] == "private"
        assert exchange(path, {"type": "flash_poll", "flash_id": FLASH})["type"] == "flash_empty"
        assert exchange(path, {"type": "flash_disconnect", "flash_id": FLASH})["type"] == "flash_closed"
        assert exchange(path, bad)["type"] == "error"
    finally:
        daemon.terminate()
        daemon.communicate(timeout=5)
