// 仅开发者验证，顺序运行真实 Worker；不是完整游戏或 Apple 运行时验收。
// Developer verification only: sequential real Workers, not full-game or Apple runtime acceptance.
const status = document.getElementById("status");
const cancel = document.getElementById("cancel");
const controller = new AbortController();
let stopped = false,
  active,
  rejectActive;
cancel.addEventListener("click", () => {
  stopped = true;
  status.textContent = "Cancelled";
  controller.abort();
  active?.terminate();
  rejectActive?.(new Error("Cancelled"));
});
async function bytes(path) {
  const response = await fetch(path, { signal: controller.signal });
  if (!response.ok) throw new Error("Application file unavailable");
  return response.arrayBuffer();
}
function compile(data) {
  if (stopped) return Promise.reject(new Error("Cancelled"));
  return new Promise((resolve, reject) => {
    const worker = new Worker("/web/toolchain/metal-air-compiler/browser-smoke-worker.mjs", {
      type: "module",
    });
    active = worker;
    let settled = false;
    const finish = (problem, result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      worker.terminate();
      active = undefined;
      rejectActive = undefined;
      if (problem) reject(problem);
      else resolve(result);
    };
    const timer = setTimeout(() => finish(new Error("Compiler timed out")), 600_000);
    rejectActive = (error) => finish(error);
    worker.onmessage = ({ data: result }) =>
      finish(result?.ok === true ? null : new Error(result?.code ?? "Worker failed"), result);
    worker.onerror = () => finish(new Error("Worker failed"));
    worker.onmessageerror = () => finish(new Error("Worker message failed"));
    try {
      worker.postMessage(data, [data.factoryBytes, data.wasmBytes]);
    } catch {
      finish(new Error("Could not start compiler"));
    }
  });
}
async function run() {
  const manifest = JSON.parse(new TextDecoder().decode(await bytes("/manifest.json")));
  const factoryBytes = await bytes("/compiler.mjs");
  const wasmBytes = await bytes("/compiler.wasm");
  const fixtures = await Promise.all(
    ["compute", "vertex", "fragment"].map(async (name) =>
      new TextDecoder("utf-8", { fatal: true }).decode(await bytes(`/fixture-${name}.metal`)),
    ),
  );
  const results = [];
  for (const [index, stage, macro, values] of [
    [0, "kernel", "FACTOR", [2, 5]],
    [1, "vertex", "SCALE", ["2.0f", "5.0f"]],
    [2, "fragment", "THRESHOLD", ["0.2f", "0.8f"]],
  ]) {
    const pair = [];
    for (const value of values) {
      status.textContent = `Compiling ${stage}: ${results.length + 1}/6`;
      const result = await compile({
        ...manifest,
        factoryBytes: factoryBytes.slice(0),
        wasmBytes: wasmBytes.slice(0),
        source: `#define ${macro} ${value}\n${fixtures[index]}`,
        entry: "main0",
        stage,
      });
      results.push({ stage, value, ...result });
      pair.push(result.inspection.functions.find((entry) => entry.name === "main0")?.bitcodeSha256);
    }
    if (!pair[0] || !pair[1] || pair[0] === pair[1])
      throw new Error("Changed source did not change executable bitcode");
  }
  return { ok: true, validation: "six-authored-browser-compilations-only", results };
}
try {
  const result = await run();
  globalThis.__airSmokeResult = result;
  status.textContent = JSON.stringify(result, null, 2);
} catch (error) {
  const message = stopped ? "Cancelled" : error.message;
  globalThis.__airSmokeResult = { ok: false, error: message };
  status.textContent = message;
} finally {
  active?.terminate();
  cancel.disabled = true;
}
