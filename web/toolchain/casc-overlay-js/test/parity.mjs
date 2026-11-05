// 实际已打包 CPython/zlib 的合成逐字节对照，不启动浏览器。 | Synthetic exact-byte oracle against packaged CPython/zlib, without a browser.
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import process from "node:process";
import { createPngCodec } from "../../png-codec-exact/codec.mjs";
import { loadPyodide } from "../../asset-generation-spike/node_modules/pyodide/pyodide.mjs";
import { createMD5 } from "hash-wasm";
import { createCascOverlayTransform } from "../overlay.mjs";
import { verifyCascOverlaySources } from "../../verify-asset-sources.mjs";
await verifyCascOverlaySources();
const py = await loadPyodide({
  indexURL: new URL("../../asset-generation-spike/node_modules/pyodide/", import.meta.url).pathname,
});
assert.equal(py.runPython('__import__("zlib").ZLIB_RUNTIME_VERSION'), "1.2.13");
for (const name of ["casc_overlay_original.py", "casc_overlay.py"])
  py.FS.writeFile(
    `/home/pyodide/${name}`,
    await readFile(
      new URL(`../../asset-generation-spike/public/generated/${name}`, import.meta.url),
    ),
  );
py.runPython("from casc_overlay import generate\nimport zlib, json");
const md5Hasher = await createMD5();
const md5 = (bytes) => md5Hasher.init().update(bytes).digest("binary");
const oracleCompress = (bytes) => {
  py.globals.set("_compress", bytes);
  const proxy = py.runPython("zlib.compress(bytes(_compress), 9)");
  try {
    return new Uint8Array(proxy.toJs());
  } finally {
    proxy.destroy();
    py.globals.delete("_compress");
  }
};
const useWasm = process.argv.includes("--wasm");
const codec = useWasm
  ? await createPngCodec(
      await readFile(new URL("../../png-codec-exact/dist/png-codec.wasm", import.meta.url)),
    )
  : null;
if (codec) assert.equal(codec.genericZlibVersion, "1.2.13");
const generate = createCascOverlayTransform({
  md5,
  compress: codec ? (bytes) => codec.compressZlib(bytes, { level: 9 }) : oracleCompress,
});
const nativeFixture = spawnSync("python3", [new URL("fixture.py", import.meta.url).pathname], {
  encoding: "utf8",
  maxBuffer: 8 * 1024 * 1024,
});
assert.equal(nativeFixture.status, 0, nativeFixture.stderr);
const base = JSON.parse(nativeFixture.stdout);
function slices(request) {
  const bytes = new Uint8Array(Buffer.from(request.payload, "base64"));
  let at = 0;
  return request.options.parts.map((length) => {
    const slice = bytes.slice(at, at + length);
    at += length;
    return slice;
  });
}
const cases = [];
function add(parts, jobs, location = [0, 2], rejects = false) {
  cases.push({
    payload: Buffer.concat(parts).toString("base64"),
    options: { parts: parts.map((part) => part.length), jobs, location },
    rejects,
  });
}
add(slices(base), base.options.jobs);
const plain = slices(base);
plain[5] = new Uint8Array();
add(plain, base.options.jobs);
const interleaved = slices(base);
add(
  [...interleaved.slice(0, 3), interleaved[6], ...interleaved.slice(3, 6)],
  [base.options.jobs[3], ...base.options.jobs.slice(0, 3)],
);
const additional = slices(base);
add(
  [
    ...additional,
    new TextEncoder().encode("shared name prefix"),
    new TextEncoder().encode("second new folder"),
  ],
  [
    ...base.options.jobs,
    { path: "data/x/newest.bin", add: true },
    { path: "data/y/other.bin", add: true },
  ],
);
const crlf = slices(base);
crlf[0] = new TextEncoder().encode(new TextDecoder().decode(crlf[0]).replaceAll("\n", "\r\n"));
add(crlf, base.options.jobs, [0, 2], true);
const extraFolders = slices(base);
add(
  [...extraFolders, new TextEncoder().encode("long folder")],
  [...base.options.jobs, { path: "data/wwww/other.bin", add: true }],
);
const missingAlias = slices(base);
missingAlias[0] = new TextEncoder().encode(
  new TextDecoder().decode(missingAlias[0]).replace(/^vfs-2-size = .*\n/m, ""),
);
add(missingAlias, base.options.jobs, [0, 2], true);
for (const change of [
  (request) => {
    request.options.parts[0]--;
  },
  (request) => {
    request.options.location = [0, 256];
  },
  (request) => {
    request.options.jobs[0].path = "data/../bad";
  },
  (request) => {
    request.options.jobs[1].oldEkey = "0".repeat(32);
  },
  (request) => {
    request.options.jobs[3].path = "data/n/new.bin";
  },
  (request) => {
    request.options.jobs[3].path = "data/x/nested/new.bin";
  },
]) {
  const request = structuredClone(base);
  change(request);
  request.rejects = true;
  cases.push(request);
}
function encodedFiles(result) {
  return {
    ...result,
    files: Object.fromEntries(
      Object.entries(result.files).map(([name, bytes]) => [
        name,
        Buffer.from(bytes).toString("base64"),
      ]),
    ),
  };
}
for (let i = 0; i < cases.length; i++) {
  const request = cases[i],
    bytes = new Uint8Array(Buffer.from(request.payload, "base64"));
  let expected, actual;
  py.globals.set("_payload", bytes);
  py.globals.set("_options", JSON.stringify(request.options));
  let proxy;
  try {
    proxy = py.runPython("generate(bytes(_payload), json.loads(_options))");
    expected = {
      ok: true,
      result: encodedFiles(proxy.toJs({ dict_converter: Object.fromEntries })),
    };
  } catch (error) {
    expected = { ok: false, error: error.message };
  } finally {
    proxy?.destroy();
    py.globals.delete("_payload");
    py.globals.delete("_options");
  }
  try {
    actual = { ok: true, result: encodedFiles(await generate(bytes, request.options)) };
  } catch (error) {
    actual = { ok: false, error: error.message };
  }
  assert.equal(expected.ok, !request.rejects, `oracle ${i}: ${expected.error}`);
  assert.equal(actual.ok, expected.ok, `case ${i}: ${JSON.stringify({ actual, expected })}`);
  if (expected.ok) assert.deepEqual(actual.result, expected.result, `case ${i}`);
}
console.log(
  `${cases.length} CASC cases match packaged CPython/zlib1.2.13 bytes and inventory (${codec ? "independent pinned WASM compressor" : "shared oracle compressor isolates serializer parity"}).`,
);
