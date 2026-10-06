"""QUTE_HTML snapshot senders and the QUTE_FIFO marionette controller."""

import argparse
import contextlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import secrets
import select
import shlex
import stat
import sys
import tempfile
import time

from protocol import (BridgeError, MAX_HTML, MAX_WAIT_MS, action, descriptor,
                      exchange, integer, origin, read_file, string)
from bulk import publish_snapshot


class ActionMarker(HTMLParser):
    def __init__(self, action_id):
        super().__init__()
        self.expected = action_id
        self.status = "unconfirmed"

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == "meta" and values.get("name") == "prowsetk-qute-action":
            if values.get("data-id") == self.expected and values.get("content") in ("ok", "error"):
                self.status = values["content"]


def snapshot(config, purpose, action_id=None):
    if os.environ.get("QUTE_MODE") != "command":
        raise BridgeError("run the bridge from command mode")
    url = os.environ.get("QUTE_URL", "")
    if origin(url) != config["origin"]:
        raise BridgeError("assistant tab is outside the approved origin")
    try:
        tab = int(os.environ.get("QUTE_TAB_INDEX", ""))
    except ValueError as error:
        raise BridgeError("missing assistant tab index") from error
    integer(tab, 1, 100000)
    html = read_file(os.environ.get("QUTE_HTML", ""), MAX_HTML).decode("utf-8")
    result = {"url": url, "html": html, "tab": tab, "purpose": purpose}
    if action_id is not None:
        if len(action_id) != 32 or any(c not in "0123456789abcdef" for c in action_id):
            raise BridgeError("invalid action identifier")
        marker = ActionMarker(action_id)
        marker.feed(html)
        result.update(action_id=action_id, action_status=marker.status)
    return result


def command_path(path):
    path = str(Path(path).absolute())
    # Qutebrowser splits ;; before spawn's shlex parser and expands {variables}.
    # Do not let paths become command syntax at either stage.
    if any(c in path for c in ("\n", "\r", "\x00", "{", "}")) or ";;" in path:
        raise BridgeError("unsupported command path")
    return string(path, 2048)


def fifo_command(command):
    if "\n" in command or "\r" in command:
        raise BridgeError("invalid Qutebrowser command")
    data = (command + "\n").encode("utf-8")
    if len(data) > 4096:
        raise BridgeError("Qutebrowser command exceeds limit")
    fd = os.open(os.environ.get("QUTE_FIFO", ""), os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISFIFO(info.st_mode) or info.st_uid != os.geteuid():
            raise BridgeError("invalid Qutebrowser FIFO")
        poller = select.poll()
        poller.register(fd, select.POLLOUT)
        # A bounded <= PIPE_BUF write prevents interleaved partial commands.
        if not poller.poll(3000) or os.write(fd, data) != len(data):
            raise BridgeError("Qutebrowser FIFO is unavailable")
    finally:
        os.close(fd)


def action_source(approved, allowed_origin, action_id):
    approved = action({key: value for key, value in approved.items() if key != "id"}, allowed_origin)
    # These are trusted, fixed operations. Caller data is encoded as JSON into a
    # private file; it never becomes Qutebrowser command text or executable code.
    config = json.dumps({"action": approved, "origin": allowed_origin, "id": action_id},
                        ensure_ascii=True, separators=(",", ":"))
    return """(() => {
  'use strict';
  const cfg = CONFIG;
  if (location.origin !== cfg.origin) return;
  let status = 'ok';
  try {
    const a = cfg.action;
    if (a.type === 'navigate') {
      if (new URL(a.url).origin !== cfg.origin) throw Error();
      location.assign(a.url);
    } else if (a.type === 'reload') {
      location.reload();
    } else if (a.type === 'capture') {
      // Fresh userscript capture below; never reuse the initial QUTE_HTML.
    } else {
      const e = document.querySelector(a.selector);
      if (!e || e.disabled || e.getAttribute('aria-disabled') === 'true') throw Error();
      for (let n = e; n; n = n.parentElement) {
        const s = getComputedStyle(n);
        if (n.hidden || s.display === 'none' || s.visibility === 'hidden') throw Error();
      }
      if (a.type === 'click') e.click();
      else if (a.type === 'focus') e.focus();
      else if (a.type === 'scroll') e.scrollIntoView({block: 'center', behavior: 'instant'});
      else if (a.type === 'submit') {
        const f = e.tagName === 'FORM' ? e : e.form;
        if (!f || new URL(f.action || location.href, location.href).origin !== cfg.origin) throw Error();
        f.requestSubmit();
      } else if (a.type === 'check') {
        if (e.tagName !== 'INPUT' || !['checkbox', 'radio'].includes(e.type) || e.readOnly) throw Error();
        if (e.type === 'radio' && !a.checked) throw Error();
        if (e.checked !== a.checked) e.click();
        if (e.checked !== a.checked) throw Error();
      } else {
        if (!['INPUT', 'TEXTAREA', 'SELECT'].includes(e.tagName)) throw Error();
        if (a.type === 'select' && e.tagName !== 'SELECT') throw Error();
        const forbidden = ['password', 'file', 'hidden', 'submit', 'button', 'checkbox', 'radio'];
        if (e.tagName === 'INPUT' && forbidden.includes(e.type)) throw Error();
        if (e.readOnly) throw Error();
        e.focus();
        let p = Object.getPrototypeOf(e), d;
        while (p && !d) { d = Object.getOwnPropertyDescriptor(p, 'value'); p = Object.getPrototypeOf(p); }
        if (!d || !d.set) throw Error();
        d.set.call(e, a.value);
        if (a.type === 'select' && e.value !== a.value) throw Error();
        e.dispatchEvent(new Event('input', {bubbles: true, composed: true}));
        e.dispatchEvent(new Event('change', {bubbles: true}));
      }
    }
  } catch (_) { status = 'error'; }
  document.querySelectorAll('meta[name="prowsetk-qute-action"]').forEach(e => e.remove());
  const marker = document.createElement('meta');
  marker.name = 'prowsetk-qute-action';
  marker.setAttribute('data-id', cfg.id);
  marker.content = status;
  (document.head || document.documentElement).appendChild(marker);
})();
""".replace("CONFIG", config, 1).encode("utf-8")


def marionette(config, descriptor_path, settle_ms):
    initial = snapshot(config, "page")
    controller = secrets.token_hex(16)
    exchange(config, {"op": "attach", "controller": controller, "tab": initial["tab"]})
    files = []
    try:
        publish_snapshot(config, initial)
        sender = command_path(Path(__file__).parent / "ptk-qute-send")
        descriptor_path = command_path(descriptor_path)
        while True:
            reply = exchange(config, {"op": "next", "controller": controller, "wait_ms": MAX_WAIT_MS})
            if reply["closed"]:
                return
            current = reply["action"]
            if current is None:
                continue
            source = action_source(current, config["origin"], current["id"])
            fd, path = tempfile.mkstemp(prefix=".action-", suffix=".js", dir=Path(descriptor_path).parent)
            files.append(path)
            with os.fdopen(fd, "wb") as stream:
                stream.write(source)
            # jseval's final argument is a raw remainder, not a shlex token:
            # do not quote the filename. --world=main avoids a split flag value.
            fifo_command("jseval --quiet --world=main --file " + command_path(path))
            # This is a settling delay, not proof of page-load/network completion.
            time.sleep(settle_ms / 1000)
            fifo_command("spawn --userscript " + shlex.quote(sender) + " --bridge " +
                         shlex.quote(descriptor_path) + " --action-id " + current["id"])
            # The next action can only arrive after the sender has returned a
            # fresh capture. Keep the JS file until then; qutebrowser reads it
            # asynchronously when it processes its FIFO.
            if len(files) > 1:
                os.unlink(files.pop(0))
    finally:
        with contextlib.suppress(BridgeError, OSError):
            exchange(config, {"op": "detach", "controller": controller}, timeout_ms=2000)
        for path in files:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(path)


def main(mode="page"):
    parser = argparse.ArgumentParser(description="ProwseTk Qutebrowser " + mode + " userscript")
    parser.add_argument("--bridge", default=os.environ.get("PROWSETK_QUTE_BRIDGE"),
                        help="owner-only bridge.json descriptor (or PROWSETK_QUTE_BRIDGE)")
    if mode == "marionette":
        parser.add_argument("--settle-ms", type=int, default=500, help="capture delay, 0..5000 ms")
    else:
        parser.add_argument("--action-id", help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if not args.bridge:
            raise BridgeError("missing bridge descriptor")
        config = descriptor(args.bridge)
        if mode == "marionette":
            integer(args.settle_ms, 0, 5000)
            marionette(config, args.bridge, args.settle_ms)
        else:
            purpose = "action" if args.action_id else mode
            publish_snapshot(config, snapshot(config, purpose, args.action_id))
        return 0
    except (BridgeError, OSError, ValueError, UnicodeError):
        sys.stderr.write("ProwseTk Qutebrowser userscript: operation failed\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
