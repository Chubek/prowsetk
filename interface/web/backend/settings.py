from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="PROWSETK_WEB_", extra="ignore")

    service_name: str = "ProwseTk Service Gateway"
    environment: str = "development"
    host: str = "127.0.0.1"
    port: int = 8090
    default_timeout_seconds: float = 25.0
    poll_interval_seconds: float = 1.2
    max_runs: int = 300
    prowsetk_upstream: str = Field(default="http://127.0.0.1:8080")


settings = Settings()
