from __future__ import annotations

import logging
from typing import Any

import httpx

log = logging.getLogger(__name__)


class UpstreamError(RuntimeError):
    """Raised when the upstream ProwseTk instance returns an error."""

    def __init__(self, message: str, status_code: int | None = None, body: str | None = None) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.body = body


class ProwseTkClient:
    """Thin typed wrapper around the upstream `prowsetk serve` HTTP API."""

    def __init__(self, base_url: str, timeout_seconds: float) -> None:
        normalized = base_url.strip().rstrip("/")
        if not normalized.startswith(("http://", "https://")):
            raise ValueError("base_url must be http(s)")
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be > 0")
        self._base_url = normalized
        self._timeout = timeout_seconds
        limits = httpx.Limits(max_keepalive_connections=10, max_connections=20)
        self._client = httpx.AsyncClient(
            base_url=self._base_url,
            timeout=httpx.Timeout(timeout_seconds, connect=min(5.0, timeout_seconds)),
            limits=limits,
            headers={"User-Agent": "ProwseTk-Web-Gateway/0.3.0", "Accept": "application/json"},
            follow_redirects=False,
        )

    async def close(self) -> None:
        try:
            await self._client.aclose()
        except Exception:
            log.debug("Error closing upstream client", exc_info=True)

    async def health(self) -> dict[str, Any]:
        try:
            response = await self._client.get("/api/health")
            response.raise_for_status()
            data = response.json()
            if not isinstance(data, dict):
                raise UpstreamError("Upstream health returned non-object", status_code=response.status_code)
            return data
        except httpx.HTTPStatusError as exc:
            raise UpstreamError(
                f"Upstream health check failed: HTTP {exc.response.status_code}",
                status_code=exc.response.status_code,
                body=exc.response.text[:2000],
            ) from exc
        except httpx.RequestError as exc:
            raise UpstreamError(f"Upstream unreachable: {exc}") from exc

    async def create_session(self) -> dict[str, Any]:
        try:
            response = await self._client.post("/api/sessions", json={})
            response.raise_for_status()
            data = response.json()
            if not isinstance(data, dict) or "id" not in data:
                raise UpstreamError("Upstream did not return session id", status_code=response.status_code)
            return data
        except httpx.HTTPStatusError as exc:
            raise UpstreamError(
                exc.response.text.strip()[:2000] or f"Upstream HTTP {exc.response.status_code}",
                status_code=exc.response.status_code,
                body=exc.response.text[:2000],
            ) from exc
        except httpx.RequestError as exc:
            raise UpstreamError(f"Upstream request failed: {exc}") from exc

    async def delete_session(self, session_id: str) -> None:
        try:
            response = await self._client.delete(f"/api/sessions/{session_id}")
            response.raise_for_status()
        except httpx.HTTPStatusError as exc:
            # 404 is not fatal for delete; propagate as UpstreamError for caller to ignore.
            raise UpstreamError(
                exc.response.text.strip()[:2000] or f"Upstream HTTP {exc.response.status_code}",
                status_code=exc.response.status_code,
                body=exc.response.text[:2000],
            ) from exc
        except httpx.RequestError as exc:
            raise UpstreamError(f"Upstream request failed: {exc}") from exc

    async def navigate(self, session_id: str, url: str) -> dict[str, Any]:
        try:
            response = await self._client.post(f"/api/sessions/{session_id}/navigate", json={"url": url})
            response.raise_for_status()
            data = response.json()
            if not isinstance(data, dict):
                raise UpstreamError("Invalid navigate response", status_code=response.status_code)
            return data
        except httpx.HTTPStatusError as exc:
            raise UpstreamError(
                exc.response.text.strip()[:2000] or f"Upstream HTTP {exc.response.status_code}",
                status_code=exc.response.status_code,
                body=exc.response.text[:2000],
            ) from exc
        except httpx.RequestError as exc:
            raise UpstreamError(f"Upstream request failed: {exc}") from exc

    async def discover_endpoints(self, session_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        try:
            response = await self._client.post(f"/api/sessions/{session_id}/endpoints", json=payload)
            response.raise_for_status()
            data = response.json()
            if not isinstance(data, dict):
                raise UpstreamError("Invalid endpoints response", status_code=response.status_code)
            return data
        except httpx.HTTPStatusError as exc:
            raise UpstreamError(
                exc.response.text.strip()[:2000] or f"Upstream HTTP {exc.response.status_code}",
                status_code=exc.response.status_code,
                body=exc.response.text[:2000],
            ) from exc
        except httpx.RequestError as exc:
            raise UpstreamError(f"Upstream request failed: {exc}") from exc
