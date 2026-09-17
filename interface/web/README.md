# ProwseTk Web Service

`interface/web` is a standalone service-layer UI/API for offering ProwseTk as a web service.

## Stack

- Backend: FastAPI + Uvicorn + HTTPX.
- Frontend: static HTML/CSS/JS, no build step.
- Upstream: delegates browser execution to `prowsetk serve`.

## What it adds over raw `prowsetk serve`

- Asynchronous run queue for endpoint discovery.
- Run lifecycle endpoints (queue, poll, cancel, result fetch).
- OpenAPI artifact download per successful run.
- Dashboard for operators.

## Layout

- `backend/` - route handlers, run service, upstream client, settings, state store.
- `frontend/` - dashboard assets.
- `scripts/` - install/dev/check/stack helpers.
- `API.yaml` - OpenAPI contract.

## Quick start

```bash
./scripts/install.sh
./scripts/dev.sh
```

Run both gateway and `prowsetk serve` together:

```bash
./scripts/run-stack.sh
```

## Configuration

Environment variables use the `PROWSETK_WEB_` prefix:

- `PROWSETK_WEB_HOST` (default `127.0.0.1`)
- `PROWSETK_WEB_PORT` (default `8090`)
- `PROWSETK_WEB_PROWSETK_UPSTREAM` (default `http://127.0.0.1:8080`)
- `PROWSETK_WEB_DEFAULT_TIMEOUT_SECONDS` (default `25.0`)
- `PROWSETK_WEB_POLL_INTERVAL_SECONDS` (default `1.2`)
- `PROWSETK_WEB_MAX_RUNS` (default `300`)
