/* global document */
// 构建时替换此摘要；此验证程序不接收用户数据。
// Build replaces this digest; no user data is accepted by this proof harness.
const MANIFEST_SHA256 = "__MANIFEST_SHA256__";
const EXPECTED = ["__EXPECTED_ASSETS__"];
const MAX_ASSET = 64 * 1024 * 1024;
const MAX_TOTAL = 256 * 1024 * 1024;
const digest = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (b) =>
    b.toString(16).padStart(2, "0"),
  ).join("");
async function readBounded(path, cap) {
  const response = await fetch(new URL(path, import.meta.url), {
    credentials: "omit",
    redirect: "error",
    cache: "no-store",
  });
  if (!response.ok || !response.body || response.headers.get("content-type")?.includes("text/html"))
    throw new Error("Missing public runtime asset");
  const reader = response.body.getReader();
  const chunks = [];
  let length = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      length += value.byteLength;
      if (length > cap) throw new Error("Public asset size exceeded");
      chunks.push(value);
    }
  } catch (error) {
    await reader.cancel();
    throw error;
  } finally {
    reader.releaseLock();
  }
  const result = new Uint8Array(length);
  let offset = 0;
  for (const chunk of chunks) {
    result.set(chunk, offset);
    offset += chunk.length;
  }
  return result;
}
let started = false;
globalThis.startOfflineDotnet = async (probes) => {
  if (started) throw new Error("One-shot proof already started");
  started = true;
  const urls = [];
  let worker;
  try {
    if (
      !isSecureContext ||
      !crypto?.subtle ||
      typeof Worker !== "function" ||
      typeof WebAssembly !== "object" ||
      navigator.serviceWorker?.controller
    )
      throw new Error("Required clean browser capability unavailable");
    if (
      !Array.isArray(probes) ||
      probes.length !== 2 ||
      !probes.every((url) => /^http:\/\/127\.0\.0\.1:\d+\/probe\/[a-z]+$/.test(url))
    )
      throw new Error("Invalid proof destinations");
    const manifestBytes = await readBounded("manifest.json", 131072);
    if ((await digest(manifestBytes)) !== MANIFEST_SHA256)
      throw new Error("Manifest digest mismatch");
    const manifest = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(manifestBytes));
    if (
      manifest.version !== 1 ||
      !Array.isArray(manifest.assets) ||
      manifest.assets.length !== EXPECTED.length
    )
      throw new Error("Unexpected public asset manifest");
    let total = 0;
    for (let i = 0; i < EXPECTED.length; i++) {
      const asset = manifest.assets[i];
      if (
        asset.path !== EXPECTED[i] ||
        !Number.isSafeInteger(asset.bytes) ||
        asset.bytes < 1 ||
        asset.bytes > MAX_ASSET ||
        !/^[a-f0-9]{64}$/.test(asset.sha256)
      )
        throw new Error("Invalid public asset entry");
      total += asset.bytes;
    }
    if (total > MAX_TOTAL) throw new Error("Public runtime total exceeded");
    const assets = new Map();
    // 顺序加载限制临时内存，并在密封前完成。
    // Sequential loads bound temporary memory and finish before sealing.
    for (const asset of manifest.assets) {
      const bytes = await readBounded(asset.path, asset.bytes);
      if (bytes.length !== asset.bytes || (await digest(bytes)) !== asset.sha256)
        throw new Error("Public asset integrity mismatch");
      assets.set(asset.path, bytes);
    }
    const seal = document.createElement("meta");
    seal.httpEquiv = "Content-Security-Policy";
    seal.content =
      "default-src 'none'; connect-src 'none'; script-src blob: 'wasm-unsafe-eval'; worker-src blob:; style-src 'none'; img-src 'none'; font-src 'none'; frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'";
    document.head.append(seal);
    // 仅用于测试的进程间通信标记：采用 Playwright 绑定，而非 HTTP 请求或 CSP 控制。
    // Test-only IPC marker: Playwright binding, not an HTTP request or CSP control.
    await globalThis.offlineProofSealed?.();
    const blocked = [];
    for (const url of probes) {
      try {
        await fetch(url);
        blocked.push(false);
      } catch {
        blocked.push(true);
      }
    }
    for (const url of probes) {
      try {
        await import(url);
        blocked.push(false);
      } catch {
        blocked.push(true);
      }
    }
    if (!blocked.every(Boolean)) throw new Error("Document network seal failed");
    function objectUrl(name) {
      const url = URL.createObjectURL(new Blob([assets.get(name)], { type: "text/javascript" }));
      urls.push(url);
      return url;
    }
    worker = new Worker(objectUrl("worker.mjs"), { type: "module" });
    const pending = new Map();
    let id = 0;
    let initializationStage = "worker-created";
    worker.onmessage = ({ data }) => {
      if (data?.type === "initialization-stage" && pending.has(0)) {
        if (typeof data.stage === "string" && /^[a-z-]{1,48}$/.test(data.stage))
          initializationStage = data.stage;
        return;
      }
      const entry = pending.get(data.id);
      if (!entry) return;
      pending.delete(data.id);
      clearTimeout(entry.timer);
      if (data.ok) entry.resolve(data.result);
      else entry.reject(new Error(data.error));
    };
    function close() {
      worker?.terminate();
      for (const url of urls) URL.revokeObjectURL(url);
      for (const entry of pending.values()) {
        clearTimeout(entry.timer);
        entry.reject(new Error("Proof worker closed"));
      }
      pending.clear();
    }
    worker.onerror = () => close();
    function request(data, transfers = []) {
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          pending.delete(data.id);
          reject(
            new Error(
              data.id === 0
                ? `Proof initialization timeout (${initializationStage})`
                : "Proof timeout",
            ),
          );
          close();
        }, 60000);
        pending.set(data.id, { resolve, reject, timer });
        worker.postMessage(data, transfers);
      });
    }
    const runtimeAssets = [...assets]
      .filter(([name]) => name !== "worker.mjs")
      .map(([name, bytes]) => [name, bytes.buffer]);
    const transfers = runtimeAssets.map(([, buffer]) => buffer);
    const result = await request(
      { id: 0, type: "initialize", assets: runtimeAssets, probes },
      transfers,
    );
    if (!result.initialized || !transfers.every((buffer) => buffer.byteLength === 0))
      throw new Error("Runtime initialization/transfer proof failed");
    return {
      stats: { ...result, documentBlocked: blocked.length, transferred: true },
      async run(job, bytes) {
        if (!(bytes instanceof Uint8Array) || bytes.byteLength > 1024 * 1024)
          throw new Error("Invalid proof input");
        const payload = bytes.slice().buffer;
        return request({ id: ++id, type: "run", job, payload }, [payload]);
      },
      close,
    };
  } catch (error) {
    worker?.terminate();
    for (const url of urls) URL.revokeObjectURL(url);
    throw error;
  }
};
