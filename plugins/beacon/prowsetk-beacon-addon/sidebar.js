/* ProwseTk Beacon sidebar UI: Flash list plus action buttons. */
"use strict";

let selectedFlashId = null;

async function run(message, success) {
  const status = document.getElementById("status");
  try {
    await browser.runtime.sendMessage(message);
    status.textContent = success;
  } catch (error) {
    status.textContent = error.message || "Beacon request failed";
  }
}

function renderFlashes(flashes) {
  const list = document.getElementById("flashes");
  list.innerHTML = "";
  for (const flash of flashes || []) {
    const item = document.createElement("li");
    item.textContent =
      flash.flash_id + " [" + flash.request_type + "] (" + flash.status + ")";
    item.addEventListener("click", () => {
      selectedFlashId = flash.flash_id;
    });
    list.appendChild(item);
  }
}

document.getElementById("list").addEventListener("click", () => {
  run({ type: "beacon-list" }, "Flash list updated");
});

document.getElementById("connect").addEventListener("click", () => {
  if (selectedFlashId === null) return;
  run({
    type: "beacon-connect",
    flash_id: selectedFlashId,
  }, "Connected to " + selectedFlashId);
});

document.getElementById("send-page").addEventListener("click", () => {
  run({ type: "beacon-send", data_type: "page_dom" }, "Page queued");
});

document.getElementById("send-network").addEventListener("click", () => {
  run({
    type: "beacon-send",
    data_type: "network_info",
  }, "Network info queued");
});

document.getElementById("send-stylesheet").addEventListener("click", () => {
  run({
    type: "beacon-send",
    data_type: "stylesheet",
  }, "Stylesheet queued");
});

document.getElementById("tracepoint").addEventListener("click", () => {
  const selector = window.prompt("CSS selector or request pattern to watch:");
  if (selector) {
    run({
      type: "beacon-tracepoint",
      selector,
    }, "Watching " + selector);
  }
});

document.getElementById("disconnect").addEventListener("click", () => {
  selectedFlashId = null;
  run({ type: "beacon-disconnect" }, "Disconnected");
  renderFlashes([]);
});

browser.runtime.onMessage.addListener((message) => {
  if (message && message.type === "beacon-flashes") {
    renderFlashes(message.flashes);
  }
});
