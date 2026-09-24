/* Page extraction runs only on requests from the connected addon background. */
"use strict";

let observer = null;
let selector = null;

function pagePayload() {
  return {
    html: document.documentElement ? document.documentElement.outerHTML : "",
    url: window.location.origin + window.location.pathname,
    timestamp: Math.floor(Date.now() / 1000),
  };
}

function stylesheetPayload() {
  const rules = [];
  for (const sheet of document.styleSheets) {
    try {
      for (const rule of sheet.cssRules || []) rules.push(rule.cssText);
    } catch (_) {
      // Cross-origin stylesheets cannot be read by a content script.
    }
  }
  return { css: rules.join("\n"), url: window.location.origin + window.location.pathname,
    timestamp: Math.floor(Date.now() / 1000) };
}

function stopTrace() {
  if (observer) observer.disconnect();
  observer = null;
  selector = null;
}

browser.runtime.onMessage.addListener((message) => {
  if (!message || typeof message.type !== "string") return undefined;
  if (message.type === "beacon-trace-stop") {
    stopTrace();
    return Promise.resolve();
  }
  if (message.type === "beacon-trace-start") {
    // Reject invalid selectors before replacing an existing watcher.
    document.querySelector(message.selector);
    stopTrace();
    selector = message.selector;
    observer = new MutationObserver((mutations) => {
      const matches = mutations.filter((m) => {
        const node = m.target.nodeType === Node.ELEMENT_NODE
          ? m.target : m.target.parentElement;
        return node && (node.matches(selector) || node.closest(selector) ||
          (m.addedNodes && Array.from(m.addedNodes).some((added) =>
            added.nodeType === Node.ELEMENT_NODE &&
            (added.matches(selector) || added.querySelector(selector)))));
      });
      if (!matches.length) return;
      browser.runtime.sendMessage({ type: "beacon-tracepoint-fire", selector,
        changes: matches.slice(0, 20).map((m) => ({ type: m.type,
          target: m.target.nodeName })) }).catch(() => {});
    });
    observer.observe(document.documentElement, { attributes: true,
      characterData: true, childList: true, subtree: true });
    return Promise.resolve();
  }
  if (message.type === "beacon-extract") {
    if (message.dataType === "page_dom") return Promise.resolve({ payload: pagePayload() });
    if (message.dataType === "stylesheet") return Promise.resolve({ payload: stylesheetPayload() });
    return Promise.reject(new Error("unsupported extraction type"));
  }
  return undefined;
});
