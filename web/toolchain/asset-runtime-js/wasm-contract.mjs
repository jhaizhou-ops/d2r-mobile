// 与三个固定包装器一致的必需导出；在首次私有输入前拒绝错误产物。
// Required exports matching the three pinned wrappers; reject wrong artifacts before private input.
const EXPORTS = Object.freeze({
  "image-kernels.wasm": [
    "image_input",
    "image_destination",
    "image_mask",
    "image_output",
    "image_bbox_result",
    "image_crop",
    "image_flip",
    "image_paste",
    "image_alpha_composite",
    "image_convert",
    "image_channel",
    "image_put_alpha",
    "image_bbox",
    "image_max_filter",
    "image_resize_lanczos",
  ],
  "png-codec.wasm": [
    "png_input",
    "png_output",
    "png_width",
    "png_height",
    "png_error",
    "png_zlib_version",
    "png_encode",
    "png_decode",
    "zlib_begin",
    "zlib_push",
    "zlib_abort",
    "generic_zlib_version",
  ],
  "font-rasterizer.wasm": [
    "font_input",
    "font_text",
    "font_mask",
    "font_canvas",
    "font_info",
    "font_error",
    "font_ft_error",
    "font_version",
    "font_open",
    "font_close",
    "font_measure",
    "font_render",
    "font_draw",
  ],
});
export function verifyWasmModule(module, name) {
  if (!Object.hasOwn(EXPORTS, name) || WebAssembly.Module.imports(module).length)
    throw new Error(`Unexpected asset WASM imports/name: ${name}`);
  const exports = new Map(
    WebAssembly.Module.exports(module).map((entry) => [entry.name, entry.kind]),
  );
  if (
    exports.get("memory") !== "memory" ||
    EXPORTS[name].some((symbol) => exports.get(symbol) !== "function")
  )
    throw new Error(`Incomplete asset WASM ABI: ${name}`);
}
