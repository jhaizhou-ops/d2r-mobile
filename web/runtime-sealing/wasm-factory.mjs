/**
 * 仅允许构建方控制初始化，任务载荷不能选择原生导出或代码。
 * Build-owned initialization only. Job payloads never choose native exports or code. */
export function byteFactory(factory, buffer, maxBytes = 64 * 1024 * 1024) {
  if (
    typeof factory !== "function" ||
    !(buffer instanceof ArrayBuffer) ||
    buffer.byteLength < 8 ||
    buffer.byteLength > maxBytes
  )
    throw new Error("SEALED_WASM_INITIALIZATION");
  const bytes = new Uint8Array(buffer);
  if (new DataView(buffer).getUint32(0, true) !== 0x6d736100) throw new Error("SEALED_WASM_MAGIC");
  const compiled = new WebAssembly.Module(bytes);
  return (options = {}) =>
    factory({
      ...options,
      wasmBinary: bytes,
      locateFile() {
        throw new Error("SEALED_WASM_EXTERNAL_RESOURCE");
      },
      instantiateWasm(imports, receive) {
        const instance = new WebAssembly.Instance(compiled, imports);
        receive(instance, compiled);
        return instance.exports;
      },
      print() {},
      printErr() {},
      onAbort() {},
    });
}

export function initializationWasm(assets, name) {
  if (
    !Array.isArray(assets) ||
    assets.length !== 1 ||
    !Array.isArray(assets[0]) ||
    assets[0].length !== 2 ||
    assets[0][0] !== name
  )
    throw new Error("SEALED_ASSET_INVENTORY");
  return assets[0][1];
}
