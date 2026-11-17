// 在工作线程内加载并调用 .NET 运行时；依据 web/toolchain/offline-runtime-spike 的模块与调用约定。
// Load and invoke the .NET runtime inside a worker using the module and calling contracts in web/toolchain/offline-runtime-spike.
let initialized = false;
let started = false;
let phase = "new";
let exports;
let queue = Promise.resolve();
const urls = [];
function setPhase(next) {
  phase = next;
  if (!initialized) self.postMessage({ type: "initialization-stage", stage: next });
}
function reportInitializationFailure(error) {
  if (!started || initialized) return;
  self.postMessage({
    id: 0,
    ok: false,
    error: `Offline .NET initialization failed (${phase}): ${String(error?.message ?? error).slice(0, 4000)}`,
  });
}
self.addEventListener("unhandledrejection", (event) => reportInitializationFailure(event.reason));
self.addEventListener("error", (event) =>
  reportInitializationFailure(event.error ?? event.message),
);

function nativeModuleConfig(resources) {
  const nativeWasm = resources.wasmNative;
  if (
    !Array.isArray(nativeWasm) ||
    nativeWasm.length !== 1 ||
    typeof nativeWasm[0]?.resolvedUrl !== "string"
  )
    throw new Error("Expected one resolved native runtime asset");
  const { name, resolvedUrl } = nativeWasm[0];
  return {
    // Emscripten 在 .NET 安装启动钩子前解析此别名；缺少 locateFile 时会在 blob URL 上调用 new URL(alias, import.meta.url)。
    // Emscripten resolves this alias before .NET installs its startup hooks.
    // Without locateFile it calls new URL(alias, import.meta.url) on a blob URL.
    locateFile(path) {
      if (path !== "dotnet.native.wasm" && path !== name)
        throw new Error("Unknown native runtime file");
      return resolvedUrl;
    },
  };
}

async function probe(destinations) {
  let blocked = 0;
  for (const url of destinations) {
    try {
      await fetch(url);
    } catch {
      blocked++;
    }
    try {
      await import(url);
    } catch {
      blocked++;
    }
  }
  if (blocked !== 4) throw new Error("Worker network seal failed");
  return blocked;
}
async function initialize(data) {
  if (started) throw new Error("One-shot initialization");
  started = true;
  const assets = new Map(data.assets);
  if (!assets.size || assets.size > 1024) throw new Error("Runtime closure cap");
  for (const [name, bytes] of assets)
    if (
      !/^[a-zA-Z0-9][a-zA-Z0-9_.+-]*$/.test(name) ||
      !(bytes instanceof ArrayBuffer) ||
      !bytes.byteLength ||
      bytes.byteLength > 64 * 1024 * 1024
    )
      throw new Error("Invalid verified asset");
  function moduleUrl(name) {
    const bytes = assets.get(name);
    if (!bytes || !name.endsWith(".js")) throw new Error("Unknown module");
    const url = URL.createObjectURL(new Blob([bytes], { type: "text/javascript" }));
    urls.push(url);
    return url;
  }
  setPhase("probe-before");
  const before = await probe(data.probes);
  setPhase("config-module");
  const { config } = await import(moduleUrl("dotnet.boot.js"));
  if (
    !config?.resources ||
    config.assets ||
    config.remoteSources?.length ||
    config.appsettings?.length
  )
    throw new Error("Unsupported runtime config");
  const allowed = new Set([
    "hash",
    "jsModuleNative",
    "jsModuleRuntime",
    "jsModuleWorker",
    "jsModuleDiagnostics",
    "wasmNative",
    "wasmSymbols",
    "coreAssembly",
    "assembly",
    "corePdb",
    "pdb",
    "icu",
    "coreVfs",
    "vfs",
    "lazyAssembly",
    "satelliteResources",
    "modulesAfterConfigLoaded",
    "modulesAfterRuntimeReady",
  ]);
  const requested = new Set();
  const declared = new Set();
  function configure(entry) {
    if (!entry || typeof entry.name !== "string" || !assets.has(entry.name))
      throw new Error("Undeclared runtime dependency");
    declared.add(entry.name);
    // 提前解析：在加载器回调前，URL(relative, blob:) 无效。
    // Resolve up front: URL(relative, blob:) is invalid before the loader callback.
    entry.resolvedUrl = entry.name.endsWith(".js")
      ? moduleUrl(entry.name)
      : `https://offline.invalid/${entry.name}`;
  }
  for (const [kind, value] of Object.entries(config.resources)) {
    if (!allowed.has(kind)) throw new Error(`Unreviewed resource kind ${kind}`);
    if (kind === "hash") continue;
    if (
      ["modulesAfterConfigLoaded", "modulesAfterRuntimeReady", "satelliteResources"].includes(kind)
    ) {
      if (Object.keys(value ?? {}).length) throw new Error(`Unsupported resource kind ${kind}`);
    } else {
      if (!Array.isArray(value)) throw new Error(`Expected asset array ${kind}`);
      for (const entry of value) {
        configure(entry);
        // .NET10 不会通过 withResourceLoader 路由符号文件。
        // Symbols are not routed through withResourceLoader by .NET10.
        if (kind === "wasmSymbols")
          entry.pendingDownload = {
            name: entry.name,
            url: entry.resolvedUrl,
            response: Promise.resolve(new Response(assets.get(entry.name).slice(0))),
          };
      }
    }
  }
  setPhase("loader-module");
  const { dotnet } = await import(moduleUrl("dotnet.js"));
  function resourceLoader(type, name, _uri, integrity) {
    if (!declared.has(name) || !assets.has(name))
      throw new Error("Unknown runtime resource request");
    requested.add(name);
    if (type === "dotnetjs") return moduleUrl(name);
    return (async () => {
      const bytes = assets.get(name);
      if (integrity) {
        const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
        const expected = "sha256-" + btoa(String.fromCharCode(...digest));
        if (integrity !== expected) throw new Error("Runtime resource integrity mismatch");
      }
      return new Response(bytes.slice(0), {
        headers: {
          "Content-Type": name.endsWith(".wasm") ? "application/wasm" : "application/octet-stream",
        },
      });
    })();
  }
  setPhase("runtime-create");
  const runtime = await dotnet
    .withConfig(config)
    .withModuleConfig(nativeModuleConfig(config.resources))
    .withResourceLoader(resourceLoader)
    .create();
  setPhase("assembly-exports");
  exports = await runtime.getAssemblyExports(runtime.getConfig().mainAssemblyName);
  if (typeof exports?.ShaderBridge?.Translate !== "function")
    throw new Error("Missing real translator export");
  setPhase("probe-after");
  const after = await probe(data.probes);
  initialized = true;
  setPhase("ready");
  return {
    initialized: true,
    blocked: before + after,
    declared: declared.size,
    reads: requested.size,
    mainAssembly: runtime.getConfig().mainAssemblyName,
  };
}
function run(data) {
  if (
    !initialized ||
    !(data.payload instanceof ArrayBuffer) ||
    data.payload.byteLength > 1024 * 1024 ||
    !data.job ||
    JSON.stringify(data.job).length > 65536 ||
    !["cs", "vs", "ps"].includes(data.job.stage)
  )
    throw new Error("Invalid bounded shader input");
  const result = JSON.parse(
    exports.ShaderBridge.Translate(JSON.stringify(data.job), new Uint8Array(data.payload)),
  );
  if (JSON.stringify(result).length > 256 * 1024) throw new Error("Shader result cap");
  return result;
}
// .NET10 根据未设置的 onmessage 槽识别独立工作线程运行时；与原始着色器工作线程一样，立即注册但不占用该槽。
// .NET10 identifies an independent worker runtime by an unset onmessage slot.
// Register immediately without occupying that slot, like the original shader worker.
self.addEventListener("message", ({ data }) => {
  if (data?.type !== "initialize" && !initialized) {
    self.postMessage({ id: data?.id, ok: false, error: "Runtime not initialized" });
    return;
  }
  queue = queue.then(async () => {
    try {
      const result = data?.type === "initialize" ? await initialize(data) : run(data);
      self.postMessage({ id: data.id, ok: true, result });
    } catch (error) {
      const detail =
        !initialized && data?.type === "initialize"
          ? `: ${String(error?.message ?? error).slice(0, 4000)}`
          : "";
      self.postMessage({
        id: data?.id ?? 0,
        ok: false,
        error: `Offline .NET operation failed (${phase})${detail}`,
      });
    }
  });
});
