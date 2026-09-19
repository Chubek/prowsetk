from __future__ import annotations

import asyncio
import logging
import uuid

import httpx

from .models import RunCreateRequest, RunStatus, RunSummary, SessionSummary
from .prowsetk_client import ProwseTkClient, UpstreamError
from .store import InMemoryStore, short_excerpt

log = logging.getLogger(__name__)


class RunService:
    def __init__(self, store: InMemoryStore, client: ProwseTkClient, max_concurrency: int = 3) -> None:
        self._store = store
        self._client = client
        self._sem = asyncio.Semaphore(max(1, max_concurrency))

    async def create_run(self, request: RunCreateRequest) -> RunSummary:
        run_id = f"run-{uuid.uuid4().hex[:12]}"
        opts = request.sanitized_options()
        run = RunSummary(
            id=run_id,
            status=RunStatus.queued,
            session_id=request.session_id or "",
            url=str(request.url),
        )
        await self._store.create_run(run)
        worker = asyncio.create_task(self._execute(run.id, request, opts), name=f"web-run-{run.id}")
        await self._store.bind_task(run.id, worker)
        log.info("Queued run %s url=%s session=%s", run_id, request.url, request.session_id)
        return run

    async def _execute(self, run_id: str, request: RunCreateRequest, sanitized_options: dict) -> None:
        # Use semaphore to bound concurrent upstream work.
        async with self._sem:
            try:
                await self._store.update_run(run_id, status=RunStatus.running)

                session_id = request.session_id
                if not session_id:
                    payload = await self._client.create_session()
                    session_id = str(payload.get("id") or "")
                    if not session_id:
                        raise RuntimeError("Unable to allocate session")

                nav = await self._client.navigate(session_id, str(request.url))

                # Build final options: start from sanitized, ensure security flags.
                options = dict(sanitized_options)
                options["redact_secrets"] = True
                options["include_provenance"] = True

                endpoints = await self._client.discover_endpoints(session_id, options)

                await self._store.upsert_session(
                    SessionSummary(
                        id=session_id,
                        title=str(nav.get("title") or str(request.url))[:512],
                        url=str(nav.get("url") or str(request.url))[:2048],
                    )
                )

                openapi_yaml = endpoints.get("openapi_yaml")
                if openapi_yaml is not None and not isinstance(openapi_yaml, str):
                    openapi_yaml = str(openapi_yaml)

                metadata = {
                    "title": str(nav.get("title") or str(request.url))[:512],
                    "url": str(nav.get("url") or str(request.url))[:2048],
                    "excerpt": short_excerpt(str(nav.get("text") or "")),
                    "endpoint_count": int(endpoints.get("endpoint_count", 0) or 0),
                    "warnings": endpoints.get("warnings", []) if isinstance(endpoints.get("warnings"), list) else [],
                    "timestamp": str(endpoints.get("timestamp", "") or ""),
                }

                await self._store.update_run(
                    run_id,
                    status=RunStatus.succeeded,
                    openapi_yaml=openapi_yaml if isinstance(openapi_yaml, str) else "",
                    metadata=metadata,
                )
                log.info("Run %s succeeded endpoints=%s", run_id, metadata["endpoint_count"])
            except asyncio.CancelledError:
                # Only mark cancelled if not already terminal.
                record = await self._store.get_run(run_id)
                if record is not None and not record.summary.status.is_terminal:
                    await self._store.update_run(run_id, status=RunStatus.cancelled, error="Cancelled by user")
                log.info("Run %s cancelled", run_id)
                raise
            except (UpstreamError, httpx.HTTPStatusError) as error:
                msg = str(error).strip()[:2048] or "Upstream failure"
                # Extract status code if UpstreamError
                status_hint = getattr(error, "status_code", None)
                if status_hint is not None:
                    msg = f"Upstream HTTP {status_hint}: {msg}" if not msg.startswith("Upstream") else msg
                await self._store.update_run(run_id, status=RunStatus.failed, error=msg)
                log.warning("Run %s failed (upstream): %s", run_id, msg)
            except Exception as error:
                msg = str(error).strip()[:2048] or error.__class__.__name__
                await self._store.update_run(run_id, status=RunStatus.failed, error=msg)
                log.exception("Run %s failed: %s", run_id, msg)
