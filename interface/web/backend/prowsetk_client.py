from __future__ import annotations

from typing import Any

import httpx


class ProwseTkClient:
    def __init__(self, base_url: str, timeout_seconds: float) -> None:
        self._client = httpx.AsyncClient(base_url=base_url.rstrip("/"), timeout=timeout_seconds)

    async def close(self) -> None:
        await self._client.aclose()

    async def health(self) -> dict[str, Any]:
        response = await self._client.get("/api/health")
        response.raise_for_status()
        return response.json()

    async def create_session(self) -> dict[str, Any]:
        response = await self._client.post("/api/sessions", json={})
        response.raise_for_status()
        return response.json()

    async def delete_session(self, session_id: str) -> None:
        response = await self._client.delete(f"/api/sessions/{session_id}")
        response.raise_for_status()

    async def navigate(self, session_id: str, url: str) -> dict[str, Any]:
        response = await self._client.post(f"/api/sessions/{session_id}/navigate", json={"url": url})
        response.raise_for_status()
        return response.json()

    async def discover_endpoints(self, session_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        response = await self._client.post(f"/api/sessions/{session_id}/endpoints", json=payload)
        response.raise_for_status()
        return response.json()
