import { openapiLink } from "./api.js";
import { activeRun, state } from "./state.js";

const byId = (id) => document.getElementById(id);

export function flash(text = "") {
  byId("flash").textContent = text;
}

export function setSystemState(ok, label) {
  const pill = byId("system-state");
  pill.textContent = label;
  pill.classList.toggle("status-ok", ok);
  pill.classList.toggle("status-bad", !ok);
}

export function formPayload() {
  return {
    url: byId("run-url").value.trim(),
    discover_options: {
      follow_links: byId("follow-links").checked,
      observe_network: byId("observe-network").checked,
      infer_schemas: byId("infer-schemas").checked,
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
    head.innerHTML = `<code>${run.id}</code><span>${statusTag(run.status)}</span>`;

    const line = document.createElement("small");
    line.textContent = run.url;

    const updated = document.createElement("small");
    updated.textContent = `Updated ${new Date(run.updated_at).toLocaleString()}`;

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
    host.append(card);
  }
}

export function drawArtifact(payload) {
  const pre = byId("artifact-preview");
  const link = byId("artifact-link");
  const current = activeRun();

  if (!payload) {
    pre.textContent = "Select a completed run to preview artifact output.";
    link.classList.add("disabled");
    link.removeAttribute("href");
    return;
  }

  if (payload.run.status !== "succeeded") {
    pre.textContent = payload.run.error || `Run ended: ${payload.run.status}`;
    link.classList.add("disabled");
    link.removeAttribute("href");
    return;
  }

  pre.textContent = payload.openapi_yaml || "No OpenAPI output in response.";
  if (current) {
    link.href = openapiLink(current.id);
    link.classList.remove("disabled");
  }
}
