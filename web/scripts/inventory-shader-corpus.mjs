// 本地诊断工具，不解包、不编译，也不认证输入来源。
// Local diagnostics only: no extraction, compilation or input authentication.
import { constants } from "node:fs";
import { lstat, open, opendir, realpath } from "node:fs/promises";
import { createHash } from "node:crypto";
import { resolve, join } from "node:path";
import { pathToFileURL } from "node:url";
import { createShaderFeatureInventory } from "../shader-feature-inventory.mjs";

const MAX_FILES = 25_000;
const MAX_SOURCE_BYTES = 8 * 1024 * 1024;
const MAX_JSON_BYTES = 1024 * 1024;
const MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024;
const STAGES = new Set(["vs", "ps", "cs", "hs", "ds"]);
const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const hash = (bytes) => createHash("sha256").update(bytes).digest();
function fail(code) {
  const error = new Error(code);
  error.code = code;
  throw error;
}
function sameFile(a, b) {
  return a.dev === b.dev && a.ino === b.ino;
}

/**
 * 仅读取指定的、已解压的单个 msl/b-* 目录。报告永远不是外壳信任证明。
 * Read only one selected, already-extracted msl/b-* directory. Reports are never shell trust proofs.
 */
export async function inventoryShaderCorpus(folder, { expectedDigest, expectedCount } = {}) {
  try {
    if (
      typeof folder !== "string" ||
      !folder ||
      (expectedDigest === undefined) !== (expectedCount === undefined) ||
      (expectedDigest !== undefined &&
        (!/^[0-9a-f]{16}$/.test(expectedDigest) ||
          !Number.isSafeInteger(expectedCount) ||
          expectedCount < 1 ||
          expectedCount > MAX_FILES))
    )
      fail("CORPUS_INVALID_ARGUMENTS");
    const root = resolve(folder);
    const rootStat = await lstat(root);
    if (!rootStat.isDirectory() || rootStat.isSymbolicLink()) fail("CORPUS_INVALID_DIRECTORY");
    const canonicalRoot = await realpath(root);
    const records = new Map();
    let files = 0,
      totalBytes = 0;
    const directory = await opendir(canonicalRoot);
    for await (const entry of directory) {
      if (++files > MAX_FILES) fail("CORPUS_FILE_LIMIT");
      if (!entry.isFile() || entry.isSymbolicLink()) fail("CORPUS_NONREGULAR_ENTRY");
      const match = /^([0-9a-f]{16})_(b\.json|b\.metallib|bf\.metallib)$/.exec(entry.name);
      if (!match) fail("CORPUS_UNEXPECTED_ENTRY");
      const record = records.get(match[1]) ?? {};
      record[match[2]] = entry.name;
      records.set(match[1], record);
    }
    if (!records.size) fail("CORPUS_EMPTY");
    for (const record of records.values())
      if (!record["b.json"] || !record["b.metallib"]) fail("CORPUS_MISSING_COMPANION");
    async function readBounded(name, limit) {
      const currentRoot = await lstat(canonicalRoot);
      if (
        !currentRoot.isDirectory() ||
        currentRoot.isSymbolicLink() ||
        !sameFile(currentRoot, rootStat)
      )
        fail("CORPUS_DIRECTORY_CHANGED");
      const path = join(canonicalRoot, name);
      const before = await lstat(path);
      if (!before.isFile() || before.isSymbolicLink()) fail("CORPUS_NONREGULAR_ENTRY");
      if (before.size > limit || before.size < 1) fail("CORPUS_FILE_SIZE");
      const file = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
      try {
        const stat = await file.stat();
        if (!stat.isFile() || !sameFile(before, stat) || stat.size !== before.size)
          fail("CORPUS_FILE_CHANGED");
        if (stat.size > MAX_TOTAL_BYTES - totalBytes) fail("CORPUS_TOTAL_SIZE");
        totalBytes += stat.size;
        const bytes = Buffer.alloc(stat.size);
        let offset = 0;
        while (offset < bytes.length) {
          const { bytesRead } = await file.read(bytes, offset, bytes.length - offset, offset);
          if (!bytesRead) fail("CORPUS_FILE_CHANGED");
          offset += bytesRead;
        }
        const after = await file.stat();
        if (
          after.size !== stat.size ||
          after.mtimeMs !== stat.mtimeMs ||
          after.ctimeMs !== stat.ctimeMs
        )
          fail("CORPUS_FILE_CHANGED");
        return bytes;
      } finally {
        await file.close();
      }
    }
    const inventory = createShaderFeatureInventory();
    const digest = createHash("sha256");
    const uniqueHashes = new Set();
    let fetchCount = 0;
    for (const key of [...records.keys()].sort()) {
      const record = records.get(key);
      let metadata;
      try {
        metadata = JSON.parse(decoder.decode(await readBounded(record["b.json"], MAX_JSON_BYTES)));
      } catch (error) {
        if (error?.code?.startsWith("CORPUS_")) throw error;
        fail("CORPUS_INVALID_METADATA");
      }
      if (
        !metadata ||
        typeof metadata !== "object" ||
        Array.isArray(metadata) ||
        !STAGES.has(metadata.stage)
      )
        fail("CORPUS_INVALID_METADATA");
      if (metadata.key !== undefined && metadata.key !== `${key}_b`)
        fail("CORPUS_METADATA_KEY_MISMATCH");
      if (record["bf.metallib"] && metadata.stage !== "ps") fail("CORPUS_INVALID_VARIANT_STAGE");
      for (const [suffix, variant] of [
        ["b.metallib", "base"],
        ["bf.metallib", "readableTile"],
      ]) {
        if (!record[suffix]) continue;
        const bytes = await readBounded(record[suffix], MAX_SOURCE_BYTES);
        if (bytes.length >= 4 && bytes.subarray(0, 4).toString("ascii") === "MTLB")
          fail("CORPUS_COMPILED_BINARY");
        let source;
        try {
          source = decoder.decode(bytes);
        } catch {
          fail("CORPUS_INVALID_UTF8");
        }
        if (source.includes("\0")) fail("CORPUS_INVALID_SOURCE");
        source = source.replaceAll("\r\n", "\n");
        const sourceHash = hash(Buffer.from(source, "utf8"));
        uniqueHashes.add(sourceHash.toString("hex"));
        if (variant === "base") {
          // 与 d2rm_pack.py 相同：排序的 *_b.metal 文件名加源码摘要原始字节。
          // Match d2rm_pack.py: sorted *_b.metal names followed by raw source-hash bytes.
          digest.update(`${key}_b.metal`, "utf8");
          digest.update(sourceHash);
        } else fetchCount++;
        inventory.add(source, { stage: metadata.stage, variant });
      }
    }
    const sourceDigest = digest.digest("hex").slice(0, 16);
    const expectation =
      expectedDigest === undefined
        ? "not-checked"
        : sourceDigest === expectedDigest && records.size === expectedCount
          ? "matches"
          : "mismatch";
    return {
      schema: 1,
      purpose: "untrusted-local-diagnostic",
      authenticated: false,
      sourceDigest,
      baseCount: records.size,
      fetchCount,
      uniqueNormalizedSourceHashCount: uniqueHashes.size,
      expectation,
      inventory: inventory.snapshot(),
    };
  } catch (error) {
    // 原始文件系统错误包含所选路径；只暴露固定安全代码。
    // Raw filesystem errors contain selected paths; expose only fixed safe codes.
    const code = error?.code;
    if (typeof code === "string" && /^(?:CORPUS|MSL_INVENTORY)_[A-Z0-9_]+$/.test(code)) throw error;
    fail("CORPUS_READ_FAILED");
  }
}

async function main(args) {
  if (args.length !== 1 && args.length !== 5) fail("CORPUS_INVALID_ARGUMENTS");
  const options = {};
  if (args.length === 5) {
    if (
      args[1] !== "--expected-digest" ||
      args[3] !== "--expected-count" ||
      !/^[1-9]\d*$/.test(args[4])
    )
      fail("CORPUS_INVALID_ARGUMENTS");
    options.expectedDigest = args[2];
    options.expectedCount = Number(args[4]);
  }
  const result = await inventoryShaderCorpus(args[0], options);
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  if (result.expectation === "mismatch") process.exitCode = 2;
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main(process.argv.slice(2)).catch((error) => {
    process.stderr.write(
      `${/^(?:CORPUS|MSL_INVENTORY)_[A-Z0-9_]+$/.test(error?.code) ? error.code : "CORPUS_READ_FAILED"}\n`,
    );
    process.exitCode = 1;
  });
}
