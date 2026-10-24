// 使用实际打包的 WASM Pillow 编码器作为参考，不使用浏览器、网络或游戏输入。
// Actual bundled WASM Pillow encoder oracle. No browser, network, or game input.
import { readFileSync, copyFileSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { pathToFileURL } from "node:url";
import { createHash } from "node:crypto";
const source = new URL("../../asset-generation-spike/public/vendor/pyodide/", import.meta.url);
const manifest = JSON.parse(readFileSync(new URL("vendor-manifest.json", source)));
const dir = mkdtempSync(join(tmpdir(), "d2r-png-oracle-"));
try {
  // 复制到仓库 type:module 范围外，使 Emscripten 的 .js Node 加载器以 CommonJS 运行；内容不变且经过供应方哈希验证。
  // Copy outside the repository's type:module scope so Emscripten's .js Node
  // loader runs as CommonJS. Contents are unchanged and vendor-hash verified.
  for (const [name, sha256] of Object.entries(manifest.sha256)) {
    const raw = readFileSync(new URL(name, source));
    if (createHash("sha256").update(raw).digest("hex") !== sha256)
      throw new Error("Oracle vendor digest mismatch");
    copyFileSync(new URL(name, source), join(dir, name));
  }
  const { loadPyodide } = await import(pathToFileURL(join(dir, "pyodide.mjs")));
  const python = await loadPyodide({ indexURL: dir + "/" });
  await python.loadPackage("pillow");
  const fixtures = python.runPython(`
import base64, io, json, random, struct, zlib
from PIL import Image, features
assert Image.__version__ == '10.2.0'
assert features.version('zlib') == '1.3.1'
rng = random.Random(805751)
cases = []
for w,h in [(1,1),(1,17),(17,1),(3,2),(31,23),(256,256),(257,263),(8192,1)]:
    for kind in ['zero','solid','random']:
        raw = bytes(w*h*4) if kind=='zero' else bytes([127,128,255,1])*(w*h) if kind=='solid' else rng.randbytes(w*h*4)
        image = Image.frombytes('RGBA',(w,h),raw)
        out = io.BytesIO(); image.save(out,'PNG')
        cases.append(dict(width=w,height=h,kind=kind,rgba=base64.b64encode(raw).decode(),png=base64.b64encode(out.getvalue()).decode()))
# Explicit repeated/directional rows exercise filter order and strict tie-breaking.
for mode in ['repeat','x','y','hidden']:
    w,h=113,71
    row = rng.randbytes(w*4)
    raw = row*h if mode=='repeat' else bytes(v for y in range(h) for x in range(w) for v in ((x%256,y%256,(x+y)%256,0) if mode=='hidden' else (x%256,)*4 if mode=='x' else (y%256,)*4))
    image = Image.frombytes('RGBA',(w,h),raw); out=io.BytesIO(); image.save(out,'PNG')
    cases.append(dict(width=w,height=h,kind=mode,rgba=base64.b64encode(raw).decode(),png=base64.b64encode(out.getvalue()).decode()))
assert zlib.ZLIB_RUNTIME_VERSION == '1.2.13'
generic=[]
for n in [0,1,255,256,32767,32768,65535,65536,262144,1048577,16777216]:
    raw=(bytes(range(251))*(n//251+1))[:n] if n==16777216 else rng.randbytes(n)
    generic.append(dict(kind='pattern' if n==16777216 else 'stored',length=n,raw=None if n==16777216 else base64.b64encode(raw).decode(),compressed=base64.b64encode(zlib.compress(raw,9)).decode()))
json.dumps(dict(backend={'pillow':Image.__version__,'pillowZlib':features.version('zlib'),'pyodide':'0.27.7','genericZlib':zlib.ZLIB_RUNTIME_VERSION},cases=cases,generic=generic),separators=(',',':'))
`);
  writeFileSync(new URL("fixtures.json", import.meta.url), fixtures);
  const parsed = JSON.parse(fixtures);
  const meta = {
    ...parsed.backend,
    cases: parsed.cases.length,
    genericCases: parsed.generic.length,
    bytes: Buffer.byteLength(fixtures),
    sha256: createHash("sha256").update(fixtures).digest("hex"),
  };
  writeFileSync(
    new URL("fixtures-meta.json", import.meta.url),
    JSON.stringify(meta, null, 2) + "\n",
  );
  console.log(JSON.stringify(meta));
} finally {
  rmSync(dir, { recursive: true, force: true });
}
