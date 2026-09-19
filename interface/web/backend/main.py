from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from starlette.middleware.cors import CORSMiddleware

from .api import router
from .deps import create_container
from .prowsetk_client import UpstreamError
from .settings import settings

ROOT_DIR = Path(__file__).resolve().parent.parent

log = logging.getLogger("prowsetk.web")

try:
    import uvicorn.logging as uvicorn_logging  # type: ignore[import-not-found]
except Exception:
    uvicorn_logging = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    app.state.container = create_container()
    log.info("Gateway starting upstream=%s max_runs=%s", settings.prowsetk_upstream, settings.max_runs)
    try:
        yield
    finally:
        try:
            await app.state.container.client.close()
        except Exception:
            log.debug("Error closing upstream client on shutdown", exc_info=True)
        log.info("Gateway stopped")


def create_app() -> FastAPI:
    app = FastAPI(
        title="ProwseTk Web Service",
        description="Job-driven gateway for offering ProwseTk as a web service.",
        version="0.3.0",
        lifespan=lifespan,
    )

    # CORS: tighten to same-origin by default; allow all in dev via settings.environment.
    allow_origins = ["*"] if settings.environment == "development" else []
    if allow_origins:
        app.add_middleware(
            CORSMiddleware,
            allow_origins=allow_origins,
            allow_credentials=False,
            allow_methods=["GET", "POST", "DELETE", "OPTIONS"],
            allow_headers=["*"],
        )

    @app.exception_handler(RequestValidationError)
    async def validation_exception_handler(request: Request, exc: RequestValidationError):
        return JSONResponse(status_code=422, content={"detail": str(exc.errors()[0].get("msg", "Validation error")) if exc.errors() else "Validation error"})

    @app.exception_handler(UpstreamError)
    async def upstream_exception_handler(request: Request, exc: UpstreamError):
        status = 502
        if exc.status_code == 404:
            status = 404
        return JSONResponse(status_code=status, content={"detail": str(exc)[:2048]})

    @app.exception_handler(Exception)
    async def generic_exception_handler(request: Request, exc: Exception):
        log.exception("Unhandled error: %s", exc)
        return JSONResponse(status_code=500, content={"detail": "Internal server error"})

    app.include_router(router)

    assets_dir = ROOT_DIR / "frontend" / "assets"
    if assets_dir.is_dir():
        app.mount("/assets", StaticFiles(directory=str(assets_dir)), name="assets")
    else:
        log.warning("Assets directory not found: %s", assets_dir)

    # Vendor assets are kept beside the web app so the dashboard remains
    # self-contained and does not depend on a CDN at runtime.
    for route, directory, name in (("/libjs", ROOT_DIR / "libjs", "libjs"), ("/libcss", ROOT_DIR / "libcss", "libcss")):
        if directory.is_dir():
            app.mount(route, StaticFiles(directory=str(directory)), name=name)
        else:
            log.warning("Library directory not found: %s", directory)

    @app.get("/", include_in_schema=False)
    async def index() -> FileResponse:
        target = ROOT_DIR / "index.html"
        if not target.is_file():
            return JSONResponse(status_code=500, content={"detail": "index.html not found"})
        return FileResponse(str(target), media_type="text/html; charset=utf-8")

    @app.get("/health", include_in_schema=False)
    async def root_health():
        return {"ok": True}

    return app


app = create_app()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        "backend.main:app",
        host=settings.host,
        port=settings.port,
        reload=False,
        log_level="info",
    )
