const MAX_RUNS = 5000;

export const state = {
  runs: [],
  selectedRunId: "",
  polling: 0,
};

export function setRuns(runs) {
  if (!Array.isArray(runs)) {
    state.runs = [];
    return;
  }
  // Shallow copy, bound length, sort by updated_at desc for deterministic UI.
  const copy = runs.slice(0, MAX_RUNS);
  copy.sort((a, b) => {
    const da = Date.parse(a.updated_at || 0) || 0;
    const db = Date.parse(b.updated_at || 0) || 0;
    return db - da;
  });
  state.runs = copy;
}

export function activeRun() {
  if (!state.selectedRunId) return null;
  return state.runs.find((run) => run.id === state.selectedRunId) || null;
}

export function clearSelection() {
  state.selectedRunId = "";
}
