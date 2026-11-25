// 已打包 Pillow 的有界测试预言机；仅构建验证使用，不进入浏览器发布包。 | Bounded bundled-Pillow test oracle; build validation only, never browser payload.
import { copyFile, mkdtemp, readFile, rm } from "node:fs/promises";
import { createHash } from "node:crypto";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import * as numeric from "../../art-kernels/kernels.mjs";
export async function createOracle() {
  const source = new URL("../../asset-generation-spike/public/vendor/pyodide/", import.meta.url);
  const manifest = JSON.parse(await readFile(new URL("vendor-manifest.json", source), "utf8"));
  const dir = await mkdtemp(join(tmpdir(), "d2r-art-recipes-"));
  let py;
  try {
    for (const [name, expected] of Object.entries(manifest.sha256)) {
      const raw = await readFile(new URL(name, source));
      if (createHash("sha256").update(raw).digest("hex") !== expected)
        throw new Error("Oracle vendor digest mismatch");
      await copyFile(new URL(name, source), join(dir, name));
    }
    const { loadPyodide } = await import(pathToFileURL(join(dir, "pyodide.mjs")));
    py = await loadPyodide({ indexURL: dir + "/" });
    await py.loadPackage("pillow");
    py.registerJsModule("art_kernel_bridge", numeric);
    for (const name of [
      "art_pixels.py",
      "native_functions.py",
      "core.py",
      "native_art_original.py",
      "native_art.py",
      "map_goal_original.py",
      "map_goal.py",
    ])
      py.FS.writeFile(
        `/home/pyodide/${name}`,
        await readFile(
          new URL(`../../asset-generation-spike/public/generated/${name}`, import.meta.url),
        ),
      );
    py.runPython(
      `import base64, io, json\nfrom PIL import Image, ImageDraw, ImageFont, features\nimport core, native_art, map_goal\nassert Image.__version__ == '10.2.0'\nassert features.version('zlib') == '1.3.1'\nassert features.version('freetype2') == '2.6.0'\nassert not features.check('raqm')`,
    );
  } catch (error) {
    await rm(dir, { recursive: true, force: true });
    throw error;
  }
  function bytes(code) {
    const proxy = py.runPython(code);
    try {
      return new Uint8Array(proxy.toJs());
    } finally {
      proxy.destroy();
    }
  }
  return {
    py,
    close: () => rm(dir, { recursive: true, force: true }),
    generate(operation, payload, options) {
      py.globals.set("_payload", payload);
      py.globals.set("_options", JSON.stringify(options));
      py.globals.set("_operation", operation);
      const result = JSON.parse(
        py.runPython(
          `_result = core.execute(_operation, bytes(_payload), json.loads(_options))\n_result['files'] = {name: base64.b64encode(value).decode() for name, value in _result['files'].items()}\njson.dumps(_result, ensure_ascii=False)`,
        ),
      );
      result.files = Object.fromEntries(
        Object.entries(result.files).map(([name, raw]) => [
          name,
          new Uint8Array(Buffer.from(raw, "base64")),
        ]),
      );
      return result;
    },
    codec: {
      zlibVersion: "1.3.1",
      encodeRgba(raw, width, height) {
        py.globals.set("_pixels", raw);
        py.globals.set("_width", width);
        py.globals.set("_height", height);
        return bytes(
          `_image = Image.frombytes('RGBA', (_width, _height), bytes(_pixels))\n_png = io.BytesIO()\n_image.save(_png, 'PNG')\n_png.getvalue()`,
        );
      },
    },
    font: {
      open(raw, size) {
        py.globals.set("_fontbytes", raw);
        py.globals.set("_size", size);
        py.runPython("_font = ImageFont.truetype(io.BytesIO(bytes(_fontbytes)), _size)");
      },
      close() {
        py.globals.delete("_font");
      },
      measure(text) {
        py.globals.set("_text", text);
        return { length: py.runPython("_font.getlength(_text)") };
      },
      draw(raw, width, height, text, options) {
        py.globals.set("_pixels", raw);
        py.globals.set("_width", width);
        py.globals.set("_height", height);
        py.globals.set("_text", text);
        py.globals.set("_draw", JSON.stringify(options));
        return bytes(
          `_drawopts = json.loads(_draw)\n_image = Image.frombytes('RGBA', (_width, _height), bytes(_pixels))\nImageDraw.Draw(_image).text((_drawopts['x'],_drawopts['y']), _text, font=_font, fill=tuple(_drawopts['fill']), anchor=_drawopts['anchor'])\n_image.tobytes()`,
        );
      },
    },
  };
}
