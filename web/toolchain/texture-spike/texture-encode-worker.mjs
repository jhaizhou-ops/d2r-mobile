// 在工作线程中验证并执行纹理编码请求；依据 ./dist/texture.mjs、./browser-texture-encoder.mjs 的模块与调用约定。
// Validate and execute texture-encoding requests inside a worker using the module and calling contracts in ./dist/texture.mjs、./browser-texture-encoder.mjs.
import createModule from "./dist/texture.mjs";
import { createTextureEncoder, validateEncodeRequest } from "./browser-texture-encoder.mjs";
let runtime;
async function python() {
  const { loadPyodide } =
    await import("../asset-generation-spike/public/vendor/pyodide/pyodide.mjs");
  const py = await loadPyodide({
    indexURL: new URL("../asset-generation-spike/public/vendor/pyodide/", import.meta.url).href,
  });
  await py.loadPackage(["pillow", "numpy"], { checkIntegrity: true });
  for (const [name, relative] of [
    ["original_bc3.py", "./dist/original_bc3.py"],
    ["bounded_encoder.py", "./bounded_encoder.py"],
  ]) {
    const response = await fetch(new URL(relative, import.meta.url));
    if (!response.ok) throw new Error(`Missing encoder source: ${name}`);
    py.FS.writeFile(`/home/pyodide/${name}`, await response.text());
  }
  py.runPython("from bounded_encoder import encode_rgba");
  return py;
}
async function encodeRGBA(bytes, width, height) {
  const py = await (runtime ??= python());
  py.globals.set("_rgba", bytes);
  py.globals.set("_width", width);
  py.globals.set("_height", height);
  let result;
  try {
    result = py.runPython("encode_rgba(bytes(_rgba), _width, _height)");
    return new Uint8Array(result.toJs());
  } finally {
    result?.destroy();
    for (const name of ["_rgba", "_width", "_height"]) py.globals.delete(name);
  }
}
const encoder = createTextureEncoder(createModule, encodeRGBA);
let queue = Promise.resolve();
self.onmessage = ({ data }) => {
  queue = queue.then(async () => {
    try {
      if (!Number.isSafeInteger(data?.id)) throw new Error("Invalid request id");
      validateEncodeRequest(data.templateHeader, data.width, data.height);
      const api = await encoder;
      let result;
      if (data.operation === "encodeAndPack")
        result = await api.encodeAndPack(data.templateHeader, data.rgba, data.width, data.height);
      else if (data.operation === "packLinearBC3")
        result = api.packLinearBC3(data.templateHeader, data.linearBC3, data.width, data.height);
      else throw new Error("Unsupported encoding operation");
      const transfers = [result.sprite.buffer];
      if (result.linearBC3) transfers.push(result.linearBC3.buffer);
      self.postMessage({ id: data.id, ok: true, result }, transfers);
    } catch (error) {
      self.postMessage({
        id: data?.id,
        ok: false,
        error: { name: error.name, code: error.code ?? -1, message: error.message },
      });
    }
  });
};
