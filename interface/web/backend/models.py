from __future__ import annotations

import re
from datetime import datetime, timezone
from enum import Enum
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, HttpUrl, field_validator

_ID_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
_OPENAPI_VERSIONS = {"3.0.0", "3.0.3", "3.1.0"}

def utc_now() -> datetime:
    return datetime.now(tz=timezone.utc)


class RunStatus(str, Enum):
    queued = "queued"
    running = "running"
    succeeded = "succeeded"
    failed = "failed"
    cancelled = "cancelled"

    @property
    def is_terminal(self) -> bool:
        return self in (RunStatus.succeeded, RunStatus.failed, RunStatus.cancelled)


class SessionSummary(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    id: str = Field(..., pattern=r"^[A-Za-z0-9_-]{1,64}$", max_length=64)
    title: str = Field(default="", max_length=512)
    url: str = Field(default="", max_length=2048)
    created_at: datetime = Field(default_factory=utc_now)
    updated_at: datetime = Field(default_factory=utc_now)

    @field_validator("title", "url", mode="before")
    @classmethod
    def _coerce_str(cls, v: Any) -> str:
        if v is None:
            return ""
        return str(v)[:2048] if isinstance(v, str) and len(v) > 2048 else str(v)


class SessionCreateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    preferred_id: str | None = Field(default=None, pattern=r"^[A-Za-z0-9_-]{1,64}$", max_length=64)


class SessionNavigateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    url: HttpUrl = Field(..., max_length=2048)


class SessionNavigateResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: str = Field(..., pattern=r"^[A-Za-z0-9_-]{1,64}$")
    title: str = Field(..., max_length=512)
    url: str = Field(..., max_length=2048)
    excerpt: str = Field(..., max_length=2000)


class DiscoverOptions(BaseModel):
    """Validated subset of endpoint discovery knobs. Security flags are enforced."""

    model_config = ConfigDict(extra="forbid")

    follow_links: bool = False
    observe_network: bool = True
    inspect_scripts: bool = True
    infer_schemas: bool = True
    include_provenance: bool = True
    redact_secrets: bool = True
    max_depth: int = Field(default=1, ge=1, le=10)
    max_pages: int = Field(default=20, ge=1, le=100)
    minimum_confidence: float = Field(default=0.45, ge=0.0, le=1.0)
    openapi_version: Literal["3.0.0", "3.0.3", "3.1.0"] = "3.1.0"

    @field_validator("openapi_version", mode="before")
    @classmethod
    def _validate_version(cls, v: Any) -> str:
        if v in _OPENAPI_VERSIONS:
            return v
        return "3.1.0"


class RunCreateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    url: HttpUrl = Field(..., max_length=2048)
    session_id: str | None = Field(default=None, pattern=r"^[A-Za-z0-9_-]{1,64}$", max_length=64)
    discover_options: DiscoverOptions | dict[str, Any] = Field(default_factory=DiscoverOptions)

    @field_validator("discover_options", mode="before")
    @classmethod
    def _coerce_options(cls, v: Any) -> DiscoverOptions:
        if v is None:
            return DiscoverOptions()
        if isinstance(v, DiscoverOptions):
            v.redact_secrets = True
            v.include_provenance = True
            return v
        if isinstance(v, dict):
            # Strip unknown keys silently, enforce security flags.
            allowed = {
                "follow_links", "observe_network", "inspect_scripts", "infer_schemas",
                "include_provenance", "redact_secrets", "max_depth", "max_pages",
                "minimum_confidence", "openapi_version",
            }
            filtered = {k: v for k, v in v.items() if k in allowed}
            filtered["redact_secrets"] = True
            filtered["include_provenance"] = True
            return DiscoverOptions.model_validate(filtered)
        return DiscoverOptions.model_validate(v)

    def sanitized_options(self) -> dict[str, Any]:
        opts = self.discover_options
        if isinstance(opts, DiscoverOptions):
            return opts.model_dump(mode="python")
        return DiscoverOptions.model_validate(opts).model_dump(mode="python")


class RunSummary(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    id: str = Field(..., pattern=r"^run-[A-Za-z0-9_-]{1,64}$", max_length=68)
    status: RunStatus
    session_id: str = Field(default="", pattern=r"^[A-Za-z0-9_-]{0,64}$", max_length=64)
    created_at: datetime = Field(default_factory=utc_now)
    updated_at: datetime = Field(default_factory=utc_now)
    url: HttpUrl | str = Field(..., max_length=2048)
    error: str | None = Field(default=None, max_length=2048)


class RunResult(BaseModel):
    model_config = ConfigDict(extra="forbid")

    run: RunSummary
    openapi_yaml: str | None = Field(default=None, max_length=2_000_000)
    metadata: dict[str, Any] = Field(default_factory=dict)


class HealthResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    service: str = Field(..., max_length=128)
    environment: str = Field(..., max_length=64)
    status: Literal["ok", "degraded"] = "ok"
    upstream_status: Literal["ok", "unreachable"] = "unreachable"
    active_sessions: int = Field(..., ge=0)
    active_runs: int = Field(..., ge=0)
    time: datetime = Field(default_factory=utc_now)


class ApiError(BaseModel):
    model_config = ConfigDict(extra="forbid")

    error: str = Field(..., max_length=64)
    message: str = Field(..., max_length=2048)


class ErrorResponse(BaseModel):
    """Wire shape used by API.yaml: { detail: string }."""

    model_config = ConfigDict(extra="forbid")

    detail: str = Field(..., max_length=2048)
