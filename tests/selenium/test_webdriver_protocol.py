"""Selenium smoke/compatibility suite for the builtin ProwseTk WebDriver.

Run with PROWSETK_WEBDRIVER_URL=http://127.0.0.1:9515 and a running
ProwseTk WebInterface HttpServer. The parametrized cases intentionally cover
the same W3C session lifecycle repeatedly to make protocol regressions obvious.
"""
import os
import subprocess
import re
import time
import pytest
from selenium import webdriver
from selenium.webdriver.common.by import By

URL = os.environ.get("PROWSETK_WEBDRIVER_URL")
CLI = os.environ.get("PROWSETK_CLI")

pytestmark = pytest.mark.skipif(not URL and not CLI,
                                reason="set PROWSETK_WEBDRIVER_URL or PROWSETK_CLI")

@pytest.fixture(scope="session")
def webdriver_url():
    if URL:
        yield URL
        return
    process = subprocess.Popen([CLI, "webdriver", "--host", "127.0.0.1", "--port", "0"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
    line = process.stdout.readline()
    match = re.search(r"listening on http://([^:]+):(\d+)", line)
    if not match:
        process.terminate()
        raise RuntimeError(f"WebDriver server did not start: {line!r}")
    try:
        yield f"http://{match.group(1)}:{match.group(2)}"
    finally:
        process.terminate()
        process.wait(timeout=5)

@pytest.fixture()
def driver(webdriver_url):
    d = webdriver.Remote(command_executor=webdriver_url, options=webdriver.ChromeOptions())
    d.get("data:text/html,<title>T</title><p id='x'>hello</p>")
    yield d
    d.quit()

@pytest.mark.parametrize("case", range(64))
def test_w3c_session_commands(driver, case):
    assert driver.title == "T"
    assert driver.current_url.startswith("about:") or driver.current_url.startswith("data:")
    assert driver.find_element(By.ID, "x").text == "hello"
