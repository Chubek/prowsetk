from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any

from .models import RunStatus, RunSummary, SessionSummary, utc_now


@dataclass
class RunRecord:
    summary: RunSummary
    openapi_yaml: str | None = None
    metadata: dict[str, Any] = field(default_factory=dict)


class InMemoryStore:
    def __init__(self, max_runs: int) -> None:
        self._max_runs = max_runs
        self._sessions: dict[str, SessionSummary] = {}
        self._runs: dict[str, RunRecord] = {}
        self._tasks: dict[str, asyncio.Task[None]] = {}
        self._lock = asyncio.Lock()

    async def list_sessions(self) -> list[SessionSummary]:
        async with self._lock:
            return sorted(self._sessions.values(), key=lambda item: item.updated_at, reverse=True)

    async def upsert_session(self, session: SessionSummary) -> SessionSummary:
        async with self._lock:
            now = utc_now()
            existing = self._sessions.get(session.id)
            session.created_at = existing.created_at if existing else now
            session.updated_at = now
            self._sessions[session.id] = session
            return session

    async def get_session(self, session_id: str) -> SessionSummary | None:
        async with self._lock:
            return self._sessions.get(session_id)

    async def delete_session(self, session_id: str) -> bool:
        async with self._lock:
            return self._sessions.pop(session_id, None) is not None

    async def list_runs(self) -> list[RunSummary]:
        async with self._lock:
            summaries = [record.summary for record in self._runs.values()]
            return sorted(summaries, key=lambda item: item.updated_at, reverse=True)

    async def create_run(self, summary: RunSummary) -> RunSummary:
        async with self._lock:
            if len(self._runs) >= self._max_runs:
                oldest = sorted(self._runs.values(), key=lambda item: item.summary.created_at)[0]
                self._runs.pop(oldest.summary.id, None)
                old_task = self._tasks.pop(oldest.summary.id, None)
                if old_task is not None and not old_task.done():
                    old_task.cancel()
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
            record.summary.status = status
            record.summary.error = error
            record.summary.updated_at = utc_now()
            if openapi_yaml is not None:
                record.openapi_yaml = openapi_yaml
            if metadata is not None:
                record.metadata = metadata
            return record.summary

    async def bind_task(self, run_id: str, task: asyncio.Task[None]) -> None:
        async with self._lock:
            self._tasks[run_id] = task

    async def cancel_run(self, run_id: str) -> bool:
        async with self._lock:
            record = self._runs.get(run_id)
            if record is None:
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
    compact = " ".join(text.split())
    if len(compact) <= max_len:
        return compact
    return compact[: max_len - 3] + "..."
