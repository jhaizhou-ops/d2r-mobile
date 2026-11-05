// 依据 texture-spike 与 Pyodide AssetWorker 契约桥接 v61 图像处理。
// Bridge v61 image processing using the texture-spike and Pyodide AssetWorker contracts.
/**
 * 组合分别经 CI 证明的纹理工作线程及 Pyodide 资源工作线程。v61 解码为内部 RGBA 中间表示；v61 仅发布 PNG。这里不实现原生 BC3 编码、重排或 v61 精灵替换。
 * Composition of the separately CI-proven texture worker and Pyodide asset worker.
 * v61 is decoded to an internal RGBA intermediate; only PNG is released for v61.
 * This does NOT implement native BC3 encoding/swizzle or v61 sprite replacement.
 */
const INPUT_LIMIT = 20 * 1024 * 1024;
const PIXEL_LIMIT = 4_194_304;
function failure() {
  return new Error("V61_ASSET_BRIDGE_FAILED");
}
function checkedHeader(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length < 40 || bytes.length > INPUT_LIMIT)
    throw failure();
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const width = view.getUint32(8, true),
    height = view.getUint32(12, true);
  const frameWidth = view.getUint16(6, true);
  if (
    view.getUint32(0, true) !== 0x31417053 ||
    view.getUint16(4, true) !== 61 ||
    !width ||
    !height ||
    width * height > PIXEL_LIMIT ||
    !frameWidth ||
    width % frameWidth
  )
    throw failure();
  return { width, height, frameWidth };
}
/**
 * 实际 texture-spike 专用 Worker 的轻量有界客户端。
 * Thin bounded client around the actual texture-spike dedicated Worker.
 */
export class TextureWorkerClient {
  #worker;
  #pending = new Map();
  #next = 1;
  constructor(
    worker = new Worker(new URL("./toolchain/texture-spike/texture-worker.mjs", import.meta.url), {
      type: "module",
    }),
  ) {
    this.#worker = worker;
    worker.onmessage = ({ data }) => {
      const entry = this.#pending.get(data?.id);
      if (!entry) return;
      this.#pending.delete(data.id);
      data.ok ? entry.resolve(data.result) : entry.reject(failure());
    };
    worker.onerror = () => this.close();
    worker.onmessageerror = () => this.close();
  }
  decodeV61(bytes) {
    checkedHeader(bytes); // 在分配原生解码器内存之前拒绝超大几何尺寸。 | Reject oversized geometry BEFORE allocating native decoder memory.
    if (!this.#worker) return Promise.reject(failure());
    const source = bytes.slice();
    const id = this.#next++;
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      try {
        this.#worker.postMessage({ id, operation: "decodeV61", source }, [source.buffer]);
      } catch {
        this.#pending.delete(id);
        reject(failure());
      }
    });
  }
  close() {
    this.#worker?.terminate();
    this.#worker = undefined;
    for (const pending of this.#pending.values()) pending.reject(failure());
    this.#pending.clear();
  }
}
/**
 * 已验证的内部格式桥接，并不声称是 v61 替换资源。
 * Validated internal format bridge, not a claimed v61 replacement asset.
 */
export function decodedV61ToInternalRgba(bytes, decoded) {
  const header = checkedHeader(bytes);
  if (
    !decoded ||
    decoded.width !== header.width ||
    decoded.height !== header.height ||
    decoded.frameWidth !== header.frameWidth ||
    decoded.pixelFormat !== 70 ||
    decoded.bytesPerPixel !== 4 ||
    !(decoded.pixels instanceof Uint8Array) ||
    decoded.pixels.byteLength !== header.width * header.height * 4
  )
    throw failure();
  const out = new Uint8Array(40 + decoded.pixels.byteLength);
  const view = new DataView(out.buffer);
  out.set([83, 112, 65, 49]);
  view.setUint16(4, 31, true);
  view.setUint16(6, header.frameWidth, true);
  view.setUint32(8, header.width, true);
  view.setUint32(12, header.height, true);
  view.setUint32(20, header.width / header.frameWidth, true);
  view.setUint32(32, decoded.pixels.byteLength, true);
  view.setUint32(36, 4, true);
  out.set(decoded.pixels, 40);
  return out;
}
/**
 * 调用方提供现有 AssetWorker；纹理工作线程默认使用真实 WASM 桥接。
 * Caller supplies existing AssetWorker; texture worker defaults to the real WASM bridge.
 */
export function createV61AssetWorker({ assetWorker, textureWorker }) {
  if (
    !assetWorker ||
    typeof assetWorker.run !== "function" ||
    typeof assetWorker.close !== "function"
  )
    throw failure();
  textureWorker ??= new TextureWorkerClient();
  if (typeof textureWorker.decodeV61 !== "function" || typeof textureWorker.close !== "function")
    throw failure();
  let closed = false;
  return Object.freeze({
    capabilities: Object.freeze({ v61Png: true, v61SpriteEncoding: false }),
    async run(operation, bytes, options = {}) {
      if (closed || !(bytes instanceof Uint8Array) || bytes.byteLength > INPUT_LIMIT)
        throw failure();
      if (bytes.length < 6 || bytes[4] !== 61 || bytes[5] !== 0)
        return assetWorker.run(operation, bytes, options);
      if (operation !== "image-default-frame") throw failure();
      checkedHeader(bytes);
      const decoded = await textureWorker.decodeV61(bytes);
      let intermediate, result;
      let accepted = false;
      try {
        if (closed) throw failure();
        intermediate = decodedV61ToInternalRgba(bytes, decoded);
        result = await assetWorker.run(operation, intermediate, options);
        if (closed || !(result?.files?.["frame.png"] instanceof ArrayBuffer)) throw failure();
        // 绝不将中间 v31 导出为 v61 输入的原生替换品。
        // Never export the intermediate v31 as a native replacement for a v61 input.
        const output = {
          ...result,
          sourceVersion: 61,
          files: { "frame.png": result.files["frame.png"] },
        };
        accepted = true;
        return output;
      } finally {
        // 工作线程转移了输出所有权；拒绝时擦除结果，但不触碰调用方输入的别名。
        // Worker output ownership is transferred; wipe rejected results, excluding caller input aliases.
        if (!accepted) {
          for (const value of Object.values(result?.files ?? {})) {
            const output =
              value instanceof ArrayBuffer
                ? new Uint8Array(value)
                : value instanceof Uint8Array
                  ? value
                  : undefined;
            if (output && output.buffer !== bytes.buffer) output.fill(0);
          }
        }
        intermediate?.fill(0);
        if (decoded?.pixels instanceof Uint8Array) decoded.pixels.fill(0);
      }
    },
    close() {
      if (closed) return;
      closed = true;
      try {
        textureWorker.close();
      } finally {
        assetWorker.close();
      }
    },
  });
}
