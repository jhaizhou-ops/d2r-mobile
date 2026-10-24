// 在浏览器中确定性生成合成 Guest Mach-O。
// 依据本模块显式编码的 Mach-O 布局与合成模块输入约定。
// Deterministically generates synthetic Guest Mach-O in the browser.
// Based on the Mach-O layout encoded here and the synthetic module input contract.

/**
 * 确定性的浏览器 Guest Mach-O 写入器。仅用于合成数据一致性：
 * 不验证游戏可玩性、iOS 加载、安装或签名。
 * 输入：有序的 {name, image: Uint8Array, textEnd, patchOff} 模块。
 * 策略：最多 48 个模块，输入总量、填充后的映像数据和最终文件均不超过 128 MiB。
 * 输出分配或哈希前验证所有大小、名称和布局。
 * 一个输出缓冲区（最大 128 MiB）；借用输入，不复制。
 * WebCrypto 在哈希 UUID 时可能复制主体（最多 128 MiB），随后每次最多
 * 复制一个 4 KiB 页。预算约 386 MiB，其中包括调用方紧凑分配的
 * 128 MiB 输入；调用方保留的超大底层缓冲区及浏览器/GC 开销不在此范围。
 * 更大的真实输入需要未来的流式写入器，而非盲目提高上限。
 * 构建期间调用方不得修改输入。AbortSignal 取消在摘要之间检查；
 * 正在运行的 WebCrypto 摘要无法停止。
 * 无需文件系统、依赖项、网络或可执行游戏数据。
 * Deterministic browser Guest Mach-O writer. Synthetic parity only: not a
 * playable-game, iOS loading, installation or signing verification.
 * Input: ordered {name, image: Uint8Array, textEnd, patchOff} modules.
 * Policy: <=48 modules, <=128 MiB aggregate input, padded image data AND final output.
 * All sizes, names and layout are validated before output allocation/hashing.
 * One output buffer (128 MiB maximum); inputs are borrowed, not copied.
 * WebCrypto may copy the body while hashing UUID (<=128 MiB), then at most
 * one 4 KiB page at a time. Budget roughly 386 MiB including 128 MiB
 * of tightly sized caller inputs; oversized backing buffers retained by the
 * caller and browser/GC overhead are outside this bound. Larger real inputs
 * require a future streaming writer rather than raising this cap blindly.
 * Caller must not mutate inputs while build runs. AbortSignal cancellation
 * is observed between digests; an in-flight WebCrypto digest cannot be stopped.
 * No filesystem, dependencies, network, or executable game data required.
 */
import { resolveShellProfile } from "./shell-profiles.mjs";

export const PAGE_SIZE = 0x4000;
export const MAX_IMAGE_BYTES = 128 * 1024 * 1024;
export const MAX_GUEST_BYTES = 128 * 1024 * 1024;
export const MAX_MODULES = 48;
const enc = new TextEncoder();
const align = (n, page = PAGE_SIZE) => Math.ceil(n / page) * page;
const integer = (n, label) => {
  if (!Number.isSafeInteger(n) || n < 0 || n > MAX_IMAGE_BYTES)
    throw new RangeError(`${label} outside supported bounds`);
};
function dylibSize(name) {
  return 24 + (Math.floor(name.length / 8) + 1) * 8;
}
const ID = "@rpath/Guest.framework/Guest",
  SYSTEM = "/usr/lib/libSystem.B.dylib";

export function planGuestFramework(modules) {
  if (!Array.isArray(modules) || !modules.length || modules.length > MAX_MODULES)
    throw new RangeError("Expected 1..48 modules");
  let total = 0,
    padded = 0;
  const names = new Set();
  const parts = modules.map((m) => {
    if (
      !m ||
      typeof m.name !== "string" ||
      !/^[A-Za-z0-9_]{1,10}$/.test(m.name) ||
      names.has(m.name)
    )
      throw new TypeError("Module names must be unique ASCII identifiers of 1..10 characters");
    names.add(m.name);
    if (!(m.image instanceof Uint8Array) || m.image.buffer instanceof SharedArrayBufferSafe)
      throw new TypeError("Image must be an unshared Uint8Array");
    const size = m.image.byteLength;
    integer(size, "Image size");
    integer(m.textEnd, "TEXT_END");
    integer(m.patchOff, "PATCH_OFF");
    if (!size || m.textEnd > size)
      throw new RangeError("Image must be nonempty and contain TEXT_END");
    const a = align(m.textEnd),
      end = align(size);
    if (m.patchOff % PAGE_SIZE || a > m.patchOff || m.patchOff > end)
      throw new RangeError("Invalid segment boundaries");
    total += size;
    padded += end;
    if (total > MAX_IMAGE_BYTES || padded > MAX_IMAGE_BYTES)
      throw new RangeError("Aggregate images exceed 128 MiB policy");
    return { name: m.name, image: m.image, a, end, patchOff: m.patchOff };
  });
  const commandSize =
    72 +
    160 * parts.length +
    152 * parts.length +
    72 +
    dylibSize(ID) +
    48 +
    24 +
    80 +
    24 +
    24 +
    dylibSize(SYSTEM) +
    16;
  if (32 + commandSize > PAGE_SIZE) throw new RangeError("Load commands exceed header page");
  const bodyEnd = PAGE_SIZE + padded,
    signatureOffset = align(bodyEnd + 8, 16);
  const signatureSize = align((Math.floor(signatureOffset / 4096) + 1) * 52, 4096) + 0x40000;
  const outputSize = signatureOffset + signatureSize;
  if (outputSize > MAX_GUEST_BYTES)
    throw new RangeError("Guest framework exceeds 128 MiB output policy");
  return {
    parts,
    commandSize,
    bodyEnd,
    signatureOffset,
    signatureSize,
    outputSize,
  };
}
// 避免引用不存在的浏览器全局对象，同时仍拒绝共享缓冲区。
// Avoid referencing an absent browser global while still rejecting shared buffers.
const SharedArrayBufferSafe = globalThis.SharedArrayBuffer ?? class UnavailableSharedBuffer {};

export async function buildGuestFramework(
  modules,
  { crypto = globalThis.crypto, signal, profileId } = {},
) {
  const checkAbort = () => {
    if (signal?.aborted) throw new DOMException("Guest framework build aborted", "AbortError");
  };
  checkAbort();
  const { guestTarget } = resolveShellProfile(profileId);
  const packedVersion = (version) =>
    version
      .split(".")
      .reduce((value, component, i) => value + Number(component) * [65536, 256, 1][i], 0);
  const p = planGuestFramework(modules);
  if (!crypto?.subtle?.digest)
    throw new Error("SHA-256 WebCrypto is required (use a secure browser context)");
  checkAbort();
  const out = new Uint8Array(p.outputSize),
    v = new DataView(out.buffer);
  const u32 = (o, n, little = true) => v.setUint32(o, n, little);
  const u64 = (o, n, little = true) => v.setBigUint64(o, BigInt(n), little);
  const string = (o, s) => out.set(enc.encode(s), o);
  let cursor = 32,
    offset = PAGE_SIZE;
  const sections = [],
    writable = [];
  function copyPart(image, lo, hi) {
    const at = offset;
    if (lo < image.length) out.set(image.subarray(lo, Math.min(hi, image.length)), at);
    offset += hi - lo;
    return at;
  }
  for (const m of p.parts) {
    sections.push({
      name: `__dm${m.name}x0`,
      at: copyPart(m.image, 0, m.a),
      size: m.a,
      flags: 0x80000400,
    });
    sections.push({
      name: `__dm${m.name}x1`,
      at: copyPart(m.image, m.patchOff, m.end),
      size: m.end - m.patchOff,
      flags: 0x80000400,
    });
  }
  const textSize = offset;
  for (const m of p.parts)
    writable.push({
      name: `__DM${m.name}W`,
      at: copyPart(m.image, m.a, m.patchOff),
      size: m.patchOff - m.a,
    });
  function command(type, size) {
    const o = cursor;
    u32(o, type);
    u32(o + 4, size);
    cursor += size;
    return o;
  }
  function segment(name, at, size, fileSize, prot, sects) {
    const o = command(0x19, 72 + 80 * sects.length);
    string(o + 8, name);
    u64(o + 24, at);
    u64(o + 32, size);
    u64(o + 40, at);
    u64(o + 48, fileSize);
    u32(o + 56, prot);
    u32(o + 60, prot);
    u32(o + 64, sects.length);
    sects.forEach((s, i) => {
      const q = o + 72 + i * 80;
      string(q, s.name);
      string(q + 16, name);
      u64(q + 32, s.at);
      u64(q + 40, s.size);
      u32(q + 48, s.at);
      u32(q + 52, 14);
      u32(q + 64, s.flags);
    });
  }
  function dylib(type, name) {
    const o = command(type, dylibSize(name));
    u32(o + 8, 24);
    u32(o + 12, 2);
    u32(o + 16, 0x10000);
    u32(o + 20, 0x10000);
    string(o + 24, name);
  }
  segment("__TEXT", 0, textSize, textSize, 5, sections);
  for (const s of writable)
    segment(s.name, s.at, s.size, s.size, 3, [{ ...s, name: "__img", flags: 0 }]);
  segment(
    "__LINKEDIT",
    p.bodyEnd,
    align(p.outputSize - p.bodyEnd),
    p.outputSize - p.bodyEnd,
    1,
    [],
  );
  dylib(0xd, ID);
  command(0x80000022, 48);
  let o = command(2, 24);
  u32(o + 8, p.bodyEnd);
  u32(o + 16, p.bodyEnd);
  u32(o + 20, 8);
  command(0xb, 80);
  o = command(0x1b, 24);
  const digest = async (bytes) => new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  const bodyHash = await digest(out.subarray(PAGE_SIZE, p.bodyEnd));
  checkAbort();
  out.set(bodyHash.subarray(0, 16), o + 8);
  o = command(0x32, 24);
  u32(o + 8, 2);
  u32(o + 12, packedVersion(guestTarget.minimumVersion));
  u32(o + 16, packedVersion(guestTarget.sdkVersion));
  dylib(0xc, SYSTEM);
  o = command(0x1d, 16);
  u32(o + 8, p.signatureOffset);
  u32(o + 12, p.signatureSize);
  if (cursor !== p.commandSize + 32 || offset !== p.bodyEnd)
    throw new Error("Internal layout mismatch");
  [0xfeedfacf, 0x0100000c, 0, 6, 10 + p.parts.length, p.commandSize, 0x00100085, 0].forEach(
    (n, i) => u32(i * 4, n),
  );
  const n = Math.ceil(p.signatureOffset / 4096),
    cdSize = 94 + n * 32,
    sig = p.signatureOffset;
  [0xfade0cc0, 20 + cdSize, 1, 0, 20].forEach((n, i) => u32(sig + i * 4, n, false));
  const cd = sig + 20;
  [0xfade0c02, cdSize, 0x20400, 0x20002, 94, 88, 0, n, sig].forEach((n, i) =>
    u32(cd + i * 4, n, false),
  );
  out.set([32, 2, 0, 12], cd + 36);
  u64(cd + 72, textSize, false);
  string(cd + 88, "Guest");
  for (let i = 0; i < n; i++) {
    checkAbort();
    const pageHash = await digest(out.subarray(i * 4096, Math.min((i + 1) * 4096, sig)));
    checkAbort();
    out.set(pageHash, cd + 94 + i * 32);
  }
  return out;
}

export function guestInfoPlist(bundleId = "local.guest", { profileId } = {}) {
  const { guestTarget } = resolveShellProfile(profileId);
  const minimumVersion = guestTarget.minimumVersion.replace(/\.0$/, "");
  if (
    typeof bundleId !== "string" ||
    !/^[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+$/.test(bundleId) ||
    bundleId.length > 255
  )
    throw new TypeError("Invalid bundle identifier");
  return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Guest</string>
<key>CFBundleIdentifier</key><string>${bundleId}</string>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>Guest</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>MinimumOSVersion</key><string>${minimumVersion}</string>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
</dict></plist>
`;
}
