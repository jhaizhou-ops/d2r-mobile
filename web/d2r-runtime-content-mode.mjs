// 从准确的 v2.0.0 外壳派生仅数据内容模式。
// 依据 v2.0.0 外壳布局及 dm/host/hle_misc.c 的缓存附加内容计数。
// Derives a data-only content mode from the exact v2.0.0 shell.
// Based on the v2.0.0 shell layout and cached add-on count in dm/host/hle_misc.c.

/**
 * 准确的 v2.0.0 外壳仅数据派生。不是指令或授权补丁。
 * Exact v2.0.0 shell data-only derivation. Not an instruction or entitlement patch.
 */
import { DEFAULT_SHELL_PROFILE_ID, resolveShellProfile } from "./shell-profiles.mjs";

export const D2R_RUNTIME_CONTENT_PIN = resolveShellProfile().contentPin;
const fail = () => {
  throw new Error("D2R_RUNTIME_CONTENT_BINDING");
};
const digest = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (b) =>
    b.toString(16).padStart(2, "0"),
  ).join("");

function cacheOffset(bytes, pin) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const range = (offset, size) => {
    if (
      !Number.isSafeInteger(offset) ||
      !Number.isSafeInteger(size) ||
      offset < 0 ||
      size < 0 ||
      offset > bytes.length - size
    )
      fail();
  };
  const u32 = (p) => {
    range(p, 4);
    return view.getUint32(p, true);
  };
  const u64 = (p) => {
    range(p, 8);
    return view.getBigUint64(p, true);
  };
  const string = (p, length, requireTerminator = true) => {
    range(p, length);
    let end = p;
    while (end < p + length && bytes[end] !== 0) end++;
    if (requireTerminator && end === p + length) fail();
    return new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(p, end));
  };
  if (u32(0) !== 0xfeedfacf || u32(4) !== 0x0100000c) fail();
  const commandCount = u32(16),
    commandBytes = u32(20);
  range(32, commandBytes);
  if (commandCount > 4096) fail();
  let cursor = 32,
    symbols;
  const sections = [];
  for (let i = 0; i < commandCount; i++) {
    const command = u32(cursor),
      size = u32(cursor + 4);
    if (size < 8 || size % 8 !== 0 || cursor + size > 32 + commandBytes) fail();
    if (command === 0x19) {
      if (size < 72) fail();
      const count = u32(cursor + 64);
      if (count > 255 || size !== 72 + count * 80) fail();
      for (let j = 0; j < count; j++) {
        const p = cursor + 72 + j * 80;
        sections.push({
          name: string(p, 16, false),
          segment: string(p + 16, 16, false),
          address: u64(p + 32),
          size: u64(p + 40),
          offset: u32(p + 48),
          flags: u32(p + 64),
        });
      }
    } else if (command === 2) {
      if (symbols || size !== 24) fail();
      symbols = {
        offset: u32(cursor + 8),
        count: u32(cursor + 12),
        strings: u32(cursor + 16),
        stringSize: u32(cursor + 20),
      };
    }
    cursor += size;
  }
  if (cursor !== 32 + commandBytes || !symbols) fail();
  range(symbols.offset, symbols.count * 16);
  range(symbols.strings, symbols.stringSize);
  const matches = [];
  for (let i = 0; i < symbols.count; i++) {
    const p = symbols.offset + i * 16,
      type = bytes[p + 4],
      index = u32(p);
    if (type & 0xe0) continue; // STAB/调试重复项不属于定义。 | STAB/debug duplicates are not definitions.
    if (index >= symbols.stringSize) fail();
    if (string(symbols.strings + index, symbols.stringSize - index) !== pin.symbol) continue;
    if ((type & 0x0e) !== 0x0e || bytes[p + 5] === 0) fail();
    const section = sections[bytes[p + 5] - 1],
      address = u64(p + 8);
    if (
      !section ||
      section.name !== "__data" ||
      section.segment !== "__DATA" ||
      (section.flags & 0xff) !== 0 ||
      address !== pin.address ||
      address < section.address ||
      address + 4n > section.address + section.size
    )
      fail();
    const offset = section.offset + Number(address - section.address);
    range(offset, 4);
    if (offset !== pin.fileOffset || u32(offset) !== 0xffffffff) fail();
    matches.push(offset);
  }
  if (matches.length !== 1) fail();
  return matches[0];
}

/**
 * 仅在原始外壳验证后对其未修改的可执行文件调用。
 * 根据已认证的选择派生有效模式，而非 UI 所有权标志。
 * 源码 dm/host/hle_misc.c:18 将 static int v 初始化为 -1；v<0 触发 getenv，
 * 否则 Count/List 使用缓存的 v。零初始化值会永久报告无附加内容。
 * 这四字节修改会使现有签名失效；输出未签名，
 * 必须由用户在所有封装修改完成后签名。
 * Invoke only after original shell validation, on its unmodified executable.
 * Derive effective mode from authenticated selection, not a UI ownership flag.
 * Source dm/host/hle_misc.c:18 initializes static int v=-1; v<0 triggers getenv,
 * otherwise Count/List use cached v. A zero initializer permanently reports none.
 * The four-byte mutation invalidates any existing signature; output is unsigned
 * and must be signed by the user after all packaging mutations.
 */
export async function deriveD2rRuntimeContentMode(
  input,
  { contentMode, profileId = DEFAULT_SHELL_PROFILE_ID } = {},
) {
  const profile = resolveShellProfile(profileId),
    pin = profile.contentPin;
  if (
    !(input instanceof Uint8Array) ||
    input.length !== pin.size ||
    !["base-update-only", "warlock"].includes(contentMode)
  )
    fail();
  const bytes = input.slice();
  if ((await digest(bytes)) !== pin.originalSha256) fail();
  const offset = cacheOffset(bytes, pin);
  if (contentMode === "base-update-only") bytes.fill(0, offset, offset + 4);
  const sha256 = await digest(bytes);
  const expected = contentMode === "base-update-only" ? pin.noDlcSha256 : pin.originalSha256;
  if (sha256 !== expected) fail();
  return {
    bytes,
    receipt: Object.freeze({
      kind: "d2r-runtime-content-mode-v1",
      ...(profile.id === DEFAULT_SHELL_PROFILE_ID ? {} : { profileId: profile.id }),
      release: pin.release,
      path: pin.executablePath,
      contentMode,
      originalSha256: pin.originalSha256,
      sha256,
      size: bytes.length,
      modifiedOffset: contentMode === "base-update-only" ? offset : null,
      modifiedBytes: contentMode === "base-update-only" ? 4 : 0,
      signatureState:
        contentMode === "base-update-only" ? "invalidated-needs-signing" : "unchanged",
    }),
  };
}
