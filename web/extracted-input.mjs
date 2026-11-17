// 独立的已提取输入来源：精确 NSO 摘要，有界资源读取，不伪造 CNMT。
// Independent extracted-input provenance: exact NSO digests, bounded resource reads, no invented CNMT.
import {
  PROGRAM_REWRITE_LIMITS,
  diagnoseRegisteredProgramDigests,
  findRegisteredProgramRelease,
} from "./program-release-catalog.mjs";
import { D2R_APPLICATION_ID } from "./d2r-expansion-policy.mjs";
import { indexFiles, REQUIRED_NSO_MODULES, safeRelativePath } from "./preflight.mjs";
import { validateResourceInventory } from "./resource-folder-export.mjs";
import { isSeparateDlcLayout } from "./game-input.mjs";
import { ROMFS_LIMITS } from "./romfs-tree.mjs";
const fail = (code) => {
  const error = new Error(code);
  error.code = code;
  throw error;
};
const sha256 = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (byte) =>
    byte.toString(16).padStart(2, "0"),
  ).join("");
function records(value, budget) {
  if (!Array.isArray(value) || !value.length || value.length > ROMFS_LIMITS.members)
    fail("EXTRACTED_INPUT_INVALID");
  budget.members += value.length;
  if (budget.members > ROMFS_LIMITS.members) fail("EXTRACTED_INPUT_INVALID");
  const encoder = new TextEncoder();
  for (const row of value) {
    if (typeof row?.path === "string" && isSeparateDlcLayout(row.path))
      fail("EXTRACTED_DLC_UNSUPPORTED");
    try {
      safeRelativePath(row?.path);
    } catch {
      fail("EXTRACTED_INPUT_INVALID");
    }
    if (
      !(row.file instanceof Blob) ||
      !Number.isSafeInteger(row.file.size) ||
      row.file.size < 0 ||
      /[<>"|?*\p{C}]/u.test(row.path)
    )
      fail("EXTRACTED_INPUT_INVALID");
    const bytes = encoder.encode(row.path).length;
    budget.paths += bytes;
    if (
      bytes > ROMFS_LIMITS.pathBytes ||
      budget.paths > ROMFS_LIMITS.totalPathBytes ||
      row.path.split("/").some((name) => encoder.encode(name).length > ROMFS_LIMITS.nameBytes)
    )
      fail("EXTRACTED_INPUT_INVALID");
  }
  try {
    indexFiles(
      value.map(({ path, file }) => ({ path, size: file.size })),
      { maxPathDepth: ROMFS_LIMITS.depth, maxPathNodes: ROMFS_LIMITS.members },
    );
  } catch {
    fail("EXTRACTED_INPUT_INVALID");
  }
  return new Map(value.map(({ path, file }) => [path, file]));
}
/**
 * 签发会话前快照本地 Blob 引用并测量可执行文件字节。不接受调用方提供的哈希、清单、内容 ID、DLC 标志或可信布尔值。本地 RomFS 没有 NCA 外层封装，现有 CASC 校验和可信目录仍须执行。
 * Snapshot local Blob references and measure executable bytes before issuing a session.
 * No caller-supplied hashes, manifests, content IDs, DLC flags or trusted booleans are accepted.
 * Local RomFS lacks the NCA envelope; existing CASC checks and trusted catalogs still apply.
 */
export async function openExtractedInput(
  inputs,
  {
    signal,
    contentMode = "optional-warlock",
    maxProgramBytes = PROGRAM_REWRITE_LIMITS.totalInputBytes,
  } = {},
) {
  if (inputs?.inputMode !== "extracted") fail("EXTRACTED_INPUT_INVALID");
  if (contentMode === "warlock") fail("EXTRACTED_DLC_UNSUPPORTED");
  if (!["optional-warlock", "base-update-only"].includes(contentMode))
    fail("EXTRACTED_INPUT_INVALID");
  if (
    !Number.isSafeInteger(maxProgramBytes) ||
    maxProgramBytes < 1 ||
    maxProgramBytes > PROGRAM_REWRITE_LIMITS.totalInputBytes
  )
    fail("EXTRACTED_INPUT_INVALID");
  signal?.throwIfAborted();
  const budget = { members: 0, paths: 0 };
  const exefs = records(inputs.exefs, budget),
    resources = records(inputs.romfs, budget);
  if (
    !REQUIRED_NSO_MODULES.every((name) => exefs.has(name)) ||
    !resources.has("data/build_manifest.psv")
  )
    fail("EXTRACTED_INPUT_INVALID");
  const modules = Object.create(null),
    digests = Object.create(null);
  const lifetime = new AbortController();
  let disposed = false;
  const check = (extra) => {
    if (disposed) fail("EXTRACTION_DISPOSED");
    signal?.throwIfAborted();
    extra?.throwIfAborted();
  };
  const dispose = () => {
    disposed = true;
    lifetime.abort();
    exefs.clear();
    resources.clear();
    for (const bytes of Object.values(modules)) bytes.fill(0);
  };
  const read = async (file, offset, count, extra) => {
    check(extra);
    if (
      typeof offset !== "bigint" ||
      offset < 0n ||
      !Number.isSafeInteger(count) ||
      count < 0 ||
      count > ROMFS_LIMITS.readBytes ||
      offset > BigInt(file.size) ||
      BigInt(count) > BigInt(file.size) - offset
    )
      fail("EXTRACTION_READ_BOUNDS");
    const combined = AbortSignal.any([lifetime.signal, signal, extra].filter(Boolean));
    // 取消立即撤销能力；稍后到达的独占缓冲区仍须擦除。
    // Cancellation revokes promptly; a late owned read buffer is still wiped.
    const pending = Promise.resolve().then(async () => {
      const bytes = new Uint8Array(
        await file.slice(Number(offset), Number(offset) + count).arrayBuffer(),
      );
      try {
        check(extra);
        if (bytes.length !== count) fail("EXTRACTED_INPUT_INVALID");
        return bytes;
      } catch (error) {
        bytes.fill(0);
        throw error;
      }
    });
    let abort;
    try {
      return await Promise.race([
        pending,
        new Promise((_, reject) => {
          abort = () => reject(new DOMException("Local input read cancelled.", "AbortError"));
          combined.addEventListener("abort", abort, { once: true });
          if (combined.aborted) abort();
        }),
      ]);
    } finally {
      combined.removeEventListener("abort", abort);
    }
  };
  try {
    let total = 0;
    for (const name of REQUIRED_NSO_MODULES) {
      const file = exefs.get(name);
      total += file.size;
      if (
        !file.size ||
        file.size > PROGRAM_REWRITE_LIMITS.moduleInputBytes ||
        total > maxProgramBytes
      )
        fail("EXTRACTION_PROGRAM_BUDGET");
    }
    for (const name of REQUIRED_NSO_MODULES) {
      const file = exefs.get(name),
        bytes = new Uint8Array(file.size);
      modules[name] = bytes;
      for (let offset = 0; offset < file.size; offset += ROMFS_LIMITS.readBytes) {
        const chunk = await read(
          file,
          BigInt(offset),
          Math.min(ROMFS_LIMITS.readBytes, file.size - offset),
        );
        try {
          bytes.set(chunk, offset);
        } finally {
          chunk.fill(0);
        }
      }
      digests[name] = await sha256(bytes);
      check();
    }
    const diagnosis = diagnoseRegisteredProgramDigests(digests);
    if (diagnosis.status !== "registered")
      fail(
        diagnosis.status === "incompatible-modules"
          ? "PROGRAM_MODULE_RELEASE_MISMATCH"
          : "PROGRAM_UNSUPPORTED_VERSION",
      );
    const release = findRegisteredProgramRelease(diagnosis.releaseId);
    if (!release || release.titleId !== D2R_APPLICATION_ID.toString(16).padStart(16, "0"))
      fail("PROGRAM_UNSUPPORTED_VERSION");
    const selection = Object.freeze({
      inputKind: "extracted",
      applicationId: D2R_APPLICATION_ID,
      programReleaseId: release.id,
      gameVersion: release.gameVersion,
      contentMode: "base-update-only",
      dlcMetadata: "not-authenticated",
      dlc: Object.freeze([]),
    });
    const romfs = Object.freeze({
      files: Object.freeze(
        [...resources].map(([path, file]) => Object.freeze({ path, size: BigInt(file.size) })),
      ),
      getFile(path) {
        check();
        const file = resources.get(path);
        return file
          ? Object.freeze({
              size: BigInt(file.size),
              readAt: (offset, count, options = {}) => read(file, offset, count, options.signal),
            })
          : undefined;
      },
    });
    try {
      validateResourceInventory(romfs);
    } catch {
      fail("EXTRACTED_RESOURCE_LAYOUT");
    }
    return Object.freeze({
      selection,
      programModules: Object.freeze(modules),
      romfs,
      check,
      dispose,
    });
  } catch (error) {
    dispose();
    throw error;
  }
}
