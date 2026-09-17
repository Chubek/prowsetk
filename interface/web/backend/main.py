from __future__ import annotations

from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .api import router
from .deps import create_container
from .settings import settings


ROOT_DIR = Path(__file__).resolve().parent.parent


@asynccontextmanager
async def lifespan(app: FastAPI):
    app.state.container = create_container()
    try:
        yield
    finally:
        await app.state.container.client.close()


def create_app() -> FastAPI:
    app = FastAPI(
        title="ProwseTk Web Service",
        description="Job-driven gateway for offering ProwseTk as a web service.",
        version="0.3.0",
        lifespan=lifespan,
    )
    app.include_router(router)
    app.mount("/assets", StaticFiles(directory=ROOT_DIR / "frontend" / "assets"), name="assets")

    @app.get("/")
    async def index() -> FileResponse:
        return FileResponse(ROOT_DIR / "index.html")

    return app


app = create_app()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        "backend.main:app",
        host=settings.host,
        port=settings.port,
        reload=False,
    )
