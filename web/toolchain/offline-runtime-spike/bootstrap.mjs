/* global document */
// 构建时替换此摘要；此验证程序不接收用户数据。
// Build replaces this digest; no user data is accepted by this proof harness.
const MANIFEST_SHA256 = "__MANIFEST_SHA256__";
const EXPECTED = ["texture.mjs", "texture.wasm", "browser-texture.mjs", "worker.mjs"];
const MAX_ASSET = 16 * 1024 * 1024;
const MAX_TOTAL = 32 * 1024 * 1024;
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
globalThis.runOfflineProof = async (probes) => {
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
    const manifestBytes = await readBounded("manifest.json", 16384);
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
    const moduleUrl = objectUrl("texture.mjs");
    const adapterUrl = objectUrl("browser-texture.mjs");
    const wasm = assets.get("texture.wasm").buffer;
    const result = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("Worker proof timed out")), 30000);
      worker.onmessage = ({ data }) => {
        clearTimeout(timer);
        resolve(data);
      };
      worker.onerror = () => {
        clearTimeout(timer);
        reject(new Error("Worker proof failed"));
      };
      worker.postMessage({ type: "initialize", moduleUrl, adapterUrl, wasm, probes }, [wasm]);
    });
    if (!result.ok) throw new Error("Worker proof failed");
    return { ...result, documentBlocked: blocked.length, transferred: wasm.byteLength === 0 };
  } finally {
    worker?.terminate();
    for (const url of urls) URL.revokeObjectURL(url);
  }
};
