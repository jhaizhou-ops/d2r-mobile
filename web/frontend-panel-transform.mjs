// 依据 dm/ios/tools/gen_fe_rightpanel.py 变换前端面板像素。
// Transform frontend panel pixels based on dm/ios/tools/gen_fe_rightpanel.py.
/**
 * 与源码等价的像素变换；BC3 编码仍由编码器客户端负责。
 * Source-equivalent pixel transform; BC3 encoding remains the encoder client's job.
 */
export const FRONTEND_PANEL_SOURCE = Object.freeze({
  path: "dm/ios/tools/gen_fe_rightpanel.py",
  sha256: "c68ea1e6d5c720eca576b298708cd637d8d2d898d192e34a92c41121a52db424",
  input: "data/hd/global/ui/frontend/hd/final/frontend_rightpanel.sprite",
  replacement: "data/hd/global/ui/frontend/hd/final/frontend_rightpanel.sprite",
  width: 401,
  height: 1080,
});
const copies = Object.freeze([
  Object.freeze([112, 52, 58]),
  Object.freeze([170, 112, 60]),
  Object.freeze([230, 53, 1]),
  Object.freeze([231, 52, 1]),
  Object.freeze([232, 52, 58]),
]);

/**
 * 复制始终读取原始像素，绝不读取已修改输出。整图集解码可能包含最多三列填充。原生 sprite() 在严格断言 401x1080 之前将其裁剪到帧宽。
 * Copies always read the original pixels, never the already modified output.
 * Whole-atlas decode can include up to three padding columns. Native sprite()
 * crops those to the frame width before its strict 401x1080 assertion.
 */
export function transformFrontendPanel({
  pixels,
  width,
  height,
  frameWidth = width,
  frameCount = 1,
  signal,
} = {}) {
  const check = () => {
    if (signal?.aborted) throw new Error("FRONTEND_PANEL_CANCELLED");
  };
  check();
  if (
    !Number.isSafeInteger(width) ||
    width < 401 ||
    width > 404 ||
    height !== 1080 ||
    frameWidth !== 401 ||
    frameCount !== 1 ||
    !(pixels instanceof Uint8Array) ||
    !(pixels.buffer instanceof ArrayBuffer) ||
    pixels.length !== width * height * 4
  )
    throw new Error("FRONTEND_PANEL_LAYOUT");
  const rgba = new Uint8Array(401 * 1080 * 4);
  try {
    for (let y = 0; y < 1080; y++) {
      if (!(y % 32)) check();
      rgba.set(pixels.subarray(y * width * 4, (y * width + 401) * 4), y * 401 * 4);
    }
    for (const [destination, source, columns] of copies) {
      for (let y = 980; y < 1040; y++) {
        check();
        rgba.set(
          pixels.subarray((y * width + source) * 4, (y * width + source + columns) * 4),
          (y * 401 + destination) * 4,
        );
      }
    }
    check();
    return { rgba, width: 401, height: 1080 };
  } catch (error) {
    rgba.fill(0);
    throw error;
  }
}

/**
 * 源字节必须由受信任发布配方固定，而非由用户输入固定。原生生成器中没有真实精灵摘要，因此不虚构摘要。
 * Source bytes must be pinned by the trusted release recipe, not by user input.
 * No real sprite digest is present in the native generator, so none is invented.
 */
export function createFrontendPanelRecipe({ sha256 } = {}) {
  if (typeof sha256 !== "string" || !/^[a-f0-9]{64}$/.test(sha256))
    throw new Error("FRONTEND_PANEL_SOURCE_HASH_REQUIRED");
  return Object.freeze({
    input: FRONTEND_PANEL_SOURCE.input,
    replacement: FRONTEND_PANEL_SOURCE.replacement,
    add: false,
    sha256,
    transform: transformFrontendPanel,
  });
}
