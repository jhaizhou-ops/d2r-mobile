// 仅包含构建方持有的受信验证代码；不提供文件选择器或真实输入接口。
// Trusted, build-owned proof code only. No file picker or real-input interface.
let started = false;
const MAX_WASM = 16 * 1024 * 1024;
async function blockedFetch(url) {
  try {
    await fetch(url);
    return false;
  } catch {
    return true;
  }
}
async function blockedImport(url) {
  try {
    await import(url);
    return false;
  } catch {
    return true;
  }
}
self.onmessage = async ({ data }) => {
  if (started) return;
  started = true;
  try {
    if (
      data?.type !== "initialize" ||
      !(data.wasm instanceof ArrayBuffer) ||
      data.wasm.byteLength < 8 ||
      data.wasm.byteLength > MAX_WASM ||
      ![data.moduleUrl, data.adapterUrl].every(
        (url) => typeof url === "string" && url.startsWith(`blob:${self.location.origin}/`),
      ) ||
      !Array.isArray(data.probes) ||
      data.probes.length !== 2 ||
      !data.probes.every((url) => /^http:\/\/127\.0\.0\.1:\d+\/probe\/[a-z]+$/.test(url))
    )
      throw new Error("Invalid bounded proof initialization");
    const blocked = await Promise.all(
      data.probes.flatMap((url) => [blockedFetch(url), blockedImport(url)]),
    );
    if (!blocked.every(Boolean)) throw new Error("Worker network seal failed");
    const { default: factory } = await import(data.moduleUrl);
    const { createTextureDecoder } = await import(data.adapterUrl);
    const wasmBytes = new Uint8Array(data.wasm);
    let instantiations = 0;
    const decoder = await createTextureDecoder(factory, {
      wasmBinary: wasmBytes,
      instantiateWasm(imports, receive) {
        const compiled = new WebAssembly.Module(wasmBytes);
        const instance = new WebAssembly.Instance(compiled, imports);
        instantiations++;
        receive(instance, compiled);
        return instance.exports;
      },
      print() {},
      printErr() {},
    });
    if (instantiations !== 1) throw new Error("Custom WASM byte path was not used");
    const descriptor = {
      target: 1,
      format: 0x41,
      flags: 0x10,
      samples: 0,
      width: 4,
      height: 4,
      depth: 1,
      levels: 1,
      stride: 0,
      pkg_block_h: 0,
    };
    const layout = decoder.layout(descriptor);
    if (layout.storageBytes < 8 || layout.storageBytes > 4096)
      throw new Error("Synthetic layout out of bounds");
    const source = new Uint8Array(layout.storageBytes);
    source.set([0, 248, 0, 0, 0, 0, 0, 0]);
    const result = decoder.decode(descriptor, source);
    if (
      result.width !== 4 ||
      result.height !== 4 ||
      result.pixels.length !== 64 ||
      !result.pixels.every((value, i) => value === [255, 0, 0, 255][i % 4])
    )
      throw new Error("BC1 known-red oracle mismatch");
    let rejected = false;
    try {
      decoder.decode(descriptor, new Uint8Array(1));
    } catch {
      rejected = true;
    }
    if (!rejected) throw new Error("Malformed source accepted");
    const after = await Promise.all(
      data.probes.flatMap((url) => [blockedFetch(url), blockedImport(url)]),
    );
    if (!after.every(Boolean)) throw new Error("Worker lost network seal");
    self.postMessage({
      ok: true,
      blocked: blocked.length + after.length,
      pixels: 64,
      instantiations,
    });
  } catch {
    self.postMessage({ ok: false, error: "Offline runtime proof failed" });
  }
};
