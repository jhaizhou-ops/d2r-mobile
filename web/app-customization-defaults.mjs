// 表单显示已固定外壳的真实身份；只把用户修改传给现有的自定义事务。
// Show the pinned shell's actual identity; pass only edits to the existing customization transaction.
import { resolveShellProfile } from "./shell-profiles.mjs";
import { normalizeAppCustomization } from "./app-customization.mjs";

const identityKeys = Object.freeze({
  displayName: "CFBundleDisplayName",
  bundleIdentifier: "CFBundleIdentifier",
  version: "CFBundleShortVersionString",
  buildVersion: "CFBundleVersion",
});

/**
 * 这些是检查所得的 plist 值，而非发布标签或当前原生构建设置。
 *
 * These are inspected plist values, not the release tag or current native build settings.
 * @returns {Readonly<{ displayName: string, bundleIdentifier: string, version: string, buildVersion: string }>}
 */
export function resolveAppCustomizationDefaults(profileId) {
  const { evidence } = resolveShellProfile(profileId);
  const plist = evidence.inspection?.plist;
  for (const key of Object.values(identityKeys))
    if (typeof plist?.[key] !== "string" || !plist[key])
      throw new Error("SHELL_IDENTITY_EVIDENCE_MISSING");
  return Object.freeze({
    displayName: plist.CFBundleDisplayName,
    bundleIdentifier: plist.CFBundleIdentifier,
    version: plist.CFBundleShortVersionString,
    buildVersion: plist.CFBundleVersion,
  });
}

/**
 * 单独记录显式编辑，使切换模板只更新未编辑字段，不覆盖用户修改。
 * Keep explicit edits separate so switching shells updates untouched fields, not user edits. */
export function updateAppCustomizationDraft(draft, defaults, field, value) {
  if (!Object.hasOwn(identityKeys, field) || typeof value !== "string")
    throw new Error("APP_CUSTOMIZATION_INVALID");
  const next = { ...draft };
  if (value === defaults[field]) delete next[field];
  else next[field] = value;
  return next;
}

/**
 * 与默认值完全相同的值不算覆盖；尤其不能拒绝固定模板的版本“1.0”，也不能为满足更严格的自定义版本约定而改写它。
 *
 * Exact defaults are not overrides. In particular, the pinned shell's version "1.0"
 * must not be rejected by or rewritten to fit the stricter custom-version contract.
 */
export function normalizeAppCustomizationDraft(draft, defaults) {
  if (!draft || typeof draft !== "object" || Array.isArray(draft))
    return normalizeAppCustomization(draft);
  const overrides = { ...draft };
  for (const field of Object.keys(identityKeys))
    if (overrides[field] === defaults[field]) delete overrides[field];
  return normalizeAppCustomization(overrides);
}
