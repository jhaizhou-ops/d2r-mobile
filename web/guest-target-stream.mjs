// 从分块 Guest Mach-O 中检查目标系统，不复制完整可执行文件。
// Inspect the target OS of a chunked Guest Mach-O without copying the whole executable.

import { guestInfoPlist, MAX_GUEST_BYTES } from "./guest-framework.mjs";
import { resolveShellProfile } from "./shell-profiles.mjs";

/**
 * 仅保留 32 字节解析前缀；跳过命令正文，但仍耗尽并计数整个条目。
 * Retain only a 32-byte parsing prefix; skip command bodies, but drain and count
 * the complete entry. The caller retains ownership of all yielded chunks.
 */
export async function inspectGuestTarget(
  entry,
  { signal, expectedSdkVersion, minimumOSVersion } = {},
) {
  if (!entry || !Number.isSafeInteger(entry.size) || entry.size < 0 || entry.size > MAX_GUEST_BYTES)
    throw new Error("GUEST_MISSING");
  const prefix = new Uint8Array(32);
  const view = new DataView(prefix.buffer);
  const u = (offset) => view.getUint32(offset, true);
  const fail = () => {
    throw new Error("GUEST_TARGET");
  };
  let total = 0,
    have = 0,
    need = 32,
    state = "header",
    end = 0,
    commandCount = 0,
    commands = 0,
    commandOffset = 32,
    skip = 0,
    target;
  const nextCommand = () => {
    have = 0;
    if (commands === commandCount) {
      if (commandOffset !== end) fail();
      state = "body";
    } else {
      if (commandOffset + 8 > end) fail();
      state = "command";
      need = 8;
    }
  };
  try {
    signal?.throwIfAborted();
    for await (const chunk of entry.chunks()) {
      signal?.throwIfAborted();
      if (!(chunk instanceof Uint8Array) || chunk.length > entry.size - total)
        throw new Error("GUEST_SIZE");
      total += chunk.length;
      let at = 0;
      while (at < chunk.length && state !== "body") {
        if (state === "skip") {
          const length = Math.min(skip, chunk.length - at);
          at += length;
          skip -= length;
          if (skip === 0) nextCommand();
          continue;
        }
        const length = Math.min(need - have, chunk.length - at);
        prefix.set(chunk.subarray(at, at + length), have);
        have += length;
        at += length;
        if (have !== need) continue;
        if (state === "header") {
          if (u(0) !== 0xfeedfacf || u(4) !== 0x0100000c || u(12) !== 6) fail();
          end = 32 + u(20);
          commandCount = u(16);
          if (end > entry.size || commandCount > 4096) fail();
          nextCommand();
        } else if (state === "command") {
          const command = u(0),
            size = u(4);
          if (size < 8 || size % 8 || commandOffset + size > end) fail();
          commandOffset += size;
          commands++;
          // 旧版目标命令即使与新目标一致也会造成歧义；拒绝混合表示。
          // Legacy target commands are ambiguous even when they agree; reject mixed encodings.
          if ([0x24, 0x25, 0x2f, 0x30].includes(command)) fail();
          if (command === 0x32) {
            if (target || size < 24) fail();
            state = "build";
            need = 24;
            skip = size - 24;
          } else {
            skip = size - 8;
            state = "skip";
            if (skip === 0) nextCommand();
          }
        } else {
          if (u(8) !== 2 || skip !== u(20) * 8) fail();
          const version = (value) => `${value >>> 16}.${(value >>> 8) & 255}.${value & 255}`;
          target = {
            platform: "ios",
            minimumVersion: version(u(12)),
            sdkVersion: version(u(16)),
          };
          state = "skip";
          if (skip === 0) nextCommand();
        }
      }
    }
    signal?.throwIfAborted();
    if (total !== entry.size) throw new Error("GUEST_SIZE");
    if (state !== "body" || !target) fail();
    // 已解析的 plist 最低版本可省略补丁版本；不接受任意或畸形版本。
    // A parsed plist minimum may omit the patch version; arbitrary or malformed versions fail.
    const normalize = (version) => {
      if (typeof version !== "string" || !/^\d{1,5}(?:\.\d{1,3}){0,2}$/.test(version)) fail();
      const parts = version.split(".").map(Number);
      if (parts.some((part, i) => part > (i === 0 ? 65535 : 255))) fail();
      return [...parts, ...Array(3 - parts.length).fill(0)].join(".");
    };
    if (expectedSdkVersion !== undefined && normalize(expectedSdkVersion) !== target.sdkVersion)
      fail();
    if (minimumOSVersion !== undefined && normalize(minimumOSVersion) !== target.minimumVersion)
      fail();
    return target;
  } finally {
    prefix.fill(0);
  }
}

// 逐字节核对生成器的规范 plist，拒绝重复键、实体和任何未经验证的 XML。
// Compare the generator's canonical plist byte-for-byte, rejecting duplicate keys, entities and unverified XML.
export async function inspectGuestPlist(entry, { profileId, signal } = {}) {
  signal?.throwIfAborted();
  const { guestTarget } = resolveShellProfile(profileId);
  const expected = new TextEncoder().encode(guestInfoPlist(undefined, { profileId }));
  if (
    !entry ||
    !Number.isSafeInteger(entry.size) ||
    entry.size < 0 ||
    entry.size > 16 * 1024 ||
    entry.size !== expected.length
  )
    throw new Error("GUEST_PLIST");
  let total = 0;
  for await (const chunk of entry.chunks()) {
    signal?.throwIfAborted();
    if (!(chunk instanceof Uint8Array) || chunk.length > entry.size - total)
      throw new Error("GUEST_PLIST");
    for (const byte of chunk) if (byte !== expected[total++]) throw new Error("GUEST_PLIST");
  }
  signal?.throwIfAborted();
  if (total !== entry.size) throw new Error("GUEST_PLIST");
  return { minimumVersion: guestTarget.minimumVersion };
}
