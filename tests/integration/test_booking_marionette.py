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


class PageProxy(Page):
    def serve(self):
        expected = "Basic " + base64.b64encode(b"proxy-user:proxy-private").decode()
        assert self.headers.get("Proxy-Authorization") == expected
        assert self.path.startswith("http://booking-fixture.invalid/"), self.path
        assert self.path.count("?") <= 1, self.path
        super().serve()

    do_GET = serve
    do_POST = serve


class UnconfirmedPage(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        body = b"<input type='password' value='form-private'><script>throw new Error('script-private')</script>"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


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
    server = agent_server(host, port, [os.environ.get("FIXTURE_SERVER_CHOICE", "stop")])
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
                "--opencode-max-requests", "16", "--opencode-wait-ms", "1000", "-v"],
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
            assert not any(urlsplit(path).path == "/logout" for _, path in page.requests), page.requests
            for secret in (PASSWORD, COOKIE, "page-private", "private-form-value", "private-response-value", "script-private"):
                assert secret not in yaml + postman_text + result.stdout + result.stderr
            assert len(agent.messages) == 4
            for diagnostic in ("preparing live session", "page request #", "OpenCode request #", "HTTP=200",
                               "route=direct", "cookies=yes", "login confirmation:", "beacon-match=true",
                               "confirmed=true", "page runtime summary:"):
                assert diagnostic in result.stderr, (diagnostic, result.stderr)

        # HTTP_PROXY carries remote page requests, including query strings and
        # Basic proxy auth. The local OpenCode control connection stays direct.
        proxy = ThreadingHTTPServer(("127.0.0.1", 0), PageProxy)
        proxy.requests = []
        agent = agent_server("127.0.0.1", 0, ["stop"])
        proxy_cookies = root / "proxy-cookies.json"
        proxy_cookies.write_text(json.dumps([{"domain": "booking-fixture.invalid", "path": "/",
                                              "name": "session", "value": COOKIE}]))
        with serving(proxy) as proxy_url, serving(agent) as agent_url:
            output = root / "proxied.yaml"
            proxy_env = dict(env, OPENCODE_BASE_URL=agent_url,
                             HTTP_PROXY=proxy_url.replace("http://", "http://proxy-user:proxy-private@"),
                             HTTPS_PROXY="http://unused.invalid:1", NO_PROXY="", no_proxy="")
            result = subprocess.run([
                "bash", str(launcher), "--no-server", "--url", "http://booking-fixture.invalid/",
                "--output", str(output), "--cookies-json", str(proxy_cookies), "--dotenv", str(dotenv),
                "--assistant-browser-force", "false", "--no-xcors", "false", "--verbose"],
                env=proxy_env, capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert proxy.requests and any("?limit=2&active=true&token=page-private" in path
                                          for _, path in proxy.requests), proxy.requests
            assert not any(urlsplit(path).path == "/logout" for _, path in proxy.requests), proxy.requests
            assert "route=http-proxy" in result.stderr
            assert "OpenCode request #1 host=127.0.0.1 route=direct cookies=no" in result.stderr
            for secret in ("proxy-user", "proxy-private", COOKIE, PASSWORD, "page-private", "private-form-value"):
                assert secret not in result.stdout + result.stderr + output.read_text()

        # A 200 sign-in page is not positive login evidence. Report the failing
        # preparation stage and individual evidence checks without page values.
        page = ThreadingHTTPServer(("127.0.0.1", 0), UnconfirmedPage)
        agent = agent_server("127.0.0.1", 0, [])
        with serving(page) as page_url, serving(agent) as agent_url:
            failed_output = root / "unconfirmed.yaml"
            result = subprocess.run([
                "bash", str(launcher), "--no-server", "--url", page_url + "/",
                "--output", str(failed_output), "--cookies-json", str(cookies), "--dotenv", str(dotenv),
                "--assistant-browser-force", "false", "--verbse",
                "--success-beacon", "xpath=//*[broken"],
                env=dict(env, OPENCODE_BASE_URL=agent_url), capture_output=True, text=True, timeout=15)
            assert result.returncode != 0 and not failed_output.exists()
            for diagnostic in ("session preparation failed [lua_error]", "missing Booking.com credentials",
                               "auth-marker=false", "beacon-error=true", "password-field=true", "confirmed=false",
                               "page JavaScript exception #1", "controller failed (exit 1)"):
                assert diagnostic in result.stderr, (diagnostic, result.stderr)
            assert not agent.bodies, "OpenCode was contacted before login confirmation"
            for secret in (COOKIE, PASSWORD, "form-private", "script-private"):
                assert secret not in result.stdout + result.stderr

        # Failed/empty/malformed capture must stop immediately, even if a valid
        # but stale cookie export exists. Commands cannot leak values to logs.
        grabber = root / "cookie-command.py"
        grabber.write_text("""from pathlib import Path
import sys
print('cookie-command-private')
print('cookie-command-private', file=sys.stderr)
if sys.argv[1] == 'failed': sys.exit(17)
Path(sys.argv[2]).write_text('[]' if sys.argv[1] == 'empty' else 'cookie-json-private')
""")
        agent = agent_server("127.0.0.1", 0, [])
        with serving(agent) as agent_url:
            for mode, expected in (("failed", "Firefox cookie capture failed"),
                                   ("empty", "Firefox cookie import returned no cookies"),
                                   ("malformed", "Firefox cookie import failed")):
                stale = root / f"{mode}-cookies.json"
                stale.write_text(cookies.read_text())
                failed_output = root / f"{mode}-capture.yaml"
                result = subprocess.run([
                    "bash", str(launcher), "--no-server", "--output", str(failed_output),
                    "--cookies-json", str(stale), "--dotenv", str(dotenv), "-v"],
                    env=dict(env, OPENCODE_BASE_URL=agent_url, PROWSETK_ASSISTANT_BROWSER="true",
                             PROWSETK_ASSISTANT_BROWSER_COOKIE_COMMAND=shlex.join([sys.executable, str(grabber), mode])),
                    input="\n", capture_output=True, text=True, timeout=15)
                assert result.returncode != 0 and not failed_output.exists()
                assert "session preparation failed" in result.stderr and expected in result.stderr, result.stderr
                assert "page request #" not in result.stderr
                for secret in (COOKIE, PASSWORD, "cookie-command-private", "cookie-json-private"):
                    assert secret not in result.stdout + result.stderr
            assert not agent.bodies

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
        spawn_env = dict(env, FIXTURE_SERVER_PID=str(pid), FIXTURE_SERVER_STOPPED=str(stopped),
                         HTTP_PROXY="http://127.0.0.1:1", NO_PROXY="", no_proxy="")
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

        # On a failed run the owned server still stops, but its default private
        # temporary log survives for diagnosis instead of being deleted by EXIT.
        temp_logs = root / "temporary logs"
        temp_logs.mkdir()
        failed_stopped = root / "failed-server.stopped"
        reserve = ThreadingHTTPServer(("127.0.0.1", 0), Agent)
        failed_port = reserve.server_port
        reserve.server_close()
        failed_output = root / "spawn-failed.yaml"
        result = subprocess.run([
            "bash", str(launcher), "--server-bin", str(fake_server), "--port", str(failed_port),
            "--html", "<a href='/api/rooms'>Rooms</a>", "--output", str(failed_output),
            "--cookies-json", str(root / "missing-cookies.json")],
            env=dict(spawn_env, TMPDIR=str(temp_logs), FIXTURE_SERVER_STOPPED=str(failed_stopped),
                     FIXTURE_SERVER_CHOICE="invented-private-action"),
            capture_output=True, text=True, timeout=20)
        assert result.returncode != 0 and not failed_output.exists()
        assert failed_stopped.exists(), "failed launcher left its server running"
        logs = list(temp_logs.glob("prowsetk-opencode-*/opencode-server.log"))
        assert len(logs) == 1 and logs[0].stat().st_mode & 0o077 == 0, logs
        assert "private server log retained at" in result.stderr
        assert PASSWORD not in result.stdout + result.stderr

        # Failed agent output must propagate instead of exporting a partial/stale spec.
        bad_agent = agent_server("127.0.0.1", 0, ["invented-private-action"])
        with serving(bad_agent) as agent_url:
            failed_output = root / "failed.yaml"
            result = subprocess.run([
                "bash", str(launcher), "--no-server", "--html", "<a href='/api/rooms'>Rooms</a>",
                "--output", str(failed_output), "--cookies-json", str(root / "missing-cookies.json"), "--verbose"],
                env=dict(env, OPENCODE_BASE_URL=agent_url), capture_output=True, text=True, timeout=15)
            assert result.returncode != 0, (result.stdout, result.stderr)
            assert not failed_output.exists()
            assert "invented-private-action" not in result.stdout + result.stderr
            assert "OpenCode exploration failed [security_violation]" in result.stderr
            assert "OpenCode decision failed" in result.stderr
        print("PASS: Booking login/cookie diagnostics, proxies, V2 actions, schemas, redaction and launcher lifecycle")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--serve-fixture":
        serve_fixture()
    else:
        main()
