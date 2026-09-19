"""Playwright/CDP compliance tests for ProwseTk.

Run with:
    PROWSETK_WEBDRIVER_URL=http://127.0.0.1:9515 pytest tests/selenium/test_playwright.py -v

Or start the CDP server and run:
    prowsetk cdp --host 127.0.0.1 --port 0 &
    pytest tests/selenium/test_playwright.py -v
"""
import json
import os
import subprocess
import re
import time
import urllib.request
import urllib.error
import pytest

URL = os.environ.get("PROWSETK_WEBDRIVER_URL")
CLI = os.environ.get("PROWSETK_CLI", "prowsetk")

pytestmark = pytest.mark.skipif(not URL and not CLI,
                                reason="set PROWSETK_WEBDRIVER_URL or PROWSETK_CLI")


@pytest.fixture(scope="session")
def cdp_url():
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


def http_get(base_url, path):
    url = base_url.rstrip("/") + "/" + path.lstrip("/")
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = resp.read().decode()
            return resp.status, body
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return None, str(e)


def http_post(base_url, path, payload):
    url = base_url.rstrip("/") + "/" + path.lstrip("/")
    data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, headers={
        "Content-Type": "application/json",
        "Accept": "application/json",
    }, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = resp.read().decode()
            return resp.status, body
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return None, str(e)


def http_delete(base_url, path):
    url = base_url.rstrip("/") + "/" + path.lstrip("/")
    req = urllib.request.Request(url, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = resp.read().decode()
            return resp.status, body
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return None, str(e)


class TestCDPVersionEndpoint:
    """Test the CDP /json/version endpoint."""

    def test_version_returns_200(self, cdp_url):
        """Should return 200 for /json/version."""
        status, body = http_get(cdp_url, "/json/version")
        assert status == 200

    def test_version_contains_product(self, cdp_url):
        """Should contain ProwseTk product info."""
        status, body = http_get(cdp_url, "/json/version")
        assert status == 200
        data = json.loads(body)
        assert "Browser" in data
        assert "ProwseTk" in data["Browser"]

    def test_version_contains_protocol(self, cdp_url):
        """Should contain protocol version."""
        status, body = http_get(cdp_url, "/json/version")
        assert status == 200
        data = json.loads(body)
        assert "Protocol-Version" in data

    def test_version_trailing_slash(self, cdp_url):
        """Should handle /json/version/ with trailing slash."""
        status, body = http_get(cdp_url, "/json/version/")
        assert status == 200
        data = json.loads(body)
        assert "Browser" in data


class TestCDPTargetList:
    """Test the CDP /json/list endpoint."""

    def test_list_returns_200(self, cdp_url):
        """Should return 200 for /json/list."""
        status, body = http_get(cdp_url, "/json/list")
        assert status == 200

    def test_list_is_array(self, cdp_url):
        """Should return a JSON array of targets."""
        status, body = http_get(cdp_url, "/json/list")
        assert status == 200
        data = json.loads(body)
        assert isinstance(data, list)

    def test_list_contains_target_info(self, cdp_url):
        """Should contain target info fields."""
        status, body = http_get(cdp_url, "/json/list")
        assert status == 200
        data = json.loads(body)
        if data:
            target = data[0]
            assert "id" in target
            assert "type" in target
            assert "url" in target
            assert "title" in target

    def test_json_endpoint(self, cdp_url):
        """Should return 200 for /json."""
        status, body = http_get(cdp_url, "/json")
        assert status == 200

    def test_json_trailing_slash(self, cdp_url):
        """Should handle /json/ with trailing slash."""
        status, body = http_get(cdp_url, "/json/")
        assert status == 200


class TestCDPNotFound:
    """Test CDP 404 handling."""

    def test_unknown_endpoint_returns_404(self, cdp_url):
        """Should return 404 for unknown endpoints."""
        status, _ = http_get(cdp_url, "/json/unknown")
        assert status == 404

    def test_post_to_get_only_returns_405(self, cdp_url):
        """Should return 405 for POST to GET-only endpoints."""
        status, _ = http_post(cdp_url, "/json/version", {})
        assert status == 405


class TestCDPWebDriverIntegration:
    """Test WebDriver protocol through the CDP-enabled interface."""

    def test_create_session(self, cdp_url):
        """Should create a session via WebDriver."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        assert "sessionId" in data.get("value", {}) or "sessionId" in data

    def test_navigate_via_webdriver(self, cdp_url):
        """Should navigate via WebDriver protocol."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_post(cdp_url, f"/session/{session_id}/url",
                                  {"url": "data:text/html,<title>NavTest</title>"})
        assert status == 200

        status, body = http_get(cdp_url, f"/session/{session_id}/title")
        assert status == 200
        data = json.loads(body)
        value = data.get("value", {})
        title = value.get("value", "") if isinstance(value, dict) else str(value)
        assert "NavTest" in title or "NavTest" in body

    def test_get_page_source(self, cdp_url):
        """Should retrieve page source via WebDriver."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_post(cdp_url, f"/session/{session_id}/url",
                                  {"url": "data:text/html,<p id='src'>hello</p>"})
        assert status == 200

        status, body = http_get(cdp_url, f"/session/{session_id}/page_source")
        assert status == 200
        assert "hello" in body

    def test_get_window_rect(self, cdp_url):
        """Should be able to get window rect."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_get(cdp_url, f"/session/{session_id}/window/rect")
        assert status == 200
        data = json.loads(body)
        rect = data.get("value", {})
        assert "x" in rect or "width" in rect

    def test_get_status(self, cdp_url):
        """Should be able to query session status."""
        status, _ = http_get(cdp_url, "/session")
        assert status == 200

    def test_get_timeouts(self, cdp_url):
        """Should be able to get timeouts."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_get(cdp_url, f"/session/{session_id}/timeouts")
        assert status == 200
        data = json.loads(body)
        assert "value" in data

    def test_get_network(self, cdp_url):
        """Should be able to get network info."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_get(cdp_url, f"/session/{session_id}/network")
        assert status == 200
        data = json.loads(body)
        assert "value" in data

    def test_delete_session(self, cdp_url):
        """Should be able to delete a session."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, _ = http_delete(cdp_url, f"/session/{session_id}")
        assert status == 200


class TestCDPWebDriverFindElements:
    """Test element finding via WebDriver."""

    def test_find_element_by_id(self, cdp_url):
        """Should find elements by ID."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, _ = http_post(cdp_url, f"/session/{session_id}/url",
                               {"url": "data:text/html,<div id='main'>Content</div>"})

        status, body = http_post(cdp_url, f"/session/{session_id}/element",
                                  {"using": "css selector", "value": "#main"})
        assert status == 200
        assert "element-6066" in body or "element-" in body

    def test_find_elements_by_class(self, cdp_url):
        """Should find multiple elements by class."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, _ = http_post(cdp_url, f"/session/{session_id}/url",
                               {"url": "data:text/html,<div class='box'>a</div><div class='box'>b</div>"})

        status, body = http_post(cdp_url, f"/session/{session_id}/elements",
                                  {"using": "css selector", "value": ".box"})
        assert status == 200
        data = json.loads(body)
        assert "value" in data
        assert len(data["value"]) >= 2


class TestCDPJavaScriptEvaluation:
    """Test JavaScript evaluation via WebDriver."""

    def test_execute_script(self, cdp_url):
        """Should be able to execute JavaScript."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, _ = http_post(cdp_url, f"/session/{session_id}/url",
                               {"url": "data:text/html,<html><body></body></html>"})

        status, body = http_post(cdp_url, f"/session/{session_id}/execute/sync",
                                  {"script": "document.readyState"})
        assert status == 200
        data = json.loads(body)
        assert "complete" in body or "complete" in data.get("value", {}).get("value", "")


class TestCDPViewportAndScreenshot:
    """Test viewport and screenshot endpoints."""

    def test_window_rect_set(self, cdp_url):
        """Should be able to set window rect."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_post(cdp_url, f"/session/{session_id}/window/rect",
                                  {"x": 100, "y": 100, "width": 800, "height": 600})
        assert status == 200

    def test_screenshot(self, cdp_url):
        """Should be able to take a screenshot."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_get(cdp_url, f"/session/{session_id}/screenshot")
        assert status == 200
        data = json.loads(body)
        assert "value" in data
        screenshot_data = data["value"] if isinstance(data["value"], str) else data["value"].get("value", "")
        assert isinstance(screenshot_data, str) and len(screenshot_data) > 0


class TestCDPNavigation:
    """Test navigation commands."""

    def test_navigate_command(self, cdp_url):
        """Should support POST /session/{id}/navigate."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_post(cdp_url, f"/session/{session_id}/navigate",
                                  {"url": "data:text/html,<title>NavTest</title>"})
        assert status == 200

    def test_back_forward_refresh(self, cdp_url):
        """Should support back, forward, refresh commands."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        for cmd in ["back", "forward", "refresh"]:
            status, body = http_post(cdp_url, f"/session/{session_id}/{cmd}", {})
            assert status == 200


class TestCDPNewWindow:
    """Test window management."""

    def test_window_new(self, cdp_url):
        """Should support creating new windows."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_post(cdp_url, f"/session/{session_id}/window/new", {})
        assert status == 200
        data = json.loads(body)
        assert "value" in data

    def test_window_delete(self, cdp_url):
        """Should support deleting windows."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, _ = http_delete(cdp_url, f"/session/{session_id}/window")
        assert status == 200


class TestCDPFrameAndActions:
    """Test frame and action commands."""

    def test_frame_switch(self, cdp_url):
        """Should support frame switching."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_post(cdp_url, f"/session/{session_id}/frame",
                                  {"id": "main"})
        assert status == 200

    def test_actions(self, cdp_url):
        """Should support actions commands."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, body = http_post(cdp_url, f"/session/{session_id}/actions", {})
        assert status == 200

    def test_send_keys(self, cdp_url):
        """Should support sending keys to elements."""
        status, body = http_post(cdp_url, "/session", {})
        assert status == 200
        data = json.loads(body)
        session_id = data.get("value", {}).get("sessionId",
                         data.get("sessionId"))
        if not session_id:
            pytest.skip("Session creation did not return sessionId")

        status, _ = http_post(cdp_url, f"/session/{session_id}/url",
                               {"url": "data:text/html,<input id='inp' type='text'/>"})

        status, body = http_post(cdp_url, f"/session/{session_id}/element",
                                  {"using": "css selector", "value": "#inp"})
        assert status == 200
        elem_data = json.loads(body)
        element_id = elem_data.get("value", {}).get("value", {}).get(
            "element-6066-11e4-a52e-4f735466cecf")
        if not element_id:
            pytest.skip("Could not find element")

        status, body = http_post(cdp_url,
                                  f"/session/{session_id}/element/{element_id}/send-keys",
                                  {"text": "hello"})
        assert status == 200


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
