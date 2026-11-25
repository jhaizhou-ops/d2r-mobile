// 依据浏览器调度器和消息端口 API，让出一个任务并在取消时释放本次等待。
// Yield one task using browser scheduler/message-port APIs and release this wait on abort.
export async function yieldBrowserTask({ signal } = {}) {
  signal?.throwIfAborted();
  await new Promise((resolve, reject) => {
    let channel,
      settled = false;
    const closePorts = () => {
      if (!channel) return;
      for (const port of [channel.port1, channel.port2]) {
        port.onmessage = null;
        port.onmessageerror = null;
        try {
          port.close();
        } catch {
          // 清理失败不得掩盖原始结果，也不能阻止另一个端口关闭。
          // Cleanup failure must not mask the result or prevent closing the other port.
        }
      }
      channel = undefined;
    };
    const finish = (ok, error) => {
      if (settled) return;
      settled = true;
      signal?.removeEventListener("abort", abort);
      closePorts();
      if (ok) resolve();
      else reject(error);
    };
    const abort = () => finish(false, signal.reason);
    signal?.addEventListener("abort", abort, { once: true });
    try {
      signal?.throwIfAborted();
      const scheduler = globalThis.scheduler;
      if (typeof scheduler?.yield === "function") {
        // 调度器没有取消参数；即使调用方已取消，也观察其最终拒绝。
        // The scheduler has no abort option; observe late rejection after caller cancellation.
        Promise.resolve(scheduler.yield()).then(
          () => finish(true),
          (error) => finish(false, error),
        );
      } else {
        channel = new MessageChannel();
        signal?.throwIfAborted();
        channel.port1.onmessage = () => finish(true);
        channel.port1.onmessageerror = () => finish(false, new Error("Task yield message failed"));
        channel.port2.postMessage(null);
      }
    } catch (error) {
      finish(false, error);
      closePorts();
    }
  });
  signal?.throwIfAborted();
}
