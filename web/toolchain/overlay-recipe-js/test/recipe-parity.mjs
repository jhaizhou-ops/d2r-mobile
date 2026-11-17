/**
 * 使用自编合成数据验证完整有界配方一致性，不使用游戏文件。SHA1 样例哨兵仅用于测试，生产来源检查保持严格。
 * Complete bounded recipe parity using authored synthetic data, no game files.
 * The SHA1 fixture sentinels are test-only. Production source checks stay strict.
 */
import process from "node:process";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { createOverlayRecipe, GENERATED } from "../recipe.mjs";
import { UPSTREAM_OVERLAY_REVISION } from "../revision.mjs";
import { buildAnnotationFixture, fixtureSha1 } from "./annotations-parity.mjs";
import { inventoryFixtures } from "./inventory-parity.mjs";
import { touchFixtures } from "./touch-parity.mjs";
import { createPngCodec } from "../../png-codec-exact/codec.mjs";
const root = fileURLToPath(new URL("../../../../", import.meta.url));
const utf8 = new TextEncoder();
const b64 = (bytes) => Buffer.from(bytes).toString("base64");
const raw = (text) => new Uint8Array(Buffer.from(text, "base64"));
const digest = (algorithm, bytes) => createHash(algorithm).update(bytes).digest("hex");
const md5 = (bytes) => digest("md5", bytes);
const nativeProgram = String.raw`
import sys,json,base64,hashlib,io,importlib.util
from PIL import Image
sys.path.insert(0,sys.argv[1]+'/web/toolchain/asset-generation-spike/public/generated')
# Use the source wrapper so a previously packaged copy cannot hide a boundary
# fix; all transformed native sources still come from the pinned source bundle.
spec=importlib.util.spec_from_file_location('overlay_recipe',sys.argv[1]+'/web/toolchain/asset-generation-spike/src/overlay_recipe.py')
overlay_recipe=importlib.util.module_from_spec(spec);spec.loader.exec_module(overlay_recipe)
value=json.load(sys.stdin)
real_sha1=hashlib.sha1
def sha1(value):
    if value.startswith(b'HASH:'):
        class Hash:
            def hexdigest(self):return value.decode().split(':')[1]+'0'*32
        return Hash()
    return real_sha1(value)
hashlib.sha1=sha1
result=overlay_recipe.generate(json.dumps(value).encode(),{})
print(json.dumps({'variants':result['variants'],'files':{k:base64.b64encode(v).decode() for k,v in result['files'].items()}},ensure_ascii=False))
`;
const imageProgram = String.raw`
import sys,json,base64,io
from PIL import Image
def encode(raw,w,h):
    out=io.BytesIO();Image.frombytes('RGBA',(w,h),raw).save(out,'PNG');return base64.b64encode(out.getvalue()).decode()
pixels=bytes((i*37+11)%256 for i in range(4*3*4))
close=open(sys.argv[1]+'/dm/ios/NativeHUD/touch/close.png','rb').read()
im=Image.open(io.BytesIO(close)).convert('RGBA')
print(json.dumps({'base':{'png':encode(pixels,4,3),'width':4,'height':3,'rgba':base64.b64encode(pixels).decode()},'close':{'png':base64.b64encode(close).decode(),'width':im.width,'height':im.height,'rgba':base64.b64encode(im.tobytes()).decode()}}))
`;
function python(program, input = "") {
  const result = spawnSync("python3", ["-c", program, root], {
    input,
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
  });
  assert.equal(result.status, 0, result.stderr);
  return JSON.parse(result.stdout);
}
const images = python(imageProgram);
const annotation = buildAnnotationFixture();
const inventory = inventoryFixtures();
const layouts = touchFixtures(inventory);
// 真实控制器模态层继承对应根层；测试纯控件样例未覆盖的跨文件重定位约定。
// The real controller modal layers inherit their root counterparts. Exercise
// that cross-file relocation contract, absent from pure widget fixtures.
for (const name of ["confirmationmodalhd.json", "dismissablemodalhd.json"]) {
  const path = "controller/" + name;
  layouts.set(path, layouts.get(path).replace("{", `{\n"basedOn": "${name}",`));
}
const assets = new Map(annotation.assets);
for (const [rel, text] of layouts) assets.set("data/global/ui/layouts/" + rel, utf8.encode(text));
for (const sub of ["", "base/"])
  assets.set("data/global/excel/" + sub + "inventory.bin", inventory.inventory);
for (const table of ["items.json", "uniques.json", "sets.json"])
  assets.set(
    "data/hd/items/" + table,
    utf8.encode(JSON.stringify([{ fixture: { asset: "authored" } }])),
  );
assets.set(
  "data/hd/items/weapon/authored.json",
  utf8.encode('{"type":"UnitDefinition","dependencies":{},"entities":[],"source":1.0}'),
);
function transparentSprite() {
  const bytes = new Uint8Array(56),
    view = new DataView(bytes.buffer);
  bytes.set(utf8.encode("SpA1"));
  view.setUint16(4, 61, true);
  view.setUint16(6, 4, true);
  view.setUint32(8, 4, true);
  view.setUint32(12, 4, true);
  view.setUint32(32, 16, true);
  bytes.fill(31, 40);
  return bytes;
}
for (let i = 0; i < 3; i++)
  assets.set(`data/hd/global/ui/controller/cursor/fixture${i}.sprite`, transparentSprite());
for (let i = 0; i < 22; i++)
  for (const platform of ["switch", "xbox"])
    assets.set(
      `data/hd/global/ui/controller/controllerbuttons/${platform}/fixture${i}.sprite`,
      transparentSprite(),
    );
const manifest = [...assets]
  .map(
    ([path, bytes]) =>
      ["", "", "", "", md5(bytes), bytes.length, "", "", "", "data:" + path, ""].join("|") + "\n",
  )
  .join("");
const generated = Object.fromEntries(
  GENERATED.map((path) => [path, b64(Uint8Array.of(1, 2, 3, 4))]),
);
generated["NativeHUD/v4/base.png"] = images.base.png;
generated["NativeHUD/v4/hud.json"] = b64(utf8.encode('{"fixture":"generated HUD"}\n'));
generated["NativeHUD/v4/hud_v4_ui/ui.json"] = b64(
  utf8.encode(
    JSON.stringify({
      ref: [1920, 2160],
      slots: [
        [100, 10, 150, 60],
        [170, 10, 221, 60],
      ],
    }),
  ),
);
const bundle = {
  manifest,
  assets: Object.fromEntries([...assets].map(([path, bytes]) => [path, b64(bytes)])),
  generated,
};
const expected = python(nativeProgram, JSON.stringify(bundle));
// 默认隔离布局/字符串/二进制编排；--wasm 还验证生产 PNG 解码器，原生 PNG 样例始终具有已知像素。
// Default isolates all layout/string/binary orchestration. --wasm also verifies
// the production PNG decoder; the native PNG fixture always has known pixels.
const png = process.argv.includes("--wasm")
  ? await createPngCodec(
      readFileSync(new URL("../../png-codec-exact/dist/png-codec.wasm", import.meta.url)),
    )
  : {
      decodeRgba(bytes) {
        const found = Object.values(images).find((image) => image.png === b64(bytes));
        assert.ok(found, "Only the two authored/pinned test PNGs may be decoded");
        return { width: found.width, height: found.height, channels: 4, data: raw(found.rgba) };
      },
    };
const recipe = createOverlayRecipe({ png, md5, sha1: fixtureSha1 });
const payload = utf8.encode(JSON.stringify(bundle));
const actual = await recipe.execute("overlay-recipe", payload, {
  revision: UPSTREAM_OVERLAY_REVISION,
});
assert.deepEqual(actual.variants, expected.variants, "Full variant receipts and ordering");
assert.deepEqual(
  Object.keys(actual.files),
  Object.keys(expected.files),
  "Full file IDs and ordering",
);
const hashes = {};
for (const [name, encoded] of Object.entries(expected.files)) {
  const reference = raw(encoded),
    output = actual.files[name];
  assert.equal(output.length, reference.length, name + " byte length");
  assert.equal(digest("sha256", output), digest("sha256", reference), name + " exact bytes");
  hashes[name] = { bytes: output.length, sha256: digest("sha256", output) };
}
// 新摘要只能在上面的独立原生比较通过后审核固定，不得复用旧版基线。
// Approve a new pin only after independent native equality above; do not reuse a legacy baseline.
const pinned = {
  revision: "e3756f8942b40eba6c851e881828a5b1bda17fd6",
  status: "requires-native-oracle-review",
  files: null,
  sha256: null,
};
assert.equal(pinned.revision, UPSTREAM_OVERLAY_REVISION);
assert.equal(
  pinned.status,
  "verified",
  "Latest-source native recipe aggregate is not yet reviewed; do not reuse the legacy baseline",
);
assert.equal(Object.keys(hashes).length, pinned.files, "Pinned full recipe file count");
assert.equal(
  digest("sha256", JSON.stringify({ variants: actual.variants, hashes })),
  pinned.sha256,
  "Pinned exact full recipe output and receipt digest",
);
await assert.rejects(recipe.execute("overlay-recipe", payload, { unchecked: true }), /OPTIONS/);
const tampered = { ...bundle, manifest: bundle.manifest.replace(/[a-f0-9]{32}/, "0".repeat(32)) };
await assert.rejects(
  recipe.execute("overlay-recipe", utf8.encode(JSON.stringify(tampered)), {
    revision: UPSTREAM_OVERLAY_REVISION,
  }),
  /CKEY_MISMATCH/,
);
const dir = new URL("../test-results/", import.meta.url);
mkdirSync(dir, { recursive: true });
writeFileSync(
  new URL("recipe-parity.json", dir),
  JSON.stringify(
    {
      fixture: "authored synthetic full recipe, not title evidence",
      png: process.argv.includes("--wasm") ? "exact WASM" : "known-pixel fixture",
      variants: actual.variants,
      hashes,
    },
    null,
    2,
  ) + "\n",
);
console.log(
  `Full overlay recipe parity: ${Object.keys(hashes).length} exact files, two exact receipts, source/option guards.`,
);
