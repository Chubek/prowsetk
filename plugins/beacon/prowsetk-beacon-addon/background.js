/* Beacon background: native messaging and explicit, tab-scoped consent. */
"use strict";

let nativePort = null;
let pending = [];
let connection = null;
let flashes = [];
let requests = [];

function connectNative() {
  if (nativePort) return;
  nativePort = browser.runtime.connectNative("prowsetk_beacon");
  nativePort.onMessage.addListener((message) => {
    const next = pending.shift();
    if (message && message.type === "flash_list" && Array.isArray(message.flashes)) {
      flashes = message.flashes;
      browser.runtime.sendMessage({ type: "beacon-flashes", flashes });
    }
    if (next) {
      if (message && message.type === "error") next.reject(new Error(message.message));
      else next.resolve(message);
    }
  });
  nativePort.onDisconnect.addListener(() => {
    nativePort = null;
    connection = null;
    requests = [];
    for (const next of pending.splice(0)) next.reject(new Error("native host disconnected"));
  });
}

function sendNative(message) {
  try { connectNative(); } catch (error) { return Promise.reject(error); }
  return new Promise((resolve, reject) => {
    pending.push({ resolve, reject });
    try { nativePort.postMessage(message); } catch (error) {
      pending.pop();
      reject(error);
    }
  });
}

function urlMatches(pattern, url) {
  // Filters are glob patterns over the URL without credentials, query or hash.
  // The broker never relies on this addon check for local socket authentication.
  if (typeof pattern !== "string" || !/^https?:\/\//.test(pattern)) return false;
  try {
    const page = new URL(url);
    if (page.username || page.password) return false;
    const path = page.origin + page.pathname;
    const glob = pattern.split(/[?#]/, 1)[0];
    let p = 0;
    let u = 0;
    let star = -1;
    let backtrack = 0;
    while (u < path.length) {
      if (p < glob.length && glob[p] === path[u]) { ++p; ++u; }
      else if (p < glob.length && glob[p] === "*") { star = p++; backtrack = u; }
      else if (star !== -1) { p = star + 1; u = ++backtrack; }
      else return false;
    }
    while (glob[p] === "*") ++p;
    return p === glob.length;
  } catch (_) { return false; }
}

async function connectFlash(flashId) {
  if (connection) throw new Error("disconnect the current Flash first");
  const flash = flashes.find((item) => item.flash_id === flashId && item.status === "seeking");
  if (!flash) throw new Error("select a seeking Flash from the list");
  const [tab] = await browser.tabs.query({ active: true, currentWindow: true });
  if (!tab || !Number.isInteger(tab.id) || !/^https?:\/\//.test(tab.url || "")) {
    throw new Error("select an HTTP(S) tab");
  }
  if (!urlMatches(flash.filters && flash.filters.url_pattern, tab.url)) {
    throw new Error("tab URL does not match Flash filter");
  }
  const reply = await sendNative({ type: "flash_connect", flash_id: flashId, tab_id: tab.id });
  if (!reply || reply.type !== "flash_connected") throw new Error("Flash connection failed");
  connection = { flashId, tabId: tab.id };
  requests = [];
  return reply;
}

async function disconnectFlash() {
  if (!connection) return;
  const previous = connection;
  connection = null;
  requests = [];
  await browser.tabs.sendMessage(previous.tabId, { type: "beacon-trace-stop" }).catch(() => {});
  await sendNative({ type: "flash_disconnect", flash_id: previous.flashId });
}

async function sendPage(dataType) {
  if (!connection) throw new Error("connect to a Flash first");
  if (!["page_dom", "network_info", "stylesheet"].includes(dataType)) {
    throw new Error("unsupported data type");
  }
  const selected = connection;
  const payload = dataType === "network_info"
    ? { entries: requests.slice(), timestamp: Math.floor(Date.now() / 1000) }
    : (await browser.tabs.sendMessage(selected.tabId,
      { type: "beacon-extract", dataType })).payload;
  if (connection !== selected) throw new Error("Flash disconnected during extraction");
  return sendNative({ type: "flash_data", flash_id: selected.flashId,
    tab_id: selected.tabId, data_type: dataType, payload });
}

browser.webRequest.onBeforeRequest.addListener((details) => {
  if (!connection || details.tabId !== connection.tabId) return;
  // Query parameters and request bodies may contain credentials.
  let path;
  try { const url = new URL(details.url); path = url.origin + url.pathname; }
  catch (_) { return; }
  requests.push({ method: details.method, url: path, type: details.type });
  if (requests.length > 500) requests.shift();
}, { urls: ["<all_urls>"] });

browser.tabs.onRemoved.addListener((tabId) => {
  if (connection && connection.tabId === tabId) disconnectFlash().catch(() => {});
});
browser.tabs.onUpdated.addListener((tabId, change) => {
  if (connection && connection.tabId === tabId && change.status === "loading") {
    disconnectFlash().catch(() => {});
  }
});

browser.runtime.onMessage.addListener((message, sender) => {
  if (!message || typeof message.type !== "string") return undefined;
  if (sender.tab && message.type !== "beacon-tracepoint-fire") return undefined;
  if (message.type === "beacon-list") return sendNative({ type: "flash_list_request" });
  if (message.type === "beacon-connect") return connectFlash(message.flash_id);
  if (message.type === "beacon-disconnect") return disconnectFlash();
  if (message.type === "beacon-send") return sendPage(message.data_type);
  if (message.type === "beacon-tracepoint") {
    if (!connection || typeof message.selector !== "string" || !message.selector.trim()) {
      return Promise.reject(new Error("connect and supply a selector first"));
    }
    return browser.tabs.sendMessage(connection.tabId,
      { type: "beacon-trace-start", selector: message.selector });
  }
  if (message.type === "beacon-tracepoint-fire" && connection && sender.tab &&
      sender.tab.id === connection.tabId && typeof message.selector === "string") {
    return sendNative({ type: "tracepoint_event", flash_id: connection.flashId,
      tab_id: connection.tabId, event: "dom_mutation",
      details: { selector: message.selector, changes: message.changes || [] } });
  }
  return undefined;
});
