// 依据 gen_widestash_art.py 的图集几何校验描述完整 v61 纹理。
// Describe complete v61 textures using gen_widestash_art.py atlas-geometry checks.
/**
 * 完整 v61 图集解码。绝不裁剪或改变存储及帧尺寸。
 * Whole v61 atlas decoding. Never crops or changes storage/frame dimensions.
 */
export function nativeArtAtlasDescriptor(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length < 40 || bytes.length > 8 * 1024 * 1024)
    throw new Error("NATIVE_ART_ATLAS_BYTES");
  const h = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (h.getUint32(0, true) !== 0x31417053 || h.getUint16(4, true) !== 61)
    throw new Error("NATIVE_ART_ATLAS_VERSION");
  const width = h.getUint32(8, true),
    height = h.getUint32(12, true),
    frameWidth = h.getUint16(6, true),
    frameCount = h.getUint16(20, true);
  // 部分源图集紧邻排列帧，不添加 BC3 块填充。
  // 完整纹理宽度仍按块对齐，并作为一张图像解码。
  // Some source atlases pack adjacent frames without BC3 block padding.
  // Their full texture width is still block aligned and decodes as one image.
  const framePitch =
    width % 4 === 0 && width === frameWidth * frameCount
      ? frameWidth
      : Math.ceil(frameWidth / 4) * 4;
  // 原始 gen_widestash_art.py 的 main 显式验证此双面板
  // 存储几何。解码整个图集，绝不裁掉面板间图像。
  // Original gen_widestash_art.py main explicitly validates this two-panel
  // storage geometry. Decode the complete atlas; never trim its inter-panel art.
  const originalMainPanel =
    frameWidth === 1440 && width === 2888 && height === 881 && frameCount === 2;
  const generatedWidePanel =
    frameWidth === 1636 && width === 1640 && height === 881 && frameCount === 1;
  if (
    !width ||
    !height ||
    width * height > 4194304 ||
    !frameWidth ||
    !frameCount ||
    width < (frameCount - 1) * framePitch + frameWidth ||
    (!originalMainPanel && !generatedWidePanel && width > frameCount * framePitch)
  )
    throw new Error("NATIVE_ART_ATLAS_GEOMETRY");
  return {
    descriptor: {
      target: 1,
      format: 0x44,
      flags: 0,
      samples: 0,
      width,
      height,
      depth: 1,
      levels: 1,
      stride: 0,
      pkg_block_h: bytes[23] + 1,
    },
    frameWidth,
    frameCount,
    framePitch,
  };
}
export class NativeArtTextureClient {
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
      const p = this.#pending.get(data?.id);
      if (!p) return;
      this.#pending.delete(data.id);
      p.cleanup();
      data.ok
        ? p.resolve({ ...data.result, ...p.geometry })
        : p.reject(new Error("NATIVE_ART_ATLAS_DECODE"));
    };
    worker.onerror = () => this.close();
    worker.onmessageerror = () => this.close();
  }
  decodeAtlas(bytes, { signal } = {}) {
    const { descriptor, ...geometry } = nativeArtAtlasDescriptor(bytes);
    if (!this.#worker || signal?.aborted)
      return Promise.reject(new Error("NATIVE_ART_ATLAS_CANCELLED"));
    const id = this.#next++,
      source = bytes.slice(40);
    return new Promise((resolve, reject) => {
      const abort = () => this.close();
      const cleanup = () => signal?.removeEventListener("abort", abort);
      signal?.addEventListener("abort", abort, { once: true });
      this.#pending.set(id, { resolve, reject, cleanup, geometry });
      try {
        this.#worker.postMessage({ id, operation: "decodePillowBc3", descriptor, source }, [
          source.buffer,
        ]);
      } catch {
        this.#pending.delete(id);
        cleanup();
        reject(new Error("NATIVE_ART_ATLAS_DECODE"));
      }
    });
  }
  close() {
    this.#worker?.terminate();
    this.#worker = undefined;
    for (const p of this.#pending.values()) {
      p.cleanup();
      p.reject(new Error("NATIVE_ART_ATLAS_CANCELLED"));
    }
    this.#pending.clear();
  }
}
