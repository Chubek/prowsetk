from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any

from .models import RunStatus, RunSummary, SessionSummary, utc_now


MAX_OPENAPI_BYTES = 2_000_000
MAX_EXCERPT_LEN = 2000


@dataclass
class RunRecord:
    summary: RunSummary
    openapi_yaml: str | None = None
    metadata: dict[str, Any] = field(default_factory=dict)


class InMemoryStore:
    """Thread-safe (async) in-memory store. Bounded by max_runs with LRU eviction."""

    def __init__(self, max_runs: int) -> None:
        if max_runs < 1:
            raise ValueError("max_runs must be >= 1")
        self._max_runs = max_runs
        self._sessions: dict[str, SessionSummary] = {}
        self._runs: dict[str, RunRecord] = {}
        self._tasks: dict[str, asyncio.Task[None]] = {}
        self._lock = asyncio.Lock()

    # -- sessions ---------------------------------------------------------------

    async def list_sessions(self) -> list[SessionSummary]:
        async with self._lock:
            return sorted(self._sessions.values(), key=lambda item: item.updated_at, reverse=True)

    async def upsert_session(self, session: SessionSummary) -> SessionSummary:
        async with self._lock:
            now = utc_now()
            existing = self._sessions.get(session.id)
            if existing is not None:
                session.created_at = existing.created_at
                # Preserve title/url if incoming is empty and existing has data.
                if not session.title and existing.title:
                    session.title = existing.title
                if not session.url and existing.url:
                    session.url = existing.url
            else:
                session.created_at = now
            session.updated_at = now
            self._sessions[session.id] = session
            return session

    async def get_session(self, session_id: str) -> SessionSummary | None:
        async with self._lock:
            return self._sessions.get(session_id)

    async def delete_session(self, session_id: str) -> bool:
        async with self._lock:
            return self._sessions.pop(session_id, None) is not None

    # -- runs ------------------------------------------------------------------

    async def list_runs(self) -> list[RunSummary]:
        async with self._lock:
            summaries = [record.summary for record in self._runs.values()]
            return sorted(summaries, key=lambda item: item.updated_at, reverse=True)

    async def create_run(self, summary: RunSummary) -> RunSummary:
        async with self._lock:
            if len(self._runs) >= self._max_runs:
                oldest = min(self._runs.values(), key=lambda item: item.summary.created_at)
                self._runs.pop(oldest.summary.id, None)
                old_task = self._tasks.pop(oldest.summary.id, None)
                if old_task is not None and not old_task.done():
                    old_task.cancel()
            if summary.id in self._runs:
                raise ValueError(f"run {summary.id} already exists")
            self._runs[summary.id] = RunRecord(summary=summary)
            return summary

    async def get_run(self, run_id: str) -> RunRecord | None:
        async with self._lock:
            return self._runs.get(run_id)

    async def update_run(
        self,
        run_id: str,
        *,
        status: RunStatus,
        error: str | None = None,
        openapi_yaml: str | None = None,
        metadata: dict[str, Any] | None = None,
    ) -> RunSummary | None:
        async with self._lock:
            record = self._runs.get(run_id)
            if record is None:
                return None
            # Do not overwrite terminal states except with same terminal.
            if record.summary.status.is_terminal and status not in (record.summary.status, RunStatus.failed):
                # Allow failed to overwrite? No. Terminal is terminal; only allow same.
                if status.is_terminal and status != record.summary.status:
                    return record.summary
            record.summary.status = status
            record.summary.error = error[:2048] if error and len(error) > 2048 else error
            record.summary.updated_at = utc_now()
            if openapi_yaml is not None:
                if len(openapi_yaml) > MAX_OPENAPI_BYTES:
                    openapi_yaml = openapi_yaml[:MAX_OPENAPI_BYTES]
                record.openapi_yaml = openapi_yaml
            if metadata is not None:
                record.metadata = metadata
            return record.summary

    async def bind_task(self, run_id: str, task: asyncio.Task[None]) -> None:
        async with self._lock:
            self._tasks[run_id] = task

        def _done(_: asyncio.Task[None]) -> None:
            # Schedule cleanup without blocking event loop; use call_soon_threadsafe fallback.
            try:
                loop = task.get_loop()
                if loop.is_closed():
                    return
                loop.call_soon(lambda: asyncio.ensure_future(self._forget_task(run_id)))
            except RuntimeError:
                pass

        task.add_done_callback(_done)

    async def _forget_task(self, run_id: str) -> None:
        async with self._lock:
            self._tasks.pop(run_id, None)

    async def cancel_run(self, run_id: str) -> bool:
        async with self._lock:
            record = self._runs.get(run_id)
            if record is None:
                return False
            if record.summary.status.is_terminal:
                return False
            task = self._tasks.get(run_id)
            if task is not None and not task.done():
                task.cancel()
            record.summary.status = RunStatus.cancelled
            record.summary.error = "Cancelled by user"
            record.summary.updated_at = utc_now()
            return True

    async def stats(self) -> tuple[int, int]:
        async with self._lock:
            active_runs = sum(1 for r in self._runs.values() if r.summary.status in (RunStatus.queued, RunStatus.running))
            return len(self._sessions), active_runs


def short_excerpt(text: str, max_len: int = 240) -> str:
    if max_len < 10:
        max_len = 10
    if max_len > MAX_EXCERPT_LEN:
        max_len = MAX_EXCERPT_LEN
    compact = " ".join((text or "").split())
    if len(compact) <= max_len:
        return compact
    return compact[: max_len - 3] + "..."
