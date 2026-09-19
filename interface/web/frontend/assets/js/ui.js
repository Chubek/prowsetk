import { openapiLink } from "./api.js";
import { activeRun, state } from "./state.js";

const byId = (id) => document.getElementById(id);

export function flash(text = "") {
  const el = byId("flash");
  if (el) el.textContent = String(text || "").slice(0, 2000);
}

export function setSystemState(ok, label) {
  const pill = byId("system-state");
  if (!pill) return;
  pill.textContent = String(label || (ok ? "System healthy" : "System degraded")).slice(0, 128);
  pill.classList.toggle("status-ok", ok);
  pill.classList.toggle("status-bad", !ok);
  pill.setAttribute("aria-label", pill.textContent);
}

export function formPayload() {
  const urlEl = byId("run-url");
  const followEl = byId("follow-links");
  const observeEl = byId("observe-network");
  const inferEl = byId("infer-schemas");
  const rawUrl = urlEl ? urlEl.value.trim().slice(0, 2048) : "";
  // Basic client-side URL validation; server will re-validate.
  let url = rawUrl;
  if (url && !/^https?:\/\//i.test(url)) {
    // Let server return 422; don't auto-prefix to avoid SSRF surprises.
  }
  return {
    url,
    discover_options: {
      follow_links: !!(followEl && followEl.checked),
      observe_network: !!(observeEl && observeEl.checked),
      infer_schemas: !!(inferEl && inferEl.checked),
    },
  };
}

function statusTag(status) {
  if (status === "succeeded") return "OK";
  if (status === "failed") return "ERR";
  if (status === "running") return "RUN";
  if (status === "cancelled") return "CXL";
  return "Q";
}

export function drawRuns({ onSelect, onCancel, onResult }) {
  const host = byId("runs");
  if (!host) return;
  host.replaceChildren();

  if (!state.runs.length) {
    const empty = document.createElement("div");
    empty.className = "empty";
    empty.textContent = "No runs yet.";
    host.append(empty);
    return;
  }

  for (const run of state.runs) {
    const card = document.createElement("article");
    card.className = "run-card";

    const head = document.createElement("strong");

    const code = document.createElement("code");
    code.textContent = String(run.id).slice(0, 68);

    const tag = document.createElement("span");
    tag.textContent = statusTag(run.status);
    tag.setAttribute("aria-label", String(run.status));

    head.append(code, tag);

    const line = document.createElement("small");
    line.textContent = String(run.url).slice(0, 2048);

    const updated = document.createElement("small");
    try {
      updated.textContent = `Updated ${new Date(run.updated_at).toLocaleString()}`;
    } catch {
      updated.textContent = `Updated ${String(run.updated_at || "")}`;
    }

    const actions = document.createElement("div");
    actions.className = "run-actions";

    const view = document.createElement("button");
    view.type = "button";
    view.textContent = "View";
    view.addEventListener("click", () => onSelect(run.id));

    const result = document.createElement("button");
    result.type = "button";
    result.textContent = "Result";
    result.disabled = !(run.status === "succeeded" || run.status === "failed" || run.status === "cancelled");
    result.addEventListener("click", () => onResult(run.id));

    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.textContent = "Cancel";
    cancel.disabled = !(run.status === "queued" || run.status === "running");
    cancel.addEventListener("click", () => onCancel(run.id));

    actions.append(view, result, cancel);
    card.append(head, line, updated, actions);

    if (run.id === state.selectedRunId) {
      card.style.outline = "2px solid var(--brand)";
      card.style.outlineOffset = "2px";
    }

    host.append(card);
  }
}

export function drawArtifact(payload) {
  const pre = byId("artifact-preview");
  const link = byId("artifact-link");
  if (!pre || !link) return;
  const current = activeRun();

  if (!payload) {
    pre.textContent = "Select a completed run to preview artifact output.";
    link.classList.add("disabled");
    link.removeAttribute("href");
    return;
  }

  const status = payload.run?.status;
  if (status !== "succeeded") {
    const err = payload.run?.error || `Run ended: ${status || "unknown"}`;
    pre.textContent = String(err).slice(0, 5000);
    link.classList.add("disabled");
    link.removeAttribute("href");
    return;
  }

  const yaml = payload.openapi_yaml;
  pre.textContent = yaml ? String(yaml).slice(0, 500000) : "No OpenAPI output in response.";
  if (current) {
    link.href = openapiLink(current.id);
    link.classList.remove("disabled");
    link.setAttribute("download", `${current.id}.yaml`);
  } else {
    link.classList.add("disabled");
    link.removeAttribute("href");
  }
}
