// 依据资源工作线程的原始表生成操作生成派生表，并按外壳容量校验。
// Generate derived tables using the asset worker's original table-generation operations and check shell capacities.
/**
 * 从已认证本地资源生成原始表。回执并非等价性证据。
 * Original table generation from authenticated local assets. Receipts are not parity evidence.
 */
const VAULT_PATHS = Object.freeze([
  ...["ItemTypes", "armor", "weapons", "misc", "UniqueItems", "Sets", "SetItems"].map(
    (name) => `data/global/excel/${name}.txt`,
  ),
  ...["uniques", "sets", "items"].map((name) => `data/hd/items/${name}.json`),
]);
const fail = (code) => {
  throw new Error(code);
};
const encode = (bytes) => {
  let value = "";
  for (let i = 0; i < bytes.length; i += 8192)
    value += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(value);
};
const hash = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (n) =>
    n.toString(16).padStart(2, "0"),
  ).join("");
const bytesOf = (value) =>
  value instanceof Uint8Array
    ? value
    : value instanceof ArrayBuffer
      ? new Uint8Array(value)
      : fail("DERIVED_OUTPUT_TYPE");

/**
 * 调用方必须另行将返回计数与独立受信任的外壳容量比较。
 * Caller must separately compare returned counts to the independently trusted shell capacities.
 */
export async function generateProductionDerived({
  logicalAssets,
  assetWorker,
  nativeArtFiles,
  signal,
}) {
  const owned = [];
  const check = () => {
    if (signal?.aborted) fail("DERIVED_CANCELLED");
  };
  const read = async (path) => {
    check();
    const bytes = await logicalAssets.read(path, { maxBytes: 2 * 1024 * 1024, signal });
    if (!(bytes instanceof Uint8Array) || !bytes.length || bytes.length > 2 * 1024 * 1024)
      fail("DERIVED_SOURCE_LIMIT");
    owned.push(bytes);
    check();
    return bytes;
  };
  const run = async (operation, payload, options = {}) => {
    check();
    const result = await assetWorker.run(operation, payload, options);
    check();
    return result;
  };
  try {
    const experienceInput = await read("data/global/excel/experience.txt");
    const experienceSourceSha256 = await hash(experienceInput);
    const experience = await run("experience-table", experienceInput);
    const assets = {};
    let total = 0;
    for (const path of VAULT_PATHS) {
      const bytes = await read(path);
      total += bytes.length;
      if (total > 12 * 1024 * 1024) fail("VAULT_TOTAL_LIMIT");
      assets[path] = encode(bytes);
    }
    const payload = new TextEncoder().encode(
      JSON.stringify({ assets, manifest: logicalAssets.itemSpriteManifest() }),
    );
    owned.push(payload);
    if (payload.length > 20 * 1024 * 1024) fail("VAULT_INPUT_LIMIT");
    const vault = await run("vault-table", payload);
    const sources = {},
      expectedSourceSha256 = {};
    const generated = {
      "dm_hud_exp.h": experience.files?.["dm_hud_exp.h"],
      "dm_vault_tab.h": vault.files?.["dm_vault_tab.h"],
      "dm_touch_native_art.c": nativeArtFiles?.["dm_touch_native_art.c"],
    };
    for (const [name, value] of Object.entries(generated)) {
      const bytes = bytesOf(value);
      if (!bytes.length || bytes.length > 2 * 1024 * 1024) fail("DERIVED_SOURCE_LIMIT");
      if (name !== "dm_touch_native_art.c") owned.push(bytes);
      sources[name] = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
      expectedSourceSha256[name] = await hash(bytes);
    }
    const derivedPayload = new TextEncoder().encode(JSON.stringify(sources));
    owned.push(derivedPayload);
    if (derivedPayload.length > 8 * 1024 * 1024) fail("DERIVED_INPUT_LIMIT");
    const result = await run("derived-tables", derivedPayload, {
      mode: "generated",
      expectedSourceSha256,
    });
    const data = bytesOf(result.files?.["derived.tsv"]);
    if (!data.length || data.length > 8 * 1024 * 1024) fail("DERIVED_OUTPUT_LIMIT");
    // 非文件回执特意与可打包文件分离。
    // Non-file receipt is intentionally separate from the packageable files.
    return {
      files: { "dm/derived.tsv": data },
      counts: result.counts,
      receipt: {
        evidence: "generated-source-receipt",
        generatedSourceSha256: result.sourceSha256,
        experienceSourceSha256,
        vaultSourceSha256: vault.sourceSha256,
        itemManifestSha256: vault.manifestSha256,
      },
    };
  } finally {
    for (const bytes of owned) bytes.fill(0);
  }
}
