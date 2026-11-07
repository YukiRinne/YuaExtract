export interface TargetInteractionState {
  jobStatus: string;
  restoreTasks: ReadonlyArray<{ status: string }>;
}

export function targetInteractionBusy(state: TargetInteractionState): boolean {
  return (
    state.jobStatus === "running" ||
    state.jobStatus === "cancelling" ||
    state.restoreTasks.some((task) => task.status === "running" || task.status === "mining")
  );
}
