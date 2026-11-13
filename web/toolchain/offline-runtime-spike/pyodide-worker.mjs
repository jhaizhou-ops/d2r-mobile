// 专用合成验证工作线程；仅在初始化完成后接受私密输入任务。
// Dedicated synthetic-only worker. Private-input jobs are accepted only after initialization.
const OPERATION_LIMITS = Object.freeze({
  "image-default-frame": [20, 4096],
  "experience-table": [20, 4096],
  "overlay-paths": [20, 4096],
  "derived-tables": [20, 4096],
  "vault-table": [20, 4096],
  "native-art": [20, 4096],
  "hud-v4": [64, 4096],
  "map-goal": [20, 4096],
  "casc-overlay": [64, 1048576],
  "mercenary-art": [20, 4096],
  "overlay-recipe": [128, 4096],
  "remaining-art": [20, 4096],
});
const RUNTIME_ORIGIN = "https://offline.invalid";
let initialized = false;
let starting = false;
let py;
let transforms;
let cascTransform;
let phase = "validate";
let queue = Promise.resolve();
const objectUrls = [];
const digest = async (bytes) => new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
const b64 = (bytes) => btoa(String.fromCharCode(...bytes));
async function probe(urls) {
  let rejected = 0;
  for (const url of urls) {
    try {
      await fetch(url);
    } catch {
      rejected++;
    }
    try {
      await import(url);
    } catch {
      rejected++;
    }
  }
  if (rejected !== 4) throw new Error("Network seal failed");
  return rejected;
}
async function initialize(data) {
  if (starting || initialized) throw new Error("Already initialized");
  starting = true;
  const probes = data.probes ?? [];
  if (
    !Array.isArray(data.assets) ||
    data.assets.length > 32 ||
    !Array.isArray(probes) ||
    ![0, 2].includes(probes.length) ||
    !probes.every((url) => /^http:\/\/127\.0\.0\.1:\d+\/probe\/[a-z]+$/.test(url))
  )
    throw new Error("Invalid proof initialization");
  const assets = new Map();
  let total = 0;
  for (const [name, bytes] of data.assets) {
    if (
      typeof name !== "string" ||
      !/^[a-zA-Z0-9_.+-]+$/.test(name) ||
      assets.has(name) ||
      !(bytes instanceof ArrayBuffer) ||
      !bytes.byteLength ||
      bytes.byteLength > 32 * 1024 * 1024
    )
      throw new Error("Invalid bounded runtime asset");
    total += bytes.byteLength;
    assets.set(name, new Uint8Array(bytes));
  }
  if (total > 96 * 1024 * 1024) throw new Error("Runtime cap exceeded");
  let reads = 0;
  // 此回调仅用于三个来源锁定的 Pyodide 内部调用点。
  // 全局 fetch、import、XHR 及浏览器 CSP 均不会被替换。
  // This callback is used only at three source-locked Pyodide internal call sites.
  // Global fetch, import, XHR and the browser CSP are never replaced.
  Object.defineProperty(globalThis, "__readOfflinePyodide", {
    value: async (value, integrity) => {
      const url = new URL(value);
      const prefix = "/v0.27.7/";
      if (
        url.origin !== RUNTIME_ORIGIN ||
        !url.pathname.startsWith(prefix) ||
        url.search ||
        url.hash
      )
        throw new Error("Unknown offline resource");
      const name = url.pathname.slice(prefix.length);
      const bytes = assets.get(name);
      if (!bytes) throw new Error("Unpackaged offline resource");
      if (integrity !== undefined && integrity !== `sha256-${b64(await digest(bytes))}`)
        throw new Error("Offline package integrity mismatch");
      reads++;
      return new Response(bytes, {
        headers: {
          "Content-Type": name.endsWith(".wasm") ? "application/wasm" : "application/octet-stream",
        },
      });
    },
  });
  const before = probes.length ? await probe(probes) : 0;
  function moduleUrl(name) {
    const bytes = assets.get(name);
    if (!bytes) throw new Error("Missing runtime code");
    const url = URL.createObjectURL(new Blob([bytes], { type: "text/javascript" }));
    objectUrls.push(url);
    return url;
  }
  phase = "import-table-transforms";
  transforms = await import(moduleUrl("asset-transforms.mjs"));
  phase = "initialize-casc-codec";
  const { createPngCodec } = await import(moduleUrl("png-codec.mjs"));
  const codec = await createPngCodec(assets.get("png-codec.wasm"));
  const { createMD5 } = await import(moduleUrl("hash-wasm.mjs"));
  const md5Hasher = await createMD5();
  const { createPinnedCascOverlayTransform } = await import(moduleUrl("casc-overlay.mjs"));
  cascTransform = createPinnedCascOverlayTransform({ codec, md5Hasher });
  phase = "import-runtime";
  await import(moduleUrl("pyodide.asm.js"));
  const { loadPyodide } = await import(moduleUrl("pyodide.mjs"));
  phase = "initialize-python";
  py = await loadPyodide({
    indexURL: `${RUNTIME_ORIGIN}/v0.27.7/`,
    stdout() {},
    stderr() {},
  });
  phase = "register-art-kernels";
  py.registerJsModule("art_kernel_bridge", await import(moduleUrl("art-kernels.mjs")));
  phase = "load-packages";
  const packageErrors = [];
  await py.loadPackage(["pillow"], {
    checkIntegrity: true,
    messageCallback() {},
    errorCallback(message) {
      if (packageErrors.length < 4) packageErrors.push(String(message).slice(0, 1000));
    },
  });
  const packages = {
    // Pyodide 记录锁文件中的元数据名称（Pillow），而不是规范化的查找键。
    // Pyodide records lock metadata name (Pillow), not normalized lookup key.
    pillow: Object.hasOwn(py.loadedPackages, "Pillow"),
  };
  if (py.version !== "0.27.7" || !packages.pillow)
    throw new Error(
      `Runtime package initialization incomplete ${JSON.stringify(packages)}: ${packageErrors.join("; ")}`,
    );
  for (const name of [
    "art_pixels.py",
    "native_functions.py",
    "core.py",
    "native_art_original.py",
    "native_art.py",
    "hud_original.py",
    "hud_design_original.py",
    "hud_support_original.py",
    "hud.py",
    "map_goal_original.py",
    "map_goal.py",
    "mercenary_original.py",
    "mercenary.py",
    "overlay_recipe_sources.py",
    "overlay_recipe.py",
    "remaining_art_original.py",
    "trove_original.py",
    "remaining_art.py",
  ]) {
    const bytes = assets.get(name);
    if (!bytes) throw new Error("Missing packaged Python source");
    py.FS.writeFile(`/home/pyodide/${name}`, bytes);
  }
  for (const [stage, code] of [
    ["import-pillow", "from PIL import Image"],
    ["import-native-functions", "import native_functions"],
    ["import-native-art", "import native_art_original, native_art"],
    ["import-hud", "import hud_original, hud_design_original, hud_support_original, hud"],
    ["import-map-goal", "import map_goal_original, map_goal"],
    ["import-mercenary", "import mercenary_original, mercenary"],
    ["import-overlay-recipe", "import overlay_recipe_sources, overlay_recipe"],
    ["import-remaining-art", "import remaining_art_original, trove_original, remaining_art"],
    ["import-core", "from core import execute"],
  ]) {
    phase = stage;
    await py.runPythonAsync(code);
  }
  const after = probes.length ? await probe(probes) : 0;
  // 未知或缺失的依赖绝不回退到网络。
  // Unknown/missing dependencies never fall back to network.
  let missingRejected = false;
  try {
    await globalThis.__readOfflinePyodide(`${RUNTIME_ORIGIN}/v0.27.7/missing.whl`);
  } catch {
    missingRejected = true;
  }
  if (!missingRejected) throw new Error("Unknown resource accepted");
  initialized = true;
  phase = "ready";
  return { initialized: true, blocked: before + after, reads, version: py.version, packages };
}
async function run(data) {
  const limits = Object.hasOwn(OPERATION_LIMITS, data?.operation)
    ? OPERATION_LIMITS[data.operation]
    : null;
  if (
    !initialized ||
    !Number.isSafeInteger(data.id) ||
    data.id < 1 ||
    !limits ||
    !(data.payload instanceof ArrayBuffer) ||
    data.payload.byteLength > limits[0] * 1024 * 1024 ||
    typeof data.options !== "object" ||
    data.options === null ||
    JSON.stringify(data.options).length > limits[1]
  )
    throw new Error("Invalid bounded asset request");
  // 表格/路径工作不再进入 Python；保留同一队列和传输边界。 | Table/path jobs bypass Python in the same serial transfer boundary.
  if (transforms.hasAssetTransform(data.operation)) {
    const result = await transforms.executeAssetTransform(
      data.operation,
      data.payload,
      data.options,
    );
    postResult(data.id, result);
    return;
  }
  if (data.operation === "casc-overlay") {
    postResult(data.id, await cascTransform(data.payload, data.options));
    return;
  }
  py.globals.set("_op", data.operation);
  py.globals.set("_payload", new Uint8Array(data.payload));
  py.globals.set("_options_json", JSON.stringify(data.options));
  let proxy;
  try {
    proxy = py.runPython('execute(_op, bytes(_payload), __import__("json").loads(_options_json))');
    const result = proxy.toJs({ dict_converter: Object.fromEntries });
    postResult(data.id, result);
  } finally {
    proxy?.destroy();
    py.globals.delete("_op");
    py.globals.delete("_payload");
    py.globals.delete("_options_json");
  }
}
function postResult(id, result) {
  const transfers = [];
  for (const [name, bytes] of Object.entries(result.files)) {
    const copy = new Uint8Array(bytes);
    result.files[name] = copy.buffer;
    transfers.push(copy.buffer);
  }
  self.postMessage({ id, ok: true, result }, transfers);
}
self.onmessage = ({ data }) => {
  // 初始化尚未证明网络隔离时，不将到达的任务加入队列。
  // Do not queue jobs arriving while initialization is network-unproven.
  if (data?.type !== "initialize" && !initialized) {
    self.postMessage({ id: data?.id, ok: false, error: "Runtime not initialized" });
    return;
  }
  queue = queue.then(async () => {
    try {
      if (data?.type === "initialize")
        self.postMessage({ id: 0, ok: true, result: await initialize(data) });
      else await run(data);
    } catch (error) {
      // 初始化不能接收任务输入；其有界诊断仅包含公开运行时及源码信息，
      // 任务失败信息仍保持脱敏。
      // Initialization cannot receive job input. Its bounded diagnostics contain
      // only public runtime/source information; job failures remain sanitized.
      const detail =
        !initialized && data?.type === "initialize"
          ? `: ${String(error?.message ?? error).slice(0, 4000)}`
          : "";
      self.postMessage({
        id: data?.id ?? 0,
        ok: false,
        error: `Offline Pyodide operation failed (${phase})${detail}`,
      });
    }
  });
};
