// 针对原始函数的字节级合成对照，不启动浏览器。 | Focused synthetic byte parity against original functions, no browser startup.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { executeAssetTransform, VAULT_PATHS } from "../transforms.mjs";
import { verifyAssetTransformSources } from "../../verify-asset-sources.mjs";
await verifyAssetTransformSources();
const encode = (value) =>
  new TextEncoder().encode(typeof value === "string" ? value : JSON.stringify(value));
const hash = (value) => createHash("sha256").update(value).digest("hex");
const requests = [];
function add(operation, value, options = {}, rejects = false) {
  requests.push({
    operation,
    payload: Buffer.from(encode(value)).toString("base64"),
    options,
    rejects,
  });
}
const exp =
  "Level\tAmazon\r\nignored\tnot-a-number\r\n" +
  Array.from({ length: 102 }, (_, i) => `${i}\t${i * i * 123}\r\n`).join("");
add("experience-table", exp);
add(
  "experience-table",
  exp.replace("Level\tAmazon", 'Level\t"Amazon"').replace("1\t123", '1\t"+001_23"'),
);
add("experience-table", exp.replace("100\t1230000", "100\tbroken"), {}, true);
add("experience-table", "Level\tAmazon\n1\t0", {}, true);
add("experience-table", exp.replace("1\t123", "１\t１２３"));
add(
  "experience-table",
  "Amazon\tLevel\n" + Array.from({ length: 100 }, (_, i) => `${i}\t${i}\n`).join("") + "123",
  {},
  true,
);
add("experience-table", exp.replaceAll("\r\n", "\r"), {}, true);
// Python 整数空白与 CSV 码点/换行边界。 | Python int whitespace and csv code-point/newline boundaries.
for (const separator of ["\u001c", "\u001d", "\u001e", "\u001f"])
  add("experience-table", exp.replace("1\t123", `1\t${separator}123`), {}, true);
add("experience-table", exp.replaceAll("\r\n", "\r\r\n"));
for (const length of [65537, 131072, 131073])
  add(
    "experience-table",
    exp
      .replace("Level\tAmazon", "Level\tAmazon\tUnused")
      .replace("1\t123", `1\t123\t${"😀".repeat(length)}`),
    {},
    length > 131072,
  );

add("overlay-paths", [
  "DATA/GLOBAL/UI/LAYOUTS/CONTROLLER/HUD.JSON",
  "data/global/ui/layouts/controller/console/foo.json",
  "data/global/ui/layouts/controller/console/switch/foo.json",
  "data/global/ui/layouts/controller/_profilehd.json",
  "data/global/ui/layouts/controller/console/pc/foo.json",
  "data/hd/global/ui/controller/controllerbuttons/switch/a.sprite",
  "data/hd/global/ui/controller/controllerbuttons/switch/sub/a.sprite",
  "data/hd/global/ui/controller/cursor/foo.sprite",
  "data/global/ui/layouts/controller/测试😀.json",
  "data/other/file",
]);
add("overlay-paths", ["data/../bad"], {}, true);
add("overlay-paths", ["data/" + "😀".repeat(1019)]);
add("overlay-paths", ["data/" + "😀".repeat(1020)], {}, true);
const sources = {
  "dm_hud_exp.h":
    "static const uint32_t k_exp[100] = {" +
    Array.from({ length: 100 }, (_, i) => i * i * 123).join(",") +
    "};\n",
  "dm_vault_tab.h":
    '#define VT_NPART 2\nstatic const char *const vt_part_name[VT_NPART] = { "Synthetic Head", "测试栏" };\nstatic const vt_row vt_rows[] = {\n    { 1, 0, 2, 5, 17, -1, "Authored \\"Unique\\"", "Items/armor/authored" },\n    { 0, 1, 0, 6, 3, 0, "测试\\\\x", "Items/misc/authored" },\n};\nstatic const char *const vt_set_key[] = { "Authored Set" };\n',
  "dm_touch_native_art.c":
    "static const struct { uint32_t id; int atlas,frame; } rows[]={\n {12,1,2},\n {21,-1,-1},\n};\n",
};
const optionsFor = (source, generated = false) => ({
  ...(generated
    ? { mode: "generated" }
    : { expectedCounts: { EXP: 100, VTPART: 2, VTROW: 2, VTSET: 1, TAROW: 2 } }),
  expectedSourceSha256: Object.fromEntries(
    Object.entries(source).map(([name, text]) => [name, hash(text)]),
  ),
});
add("derived-tables", sources, optionsFor(sources));
add("derived-tables", sources, optionsFor(sources, true));
for (const [file, before, after] of [
  ["dm_touch_native_art.c", "{21,-1,-1}", "{12,-1,-1}"],
  ["dm_touch_native_art.c", "{21,-1,-1}", "{4294967296,-1,-1}"],
  ["dm_vault_tab.h", "Items/misc/authored", "Items\\tbad"],
  ["dm_vault_tab.h", "Items/misc/authored", "x".repeat(8190)],
  ["dm_hud_exp.h", "{0,", "{4294967296,"],
  ["dm_vault_tab.h", "1, 0, 2, 5, 17", "1, 0, 2, 65536, 17"],
]) {
  const altered = { ...sources, [file]: sources[file].replace(before, after) };
  add("derived-tables", altered, optionsFor(altered), true);
}
add(
  "derived-tables",
  sources,
  { ...optionsFor(sources), expectedCounts: { EXP: 100, VTPART: 2, VTROW: 3, VTSET: 1, TAROW: 2 } },
  true,
);
const tables = {
  ItemTypes: "Code\tEquiv1\tEquiv2\nsynthetic\tchild\tring\nchild\tparent\t\nparent\thelm\tchild\n",
  armor:
    "code\ttype\tlevel\tubercode\tultracode\nsynt\tsynthetic\t3\texc\teli\nexc\thelm\t20\texc\teli\neli\thelm\t50\texc\teli\n",
  weapons: "code\ttype\tlevel\nwand\twand\t10\n",
  misc: "code\ttype\tlevel\nring\tring\t1\n",
  UniqueItems:
    '*ID\tindex\tcode\tlvl\tspawnable\tdisabled\n3\tSynthetic One\tsynt\t7\t1\t\n4\tSynthetic Two\texc\t8\t1\t\n5\tSynthetic Three\teli\t9\t1\t\n6\tOther\'s \\"Unique\\"\twand\t10\t1\t\n8\tDisabled\tsynt\t1\t0\t\n',
  Sets: "index\nUnused\nSynthetic Set\nOther Set\n",
  SetItems:
    "*ID\tindex\titem\tlvl\tspawnable\tdisabled\tset\n8\tSynthetic Piece\tring\t1\t1\t\tSynthetic Set\n7\tOther Piece\tsynt\t5\t1\t\tOther Set\n",
};
const itemJson = {
  uniques: [{ synthetic_one: { normal: "Special" } }],
  sets: [],
  items: [
    {
      synt: { asset: "Base" },
      exc: { asset: "Exceptional" },
      eli: { asset: "Elite" },
      wand: { asset: "Wand" },
      ring: { asset: "Ring" },
    },
  ],
};
const assets = Object.fromEntries(
  VAULT_PATHS.map((path) => {
    const name = path
      .split("/")
      .at(-1)
      .replace(/\.(txt|json)$/, "");
    return [
      path,
      Buffer.from(path.endsWith(".txt") ? tables[name] : JSON.stringify(itemJson[name])).toString(
        "base64",
      ),
    ];
  }),
);
const manifest =
  ["armor/special", "armor/base", "armor/exceptional", "armor/elite", "weapon/wand", "misc/ring"]
    .map((path) =>
      Array.from({ length: 11 }, (_, i) =>
        i === 9 ? `data:data/hd/global/ui/items/${path}.sprite` : "",
      ).join("|"),
    )
    .join("\n") + "\n";
add("vault-table", { assets, manifest });
add("vault-table", { assets, manifest: "" }, {}, true);
const generatedExperience = await executeAssetTransform("experience-table", encode(exp));
const generatedVault = await executeAssetTransform("vault-table", encode({ assets, manifest }));
const generatedSources = {
  "dm_hud_exp.h": new TextDecoder().decode(generatedExperience.files["dm_hud_exp.h"]),
  "dm_vault_tab.h": new TextDecoder().decode(generatedVault.files["dm_vault_tab.h"]),
  "dm_touch_native_art.c": sources["dm_touch_native_art.c"],
};
add("derived-tables", generatedSources, optionsFor(generatedSources, true));
const escapedSources = {
  ...sources,
  "dm_vault_tab.h": sources["dm_vault_tab.h"].replace(
    "Synthetic Head",
    String.raw`\x53ynthetic \110ead`,
  ),
};
add("derived-tables", escapedSources, optionsFor(escapedSources));
add(
  "derived-tables",
  sources,
  {
    ...optionsFor(sources),
    expectedSourceSha256: {
      ...optionsFor(sources).expectedSourceSha256,
      "dm_hud_exp.h": "0".repeat(64),
    },
  },
  true,
);
const oracle = spawnSync("python3", [new URL("reference.py", import.meta.url).pathname], {
  input: requests.map((r) => JSON.stringify(r)).join("\n") + "\n",
  encoding: "utf8",
  maxBuffer: 16 * 1024 * 1024,
});
assert.equal(oracle.status, 0, oracle.stderr);
const expected = oracle.stdout
  .trim()
  .split("\n")
  .map((line) => JSON.parse(line));
assert.equal(expected.length, requests.length);
for (let i = 0; i < requests.length; i++) {
  const request = requests[i],
    original = expected[i];
  let actual;
  try {
    const result = await executeAssetTransform(
      request.operation,
      new Uint8Array(Buffer.from(request.payload, "base64")),
      request.options,
    );
    actual = {
      ok: true,
      result: {
        ...result,
        files: Object.fromEntries(
          Object.entries(result.files).map(([name, bytes]) => [
            name,
            Buffer.from(bytes).toString("base64"),
          ]),
        ),
      },
    };
  } catch (error) {
    actual = { ok: false, error: error.message };
  }
  assert.equal(
    original.ok,
    !request.rejects,
    `oracle ${i} ${request.operation}: ${JSON.stringify(original)}`,
  );
  assert.equal(
    actual.ok,
    original.ok,
    `${i} ${request.operation}: ${JSON.stringify({ actual, original })}`,
  );
  if (original.ok) assert.deepEqual(actual.result, original.result, `${i} ${request.operation}`);
}
console.log(
  `${requests.length} focused table/path cases match the original Python functions, including reject cases.`,
);
