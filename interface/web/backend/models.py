from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, HttpUrl


def utc_now() -> datetime:
    return datetime.now(tz=timezone.utc)


class RunStatus(str, Enum):
    queued = "queued"
    running = "running"
    succeeded = "succeeded"
    failed = "failed"
    cancelled = "cancelled"


class SessionSummary(BaseModel):
    id: str
    title: str = ""
    url: str = ""
    created_at: datetime = Field(default_factory=utc_now)
    updated_at: datetime = Field(default_factory=utc_now)


class SessionCreateRequest(BaseModel):
    preferred_id: str | None = None


class SessionNavigateRequest(BaseModel):
    url: HttpUrl


class SessionNavigateResponse(BaseModel):
    id: str
    title: str
    url: str
    excerpt: str


class RunCreateRequest(BaseModel):
    url: HttpUrl
    session_id: str | None = None
    discover_options: dict[str, Any] = Field(default_factory=dict)


class RunSummary(BaseModel):
    id: str
    status: RunStatus
    session_id: str = ""
    created_at: datetime = Field(default_factory=utc_now)
    updated_at: datetime = Field(default_factory=utc_now)
    url: str
    error: str | None = None


class RunResult(BaseModel):
    model_config = ConfigDict(extra="allow")

    run: RunSummary
    openapi_yaml: str | None = None
    metadata: dict[str, Any] = Field(default_factory=dict)


class HealthResponse(BaseModel):
    service: str
    environment: str
    status: str
    upstream_status: str
    active_sessions: int
    active_runs: int
    time: datetime = Field(default_factory=utc_now)


class ApiError(BaseModel):
    error: str
    message: str
