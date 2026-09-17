export const state = {
  runs: [],
  selectedRunId: "",
  polling: 0,
};

export function setRuns(runs) {
  state.runs = runs;
}

export function activeRun() {
  return state.runs.find((run) => run.id === state.selectedRunId) || null;
}
