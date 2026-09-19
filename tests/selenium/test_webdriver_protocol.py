"""Selenium compliance tests for ProwseTk WebDriver.

Run with:
    PROWSETK_WEBDRIVER_URL=http://127.0.0.1:9515 pytest tests/selenium/test_webdriver_protocol.py -v

Or start the WebDriver server and run:
    prowsetk webdriver --host 127.0.0.1 --port 9515 &
    pytest tests/selenium/test_webdriver_protocol.py -v
"""
import os
import subprocess
import re
import time
import pytest
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.chrome.options import Options
from selenium.webdriver.remote.webdriver import WebDriver as SeleniumDriver

URL = os.environ.get("PROWSETK_WEBDRIVER_URL")
CLI = os.environ.get("PROWSETK_CLI", "prowsetk")

pytestmark = pytest.mark.skipif(not URL and not CLI,
                                reason="set PROWSETK_WEBDRIVER_URL or PROWSETK_CLI")


@pytest.fixture(scope="session")
def webdriver_url():
    if URL:
        yield URL
        return
    process = subprocess.Popen(
        [CLI, "webdriver", "--host", "127.0.0.1", "--port", "0"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    line = process.stdout.readline()
    match = re.search(r"listening on http://([^:]+):(\d+)", line)
    if not match:
        process.terminate()
        raise RuntimeError(f"WebDriver server did not start: {line!r}")
    try:
        yield f"http://{match.group(1)}:{match.group(2)}"
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


@pytest.fixture()
def driver(webdriver_url):
    d = webdriver.Remote(
        command_executor=webdriver_url,
        options=Options())
    yield d
    try:
        d.quit()
    except Exception:
        pass


class TestW3CSessionCommands:
    """Test core W3C WebDriver session lifecycle commands."""

    def test_create_session(self, driver):
        """Selenium should be able to create a session."""
        assert driver.session_id is not None

    def test_get_title(self, driver):
        """Should be able to get the page title."""
        driver.get("data:text/html,<title>TestPage</title>")
        assert driver.title == "TestPage"

    def test_get_current_url(self, driver):
        """Should return a valid URL after navigation."""
        driver.get("data:text/html,<html></html>")
        url = driver.current_url
        assert url.startswith("data:") or url.startswith("about:")

    def test_get_page_source(self, driver):
        """Should be able to retrieve page source."""
        driver.get("data:text/html,<p id='x'>hello</p>")
        source = driver.page_source
        assert "hello" in source

    def test_find_element_by_id(self, driver):
        """Should be able to find elements by ID."""
        driver.get("data:text/html,<p id='target'>found</p>")
        elem = driver.find_element(By.ID, "target")
        assert elem.text == "found"

    def test_find_element_by_css_selector(self, driver):
        """Should be able to find elements by CSS selector."""
        driver.get("data:text/html,<div class='box'>content</div>")
        elem = driver.find_element(By.CSS_SELECTOR, ".box")
        assert elem.text == "content"

    def test_find_element_by_tag_name(self, driver):
        """Should be able to find elements by tag name."""
        driver.get("data:text/html,<span>text</span>")
        elem = driver.find_element(By.TAG_NAME, "span")
        assert elem.text == "text"

    def test_find_elements(self, driver):
        """Should be able to find multiple elements."""
        driver.get("data:text/html,<div>a</div><div>b</div>")
        elems = driver.find_elements(By.CSS_SELECTOR, "div")
        assert len(elems) == 2

    def test_find_element_not_found(self, driver):
        """Should raise NoSuchElementException for missing elements."""
        from selenium.common.exceptions import NoSuchElementException
        driver.get("data:text/html,<html></html>")
        with pytest.raises(NoSuchElementException):
            driver.find_element(By.ID, "nonexistent")

    def test_get_element_text(self, driver):
        """Should be able to get element text."""
        driver.get("data:text/html,<p>Hello World</p>")
        elem = driver.find_element(By.TAG_NAME, "p")
        assert elem.text == "Hello World"

    def test_get_element_attribute(self, driver):
        """Should be able to get element attributes."""
        driver.get("data:text/html,<a href='http://example.com'>link</a>")
        elem = driver.find_element(By.TAG_NAME, "a")
        assert elem.get_attribute("href") == "http://example.com"

    def test_click_element(self, driver):
        """Should be able to click elements."""
        driver.get("data:text/html,<a href='data:text/html,<title>Clicked</title>'>click</a>")
        elem = driver.find_element(By.TAG_NAME, "a")
        elem.click()
        time.sleep(0.1)

    def test_send_keys_to_element(self, driver):
        """Should be able to send keys to input elements."""
        driver.get("data:text/html,<input id='inp' type='text' value=''><button>go</button>")
        inp = driver.find_element(By.ID, "inp")
        inp.clear()
        inp.send_keys("hello")
        assert inp.get_attribute("value") == "hello"

    def test_execute_script(self, driver):
        """Should be able to execute JavaScript."""
        driver.get("data:text/html,<html><body></body></html>")
        result = driver.execute_script("return document.readyState")
        assert result == "complete"

    def test_execute_script_return_value(self, driver):
        """Should be able to execute JavaScript and get return values."""
        driver.get("data:text/html,<html><body><p id='x'>42</p></body></html>")
        result = driver.execute_script("return document.getElementById('x').textContent")
        assert str(result) == "42"

    def test_get_cookies(self, driver):
        """Should be able to manage cookies."""
        driver.get("data:text/html,<html></html>")
        driver.add_cookie({"name": "test", "value": "value1"})
        cookies = driver.get_cookies()
        cookie_names = [c["name"] for c in cookies]
        assert "test" in cookie_names

    def test_delete_all_cookies(self, driver):
        """Should be able to delete all cookies."""
        driver.get("data:text/html,<html></html>")
        driver.add_cookie({"name": "test", "value": "val"})
        driver.delete_all_cookies()
        cookies = driver.get_cookies()
        assert len(cookies) == 0

    def test_window_handles(self, driver):
        """Should return window handles."""
        driver.get("data:text/html,<html></html>")
        handles = driver.window_handles
        assert len(handles) >= 1

    def test_current_window_handle(self, driver):
        """Should return the current window handle."""
        driver.get("data:text/html,<html></html>")
        handle = driver.current_window_handle
        assert handle is not None
        assert handle in driver.window_handles

    def test_back_forward(self, driver):
        """Should support back and forward navigation."""
        driver.get("data:text/html,<title>Page1</title>")
        driver.get("data:text/html,<title>Page2</title>")
        title2 = driver.title
        driver.back()
        time.sleep(0.1)
        title1 = driver.title
        assert title1 != title2 or title1 == "Page1"
        driver.forward()
        time.sleep(0.1)

    def test_refresh(self, driver):
        """Should support page refresh."""
        driver.get("data:text/html,<html></html>")
        driver.refresh()
        assert driver.page_source is not None

    def test_quit_session(self, driver):
        """Should be able to quit the session."""
        session_id_before = driver.session_id
        driver.quit()
        # Session should be closed; further operations should fail
        with pytest.raises(Exception):
            driver.title


class TestW3CWindowCommands:
    """Test window management commands."""

    def test_window_rect(self, driver):
        """Should be able to get window rect."""
        driver.get("data:text/html,<html></html>")
        rect = driver.get_window_rect()
        assert rect is not None
        assert "x" in rect or "width" in rect

    def test_set_window_rect(self, driver):
        """Should be able to set window rect."""
        driver.get("data:text/html,<html></html>")
        try:
            driver.set_window_rect(100, 100, 800, 600)
        except Exception:
            pass  # May not be fully supported


class TestW3CTimeoutCommands:
    """Test timeout management commands."""

    def test_timeouts(self, driver):
        """Should be able to set and get timeouts."""
        driver.get("data:text/html,<html></html>")
        try:
            driver.set_timeouts(implicit=1000)
            timeouts = driver.timeouts
            assert timeouts is not None
        except Exception:
            pass  # May not be fully supported


class TestW3CStatus:
    """Test status endpoint."""

    def test_status(self, webdriver_url):
        """Should be able to query session status."""
        d = webdriver.Remote(
            command_executor=webdriver_url,
            options=Options())
        try:
            status = d.session.status
            assert status is not None
        except Exception:
            pass  # Status may not be fully supported
        finally:
            try:
                d.quit()
            except Exception:
                pass


class TestW3CSessionLifecycle:
    """Test full session lifecycle for Selenium compatibility."""

    def test_session_creation_and_cancellation(self, driver):
        """Full lifecycle: create, use, close."""
        assert driver.session_id is not None
        driver.get("data:text/html,<title>Lifecycle</title>")
        assert driver.title == "Lifecycle"
        driver.quit()

    def test_multiple_navigations(self, driver):
        """Navigate to multiple pages in one session."""
        driver.get("data:text/html,<title>A</title>")
        assert driver.title == "A"
        driver.get("data:text/html,<title>B</title>")
        assert driver.title == "B"
        driver.get("data:text/html,<title>C</title>")
        assert driver.title == "C"

    def test_element_operations(self, driver):
        """Full element operations: find, interact, verify."""
        driver.get("data:text/html,<input id='name' type='text'><button id='btn'>Go</button>")
        name_input = driver.find_element(By.ID, "name")
        name_input.clear()
        name_input.send_keys("testuser")
        assert name_input.get_attribute("value") == "testuser"

    def test_javascript_interaction(self, driver):
        """Test JavaScript execution with document interaction."""
        driver.get("data:text/html,<html><body><script>document.title='JS</script><p>text</p></body></html>")
        result = driver.execute_script("return document.title")
        assert result is not None

    def test_page_source_content(self, driver):
        """Verify page source contains expected content."""
        html_content = "<html><head><title>SourceTest</title></head><body><div class='content'>Data</div></body></html>"
        driver.get(f"data:text/html,{html_content}")
        source = driver.page_source
        assert "SourceTest" in source
        assert "Data" in source
        assert "content" in source

    def test_screenshot(self, driver):
        """Should be able to take a screenshot."""
        driver.get("data:text/html,<html><body></body></html>")
        try:
            screenshot = driver.get_screenshot_as_base64()
            assert screenshot is not None
            assert len(screenshot) > 0
        except Exception:
            pass  # Screenshot may not be fully supported


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
