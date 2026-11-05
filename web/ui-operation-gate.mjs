// 同步占用异步界面操作；React 状态提交前也不能启动第二个操作。
// Acquire asynchronous UI operations synchronously, before React commits rendered busy state.
export function createUiOperationGate() {
  let active;
  return Object.freeze({
    get busy() {
      return active !== undefined;
    },
    begin() {
      if (active) return null;
      const controller = new AbortController();
      const lease = Object.freeze({
        controller,
        isCurrent: () => active === lease,
        finish() {
          if (active === lease) active = undefined;
        },
      });
      active = lease;
      return lease;
    },
    // 先撤销身份再取消；过期回调不能覆盖新操作或清除其 busy 状态。
    // Revoke identity before aborting; stale callbacks cannot overwrite a new operation or clear its busy state.
    cancel() {
      const previous = active;
      active = undefined;
      previous?.controller.abort();
    },
  });
}
