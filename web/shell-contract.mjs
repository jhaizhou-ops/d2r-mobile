// 验证仅元数据的外壳与组装约定。
// 依据可信发布配方及独立观察的包元数据。
// Validates the metadata-only shell and assembly contract.
// Based on trusted release recipes and independently observed bundle metadata.

/**
 * 仅元数据的外壳/组装约定。不读取或写入 ZIP，不验证内容或签名。
 * Metadata-only shell/assembly contract. No ZIP reads, writes, content verification or signing.
 */
import { validateCascArchiveName } from "./astc-output-validator.mjs";

export const SHELL_LIMITS = Object.freeze({
  entries: 20000,
  requiredPaths: 1024,
  text: 262144,
  path: 512,
  bytes: 4 * 1024 ** 3,
});
const fail = (message) => {
  throw new Error(message);
};
function path(value) {
  if (
    typeof value !== "string" ||
    !value.length ||
    value.length > SHELL_LIMITS.path ||
    // Apple 包资源使用 @2x~ipad 等密度/设备后缀。
    // 这些是字面文件名字符，不作路径或 URL 解释。
    // Apple bundle resources use density/device suffixes such as @2x~ipad.
    // These are literal filename characters, not path or URL interpretation.
    !/^[A-Za-z0-9_./ @~-]+$/.test(value) ||
    value.startsWith("/") ||
    value
      .replace(/\/$/, "")
      .split("/")
      .some((p) => !p || p === "." || p === ".." || /[. ]$/.test(p))
  )
    fail("Unsafe or unsupported path");
  return value;
}
function text(value) {
  if (
    typeof value !== "string" ||
    value.length > SHELL_LIMITS.text ||
    /[^\x20-\x7e\r\n]/.test(value)
  )
    fail("Invalid or oversized contract text");
  return value.split(/\r?\n/).filter((line) => line.length);
}
function paths(values) {
  if (!Array.isArray(values) || !values.length || values.length > SHELL_LIMITS.requiredPaths)
    fail("Invalid required paths");
  const result = values.map(path);
  const seen = new Set();
  for (const p of result) {
    const key = p.replace(/\/$/, "").toLowerCase();
    if (seen.has(key)) fail("Duplicate required path");
    seen.add(key);
  }
  return result;
}
function target(value) {
  if (
    !value ||
    !["ios", "mac"].includes(value.platform) ||
    typeof value.minimumVersion !== "string" ||
    !/^\d{1,2}\.\d{1,2}(?:\.\d{1,2})?$/.test(value.minimumVersion)
  )
    fail("Missing or unsupported targetOS");
  const parts = value.minimumVersion.split(".").map(Number);
  if (parts[0] < 1) fail("Unsupported targetOS version");
  return {
    platform: value.platform,
    minimumVersion: [...parts, ...Array(3 - parts.length).fill(0)].join("."),
  };
}
function matchTarget(actual, expected) {
  const got = target(actual);
  if (got.platform !== expected.platform || got.minimumVersion !== expected.minimumVersion)
    fail("targetOS version mismatch");
}
function recipe(value) {
  if (!value || value.schemaVersion !== 1) fail("Unsupported trusted recipe version");
  for (const key of ["product", "chain", "kind", "shaderDigest"]) {
    if (typeof value[key] !== "string" || !/^[A-Za-z0-9_.-]{1,128}$/.test(value[key]))
      fail(`Invalid trusted recipe ${key}`);
  }
  if (!["ios", "mac"].includes(value.kind) || !/^[0-9a-f]{16}$/.test(value.shaderDigest))
    fail("Unsupported trusted recipe kind or shader digest");
  const targetOS = target(value.targetOS);
  if (targetOS.platform !== value.kind) fail("Recipe KIND/targetOS mismatch");
  return {
    ...value,
    targetOS,
    requiredPaths: paths(value.requiredPaths),
    extraFiles: value.extraFiles?.length ? paths(value.extraFiles) : [],
  };
}
function generatedOverlayArchive(name, size) {
  if (size <= 0 || size > 128 * 1024 * 1024) return false;
  const match =
    /^(?:Payload\/DmIOS\.app\/)?romfs_overlay(?:_astc)?(?:_variants\/item_notes)?\/data\/([0-9]{2,3}-[0-9]{8}\.data)$/.exec(
      name,
    );
  if (!match) return false;
  try {
    // 与覆盖生成器共享规范归档编号，保留根目录和独立大小限制。
    // Share canonical archive numbering with overlay validation, retaining root and size bounds.
    validateCascArchiveName(match[1]);
    return true;
  } catch {
    return false;
  }
}
function inventory(entries, { allowOverlayArchives = false } = {}) {
  if (!Array.isArray(entries) || !entries.length || entries.length > SHELL_LIMITS.entries)
    fail("Inventory entry limit");
  const nodes = new Map(),
    explicit = new Set();
  let total = 0;
  const result = entries.map((entry) => {
    if (!entry || typeof entry !== "object") fail("Invalid entry metadata");
    const name = path(entry.path),
      directory = name.endsWith("/");
    if (
      !Number.isSafeInteger(entry.size) ||
      entry.size < 0 ||
      entry.size > SHELL_LIMITS.bytes ||
      !Number.isInteger(entry.mode) ||
      entry.mode < 0 ||
      entry.mode > 0xffff
    )
      fail("Invalid size or UNIX mode");
    const type = entry.mode & 0xf000;
    if ((type && type !== (directory ? 0x4000 : 0x8000)) || entry.mode & 0o7000)
      fail("Symlink, special file or unsafe mode");
    if (directory && entry.size !== 0) fail("Nonempty directory entry");
    total += entry.size;
    if (total > SHELL_LIMITS.bytes) fail("Inventory byte limit");
    const components = name.replace(/\/$/, "").split("/");
    const lower = components.map((p) => p.toLowerCase());
    if (
      lower.some((p) =>
        [
          "_codesignature",
          "embedded.mobileprovision",
          "embedded.provisionprofile",
          "plugins",
          "extensions",
          "romfs",
          "exefs",
        ].includes(p),
      ) ||
      /\.(?:nsp|xci|nca|p12|pfx|pem|key|keys|mobileprovision)$/i.test(name) ||
      ((/\d{2}-\d{8}\.data$/i.test(name) ||
        components.some((component) => /^[0-9]+-[0-9]+\.data$/i.test(component))) &&
        !(allowOverlayArchives && generatedOverlayArchive(name, entry.size)))
    )
      fail("Forbidden signing, credential or bulk RomFS archive path");
    for (let i = 0; i < components.length; i++) {
      const original = components.slice(0, i + 1).join("/"),
        key = original.toLowerCase();
      const dir = i < components.length - 1 || directory;
      const previous = nodes.get(key);
      if (previous && (previous.original !== original || previous.directory !== dir))
        fail("Case collision or file-directory conflict");
      if (i === components.length - 1) {
        if (explicit.has(key)) fail("Duplicate archive entry");
        explicit.add(key);
      }
      nodes.set(key, { original, directory: dir });
    }
    // 返回相关元数据的独立快照，绝不移除签名。
    // Return a detached snapshot of the relevant metadata, never strip signatures.
    const copy = { path: name, size: entry.size, mode: entry.mode };
    if (entry.signature !== undefined) {
      const s = entry.signature;
      if (
        !s ||
        s.kind !== "ad-hoc" ||
        !Number.isSafeInteger(s.offset) ||
        s.offset < 0 ||
        !Number.isSafeInteger(s.size) ||
        s.size <= 0 ||
        s.offset > entry.size - s.size ||
        directory
      )
        fail("Invalid structural ad-hoc signature metadata");
      copy.signature = { kind: s.kind, offset: s.offset, size: s.size };
    }
    return copy;
  });
  return { entries: result, nodes, total };
}
function baseOf(inv) {
  const apps = new Set();
  for (const entry of inv.entries) {
    if (entry.path === "Payload/") continue;
    const m = /^Payload\/([^/]+\.app)\//.exec(entry.path);
    if (!m) fail("Entry outside the single Payload app");
    apps.add(m[1]);
  }
  if (apps.size !== 1) fail("Expected exactly one Payload/*.app");
  return `Payload/${[...apps][0]}/`;
}
function requiredFile(inv, name) {
  if (inv.nodes.get(name.toLowerCase())?.directory !== false)
    fail(`Missing required file: ${name}`);
}
/**
 * 配方必须来自独立可信的发布注册表，而非外壳内容。
 * observedTargetOS 必须来自独立的 Mach-O/plist 解析，而非 shell_info。
 * ZIP 元数据条目：{path,size,mode,signature?:{kind:'ad-hoc',offset,size}}。
 * Recipe must come from a separately trusted release registry, NOT shell contents.
 * observedTargetOS must come from independent Mach-O/plist parsing, not shell_info.
 * ZIP metadata entries: {path,size,mode,signature?:{kind:'ad-hoc',offset,size}}.
 */
export function validateShellContract({
  entries,
  shellInfoText,
  shellManifestText,
  trustedRecipe,
  observedTargetOS,
}) {
  const r = recipe(trustedRecipe),
    inv = inventory(entries),
    base = baseOf(inv);
  matchTarget(observedTargetOS, r.targetOS);
  const info = Object.create(null);
  for (const line of text(shellInfoText)) {
    const m = /^(PRODUCT|CHAIN|KIND|SHADER_SRC|SPIRV_CROSS)=([^=]+)$/.exec(line);
    if (!m || Object.hasOwn(info, m[1])) fail("Unknown, malformed or duplicate shell_info field");
    info[m[1]] = m[2];
  }
  for (const [field, expected] of [
    ["PRODUCT", r.product],
    ["CHAIN", r.chain],
    ["KIND", r.kind],
    ["SHADER_SRC", r.shaderDigest],
  ]) {
    if (info[field] !== expected) fail(`Shell ${field} mismatch`);
  }
  const required = paths(text(shellManifestText));
  if (JSON.stringify([...required].sort()) !== JSON.stringify([...r.requiredPaths].sort()))
    fail("Shell manifest differs from trusted recipe");
  for (const f of [
    "Info.plist",
    base.split("/")[1].slice(0, -4),
    "dm/shell_info.txt",
    "dm/shell_manifest.txt",
  ])
    requiredFile(inv, base + f);
  for (const p of required) {
    if (
      inv.entries.some(
        (e) => e.path === base + p || (p.endsWith("/") && e.path.startsWith(base + p)),
      )
    )
      fail("Shell already contains generated manifest assets");
  }
  return {
    status: "shell-inventory-valid",
    installable: false,
    base,
    targetOS: r.targetOS,
    entries: inv.entries,
    requiredPaths: required,
  };
}
/**
 * 重新验证外壳，不接受伪造的验证结果。生成的条目使用相对于 app 的路径；
 * observedOutputTargetOS 由独立解析获得。
 * Revalidates shell rather than accepting a forged validation result. Generated
 * entries use app-relative paths; observedOutputTargetOS is independently parsed.
 */
export function validateAssemblyInventory({ shell, generatedEntries, observedOutputTargetOS }) {
  const validated = validateShellContract(shell),
    r = recipe(shell.trustedRecipe);
  matchTarget(observedOutputTargetOS, r.targetOS);
  const generated = inventory(generatedEntries, { allowOverlayArchives: true });
  for (const entry of generated.entries) {
    const allowed = [...r.requiredPaths, ...r.extraFiles].some((p) =>
      p.endsWith("/") ? entry.path.startsWith(p) : entry.path === p,
    );
    // 仅允许显式目录条目作为获准输出的祖先。
    // Explicit directory entries are allowed only as ancestors of approved outputs.
    const ancestor =
      entry.path.endsWith("/") &&
      [...r.requiredPaths, ...r.extraFiles].some((p) => p.startsWith(entry.path));
    if (!allowed && !ancestor) fail("Generated entry outside trusted manifest/extraFiles");
  }
  for (const p of r.requiredPaths) {
    if (p.endsWith("/")) {
      if (!generated.entries.some((e) => !e.path.endsWith("/") && e.path.startsWith(p)))
        fail(`Missing or empty required directory: ${p}`);
    } else requiredFile(generated, p);
  }
  const guest = generated.entries.find((e) => e.path === "Frameworks/Guest.framework/Guest");
  if (!guest || (guest.mode & 0o111) !== 0o111 || !guest.signature)
    fail("Guest needs executable mode and structural ad-hoc signature metadata");
  const joined = inventory(
    [
      ...validated.entries,
      ...generated.entries.map((e) => ({ ...e, path: validated.base + e.path })),
    ],
    { allowOverlayArchives: true },
  );
  baseOf(joined);
  return {
    status: "assembly-inventory-valid",
    installable: false,
    contentVerified: false,
    base: validated.base,
    targetOS: r.targetOS,
    totalBytes: joined.total,
    entries: joined.entries,
  };
}
