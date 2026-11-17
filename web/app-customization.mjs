// 独立的未签名 IPA 外观事务；不修改游戏、着色器、执行文件或签名授权。
// Independent unsigned-IPA identity transaction; never changes game data, shaders, executables or signing grants.
import { inspectUnsignedSigningArchive, createSignedSigningArchive } from "./ipa-signing-zip.mjs";
import {
  readCustomizationPlist,
  writeCustomizationPlist,
  CUSTOM_PLIST_BYTES,
} from "./app-customization-plist.mjs";
import {
  prepareCustomizationIcons,
  CUSTOM_ICON_FILES,
  CUSTOM_ICON_LIMITS,
} from "./app-customization-icon.mjs";
export const APP_CUSTOMIZATION_LIMITS = Object.freeze({
  displayNameCharacters: 80,
  bundleIdentifierCharacters: 255,
  versionCharacters: 32,
  iconBytes: CUSTOM_ICON_LIMITS.bytes,
  iconMinSide: CUSTOM_ICON_LIMITS.minSide,
  iconMaxSide: CUSTOM_ICON_LIMITS.maxSide,
});
export const APP_CUSTOMIZATION_ERROR_CODES = Object.freeze([
  "APP_CUSTOMIZATION_INVALID",
  "APP_CUSTOMIZATION_NAME_INVALID",
  "APP_CUSTOMIZATION_BUNDLE_INVALID",
  "APP_CUSTOMIZATION_VERSION_INVALID",
  "APP_CUSTOMIZATION_BUILD_INVALID",
  "APP_CUSTOMIZATION_ICON_INVALID",
  "APP_CUSTOMIZATION_PLIST_INVALID",
  "APP_CUSTOMIZATION_ARCHIVE_INVALID",
  "APP_CUSTOMIZATION_LOCALIZED_METADATA",
  "APP_CUSTOMIZATION_FAILED",
]);
const fail = (code) => {
  throw new Error(code);
};
const prepared = new WeakMap();
const keys = ["displayName", "bundleIdentifier", "version", "buildVersion", "icon"];
/**
 * 空字段保留固定模板元数据；不接受路径、权限或可执行文件选项。
 * Blank fields retain pinned shell metadata. No paths, entitlements or executable options. */
export function normalizeAppCustomization(value = {}) {
  if (
    !value ||
    typeof value !== "object" ||
    Array.isArray(value) ||
    Object.keys(value).some((key) => !keys.includes(key))
  )
    fail("APP_CUSTOMIZATION_INVALID");
  const result = {};
  for (const key of keys) {
    const field = value[key];
    if (field === undefined || field === "") continue;
    if (key === "icon") {
      if (!(field instanceof Blob) || field.size === 0 || field.size > CUSTOM_ICON_LIMITS.bytes)
        fail("APP_CUSTOMIZATION_ICON_INVALID");
    } else {
      if (typeof field !== "string") fail("APP_CUSTOMIZATION_INVALID");
      if (
        key === "displayName" &&
        (field.trim() !== field ||
          !field.trim() ||
          [...field].length > APP_CUSTOMIZATION_LIMITS.displayNameCharacters ||
          // eslint-disable-next-line no-control-regex -- Reject controls in a visible app name.
          /[\u0000-\u001f\u007f-\u009f\ufffe\uffff]/u.test(field) ||
          !field.isWellFormed())
      )
        fail("APP_CUSTOMIZATION_NAME_INVALID");
      if (
        key === "bundleIdentifier" &&
        (field.length > APP_CUSTOMIZATION_LIMITS.bundleIdentifierCharacters ||
          !/^[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+$/.test(field))
      )
        fail("APP_CUSTOMIZATION_BUNDLE_INVALID");
      if (
        key === "version" &&
        (field.length > APP_CUSTOMIZATION_LIMITS.versionCharacters ||
          !/^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)$/.test(field))
      )
        fail("APP_CUSTOMIZATION_VERSION_INVALID");
      // 稳定构建号子集，不接受开发后缀；首段 1..9999，随后两段各 0..99。
      // Stable build-number subset, without development suffixes: first 1..9999, others 0..99.
      if (key === "buildVersion" && !/^[1-9]\d{0,3}(?:\.(?:0|[1-9]\d?)){0,2}$/.test(field))
        fail("APP_CUSTOMIZATION_BUILD_INVALID");
    }
    result[key] = field;
  }
  return Object.freeze(result);
}
/**
 * 消耗打包任务前先预检；返回对象可传给 generate()。
 * Preflight before consuming a packaging job; the returned object can be passed to generate(). */
export async function prepareAppCustomization(value, { signal } = {}) {
  signal?.throwIfAborted();
  if (value && prepared.has(value)) return value;
  const options = normalizeAppCustomization(value);
  const icons = options.icon ? await prepareCustomizationIcons(options.icon, { signal }) : [];
  signal?.throwIfAborted();
  prepared.set(options, icons);
  return options;
}
const dictionary = (entries) => Object.assign(Object.create(null), entries);
const validBundle = (value) =>
  typeof value === "string" &&
  value.length > 0 &&
  value.length <= 255 &&
  /^[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*$/.test(value);
/**
 * 使用有界规范 ZIP 校验及不可变 Blob 切片，不对整个 IPA 调用 arrayBuffer()。
 * Uses bounded canonical ZIP validation and immutable Blob slices, never whole-IPA arrayBuffer(). */
export async function customizeUnsignedIpa(blob, options, { signal } = {}) {
  if (!options || !prepared.has(options)) fail("APP_CUSTOMIZATION_INVALID");
  if (Object.keys(options).length === 0) return Object.freeze({ blob, changed: false });
  signal?.throwIfAborted();
  try {
    const archive = await inspectUnsignedSigningArchive(blob, { signal });
    const path = archive.appRoot + "Info.plist",
      info = archive.entries.find((entry) => entry.path === path);
    if (!info || info.size > CUSTOM_PLIST_BYTES) fail("APP_CUSTOMIZATION_PLIST_INVALID");
    // 不覆盖本地化元数据，以免显示名在某些语言下仍回退到旧名称。
    // Reject localized overrides rather than silently leave a different name/version in some languages.
    if (
      (options.displayName || options.version) &&
      archive.entries.some(
        (entry) =>
          /^.*\.lproj\/InfoPlist\.strings$/i.test(entry.path.slice(archive.appRoot.length)) &&
          !entry.path.slice(archive.appRoot.length).includes(".framework/"),
      )
    )
      fail("APP_CUSTOMIZATION_LOCALIZED_METADATA");
    const plist = readCustomizationPlist(new Uint8Array(await info.blob.arrayBuffer()));
    signal?.throwIfAborted();
    if (
      !validBundle(plist.CFBundleIdentifier) ||
      plist.CFBundlePackageType !== "APPL" ||
      typeof plist.CFBundleExecutable !== "string" ||
      !/^[A-Za-z0-9_.-]{1,255}$/.test(plist.CFBundleExecutable) ||
      [".", ".."].includes(plist.CFBundleExecutable) ||
      !archive.entries.some(
        (entry) => entry.path === archive.appRoot + plist.CFBundleExecutable && entry.mode & 0o111,
      )
    )
      fail("APP_CUSTOMIZATION_PLIST_INVALID");
    if (options.displayName) {
      plist.CFBundleDisplayName = options.displayName;
      plist.CFBundleName = options.displayName;
      if (Object.hasOwn(plist, "CFBundleSpokenName"))
        plist.CFBundleSpokenName = options.displayName;
    }
    if (options.bundleIdentifier) plist.CFBundleIdentifier = options.bundleIdentifier;
    if (options.version) plist.CFBundleShortVersionString = options.version;
    if (options.buildVersion) plist.CFBundleVersion = options.buildVersion;
    const icons = prepared.get(options);
    if (icons.length) {
      const files = (family) =>
        CUSTOM_ICON_FILES.filter(
          ([, , group]) => group === family || (family === "pad" && group === "phone"),
        ).map(([name]) => name);
      for (const [key, family] of [
        ["CFBundleIcons", "phone"],
        ["CFBundleIcons~ipad", "pad"],
      ])
        plist[key] = dictionary({
          CFBundlePrimaryIcon: dictionary({
            CFBundleIconFiles: files(family),
            UIPrerenderedIcon: true,
          }),
        });
      for (const key of [
        "CFBundleIconFile",
        "CFBundleIconName",
        "CFBundleIconFiles",
        "CFBundleIconFiles~ipad",
        "CFBundleIconName~ipad",
        "CFBundleIconFile~ipad",
      ])
        delete plist[key];
    }
    const newInfo = new Blob([writeCustomizationPlist(plist)]);
    const entries = archive.entries.map((entry) =>
      entry.path === path ? { ...entry, size: newInfo.size, blob: newInfo } : entry,
    );
    for (const icon of icons) {
      const iconPath = archive.appRoot + icon.path;
      if (entries.some((entry) => entry.path === iconPath))
        fail("APP_CUSTOMIZATION_ARCHIVE_INVALID");
      entries.push({ path: iconPath, size: icon.blob.size, mode: 0o644, blob: icon.blob });
    }
    const output = await createSignedSigningArchive(entries, { signal });
    signal?.throwIfAborted();
    return Object.freeze({
      blob: output,
      changed: true,
      iconMode: icons.length ? "loose-png-only" : "unchanged",
      manifest: Object.freeze(
        entries
          .map(({ path, size, mode }) => Object.freeze({ path, size, mode }))
          .sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0)),
      ),
    });
  } catch (error) {
    signal?.throwIfAborted();
    if (APP_CUSTOMIZATION_ERROR_CODES.includes(error?.message)) throw error;
    fail("APP_CUSTOMIZATION_FAILED");
  }
}
