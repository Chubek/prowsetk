import { api } from "./api.js";
import { activeRun, setRuns, state } from "./state.js";
import { drawArtifact, drawRuns, flash, formPayload, setSystemState } from "./ui.js";

const byId = (id) => document.getElementById(id);
const POLL_MS = 1500;
let polling = 0;
let inFlight = false;

async function refreshHealth() {
  try {
    const health = await api.health();
    const ok = health.status === "ok";
    setSystemState(ok, ok ? "System healthy" : "System degraded");
    // Surface upstream status in title if degraded
    if (health.upstream_status && health.upstream_status !== "ok") {
      setSystemState(false, `Upstream ${health.upstream_status}`);
    }
  } catch (error) {
    setSystemState(false, "Service unreachable");
    // Don't spam flash on poll; only on explicit refresh we might flash.
  }
}

async function refreshRuns() {
  try {
    const runs = await api.listRuns();
    if (!Array.isArray(runs)) throw new Error("Invalid runs payload");
    setRuns(runs);
    if (!state.selectedRunId && runs.length) {
      state.selectedRunId = runs[0].id;
    }
    drawRuns({
      onSelect: (runId) => {
        state.selectedRunId = runId;
        drawRuns({
          onSelect: (id) => { state.selectedRunId = id; },
          onCancel: handleCancel,
          onResult: handleResult,
        });
        refreshArtifact().catch(() => {});
      },
      onCancel: handleCancel,
      onResult: handleResult,
    });
  } catch (error) {
    flash(error.message || String(error));
  }
}

async function handleCancel(runId) {
  try {
    await api.cancelRun(runId);
    flash(`Cancelled ${runId}`);
    await refreshRuns();
  } catch (error) {
    flash(error.message || String(error));
  }
}

async function handleResult(runId) {
  state.selectedRunId = runId;
  await refreshArtifact();
}

async function refreshArtifact() {
  const run = activeRun();
  if (!run) {
    drawArtifact(null);
    return;
  }

  if (run.status === "queued" || run.status === "running") {
    drawArtifact({ run, openapi_yaml: null });
    return;
  }

  try {
    const payload = await api.runResult(run.id);
    drawArtifact(payload);
  } catch (error) {
    // 409 means still in progress; show progress card instead of error flash
    if (error.status === 409) {
      drawArtifact({ run, openapi_yaml: null });
      return;
    }
    flash(error.message || String(error));
  }
}

async function submitRun(event) {
  event.preventDefault();
  flash("");

  try {
    const payload = formPayload();
    if (!payload.url) {
      flash("Please provide a URL.");
      return;
    }
    // Quick client-side URL sanity
    try {
      const parsed = new URL(payload.url);
      if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
        flash("URL must be http or https.");
        return;
      }
    } catch {
      flash("Please provide a valid http(s) URL.");
      return;
    }
    const submitBtn = byId("run-submit");
    if (submitBtn) submitBtn.disabled = true;
    const run = await api.createRun(payload);
    state.selectedRunId = run.id;
    flash(`Queued ${run.id}`);
    await refreshRuns();
    await refreshArtifact();
    if (submitBtn) submitBtn.disabled = false;
  } catch (error) {
    const submitBtn = byId("run-submit");
    if (submitBtn) submitBtn.disabled = false;
    flash(error.message || String(error));
  }
}

function startPolling() {
  if (polling) {
    clearInterval(polling);
  }
  polling = window.setInterval(async () => {
    if (inFlight) return;
    inFlight = true;
    try {
      await refreshHealth();
      await refreshRuns();
      // Only refresh artifact if selected run is not terminal? Always try but avoid spam
      const run = activeRun();
      if (run && (run.status === "queued" || run.status === "running")) {
        await refreshArtifact();
      }
    } finally {
      inFlight = false;
    }
  }, POLL_MS);
}

function wire() {
  const form = byId("run-form");
  if (form) form.addEventListener("submit", submitRun);
  const btn = byId("refresh-runs");
  if (btn) btn.addEventListener("click", async () => {
    await refreshRuns();
    await refreshArtifact();
  });
}

async function bootstrap() {
  wire();
  await refreshHealth();
  await refreshRuns();
  await refreshArtifact();
  startPolling();
}

bootstrap().catch((error) => flash(error.message || String(error)));
