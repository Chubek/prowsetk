const API_ROOT = "/api/v1";

async function call(path, options = {}) {
  const response = await fetch(`${API_ROOT}${path}`, {
    ...options,
    headers: {
      "Content-Type": "application/json",
      ...(options.headers || {}),
    },
  });

  const text = await response.text();
  const payload = text ? (() => {
    try {
      return JSON.parse(text);
    } catch {
      return text;
    }
  })() : {};

  if (!response.ok) {
    const detail = payload?.detail || payload?.message || `Request failed (${response.status})`;
    throw new Error(detail);
  }
  return payload;
}

export const api = {
  health: () => call("/health"),
  listRuns: () => call("/runs"),
  createRun: (body) => call("/runs", { method: "POST", body: JSON.stringify(body) }),
  cancelRun: (runId) => call(`/runs/${encodeURIComponent(runId)}/cancel`, { method: "POST" }),
  runResult: (runId) => call(`/runs/${encodeURIComponent(runId)}/result`),
};

export function openapiLink(runId) {
  return `${API_ROOT}/runs/${encodeURIComponent(runId)}/openapi.yaml`;
}
