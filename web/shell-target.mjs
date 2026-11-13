// 从已验证的单架构 arm64 可执行字节读取 iOS 部署目标。
// 依据 Mach-O 部署版本加载命令。
// Reads the iOS deployment target from verified thin arm64 executable bytes.
// Based on Mach-O deployment-version load commands.

/**
 * 从已验证的单架构 arm64 可执行字节读取 iOS 部署目标。
 * Observe iOS deployment target from verified thin arm64 executable bytes.
 */
export function inspectMachOTarget(bytes) {
  const fail = () => {
    throw new Error("SHELL_MACHO_TARGET_UNSUPPORTED");
  };
  if (!(bytes instanceof Uint8Array) || bytes.length < 32 || bytes.length > 64 * 1024 * 1024)
    fail();
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u32 = (offset) => view.getUint32(offset, true);
  if (u32(0) !== 0xfeedfacf || u32(4) !== 0x0100000c || u32(12) !== 2) fail();
  const count = u32(16),
    size = u32(20),
    end = 32 + size;
  if (!count || count > 4096 || size > 1024 * 1024 || end > bytes.length) fail();
  let offset = 32,
    target;
  for (let i = 0; i < count; i++) {
    if (offset + 8 > end) fail();
    const command = u32(offset),
      length = u32(offset + 4);
    if (length < 8 || length % 8 || offset + length > end) fail();
    // 旧版本命令与现代命令同时存在时，此处视为有歧义。
    // Legacy version commands alongside a modern command are ambiguous here.
    if ([0x24, 0x25, 0x2f, 0x30].includes(command)) fail();
    if (command === 0x32) {
      if (target || length < 24 || u32(offset + 8) !== 2) fail();
      const minimum = u32(offset + 12),
        tools = u32(offset + 20);
      if (minimum >>> 16 === 0 || tools > 128 || length !== 24 + 8 * tools) fail();
      target = Object.freeze({
        platform: "ios",
        minimumVersion: `${minimum >>> 16}.${(minimum >>> 8) & 255}.${minimum & 255}`,
      });
    }
    offset += length;
  }
  if (offset !== end || !target) fail();
  return target;
}

/**
 * executablePath 由可信发布注册表提供，而非未经验证的 ZIP 文本。
 * executablePath is supplied by the trusted release registry, not unverified ZIP text.
 */
export function createShellTargetInspector({ executablePath } = {}) {
  if (
    typeof executablePath !== "string" ||
    !/^[A-Za-z0-9_.-]{1,255}$/.test(executablePath) ||
    [".", ".."].includes(executablePath)
  )
    throw new Error("SHELL_EXECUTABLE_PATH_INVALID");
  return async ({ base, entries, readEntry, signal }) => {
    signal?.throwIfAborted();
    const path = base + executablePath;
    const entry = entries.find((entry) => entry.path === path);
    if (!entry || entry.directory || (entry.mode & 0o111) === 0 || typeof readEntry !== "function")
      throw new Error("SHELL_EXECUTABLE_MISSING");
    const bytes = await readEntry(path);
    signal?.throwIfAborted();
    return inspectMachOTarget(bytes);
  };
}
