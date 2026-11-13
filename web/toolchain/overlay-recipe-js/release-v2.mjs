// b811 发布外壳早于 PS/配音新增项；按审查过的四键差异保留其原始字符串编号。
// The b811 release predates PS/voice additions; the reviewed four-key delta preserves original string IDs.
import data from "./annotation-data.json" with { type: "json" };
import release from "./release-v2-data.json" with { type: "json" };
const excluded = new Set(release.i18nExcludedKeys);
export const releaseI18n = {
  ...data.i18n,
  rows: data.i18n.rows.filter((row) => !excluded.has(row[0])),
};
