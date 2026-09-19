from __future__ import annotations

from pydantic import Field, HttpUrl, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="PROWSETK_WEB_", extra="ignore")

    service_name: str = Field(default="ProwseTk Service Gateway", max_length=128)
    environment: str = Field(default="development", max_length=64)
    host: str = Field(default="127.0.0.1", max_length=253)
    port: int = Field(default=8090, ge=1, le=65535)
    default_timeout_seconds: float = Field(default=25.0, ge=1.0, le=300.0)
    poll_interval_seconds: float = Field(default=1.2, ge=0.1, le=10.0)
    max_runs: int = Field(default=300, ge=1, le=5000)
    prowsetk_upstream: str = Field(default="http://127.0.0.1:8080", max_length=2048)

    @field_validator("host", mode="before")
    @classmethod
    def _strip_host(cls, v: str) -> str:
        return str(v).strip() if isinstance(v, str) else v

    @field_validator("prowsetk_upstream", mode="before")
    @classmethod
    def _normalize_upstream(cls, v: str) -> str:
        raw = str(v).strip().rstrip("/")
        if not raw.startswith(("http://", "https://")):
            raise ValueError("prowsetk_upstream must be http(s) URL")
        # HttpUrl validationwill catch malformed, but keep as str for httpx.
        return raw


settings = Settings()
