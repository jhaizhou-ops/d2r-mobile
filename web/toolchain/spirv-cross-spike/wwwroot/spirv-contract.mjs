// 纯编译器契约；无需导入生成的 WASM 模块，可安全用于应用包。
// Pure compiler contract; safe in the application bundle without generated WASM imports.
export const MSL_ARGUMENTS = Object.freeze([
  "--msl",
  "--msl-version",
  "20300",
  "--msl-argument-buffers",
  "--msl-texture-buffer-native",
  "--msl-pad-fragment-output",
]);
export const CROSS_COMMIT = "f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7";
export function validateSpirv(input) {
  if (!(input instanceof Uint8Array)) throw new TypeError("SPIR-V must be Uint8Array");
  if (input.byteLength < 20 || input.byteLength % 4 || input.byteLength > 64 * 1024 * 1024)
    throw new RangeError("SPIR-V must contain 5–16777216 whole words");
  const data = new DataView(input.buffer, input.byteOffset, input.byteLength);
  if (data.getUint32(0, true) !== 0x07230203) throw new Error("Invalid little-endian SPIR-V magic");
  if (!data.getUint32(12, true) || data.getUint32(16, true))
    throw new Error("Invalid SPIR-V header");
  for (let offset = 20; offset < input.byteLength;) {
    const words = data.getUint32(offset, true) >>> 16;
    if (!words || offset + words * 4 > input.byteLength)
      throw new Error("Invalid SPIR-V instruction size");
    offset += words * 4;
  }
  // Buffer 等 Uint8Array 子类的 slice() 可能返回借用视图。
  // Buffer and other Uint8Array subclasses may implement slice() as a borrowed view.
  return new Uint8Array(input);
}
