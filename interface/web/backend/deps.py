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


def create_container() -> Container:
    store = InMemoryStore(max_runs=settings.max_runs)
    client = ProwseTkClient(settings.prowsetk_upstream, settings.default_timeout_seconds)
    return Container(store=store, client=client, runs=RunService(store=store, client=client))
