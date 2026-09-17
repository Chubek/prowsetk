from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.responses import PlainTextResponse

from .deps import Container
from .models import (
    HealthResponse,
    RunCreateRequest,
    RunResult,
    RunStatus,
    RunSummary,
    SessionCreateRequest,
    SessionNavigateRequest,
    SessionNavigateResponse,
    SessionSummary,
)
from .settings import settings
from .store import short_excerpt


router = APIRouter(prefix="/api/v1", tags=["prowsetk-service"])


def container(request: Request) -> Container:
    value = getattr(request.app.state, "container", None)
    if value is None:
        raise HTTPException(status_code=500, detail="App not initialized")
    return value


@router.get("/health", response_model=HealthResponse)
async def health(deps: Container = Depends(container)) -> HealthResponse:
    upstream_status = "unreachable"
    try:
        await deps.client.health()
        upstream_status = "ok"
    except Exception:
        upstream_status = "unreachable"
    session_count, active_runs = await deps.store.stats()
    status = "ok" if upstream_status == "ok" else "degraded"
    return HealthResponse(
        service=settings.service_name,
        environment=settings.environment,
        status=status,
        upstream_status=upstream_status,
        active_sessions=session_count,
        active_runs=active_runs,
    )


@router.get("/sessions", response_model=list[SessionSummary])
async def list_sessions(deps: Container = Depends(container)) -> list[SessionSummary]:
    return await deps.store.list_sessions()


@router.post("/sessions", response_model=SessionSummary)
async def create_session(
    request: SessionCreateRequest,
    deps: Container = Depends(container),
) -> SessionSummary:
    if request.preferred_id:
        return await deps.store.upsert_session(SessionSummary(id=request.preferred_id))

    upstream = await deps.client.create_session()
    session_id = upstream.get("id")
    if not session_id:
        raise HTTPException(status_code=502, detail="Upstream did not return a session id")
    return await deps.store.upsert_session(SessionSummary(id=session_id))


@router.get("/sessions/{session_id}", response_model=SessionSummary)
async def get_session(session_id: str, deps: Container = Depends(container)) -> SessionSummary:
    session = await deps.store.get_session(session_id)
    if session is None:
        raise HTTPException(status_code=404, detail="Session not found")
    return session


@router.post("/sessions/{session_id}/navigate", response_model=SessionNavigateResponse)
async def navigate(
    session_id: str,
    request: SessionNavigateRequest,
    deps: Container = Depends(container),
) -> SessionNavigateResponse:
    try:
        payload = await deps.client.navigate(session_id, str(request.url))
    except Exception as error:
        raise HTTPException(status_code=502, detail=f"Upstream navigation failed: {error}") from error

    title = payload.get("title") or str(request.url)
    url = payload.get("url") or str(request.url)
    text = payload.get("text") or ""
    await deps.store.upsert_session(SessionSummary(id=session_id, title=title, url=url))
    return SessionNavigateResponse(id=session_id, title=title, url=url, excerpt=short_excerpt(text))


@router.delete("/sessions/{session_id}")
async def delete_session(session_id: str, deps: Container = Depends(container)) -> dict[str, bool]:
    await deps.store.delete_session(session_id)
    try:
        await deps.client.delete_session(session_id)
    except Exception:
        pass
    return {"deleted": True}


@router.get("/runs", response_model=list[RunSummary])
async def list_runs(deps: Container = Depends(container)) -> list[RunSummary]:
    return await deps.store.list_runs()


@router.post("/runs", response_model=RunSummary)
async def create_run(request: RunCreateRequest, deps: Container = Depends(container)) -> RunSummary:
    return await deps.runs.create_run(request)


@router.get("/runs/{run_id}", response_model=RunSummary)
async def get_run(run_id: str, deps: Container = Depends(container)) -> RunSummary:
    record = await deps.store.get_run(run_id)
    if record is None:
        raise HTTPException(status_code=404, detail="Run not found")
    return record.summary


@router.post("/runs/{run_id}/cancel", response_model=RunSummary)
async def cancel_run(run_id: str, deps: Container = Depends(container)) -> RunSummary:
    ok = await deps.store.cancel_run(run_id)
    if not ok:
        raise HTTPException(status_code=404, detail="Run not found")
    record = await deps.store.get_run(run_id)
    assert record is not None
    return record.summary


@router.get("/runs/{run_id}/result", response_model=RunResult)
async def run_result(run_id: str, deps: Container = Depends(container)) -> RunResult:
    record = await deps.store.get_run(run_id)
    if record is None:
        raise HTTPException(status_code=404, detail="Run not found")
    if record.summary.status not in (RunStatus.succeeded, RunStatus.failed, RunStatus.cancelled):
        raise HTTPException(status_code=409, detail="Run still in progress")
    return RunResult(run=record.summary, openapi_yaml=record.openapi_yaml, metadata=record.metadata)


@router.get("/runs/{run_id}/openapi.yaml", response_class=PlainTextResponse)
async def run_openapi(run_id: str, deps: Container = Depends(container)) -> PlainTextResponse:
    record = await deps.store.get_run(run_id)
    if record is None:
        raise HTTPException(status_code=404, detail="Run not found")
    if record.summary.status != RunStatus.succeeded or not record.openapi_yaml:
        raise HTTPException(status_code=409, detail="No OpenAPI artifact available")
    return PlainTextResponse(record.openapi_yaml, media_type="application/yaml")
