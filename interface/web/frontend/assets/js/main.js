import { api } from "./api.js";
import { activeRun, setRuns, state } from "./state.js";
import { drawArtifact, drawRuns, flash, formPayload, setSystemState } from "./ui.js";

const byId = (id) => document.getElementById(id);

async function refreshHealth() {
  try {
    const health = await api.health();
    setSystemState(health.status === "ok", health.status === "ok" ? "System healthy" : "System degraded");
  } catch (error) {
    setSystemState(false, "Service unreachable");
    flash(error.message);
  }
}

async function refreshRuns() {
  try {
    const runs = await api.listRuns();
    setRuns(runs);
    if (!state.selectedRunId && runs.length) {
      state.selectedRunId = runs[0].id;
    }
    drawRuns({
      onSelect: (runId) => {
        state.selectedRunId = runId;
      },
      onCancel: async (runId) => {
        try {
          await api.cancelRun(runId);
          flash(`Cancelled ${runId}`);
          await refreshRuns();
        } catch (error) {
          flash(error.message);
        }
      },
      onResult: async (runId) => {
        state.selectedRunId = runId;
        await refreshArtifact();
      },
    });
  } catch (error) {
    flash(error.message);
  }
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
    flash(error.message);
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
    const run = await api.createRun(payload);
    state.selectedRunId = run.id;
    flash(`Queued ${run.id}`);
    await refreshRuns();
    await refreshArtifact();
  } catch (error) {
    flash(error.message);
  }
}

function startPolling() {
  if (state.polling) {
    clearInterval(state.polling);
  }
  state.polling = window.setInterval(async () => {
    await refreshHealth();
    await refreshRuns();
    await refreshArtifact();
  }, 1500);
}

function wire() {
  byId("run-form").addEventListener("submit", submitRun);
  byId("refresh-runs").addEventListener("click", async () => {
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

bootstrap().catch((error) => flash(error.message));
