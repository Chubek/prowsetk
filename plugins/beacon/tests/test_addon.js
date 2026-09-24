/* Hermetic Firefox API simulation: consent, tab scope, navigation revocation. */
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

const listeners = {};
const sent = [];
const tabsRead = [];
let activeTab = { id: 7, url: "https://example.test/page" };
const port = {
  onMessage: { addListener(fn) { listeners.native = fn; } },
  onDisconnect: { addListener(fn) { listeners.disconnect = fn; } },
  postMessage(message) {
    sent.push(message);
    const replies = {
      flash_list_request: { type: "flash_list", flashes: [{ flash_id: "test",
        status: "seeking", request_type: "page_dom",
        filters: { url_pattern: "https://example.test/*" } }] },
      flash_connect: { type: "flash_connected", flash_id: "test" },
      flash_disconnect: { type: "flash_closed" },
      flash_data: { type: "flash_queued" },
      tracepoint_event: { type: "flash_queued" },
    };
    queueMicrotask(() => listeners.native(replies[message.type]));
  },
};
const browser = {
  runtime: {
    connectNative() { return port; },
    sendMessage() { return Promise.resolve(); },
    onMessage: { addListener(fn) { listeners.runtime = fn; } },
  },
  tabs: {
    query: async () => [activeTab],
    sendMessage: async (id, request) => {
      tabsRead.push({ id, request });
      return { payload: { html: "consented" } };
    },
    onRemoved: { addListener(fn) { listeners.removed = fn; } },
    onUpdated: { addListener(fn) { listeners.updated = fn; } },
  },
  webRequest: { onBeforeRequest: { addListener(fn) { listeners.webRequest = fn; } } },
};
vm.runInNewContext(fs.readFileSync(process.argv[2], "utf8"),
  { browser, Promise, URL, queueMicrotask, Date, Error });
const call = (message, sender = {}) => listeners.runtime(message, sender);

(async () => {
  await assert.rejects(call({ type: "beacon-send", data_type: "page_dom" }), /connect/);
  assert.equal(tabsRead.length, 0);
  await call({ type: "beacon-list" });
  activeTab = { id: 8, url: "https://other.test/" };
  await assert.rejects(call({ type: "beacon-connect", flash_id: "test" }), /filter/);
  activeTab = { id: 7, url: "https://example.test/page" };
  await call({ type: "beacon-connect", flash_id: "test" });
  listeners.webRequest({ tabId: 8, url: "https://other.test/?token=secret", method: "GET" });
  listeners.webRequest({ tabId: 7, url: "https://example.test/a?token=secret", method: "POST", type: "xmlhttprequest" });
  activeTab = { id: 8, url: "https://other.test/" };
  await call({ type: "beacon-send", data_type: "page_dom" });
  assert.equal(tabsRead[0].id, 7);
  assert.equal(sent.at(-1).tab_id, 7);
  await call({ type: "beacon-send", data_type: "network_info" });
  assert.equal(sent.at(-1).payload.entries.length, 1);
  assert.equal(sent.at(-1).payload.entries[0].url, "https://example.test/a");
  assert.equal(JSON.stringify(sent).includes("secret"), false);
  assert.equal(call({ type: "beacon-send", data_type: "page_dom" },
    { tab: { id: 7 } }), undefined);
  listeners.updated(7, { status: "loading" });
  await new Promise((resolve) => setImmediate(resolve));
  await assert.rejects(call({ type: "beacon-send", data_type: "page_dom" }), /connect/);
})().catch((error) => { console.error(error); process.exitCode = 1; });
