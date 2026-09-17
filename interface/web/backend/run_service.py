from __future__ import annotations

import asyncio
import uuid
from typing import Any

import httpx

from .models import RunCreateRequest, RunStatus, RunSummary, SessionSummary
from .prowsetk_client import ProwseTkClient
from .store import InMemoryStore, short_excerpt


class RunService:
    def __init__(self, store: InMemoryStore, client: ProwseTkClient) -> None:
        self._store = store
        self._client = client

    async def create_run(self, request: RunCreateRequest) -> RunSummary:
        run = RunSummary(
            id=f"run-{uuid.uuid4().hex[:12]}",
            status=RunStatus.queued,
            session_id=request.session_id or "",
            url=str(request.url),
        )
        await self._store.create_run(run)
        worker = asyncio.create_task(self._execute(run.id, request), name=f"web-run-{run.id}")
        await self._store.bind_task(run.id, worker)
        return run

    async def _execute(self, run_id: str, request: RunCreateRequest) -> None:
        try:
            await self._store.update_run(run_id, status=RunStatus.running)

            session_id = request.session_id
            if not session_id:
                payload = await self._client.create_session()
                session_id = payload.get("id")
            if not session_id:
                raise RuntimeError("Unable to allocate session")

            nav = await self._client.navigate(session_id, str(request.url))

            options: dict[str, Any] = {
                "follow_links": False,
                "observe_network": True,
                "infer_schemas": True,
                "include_provenance": True,
                "redact_secrets": True,
                "max_depth": 1,
                "max_pages": 20,
                "minimum_confidence": 0.45,
                "openapi_version": "3.1.0",
            }
            options.update(request.discover_options)
            endpoints = await self._client.discover_endpoints(session_id, options)

            await self._store.upsert_session(
                SessionSummary(
                    id=session_id,
                    title=nav.get("title") or str(request.url),
                    url=nav.get("url") or str(request.url),
                )
            )

            metadata = {
                "title": nav.get("title") or str(request.url),
                "url": nav.get("url") or str(request.url),
                "excerpt": short_excerpt(nav.get("text") or ""),
                "endpoint_count": endpoints.get("endpoint_count", 0),
                "timestamp": endpoints.get("timestamp", ""),
            }

            await self._store.update_run(
                run_id,
                status=RunStatus.succeeded,
                openapi_yaml=endpoints.get("openapi_yaml", ""),
                metadata=metadata,
            )
        except asyncio.CancelledError:
            await self._store.update_run(run_id, status=RunStatus.cancelled, error="Cancelled by user")
            raise
        except httpx.HTTPStatusError as error:
            message = error.response.text.strip() or f"Upstream HTTP {error.response.status_code}"
            await self._store.update_run(run_id, status=RunStatus.failed, error=message)
        except Exception as error:
            await self._store.update_run(run_id, status=RunStatus.failed, error=str(error))
