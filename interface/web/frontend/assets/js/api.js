const API_ROOT = "/api/v1";
const DEFAULT_TIMEOUT_MS = 25000;

async function call(path, options = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(`${API_ROOT}${path}`, {
      ...options,
      signal: controller.signal,
      headers: {
        "Content-Type": "application/json",
        ...(options.headers || {}),
      },
    });

    const text = await response.text();
    let payload;
    if (text) {
      try {
        payload = JSON.parse(text);
      } catch {
        payload = text;
      }
    } else {
      payload = {};
    }

    if (!response.ok) {
      const detail =
        (payload && typeof payload === "object" && (payload.detail || payload.message)) ||
        (typeof payload === "string" ? payload : "") ||
        `Request failed (${response.status})`;
      const err = new Error(String(detail).slice(0, 2000));
      err.status = response.status;
      err.payload = payload;
      throw err;
    }
    return payload;
  } catch (error) {
    if (error.name === "AbortError") {
      throw new Error(`Request timed out after ${timeoutMs}ms`);
    }
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

export const api = {
  health: () => call("/health", {}, 5000),
  listRuns: () => call("/runs", {}, 10000),
  createRun: (body) => call("/runs", { method: "POST", body: JSON.stringify(body) }),
  cancelRun: (runId) => call(`/runs/${encodeURIComponent(runId)}/cancel`, { method: "POST" }),
  runResult: (runId) => call(`/runs/${encodeURIComponent(runId)}/result`),
};

export function openapiLink(runId) {
  return `${API_ROOT}/runs/${encodeURIComponent(runId)}/openapi.yaml`;
}
