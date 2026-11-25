// 根据认证元数据约束 D2R 扩展包输入选择。
// 依据内容选择约定与本模块显式列出的 D2R 标题 ID。
// Constrains D2R expansion-package input selection using authenticated metadata.
// Based on the content-selection contract and D2R title IDs explicitly listed here.

import { ContentSelectionError, resolveContentSelection } from "./content-selection.mjs";

export const D2R_APPLICATION_ID = 0x0100726014352000n;
export const D2R_PATCH_ID = 0x0100726014352800n;
export const D2R_WARLOCK_ID = 0x0100726014353001n;
export const D2R_CONTENT_MODES = Object.freeze(["optional-warlock", "base-update-only", "warlock"]);

/**
 * 本地包输入策略，不验证合法所有权或票据。
 * 调用方必须从可信提取边界提供保留且已认证的 Meta-NCA 记录，
 * 以及独立测量的完整 NCA 清单。此纯函数不能认证任意 JS 对象
 * 或授权运行时行为。返回的选择结构不变；DLC 保留在 requiredContents 中，
 * 但绝不作为 Program RomFS 叠加层。仍必须严格执行可执行文件允许列表。
 * Local package-input policy, not legal ownership or ticket verification.
 * The caller MUST supply retained, authenticated Meta-NCA records and independently
 * measured whole-NCA inventory from its trusted extraction boundary. This pure
 * function cannot authenticate arbitrary JS objects or authorize runtime behavior.
 * Returns the unchanged selection shape; DLC remains in requiredContents but is
 * never a Program RomFS overlay. Exact executable allowlisting remains mandatory.
 */
export function resolveD2rExpansionSelection(records, inventory, options = {}) {
  const contentMode = options?.contentMode ?? "warlock";
  if (!D2R_CONTENT_MODES.includes(contentMode))
    throw new ContentSelectionError("D2R_CONTENT_MODE_INVALID", "Unsupported content mode.");
  const selection = resolveContentSelection(records, inventory, options);
  const reject = (code, message) => {
    throw new ContentSelectionError(code, message);
  };
  if (selection.applicationId !== D2R_APPLICATION_ID || selection.base.patchId !== D2R_PATCH_ID)
    reject("D2R_APPLICATION_REQUIRED", "The supported D2R application is required.");
  if (!selection.patch || selection.patch.titleId !== D2R_PATCH_ID)
    reject("D2R_UPDATE_REQUIRED", "The supported D2R update input is required.");
  if (
    contentMode === "base-update-only" ||
    (contentMode === "optional-warlock" && selection.dlc.length === 0)
  ) {
    if (selection.dlc.length)
      reject("D2R_UNEXPECTED_EXPANSION", "This mode accepts only your base and update packages.");
    // 此处仅选择输入内容。输出外壳必须独立配置为报告零个附加内容；
    // 这不会改变其原生 HLE。
    // This selects input content only. The output shell must independently be
    // configured to report zero add-ons; this does not change its native HLE.
    return selection;
  }
  if (!selection.dlc.length)
    reject("D2R_EXPANSION_REQUIRED", "Your Reign of the Warlock package input is required.");
  if (selection.dlc.length !== 1 || selection.dlc[0].titleId !== D2R_WARLOCK_ID)
    reject("D2R_UNKNOWN_EXPANSION", "Only the known Reign of the Warlock add-on is supported.");
  // 保守的可接受结构，不声称测量过真实包。
  // 只有审核已认证元数据和运行时依赖后才可扩展。
  // Conservative accepted shape, not a claim that a real package was measured.
  // Expand only after reviewing authenticated metadata and runtime dependencies.
  const contents = selection.dlc[0].contents;
  if (
    contents.length !== 1 ||
    contents[0].type !== 2 ||
    contents[0].idOffset !== 0 ||
    contents[0].attributes !== 0 ||
    contents[0].size === 0n
  )
    reject("D2R_EXPANSION_LAYOUT_UNSUPPORTED", "The add-on content layout is unsupported.");
  return selection;
}
