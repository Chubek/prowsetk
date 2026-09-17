"use strict";

const state = { sessions: [], selected: null, view: "text", busy: false, document: null };
const $ = (id) => document.getElementById(id);

async function api(path, method = "GET", body) {
  const response = await fetch(path, {
    method,
    headers: body === undefined ? {} : { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body)
  });
  const result = await response.json();
  if (!response.ok) throw new Error(result.message || `Request failed (${response.status})`);
  return result;
}

function message(value = "") { $("status").textContent = value; }
function empty(value) {
  $("results").replaceChildren();
  const block = document.createElement("div");
  block.className = "empty";
  block.textContent = value;
  $("results").append(block);
}
function showText(value, code = false) {
  const node = document.createElement(code ? "pre" : "div");
  node.textContent = value;
  $("results").replaceChildren(node);
}
function setBusy(value) {
  state.busy = value;
  $("go").disabled = value;
  $("new-session").disabled = value;
  for (const button of $("view-toolbar").querySelectorAll("button")) button.disabled = value;
  $("page-state").textContent = value ? "Working" : state.document ? "Loaded" : "Idle";
}
async function task(action) {
  if (state.busy) return;
  message();
  setBusy(true);
  try { await action(); }
  catch (error) { message(error.message); }
  finally { setBusy(false); }
}

function sessionPath(action = "") {
  return `/api/sessions/${encodeURIComponent(state.selected)}${action}`;
}
function renderSessions() {
  const list = $("sessions");
  list.replaceChildren();
  for (const session of state.sessions) {
    const row = document.createElement("div");
    row.className = "session-row" + (session.id === state.selected ? " current" : "");
    const select = document.createElement("button");
    select.className = "select-session";
    select.title = `Switch to ${session.id}`;
    const label = document.createElement("span");
    label.textContent = session.title || session.id;
    select.append(label);
    if (session.url) {
      const subtitle = document.createElement("small");
      subtitle.textContent = session.url;
      select.append(subtitle);
    }
    select.addEventListener("click", () => selectSession(session.id));
    const close = document.createElement("button");
    close.className = "close-session";
    close.title = `Close ${session.id}`;
    close.setAttribute("aria-label", close.title);
    close.textContent = "×";
    close.addEventListener("click", () => task(async () => {
      await api(`/api/sessions/${encodeURIComponent(session.id)}`, "DELETE");
      if (state.selected === session.id) { state.selected = null; state.document = null; }
      await refreshSessions();
      selectSession(state.selected || state.sessions[0]?.id || null);
    }));
    row.append(select, close);
    list.append(row);
  }
  $("session-count").textContent = `${state.sessions.length} session${state.sessions.length === 1 ? "" : "s"}`;
}
async function refreshSessions() {
  state.sessions = (await api("/api/sessions")).sessions;
  renderSessions();
}
function selectSession(id) {
  if (state.busy && state.selected && id !== state.selected) return;
  state.selected = id;
  state.document = id ? state.sessions.find((session) => session.id === id) : null;
  $("page-title").textContent = state.document?.title || "No document loaded";
  $("page-url").textContent = state.document?.url || "Create a session and navigate to begin.";
  $("address").value = state.document?.url || "";
  $("page-state").textContent = state.document?.url ? "Loaded" : "Idle";
  message();
  renderSessions();
  renderView();
}
async function ensureSession() {
  if (!state.selected) {
    const { id } = await api("/api/sessions", "POST", {});
    await refreshSessions();
    selectSession(id);
  }
}
function toolbarInput(placeholder, type = "text") {
  const input = document.createElement("input");
  input.type = type;
  input.placeholder = placeholder;
  input.setAttribute("aria-label", placeholder);
  $("view-toolbar").append(input);
  return input;
}
function toolbarButton(label, onClick) {
  const button = document.createElement("button");
  button.textContent = label;
  button.addEventListener("click", () => task(onClick));
  $("view-toolbar").append(button);
}
function showItems(items) {
  $("results").replaceChildren();
  if (!items.length) return empty("No matches found");
  for (const item of items) {
    const row = document.createElement("div");
    row.className = "list-item";
    if (item.href) {
      const link = document.createElement("a");
      let target;
      try { target = new URL(item.href, state.document.url); } catch { /* Show the raw reference. */ }
      if (target && (target.protocol === "http:" || target.protocol === "https:")) {
        link.href = target.href;
        link.target = "_blank";
        link.rel = "noopener noreferrer";
        link.textContent = item.text || item.href;
        row.append(link);
      } else {
        row.append(document.createTextNode(item.text || item.href));
      }
      const detail = document.createElement("small");
      detail.textContent = item.href;
      row.append(detail);
    } else {
      row.textContent = item.text || JSON.stringify(item);
    }
    $("results").append(row);
  }
}
async function loadView() {
  if (!state.document?.url) return empty("Navigate to a document to inspect it.");
  const view = state.view;
  if (view === "text" || view === "html") {
    const data = await api(sessionPath(`/${view === "html" ? "content" : "text"}`), "POST", {});
    showText(data[view], view === "html");
  } else if (view === "links") {
    showItems((await api(sessionPath("/links"), "POST", {})).links);
  } else if (view === "endpoints") {
    const data = await api(sessionPath("/endpoints"), "POST", {});
    showText(data.openapi_yaml, true);
    const download = $("view-toolbar").querySelector("button");
    download.disabled = false;
    download.onclick = () => {
      const url = URL.createObjectURL(new Blob([data.openapi_yaml], { type: "text/yaml" }));
      const link = document.createElement("a");
      link.href = url;
      link.download = "openapi.yaml";
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    };
  }
}
function renderView() {
  $("view-toolbar").replaceChildren();
  if (state.view === "selector") {
    const input = toolbarInput("CSS selector or XPath expression");
    const mode = document.createElement("select");
    mode.setAttribute("aria-label", "Query language");
    for (const name of ["CSS", "XPath"]) mode.add(new Option(name, name));
    $("view-toolbar").append(mode);
    toolbarButton("Query", async () => {
      if (!input.value.trim()) return message("Enter a selector or expression.");
      if (mode.value === "XPath") {
        const data = await api(sessionPath("/xpath"), "POST", { expression: input.value });
        showText(data.type === "nodeset" ? data.string_values.join("\n") : data.string_value || String(data.number_value || data.boolean_value));
      } else {
        const data = await api(sessionPath("/scrape"), "POST", { selectors: [input.value] });
        showItems(data.results[0].items);
      }
    });
  } else if (state.view === "script") {
    const input = toolbarInput("JavaScript expression, e.g. 1 + 1");
    toolbarButton("Evaluate", async () => {
      const data = await api(sessionPath("/evaluate"), "POST", { script: input.value });
      showText(data.value);
    });
  } else if (state.view === "endpoints") {
    toolbarButton("Download YAML", () => {});
    $("view-toolbar").querySelector("button").disabled = true;
  }
  empty(state.document?.url ? "Loading…" : "Navigate to a document to inspect it.");
  if (state.document?.url && !state.busy) task(loadView);
}

$("new-session").addEventListener("click", () => task(async () => {
  const { id } = await api("/api/sessions", "POST", {});
  await refreshSessions();
  selectSession(id);
}));
$("navigate-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const url = $("address").value.trim();
  task(async () => {
    await ensureSession();
    const data = await api(sessionPath("/navigate"), "POST", { url });
    state.document = { id: state.selected, ...data };
    $("page-title").textContent = data.title || data.url;
    $("page-url").textContent = data.url;
    if (state.view === "text") showText(data.text);
    else await loadView();
    await refreshSessions();
  });
});
for (const tab of document.querySelectorAll(".tab")) {
  tab.addEventListener("click", () => {
    state.view = tab.dataset.view;
    for (const other of document.querySelectorAll(".tab")) {
      other.classList.toggle("active", other === tab);
      other.setAttribute("aria-selected", String(other === tab));
    }
    renderView();
  });
}

api("/api/health").then(async () => {
  $("connection").textContent = "Connected";
  $("connection-dot").classList.add("online");
  await refreshSessions();
  if (state.sessions.length) selectSession(state.sessions[0].id);
}).catch((error) => {
  $("connection").textContent = "Disconnected";
  message(error.message);
});
