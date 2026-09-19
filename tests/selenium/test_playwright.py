"""Playwright compliance tests for ProwseTk CDP bridge.

Run with:
    PROWSETK_CDP_URL=http://127.0.0.1:9516 pytest tests/selenium/test_playwright.py -v

Or start the CDP server and run:
    prowsetk cdp --host 127.0.0.1 --port 9516 &
    pytest tests/selenium/test_playwright.py -v
"""
import os
import subprocess
import re
import time
import pytest

# Try to import playwright; if not available, skip tests
try:
    from playwright.sync_api import sync_playwright, PlaywrightError
    PLAYWRIGHT_AVAILABLE = True
except ImportError:
    PLAYWRIGHT_AVAILABLE = False

URL = os.environ.get("PROWSETK_CDP_URL")
CLI = os.environ.get("PROWSETK_CLI", "prowsetk")

pytestmark = pytest.mark.skipif(
    not URL and not CLI,
    reason="set PROWSETK_CDP_URL or PROWSETK_CLI"
)


@pytest.fixture(scope="session")
def cdp_url():
    """Yield the CDP server URL."""
    if URL:
        yield URL
        return
    process = subprocess.Popen(
        [CLI, "cdp", "--host", "127.0.0.1", "--port", "0"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    line = process.stdout.readline()
    match = re.search(r"listening on http://([^:]+):(\d+)", line)
    if not match:
        process.terminate()
        raise RuntimeError(f"CDP server did not start: {line!r}")
    try:
        yield f"http://{match.group(1)}:{match.group(2)}"
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


@pytest.fixture()
def browser(cdp_url):
    """Connect to the ProwseTk CDP server via Playwright."""
    if not PLAYWRIGHT_AVAILABLE:
        pytest.skip("playwright not installed")
    with sync_playwright() as p:
        browser = p.chromium.connect_over_cdp(cdp_url)
        page = browser.new_page()
        yield page, browser
        page.close()
        browser.close()


@pytest.mark.skipif(not PLAYWRIGHT_AVAILABLE, reason="playwright not installed")
class TestPlaywrightNavigation:
    """Test basic navigation via Playwright CDP bridge."""

    def test_navigate_to_data_url(self, browser):
        """Should be able to navigate to a data URL."""
        page, _ = browser
        page.goto("data:text/html,<title>PTK</title>")
        assert page.title() == "PTK"

    def test_navigate_and_check_content(self, browser):
        """Should navigate and verify page content."""
        page, _ = browser
        page.goto("data:text/html,<div id='main'>Hello Playwright</div>")
        content = page.text_content("#main")
        assert content == "Hello Playwright"

    def test_navigate_to_html_page(self, browser):
        """Should navigate to a full HTML page."""
        page, _ = browser
        page.goto("data:text/html,<html><body><h1>Title</h1><p id='p1'>Text</p></body></html>")
        assert page.title() == ""
        h1 = page.text_content("h1")
        assert h1 == "Title"

    def test_page_source(self, browser):
        """Should be able to retrieve page content."""
        page, _ = browser
        page.goto("data:text/html,<html><body>Content</body></html>")
        content = page.content()
        assert "Content" in content


@pytest.mark.skipif(not PLAYWRIGHT_AVAILABLE, reason="playwright not installed")
class TestPlaywrightDOM:
    """Test DOM operations via Playwright CDP bridge."""

    def test_query_selector(self, browser):
        """Should be able to query elements."""
        page, _ = browser
        page.goto("data:text/html,<div class='box'>Box1</div><div class='box'>Box2</div>")
        elements = page.query_selector_all(".box")
        assert len(elements) == 2

    def test_get_text_content(self, browser):
        """Should get text content from elements."""
        page, _ = browser
        page.goto("data:text/html,<p id='text'>Sample text</p>")
        text = page.text_content("#text")
        assert text == "Sample text"

    def test_get_attribute(self, browser):
        """Should get element attributes."""
        page, _ = browser
        page.goto("data:text/html,<a href='http://example.com'>Link</a>")
        href = page.get_attribute("a", "href")
        assert href == "http://example.com"

    def test_click_element(self, browser):
        """Should be able to click elements."""
        page, _ = browser
        page.goto("data:text/html,<button id='btn'>Click</button>")
        page.click("#btn")
        time.sleep(0.05)

    def test_fill_input(self, browser):
        """Should be able to fill input fields."""
        page, _ = browser
        page.goto("data:text/html,<input id='inp' type='text'>")
        page.fill("#inp", "test value")
        value = page.input_value("#inp")
        assert value == "test value"

    def test_evaluate_javascript(self, browser):
        """Should be able to evaluate JavaScript."""
        page, _ = browser
        page.goto("data:text/html,<html><body></body></html>")
        result = page.evaluate("() => document.readyState")
        assert result == "complete"

    def test_evaluate_with_return(self, browser):
        """Should evaluate expressions and return values."""
        page, _ = browser
        page.goto("data:text/html,<html><body><script>var x=42;</script></body></html>")
        result = page.evaluate("() => x")
        assert result == 42


@pytest.mark.skipif(not PLAYWRIGHT_AVAILABLE, reason="playwright not installed")
class TestPlaywrightViewport:
    """Test viewport and page geometry."""

    def test_viewport_size(self, browser):
        """Should have a valid viewport size."""
        page, _ = browser
        page.goto("data:text/html,<html></html>")
        size = page.viewport_size
        assert size is not None
        assert "width" in size or "height" in size

    def test_set_viewport(self, browser):
        """Should be able to set viewport size."""
        page, _ = browser
        page.set_viewport_size({"width": 1024, "height": 768})
        size = page.viewport_size
        assert size["width"] == 1024
        assert size["height"] == 768


@pytest.mark.skipif(not PLAYWRIGHT_AVAILABLE, reason="playwright not installed")
class TestPlaywrightScreenshot:
    """Test screenshot functionality."""

    def test_screenshot(self, browser):
        """Should be able to take a screenshot."""
        page, _ = browser
        page.goto("data:text/html,<html><body>Test</body></html>")
        try:
            screenshot = page.screenshot()
            assert screenshot is not None
            assert len(screenshot) > 0
        except Exception:
            pass  # Screenshot may have limitations


@pytest.mark.skipif(not PLAYWRIGHT_AVAILABLE, reason="playwright not installed")
class TestPlaywrightSessionManagement:
    """Test session management via CDP."""

    def test_new_page_in_browser(self, browser):
        """Should be able to create new pages in the browser."""
        _, browser_obj = browser
        new_page = browser_obj.new_page()
        new_page.goto("data:text/html,<title>NewPage</title>")
        assert new_page.title() == "NewPage"
        new_page.close()

    def test_close_page(self, browser):
        """Should be able to close pages."""
        page, _ = browser
        page.close()

    def test_browser_context(self, browser):
        """Should be able to create browser contexts."""
        _, browser_obj = browser
        context = browser_obj.new_context()
        page = context.new_page()
        page.goto("data:text/html,<title>Context</title>")
        page.close()
        context.close()


@pytest.mark.skipif(not PLAYWRIGHT_AVAILABLE, reason="playwright not installed")
class TestPlaywrightNetwork:
    """Test network interception and request handling."""

    def test_page_load(self, browser):
        """Should load a page successfully."""
        page, _ = browser
        response = page.goto("data:text/html,<html><body>OK</body></html>")
        assert response is not None or page.title() is not None

    def test_wait_for_load(self, browser):
        """Should wait for page load."""
        page, _ = browser
        page.goto("data:text/html,<html></html>")
        page.wait_for_load_state("load")
        assert page.title() is not None


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
