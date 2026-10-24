// 按本次请求的文件名分组，仅保留匹配的原始行，不持久化清单索引或解码数据。
// Group this request by basename; retain matching original rows only, never a persistent catalog index or decoded data.
export const LOGICAL_SUFFIX_BATCH_LIMITS = Object.freeze({ requests: 4096, bytes: 1024 * 1024 });
const fail = (code) => {
  throw new Error(code);
};
function requests(suffixes, check) {
  check();
  if (!Array.isArray(suffixes) || suffixes.length > LOGICAL_SUFFIX_BATCH_LIMITS.requests)
    fail("EXTRACTION_ASSET_SUFFIX_LIMIT");
  let bytes = 0;
  for (const suffix of suffixes) {
    check();
    if (
      typeof suffix !== "string" ||
      suffix.length > 512 ||
      !/^\/[a-z0-9_/.-]+$/.test(suffix) ||
      suffix.split("/").some((part) => part === "." || part === "..")
    )
      fail("EXTRACTION_ASSET_SUFFIX");
    bytes += suffix.length;
    if (bytes > LOGICAL_SUFFIX_BATCH_LIMITS.bytes) fail("EXTRACTION_ASSET_SUFFIX_LIMIT");
  }
}
/**
 * 只解析传入的不可变逻辑行；该辅助函数不授予认证能力。
 * Resolve supplied immutable logical rows only; this helper grants no authentication capability.
 */
export function findUniqueLogicalPathSuffixes(logical, suffixes, check = () => {}) {
  requests(suffixes, check);
  if (!suffixes.length) return Object.freeze([]);
  const groups = new Map(),
    matches = new Map(),
    ambiguous = new Set();
  for (const suffix of new Set(suffixes)) {
    const basename = suffix.slice(suffix.lastIndexOf("/") + 1);
    const group = groups.get(basename) ?? [];
    group.push(suffix);
    groups.set(basename, group);
  }
  for (const row of logical.entries()) {
    check();
    const normalized = row[9].toLowerCase();
    const group = groups.get(normalized.slice(normalized.lastIndexOf("/") + 1));
    if (!group) continue;
    for (const suffix of group) {
      if (!normalized.endsWith(suffix)) continue;
      if (matches.has(suffix) || logical.hasDuplicatePath(row[9])) ambiguous.add(suffix);
      else matches.set(suffix, row);
    }
  }
  // 扫描后按请求顺序报告错误，避免后面的歧义掩盖前面的缺失。
  // Report failures in request order after scanning, so later ambiguity cannot hide earlier missing paths.
  return Object.freeze(
    suffixes.map((suffix) => {
      check();
      if (ambiguous.has(suffix)) fail("EXTRACTION_ASSET_AMBIGUOUS");
      const row = matches.get(suffix);
      if (!row) fail("EXTRACTION_ASSET_MISSING");
      return row[9];
    }),
  );
}
/**
 * 旧测试适配器仍可提供单项接口；实际提取能力提供批量接口。
 * Older test adapters may retain the singular interface; real extraction capabilities provide the batch interface.
 */
export function resolveLogicalAssetSuffixes(logicalAssets, suffixes, check = () => {}) {
  requests(suffixes, check);
  if (typeof logicalAssets.findUniquePathSuffixes === "function")
    return logicalAssets.findUniquePathSuffixes(suffixes);
  return Object.freeze(
    suffixes.map((suffix) => {
      check();
      return logicalAssets.findUniquePathSuffix(suffix);
    }),
  );
}
