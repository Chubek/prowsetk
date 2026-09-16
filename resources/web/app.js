"use strict";

const state = {
  sessionId: null,
  endpoints: null,
  currentUrl: null,
};

function $(id) {
  return document.getElementById(id);
}

function setStatus(text, kind) {
  const bar = $("status");
  bar.textContent = text;
  bar.className = "statusbar" + (kind ? " " + kind : "");
}

async function api(path, method, body) {
  const options = { method: method || "GET", headers: {} };
  if (body !== undefined && body !== null) {
    options.headers["Content-Type"] = "application/json";
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  const text = await response.text();
  let data = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch (_) {
    data = { raw: text };
  }
  if (!response.ok) {
    const message = data && data.message ? data.message : ("HTTP " + response.status);
    throw new Error(message);
  }
  return data;
}

function renderJson(pre, value) {
  pre.textContent = typeof value === "string" ? value : JSON.stringify(value, null, 2);
}

function escapeHtml(text) {
  return String(text)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function renderLinks(container, links) {
  if (!links || links.length === 0) {
    container.textContent = "No links found.";
    return;
  }
  const rows = links
    .map((l) => `<tr><td><a href="${escapeHtml(l.href)}" target="_blank" rel="noopener">${escapeHtml(l.href)}</a></td><td>${escapeHtml(l.text)}</td></tr>`)
    .join("");
  container.innerHTML = `<table><thead><tr><th>href</th><th>text</th></tr></thead><tbody>${rows}</tbody></table>`;
}

function renderCapabilities(container, capabilities) {
  if (!capabilities || capabilities.length === 0) {
    container.textContent = "No capabilities reported.";
    return;
  }
  const rows = capabilities
    .map((c) => `<tr><td class="mono">${escapeHtml(c.name)}</td><td>${escapeHtml(c.classification)}</td><td>${escapeHtml(c.notes || "")}</td></tr>`)
    .join("");
  container.innerHTML = `<table><thead><tr><th>capability</th><th>classification</th><th>notes</th></tr></thead><tbody>${rows}</tbody></table>`;
}

async function ensureSession() {
  if (state.sessionId) return;
  const data = await api("/api/sessions", "POST", {});
  state.sessionId = data.id;
  $("session-id").textContent = data.id;
}

function sessionPath(action) {
  return `/api/sessions/${state.sessionId}${action ? "/" + action : ""}`;
}

async function navigate(url) {
  await ensureSession();
  setStatus("navigating…");
  try {
    const data = await api(sessionPath("navigate"), "POST", { url });
    state.currentUrl = data.url;
    $("doc-url").textContent = data.url;
    $("doc-title").textContent = data.title || "—";
    renderJson($("doc-body"), data.text || "");
    setStatus("loaded " + data.url, "ok");
  } catch (error) {
    setStatus("error: " + error.message, "err");
    renderJson($("doc-body"), error.message);
  }
}

function setupTabs() {
  const tabs = document.querySelectorAll(".tab");
  tabs.forEach((tab) => {
    tab.addEventListener("click", () => {
      tabs.forEach((t) => t.classList.remove("active"));
      tab.classList.add("active");
      document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
      $(tab.dataset.tab).classList.add("active");
    });
  });
}

function setupNavigation() {
  $("nav-form").addEventListener("submit", (event) => {
    event.preventDefault();
    const url = $("url").value.trim();
    if (url) navigate(url);
  });
}

function setupDocument() {
  $("content-button").addEventListener("click", async () => {
    try {
      await ensureSession();
      const data = await api(sessionPath("content"), "POST", {});
      renderJson($("doc-body"), data.html || "");
      setStatus("html fetched", "ok");
    } catch (error) {
      setStatus("error: " + error.message, "err");
    }
  });
  $("text-button").addEventListener("click", async () => {
    try {
      await ensureSession();
      const data = await api(sessionPath("text"), "POST", {});
      renderJson($("doc-body"), data.text || "");
      setStatus("text extracted", "ok");
    } catch (error) {
      setStatus("error: " + error.message, "err");
    }
  });
}

function setupScrape() {
  $("scrape-button").addEventListener("click", async () => {
    try {
      await ensureSession();
      const selectors = $("scrape-selector").value
        .split(",")
        .map((s) => s.trim())
        .filter(Boolean);
      if (selectors.length === 0) return;
      setStatus("scraping…");
      const data = await api(sessionPath("scrape"), "POST", { selectors });
      renderJson($("scrape-output"), data.results);
      setStatus(`scraped ${selectors.length} selector(s)`, "ok");
    } catch (error) {
      setStatus("error: " + error.message, "err");
      renderJson($("scrape-output"), error.message);
    }
  });
}

function setupXPath() {
  $("xpath-button").addEventListener("click", async () => {
    try {
      await ensureSession();
      const expression = $("xpath-expr").value.trim();
      if (!expression) return;
      const data = await api(sessionPath("xpath"), "POST", { expression });
      renderJson($("xpath-output"), data);
      setStatus("xpath evaluated", "ok");
    } catch (error) {
      setStatus("error: " + error.message, "err");
      renderJson($("xpath-output"), error.message);
    }
  });
}

function setupLinks() {
  $("links-button").addEventListener("click", async () => {
    try {
      await ensureSession();
      const data = await api(sessionPath("links"), "POST", {});
      renderLinks($("links-output"), data.links);
      setStatus(`${data.links.length} link(s) found`, "ok");
    } catch (error) {
      setStatus("error: " + error.message, "err");
    }
  });
}

function setupJavaScript() {
  $("js-button").addEventListener("click", async () => {
    try {
      await ensureSession();
      const script = $("js-script").value;
      setStatus("evaluating…");
      const data = await api(sessionPath("evaluate"), "POST", { script });
      renderJson($("js-output"), data.value);
      setStatus("evaluated", "ok");
    } catch (error) {
      setStatus("error: " + error.message, "err");
      renderJson($("js-output"), error.message);
    }
  });
}

function setupEndpoints() {
  $("endpoints-button").addEventListener("click", async () => {
    try {
      await ensureSession();
      const options = {
        follow_links: $("opt-follow").checked,
        observe_network: $("opt-observe").checked,
        redact_secrets: $("opt-redact").checked,
      };
      setStatus("extracting endpoints…");
      const data = await api(sessionPath("endpoints"), "POST", options);
      state.endpoints = data;
      renderJson($("endpoints-output"), {
        endpoint_count: data.endpoint_count,
        endpoints: data.endpoints,
        warnings: data.warnings,
      });
      setStatus(`${data.endpoint_count} endpoint(s) found`, "ok");
    } catch (error) {
      setStatus("error: " + error.message, "err");
      renderJson($("endpoints-output"), error.message);
    }
  });
  $("yaml-button").addEventListener("click", () => {
    if (!state.endpoints) {
      setStatus("run endpoint extraction first", "err");
      return;
    }
    renderJson($("endpoints-output"), state.endpoints.openapi_yaml || "");
  });
}

function setupApi() {
  $("capabilities-button").addEventListener("click", async () => {
    try {
      const data = await api("/api/capabilities");
      renderCapabilities($("api-output"), data.capabilities);
      setStatus("capabilities refreshed", "ok");
    } catch (error) {
      setStatus("error: " + error.message, "err");
    }
  });
}

async function init() {
  setupTabs();
  setupNavigation();
  setupDocument();
  setupScrape();
  setupXPath();
  setupLinks();
  setupJavaScript();
  setupEndpoints();
  setupApi();

  try {
    await ensureSession();
    const health = await api("/api/health");
    setStatus(`ProwseTk ${health.version} — ${health.engine} engine`, "ok");
  } catch (error) {
    setStatus("error: " + error.message, "err");
  }
}

document.addEventListener("DOMContentLoaded", init);
