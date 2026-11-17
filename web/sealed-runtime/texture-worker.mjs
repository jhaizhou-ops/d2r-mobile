// 依据已验证的纹理运行时字节及工作线程消息契约桥接纹理解码。
// 受信任且由应用拥有的运行时桥接。所有代码及 WASM 均以已验证字节到达。
// Bridge texture decoding using verified texture-runtime bytes and the worker message contract.
// Trusted app-owned runtime bridge. All code and WASM arrive as verified bytes.
let ready = false,
  started = false,
  decoder,
  encoder;
let queue = Promise.resolve();
const urls = [];
async function initialize(data) {
  if (started) throw new Error("One-shot runtime");
  started = true;
  const assets = new Map(data.assets);
  const expected = [
    "texture.mjs",
    "texture.wasm",
    "bc3.wasm",
    "browser-texture.mjs",
    "browser-texture-encoder.mjs",
  ];
  if (
    assets.size !== expected.length ||
    expected.some(
      (name) =>
        !(assets.get(name) instanceof ArrayBuffer) ||
        !assets.get(name).byteLength ||
        assets.get(name).byteLength > 64 * 1024 * 1024,
    )
  )
    throw new Error("Invalid verified texture closure");
  function url(bytes) {
    const u = URL.createObjectURL(new Blob([bytes], { type: "text/javascript" }));
    urls.push(u);
    return u;
  }
  const decoderUrl = url(assets.get("browser-texture.mjs"));
  const encoderSource = new TextDecoder("utf-8", { fatal: true }).decode(
    assets.get("browser-texture-encoder.mjs"),
  );
  const link = 'from "./browser-texture.mjs"';
  if (encoderSource.split(link).length !== 2) throw new Error("Encoder module linkage drift");
  const encoderUrl = url(encoderSource.replace(link, `from ${JSON.stringify(decoderUrl)}`));
  const { default: factory } = await import(url(assets.get("texture.mjs")));
  const { createTextureDecoder } = await import(decoderUrl);
  const { createTextureEncoder } = await import(encoderUrl);
  const wasm = new Uint8Array(assets.get("texture.wasm"));
  let instantiations = 0;
  const module = await factory({
    wasmBinary: wasm,
    instantiateWasm(imports, receive) {
      const compiled = new WebAssembly.Module(wasm);
      const instance = new WebAssembly.Instance(compiled, imports);
      instantiations++;
      receive(instance, compiled);
      return instance.exports;
    },
    print() {},
    printErr() {},
  });
  if (instantiations !== 1) throw new Error("WASM byte instantiation not used");
  decoder = await createTextureDecoder(async () => module);
  const bc3Module = new WebAssembly.Module(assets.get("bc3.wasm"));
  if (WebAssembly.Module.imports(bc3Module).length) throw new Error("BC3 host imports forbidden");
  const bc3 = new WebAssembly.Instance(bc3Module, {}).exports;
  encoder = await createTextureEncoder(
    async () => module,
    (rgba, width, height) => {
      let outputLength = 0;
      try {
        new Uint8Array(bc3.memory.buffer, bc3.bc3_input(), rgba.byteLength).set(rgba);
        outputLength = bc3.bc3_encode(width, height, rgba.byteLength);
        if (outputLength !== Math.ceil(width / 4) * Math.ceil(height / 4) * 16)
          throw new Error("BC3 encoder rejected input");
        return new Uint8Array(bc3.memory.buffer, bc3.bc3_output(), outputLength).slice();
      } finally {
        bc3.bc3_clear(rgba.byteLength, outputLength);
      }
    },
  );
  ready = true;
  return { initialized: true, instantiations };
}
async function run(data) {
  if (!ready || !Number.isSafeInteger(data.id) || data.id < 1)
    throw new Error("Runtime not initialized");
  let result;
  if (data.operation === "layout") result = decoder.layout(data.descriptor, data.options);
  else if (data.operation === "decode") {
    if (!(data.source instanceof Uint8Array) || data.source.byteLength > 64 * 1024 * 1024)
      throw new Error("Input cap");
    result = decoder.decode(data.descriptor, data.source, data.options);
  } else if (data.operation === "decodePillowBc3") {
    if (!(data.source instanceof Uint8Array) || data.source.byteLength > 64 * 1024 * 1024)
      throw new Error("Input cap");
    result = decoder.decodePillowBc3(data.descriptor, data.source, data.options);
  } else if (data.operation === "decodeV61") {
    if (!(data.source instanceof Uint8Array) || data.source.byteLength > 64 * 1024 * 1024 + 40)
      throw new Error("Input cap");
    result = decoder.decodeV61(data.source);
  } else if (data.operation === "encodeAndPack")
    result = await encoder.encodeAndPack(data.templateHeader, data.rgba, data.width, data.height);
  else if (data.operation === "packLinearBC3")
    result = encoder.packLinearBC3(data.templateHeader, data.linearBC3, data.width, data.height);
  else throw new Error("Unsupported texture operation");
  const transfers = [];
  for (const key of ["pixels", "sprite", "linearBC3"])
    if (result[key] instanceof Uint8Array) transfers.push(result[key].buffer);
  self.postMessage({ id: data.id, ok: true, result }, transfers);
}
self.addEventListener("message", ({ data }) => {
  if (data?.type !== "initialize" && !ready) {
    self.postMessage({ id: data?.id, ok: false, error: "Runtime not initialized" });
    return;
  }
  queue = queue.then(async () => {
    try {
      if (data?.type === "initialize")
        self.postMessage({ id: 0, ok: true, result: await initialize(data) });
      else await run(data);
    } catch (error) {
      self.postMessage({
        id: data?.id ?? 0,
        ok: false,
        error:
          data?.type === "initialize"
            ? String(error?.message ?? "Texture initialization failed").slice(0, 1200)
            : "Sealed texture operation failed",
      });
    }
  });
});
