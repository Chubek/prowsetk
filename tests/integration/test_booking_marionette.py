"""Exercise the Booking launcher, real controller, V2 IPC and schema exports."""
import base64
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
from urllib.parse import urlsplit

PASSWORD = "fixture-agent-password"
COOKIE = "fixture-booking-cookie"


class Agent(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, body):
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def check_auth(self):
        expected = "Basic " + base64.b64encode(f"opencode:{PASSWORD}".encode()).decode()
        assert self.headers.get("Authorization") == expected
        assert "Cookie" not in self.headers

    def do_GET(self):
        self.check_auth()
        path = urlsplit(self.path).path
        if path == "/api/session/active":
            self.reply({"data": []})
        else:
            assert path == "/api/session/ses_fixture/message", self.path
            self.reply({"data": self.server.messages, "cursor": {}})

    def do_POST(self):
        self.check_auth()
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.bodies.append(body)
        assert COOKIE not in json.dumps(body)
        assert "page-private" not in json.dumps(body)
        assert "script-private" not in json.dumps(body)
        if self.path == "/api/session":
            assert body == {"permissions": [{"action": "*", "resource": "*", "effect": "deny"}]}
            self.reply({"data": {"id": "ses_fixture"}})
            return
        assert self.path == "/api/session/ses_fixture/prompt", self.path
        assert set(body) == {"text"}
        turn = len(self.server.messages)
        choice = self.server.choices[turn]
        if not choice.startswith("invented-"):
            assert choice == "stop" or choice in body["text"]
        self.server.messages.insert(0, {
            "id": f"msg_{turn}", "type": "assistant", "time": {"created": 1, "completed": 2},
            "content": [{"type": "reasoning", "text": "ignored"},
                        {"type": "text", "text": json.dumps({"action": choice})}]})
        self.reply({"data": {"id": f"msg_user_{turn}"}})


class Page(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def serve(self):
        assert "Authorization" not in self.headers
        assert f"session={COOKIE}" in self.headers.get("Cookie", "")
        self.server.requests.append((self.command, self.path))
        path = urlsplit(self.path).path
        content_type = "text/html"
        if path == "/":
            body = """<h1>Joe Litty Rooms</h1><a href='/logout'>Log out</a>
                <nav><a href='/reservations'>Reservations</a></nav>
                <a href='/api/rooms/123?limit=2&amp;active=true&amp;token=page-private'>Rooms</a>
                <form action='/api/search' method='post'>
                <input type='number' name='limit' required value='private-form-value'></form>"""
        elif path == "/reservations":
            body = """<button data-testid='reservation-details' aria-expanded='false'>Details</button>
                <a href='/api/reservations'>Reservations</a><a href='/last' rel='next'>Next</a>
                <script>var secret='script-private';
                document.querySelector('button').addEventListener('click', function() {
                  this.setAttribute('aria-expanded','true');
                  Promise.resolve().then(()=>fetch('/api/' + ['details'].join('')));
                });</script>"""
        elif path == "/last":
            body = "<a href='/api/final'>Final records</a>"
        elif path == "/logout":
            raise AssertionError("controller clicked logout")
        elif path.startswith("/api/"):
            assert self.command == "GET", "schema discovery must not probe POST"
            content_type = "application/json"
            body = json.dumps({"id": 123, "active": True, "rooms": [{"name": "private-response-value"}],
                               "token": "page-private"})
        else:
            raise AssertionError(path)
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_GET = serve
    do_POST = serve


def agent_server(host, port, choices):
    server = ThreadingHTTPServer((host, port), Agent)
    server.messages = []
    server.bodies = []
    server.choices = choices
    return server


@contextmanager
def serving(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def serve_fixture():
    args = sys.argv[2:]
    port = int(args[args.index("--port") + 1])
    host = args[args.index("--hostname") + 1]
    server = agent_server(host, port, ["stop"])
    Path(os.environ["FIXTURE_SERVER_PID"]).write_text(str(os.getpid()))

    def stop(*_):
        Path(os.environ["FIXTURE_SERVER_STOPPED"]).write_text("stopped")
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    print(f"server password {PASSWORD}", flush=True)
    server.serve_forever()


def main():
    runner, source = sys.argv[1:]
    source = Path(source)
    launcher = source / "scripts/run-scrape-booking.sh"
    env = dict(os.environ, PROWSETK_MARIONETTE_BIN=runner,
               OPENCODE_SERVER_USERNAME="opencode", OPENCODE_SERVER_PASSWORD=PASSWORD)
    # Page and OpenCode connections bypass developer proxy settings in this fixture.
    for name in ("HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy"):
        env.pop(name, None)
    with tempfile.TemporaryDirectory(prefix="booking-marionette-", dir=os.getcwd()) as tmp:
        root = Path(tmp)
        cookies = root / "cookies.json"
        cookies.write_text(json.dumps([{"domain": "127.0.0.1", "path": "/", "name": "session", "value": COOKIE}]))
        dotenv = root / "blank.env"
        dotenv.write_text("")
        page = ThreadingHTTPServer(("127.0.0.1", 0), Page)
        page.requests = []
        agent = agent_server("127.0.0.1", 0,
                             ["view-reservations", "expand-reservation-details", "next-data-page", "stop"])
        with serving(page) as page_url, serving(agent) as agent_url:
            output = root / "live outputs/api.yaml"
            result = subprocess.run([
                "bash", str(launcher), "--no-server", "--url", page_url + "/",
                "--output", str(output), "--cookies-json", str(cookies), "--dotenv", str(dotenv),
                "--assistant-browser-force", "false", "--no-xcors", "false",
                "--max-steps", "4", "--max-page-requests", "32", "--max-get-probes", "16",
                "--opencode-max-requests", "16", "--opencode-wait-ms", "1000"],
                env=dict(env, OPENCODE_BASE_URL=agent_url), capture_output=True, text=True, timeout=25)
            assert result.returncode == 0, (result.stdout, result.stderr)
            yaml = output.read_text()
            postman_path = output.with_suffix(".postman_collection.json")
            postman_text = postman_path.read_text()
            postman = json.loads(postman_text)
            assert postman["item"], postman
            for path in ("/api/rooms/{id}", "/api/reservations", "/api/details", "/api/final", "/api/search"):
                assert path in yaml, (path, yaml)
            for text in ("requestBody:", "type: integer", "type: boolean", "x-prowsetk-schema:",
                         "x-prowsetk-marionette:", "used: true", "steps: 3", "coverage-complete: false",
                         "authenticated: true", "reason: agent-stop"):
                assert text in yaml, (text, yaml)
            assert any(method == "GET" and path == "/api/details" for method, path in page.requests)
            assert all(method == "GET" for method, _ in page.requests), page.requests
            for secret in (PASSWORD, COOKIE, "page-private", "private-form-value", "private-response-value", "script-private"):
                assert secret not in yaml + postman_text + result.stdout + result.stderr
            assert len(agent.messages) == 4

        # Start/stop a separate server through the actual launcher, use the default
        # Booking action policy and origin filter, and confirm offline schema hints.
        fake_server = root / "fake opencode"
        fake_server.write_text("#!/bin/sh\nexec " + shlex.join([sys.executable, str(Path(__file__).resolve()),
                                                                  "--serve-fixture"]) + ' "$@"\n')
        fake_server.chmod(0o700)
        work = root / "private server"
        pid = root / "server.pid"
        stopped = root / "server.stopped"
        output = root / "offline/api.yaml"
        # Reserve a loopback port; launcher owns the subsequent listener.
        reserve = ThreadingHTTPServer(("127.0.0.1", 0), Agent)
        port = reserve.server_port
        reserve.server_close()
        spawn_env = dict(env, FIXTURE_SERVER_PID=str(pid), FIXTURE_SERVER_STOPPED=str(stopped))
        spawn_env.pop("OPENCODE_BASE_URL", None)
        result = subprocess.run([
            "bash", str(launcher), "--server-bin", str(fake_server), "--port", str(port),
            "--work-dir", str(work), "--html", "<script>fetch('/api/orders?limit=2')</script>",
            "--output", str(output), "--cookies-json", str(root / "missing-cookies.json")],
            env=spawn_env, capture_output=True, text=True, timeout=25)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert stopped.exists(), "launcher left its server running"
        assert work.joinpath("opencode-server.log").stat().st_mode & 0o077 == 0
        assert PASSWORD not in result.stdout + result.stderr
        assert "get-probes: 0" in output.read_text()
        assert "authenticated: false" in output.read_text()
        assert "/api/orders" in output.read_text()
        json.loads(output.with_suffix(".postman_collection.json").read_text())

        # Failed agent output must propagate instead of exporting a partial/stale spec.
        bad_agent = agent_server("127.0.0.1", 0, ["invented-private-action"])
        with serving(bad_agent) as agent_url:
            failed_output = root / "failed.yaml"
            result = subprocess.run([
                "bash", str(launcher), "--no-server", "--html", "<a href='/api/rooms'>Rooms</a>",
                "--output", str(failed_output), "--cookies-json", str(root / "missing-cookies.json")],
                env=dict(env, OPENCODE_BASE_URL=agent_url), capture_output=True, text=True, timeout=15)
            assert result.returncode != 0, (result.stdout, result.stderr)
            assert not failed_output.exists()
            assert "invented-private-action" not in result.stdout + result.stderr
        print("PASS: Booking login, V2 decisions, clicks/navigation, typed schemas, redaction and launcher lifecycle")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--serve-fixture":
        serve_fixture()
    else:
        main()
