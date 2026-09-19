"""Playwright compatibility tests for ProwseTk's native CDP bridge.

The suite is intentionally small and deterministic. It exercises the same
browser lifecycle that Playwright uses for a remote Chromium connection, while
using a data URL so no external network is required.
"""

import os
import re
import subprocess
import importlib.util

import pytest

PLAYWRIGHT_AVAILABLE = (
    importlib.util.find_spec("playwright.sync_api") is not None
)
if PLAYWRIGHT_AVAILABLE:
    from playwright.sync_api import sync_playwright


CLI = os.environ.get("PROWSETK_CLI")
pytestmark = [
    pytest.mark.skipif(not PLAYWRIGHT_AVAILABLE,
                       reason="Playwright is not installed"),
    pytest.mark.skipif(not CLI,
                       reason="set PROWSETK_CLI to the built prowsetk executable"),
]


@pytest.fixture(scope="session")
def cdp_url():
    process = subprocess.Popen(
        [CLI, "playwright", "--host", "127.0.0.1", "--port", "0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    line = process.stdout.readline()
    match = re.search(r"listening on http://([^:]+):(\d+)", line)
    if not match:
        process.terminate()
        process.wait(timeout=5)
        if "cannot create listening socket" in line:
            pytest.skip("loopback sockets are unavailable in this environment")
        raise RuntimeError(f"Playwright/CDP server did not start: {line!r}")
    try:
        yield f"http://{match.group(1)}:{match.group(2)}"
    finally:
        process.terminate()
        process.wait(timeout=5)


def test_playwright_connect_over_cdp(cdp_url):
    with sync_playwright() as playwright_instance:
        browser = playwright_instance.chromium.connect_over_cdp(cdp_url)
        try:
            assert browser.contexts
            context = browser.contexts[0]
            page = context.pages[0] if context.pages else context.new_page()
            page.goto(
                "data:text/html,"
                "<title>Playwright</title>"
                "<input id='name' value='before'>"
                "<p id='message'>hello</p>"
            )
            assert page.title() == "Playwright"
            assert page.locator("#message").inner_text() == "hello"
            assert page.locator("#name").input_value() == "before"
        finally:
            browser.close()
