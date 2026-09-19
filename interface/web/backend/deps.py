from __future__ import annotations

from dataclasses import dataclass

from .prowsetk_client import ProwseTkClient
from .run_service import RunService
from .settings import settings
from .store import InMemoryStore


@dataclass
class Container:
    store: InMemoryStore
    client: ProwseTkClient
    runs: RunService


def create_container(
    *,
    max_runs: int | None = None,
    upstream: str | None = None,
    timeout_seconds: float | None = None,
) -> Container:
    store = InMemoryStore(max_runs=max_runs if max_runs is not None else settings.max_runs)
    client = ProwseTkClient(
        upstream if upstream is not None else settings.prowsetk_upstream,
        timeout_seconds if timeout_seconds is not None else settings.default_timeout_seconds,
    )
    return Container(store=store, client=client, runs=RunService(store=store, client=client))
