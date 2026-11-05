// 根据固定编译输出划分可选公开资产；不允许把通用依赖或外壳移入可选组。
// Partition optional public assets by fixed compiler outputs; common dependencies and shells cannot become optional.
export const OPTIONAL_RUNTIME_ASSET_GROUPS = Object.freeze({
  "ipa-signing": Object.freeze([
    "sealed-workers/signing-worker.mjs",
    "toolchain/zsign-wasm/dist/zsign.wasm",
    "toolchain/zsign-wasm/dist/ZSIGN-LICENSE.txt",
    "toolchain/zsign-wasm/dist/PORT-LICENSE.txt",
    "toolchain/zsign-wasm/dist/OPENSSL-LICENSE.txt",
    "toolchain/zsign-wasm/dist/OPENSSL-NOTICE.txt",
    "toolchain/zsign-wasm/dist/pins.json",
    "toolchain/zsign-wasm/dist/patch-report.json",
    "toolchain/zsign-wasm/dist/compiler-version.txt",
    "toolchain/zsign-wasm/dist/build-report.json",
  ]),
  "legacy-astc": Object.freeze([
    "sealed-workers/astc-producer-worker.mjs",
    "toolchain/astc-encoder-wasm/dist/astc-encoder.mjs",
    "toolchain/astc-encoder-wasm/dist/astc-encoder.wasm",
    "toolchain/astc-encoder-wasm/dist/ARM-LICENSE.txt",
    "toolchain/astc-encoder-wasm/dist/ARM-COMMIT.txt",
    "toolchain/astc-encoder-wasm/dist/compiler-version.txt",
    "toolchain/sha256-stream-wasm/dist/sha256.wasm",
    "toolchain/sha256-stream-wasm/dist/LICENSE",
    "toolchain/sha256-stream-wasm/dist/apache-2.0.txt",
    "toolchain/sha256-stream-wasm/dist/build-report.json",
  ]),
});

const pathGroups = new Map(
  Object.entries(OPTIONAL_RUNTIME_ASSET_GROUPS).flatMap(([group, paths]) =>
    paths.map((path) => [path, group]),
  ),
);
export const runtimeContentKey = (record) => `${record.sha256}:${record.bytes}`;

/**
 * 验证完整可选闭包并返回每种内容的有效归属；通用字节优先，重复许可证无需再次传输。
 * Validate complete optional closures and return effective content ownership; common bytes win so duplicate licenses travel once.
 */
export function validateRuntimeAssetGroups(assets) {
  if (!Array.isArray(assets)) throw new Error("Invalid runtime asset groups");
  const found = new Map(),
    ownership = new Map();
  for (const asset of assets) {
    const group = asset.optionalGroup;
    if (group !== pathGroups.get(asset.path)) throw new Error("Invalid optional runtime asset");
    if (group !== undefined) {
      if (!found.has(group)) found.set(group, new Set());
      const paths = found.get(group);
      if (paths.has(asset.path)) throw new Error("Duplicate optional runtime asset");
      paths.add(asset.path);
    }
    const key = runtimeContentKey(asset);
    if (!ownership.has(key) || group === undefined) ownership.set(key, group ?? "");
    else if (ownership.get(key) !== "") {
      // 两个可选闭包共用的内容必须独立分包，不得夹带任一未选组的独有字节。
      // Shared optional-only bytes get their own partition, never either group's unique payload.
      ownership.set(key, [...new Set([...ownership.get(key).split("|"), group])].sort().join("|"));
    }
  }
  for (const [group, paths] of found)
    if (paths.size !== OPTIONAL_RUNTIME_ASSET_GROUPS[group].length)
      throw new Error("Incomplete optional runtime asset group");
  return ownership;
}

export function validateRuntimeGroupSelection(optionalGroups = []) {
  if (
    !Array.isArray(optionalGroups) ||
    new Set(optionalGroups).size !== optionalGroups.length ||
    optionalGroups.some((group) => !Object.hasOwn(OPTIONAL_RUNTIME_ASSET_GROUPS, group))
  )
    throw new Error("Invalid optional runtime group selection");
  return Object.freeze([...optionalGroups]);
}

export function selectRuntimeAssetGroups(assets, optionalGroups = []) {
  validateRuntimeAssetGroups(assets);
  optionalGroups = validateRuntimeGroupSelection(optionalGroups);
  for (const group of optionalGroups)
    if (!assets.some((asset) => asset.optionalGroup === group))
      throw new Error("Selected optional runtime group is unavailable");
  return assets.filter(
    (asset) => asset.optionalGroup === undefined || optionalGroups.includes(asset.optionalGroup),
  );
}

/**
 * 验证所有物理包（包括未选组），防止默认下载夹带可选独有字节。
 * Validate every physical pack, including omitted groups, so default downloads cannot carry optional-only bytes.
 */
export function validateRuntimeTransportGroups(assets, entries) {
  const ownership = validateRuntimeAssetGroups(assets),
    files = new Map();
  for (const entry of entries) {
    const key = runtimeContentKey(entry);
    if (!ownership.has(key)) throw new Error("Unknown runtime transport content");
    const group = ownership.get(key);
    if (files.has(entry.file) && files.get(entry.file) !== group)
      throw new Error("Optional runtime payload is shared");
    files.set(entry.file, group);
  }
  return ownership;
}
