#!/usr/bin/env node
// 本地只读检查本应用生成的 IPA，不上传、不提取、不执行着色器。
// Read-only local validation of this application's IPA output; no upload, extraction or shader execution.
import { open } from "node:fs/promises";
import { resolve } from "node:path";
import process from "node:process";
import { pathToFileURL } from "node:url";
import { MAX_READ_BYTES, readRange } from "../preflight.mjs";
import { STAGED_ZIP_LIMITS, planStagedZip, validateStagedZip } from "../staged-zip-validator.mjs";
import { inspectCompiledMetalLibrary } from "../compiled-shader-boundary.mjs";

const MAX_LIBRARY_BYTES = 16 * 1024 * 1024;
const decoder = new TextDecoder("utf-8", { fatal: true });
const requireValue = (value, message = "Invalid app-generated IPA archive") => {
  if (!value) throw new Error(message);
};
const safe = (value) => {
  requireValue(value >= 0n && value <= BigInt(Number.MAX_SAFE_INTEGER));
  return Number(value);
};
const view = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
const counts = () => ({ total: 0, compiled: 0, sourceText: 0, invalid: 0 });

// 只派生有界清单；现有独立校验器随后针对清单检查每条本地/中央记录、数据描述符、CRC 及归档字节。
// Derive only the bounded manifest. The existing independent validator subsequently checks
// every local/central record, data descriptor, CRC and archive byte against that manifest.
async function readManifest(source, signal) {
  requireValue(source.size >= 22);
  const end = view(await readRange(source, source.size - 22, 22, signal));
  requireValue(
    end.getUint32(0, true) === 0x06054b50 && end.getUint16(20, true) === 0,
    "Expected an app-generated IPA without ZIP comments; repacked archives are unsupported",
  );
  let count = end.getUint16(10, true);
  let centralBytes = end.getUint32(12, true);
  let centralOffset = end.getUint32(16, true);
  const zip64 = count === 0xffff || centralBytes === 0xffffffff || centralOffset === 0xffffffff;
  let centralEnd = source.size - 22;
  if (zip64) {
    requireValue(source.size >= 98);
    const footer = view(await readRange(source, source.size - 98, 76, signal));
    requireValue(
      footer.getUint32(0, true) === 0x06064b50 &&
        footer.getBigUint64(4, true) === 44n &&
        footer.getUint32(56, true) === 0x07064b50,
    );
    count = safe(footer.getBigUint64(32, true));
    centralBytes = safe(footer.getBigUint64(40, true));
    centralOffset = safe(footer.getBigUint64(48, true));
    centralEnd = source.size - 98;
  }
  requireValue(
    count > 0 &&
      count <= STAGED_ZIP_LIMITS.maxEntries &&
      centralBytes <= STAGED_ZIP_LIMITS.maxMetadataBytes &&
      centralOffset <= centralEnd &&
      centralBytes === centralEnd - centralOffset,
  );
  const manifest = [];
  let cursor = centralOffset,
    appRoot;
  for (let index = 0; index < count; index++) {
    requireValue(cursor + 46 <= centralEnd);
    const header = view(await readRange(source, cursor, 46, signal));
    requireValue(header.getUint32(0, true) === 0x02014b50);
    requireValue(
      header.getUint16(10, true) === 0,
      "Only this application's STORE ZIP32/ZIP64 IPA output is supported; do not recompress it",
    );
    const nameLength = header.getUint16(28, true),
      extraLength = header.getUint16(30, true);
    requireValue(
      nameLength > 0 &&
        nameLength <= STAGED_ZIP_LIMITS.maxPathBytes &&
        extraLength === (zip64 ? 28 : 0) &&
        header.getUint16(32, true) === 0 &&
        cursor + 46 + nameLength + extraLength <= centralEnd,
    );
    const variable = await readRange(source, cursor + 46, nameLength + extraLength, signal);
    const path = decoder.decode(variable.subarray(0, nameLength));
    const match = /^Payload\/[^/]+\.app\//.exec(path);
    requireValue(match, "Expected files inside one Payload/*.app bundle");
    appRoot ??= match[0];
    requireValue(appRoot === match[0], "Multiple app bundles are unsupported");
    let size = header.getUint32(24, true);
    if (zip64) {
      const extra = view(variable.subarray(nameLength));
      requireValue(extra.getUint16(0, true) === 1 && extra.getUint16(2, true) === 24);
      size = safe(extra.getBigUint64(4, true));
    }
    manifest.push({ path, size, mode: header.getUint32(38, true) >>> 16 });
    cursor += 46 + nameLength + extraLength;
  }
  requireValue(
    cursor === centralEnd && manifest.some((entry) => entry.path === appRoot + "Info.plist"),
  );
  return { manifest, archiveFormat: zip64 ? "zip64" : "zip32" };
}

// 这些字段只是自述的结构预期，不是可信编译器来源；下方独立检查所有偏移、标签、元数据段及内嵌哈希。
// These fields are self-described structural expectations, not trusted compiler provenance.
// All offsets, tags, metadata sections and embedded hashes are independently checked below.
function declaredMetalIdentity(bytes) {
  requireValue(bytes.length >= 88, "Truncated MTLB header");
  const header = view(bytes);
  const start = safe(header.getBigUint64(24, true));
  const length = safe(header.getBigUint64(32, true)) + 4;
  requireValue(start >= 88 && length >= 12 && start <= bytes.length - length);
  requireValue(header.getUint32(start, true) > 0);
  const groupLength = header.getUint32(start + 4, true);
  requireValue(groupLength >= 8 && groupLength <= length - 4);
  const end = start + 4 + groupLength,
    fields = new Map();
  let cursor = start + 8;
  while (cursor < end) {
    requireValue(cursor + 4 <= end);
    const name = String.fromCharCode(...bytes.subarray(cursor, cursor + 4));
    cursor += 4;
    if (name === "ENDT") {
      requireValue(cursor === end);
      break;
    }
    requireValue(cursor + 2 <= end && !fields.has(name));
    const size = header.getUint16(cursor, true);
    cursor += 2;
    requireValue(cursor + size <= end);
    fields.set(name, bytes.subarray(cursor, cursor + size));
    cursor += size;
  }
  const name = fields.get("NAME"),
    type = fields.get("TYPE"),
    versions = fields.get("VERS");
  requireValue(
    name?.length >= 2 &&
      name.length <= 1024 &&
      name.at(-1) === 0 &&
      type?.length === 1 &&
      type[0] <= 2 &&
      versions?.length === 8,
  );
  const version = view(versions);
  return {
    entry: decoder.decode(name.subarray(0, -1)),
    stage: ["vertex", "fragment", "kernel"][type[0]],
    minimumOS: [header.getUint16(12, true), bytes[14]],
    languageVersion: [version.getUint16(4, true), version.getUint16(6, true)],
    metadataLengthIncludesPrefix: false,
  };
}
function looksLikeSource(bytes) {
  if (!bytes.length) return false;
  try {
    // 解码有界前缀，允许末尾 UTF-8 字符不完整。
    // Decode a bounded prefix, allowing a partial trailing UTF-8 character.
    const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(0, 4096), {
      stream: true,
    });
    return (
      !!text.trim() &&
      [...text].every((character) => {
        const point = character.codePointAt(0);
        return (point >= 32 && point !== 127) || [9, 10, 13].includes(point);
      })
    );
  } catch {
    return false;
  }
}

export async function validateIpaShaderFile(filename, { expectedCount, signal, onProgress } = {}) {
  requireValue(
    expectedCount === undefined ||
      (Number.isSafeInteger(expectedCount) &&
        expectedCount >= 1 &&
        expectedCount <= STAGED_ZIP_LIMITS.maxEntries),
    "Invalid expected game-library count",
  );
  const file = await open(filename, "r");
  try {
    const before = await file.stat({ bigint: true });
    requireValue(before.isFile(), "Expected a regular IPA file");
    const size = safe(before.size);
    const source = {
      size,
      async read(offset, length) {
        signal?.throwIfAborted();
        requireValue(
          Number.isSafeInteger(offset) &&
            Number.isSafeInteger(length) &&
            offset >= 0 &&
            length >= 0 &&
            length <= MAX_READ_BYTES &&
            offset <= size - length,
        );
        const bytes = new Uint8Array(length);
        let consumed = 0;
        while (consumed < length) {
          const result = await file.read(bytes, consumed, length - consumed, offset + consumed);
          requireValue(result.bytesRead > 0, "IPA changed or was truncated while reading");
          consumed += result.bytesRead;
        }
        signal?.throwIfAborted();
        return bytes;
      },
    };
    const { manifest, archiveFormat } = await readManifest(source, signal);
    const options = { archiveFormat, maxOutputBytes: size, signal };
    onProgress?.({ phase: "archive-integrity", entries: manifest.length });
    await validateStagedZip(source, manifest, options);
    const plan = planStagedZip(manifest, options);
    const report = {
      ok: false,
      validation: "zip-crc-and-metallib-container-integrity-only",
      archiveFormat,
      archiveBytes: size,
      archiveEntries: manifest.length,
      libraries: counts(),
      gameLibraries: counts(),
      expectedGameLibraries: expectedCount ?? null,
      issueCount: 0,
      issues: [],
    };
    const issue = (path, code) => {
      report.issueCount++;
      if (report.issues.length < 20) report.issues.push({ path, code });
    };
    const entries = plan.entries.filter((entry) => /\.metallib$/i.test(entry.path));
    for (const [index, entry] of entries.entries()) {
      signal?.throwIfAborted();
      const game = /\/msl\/b-[^/]+\/[^/]+\.metallib$/i.test(entry.path);
      const counters = game ? [report.libraries, report.gameLibraries] : [report.libraries];
      for (const counter of counters) counter.total++;
      let bytes;
      let classification = "invalid";
      try {
        requireValue(entry.size <= MAX_LIBRARY_BYTES, "Library exceeds 16 MiB inspection limit");
        bytes = new Uint8Array(entry.size);
        const offset = entry.offset + (archiveFormat === "zip64" ? 50 : 30) + entry.name.length;
        for (let at = 0; at < bytes.length; at += MAX_READ_BYTES) {
          bytes.set(
            await readRange(
              source,
              offset + at,
              Math.min(MAX_READ_BYTES, bytes.length - at),
              signal,
            ),
            at,
          );
        }
        if (bytes.length < 4 || String.fromCharCode(...bytes.subarray(0, 4)) !== "MTLB") {
          classification = looksLikeSource(bytes) ? "sourceText" : "invalid";
          issue(
            entry.path,
            classification === "sourceText" ? "SOURCE_TEXT_NOT_COMPILED" : "NOT_MTLB",
          );
        } else {
          const identity = declaredMetalIdentity(bytes);
          if (game)
            requireValue(
              identity.minimumOS.join(".") === "18.0" &&
                identity.languageVersion.join(".") === "3.2",
              "Game library target/profile mismatch",
            );
          const inspection = await inspectCompiledMetalLibrary(bytes, { ...identity, signal });
          requireValue(inspection.functions.every((item) => item.type >= 0 && item.type <= 2));
          classification = "compiled";
        }
      } catch {
        signal?.throwIfAborted();
        issue(
          entry.path,
          entry.size > MAX_LIBRARY_BYTES ? "LIBRARY_SIZE_LIMIT" : "INVALID_COMPILED_LIBRARY",
        );
      } finally {
        bytes?.fill(0);
      }
      for (const counter of counters) counter[classification]++;
      if (index % 256 === 0 || index + 1 === entries.length)
        onProgress?.({ phase: "libraries", checked: index + 1, total: entries.length });
    }
    if (!report.gameLibraries.total) issue(null, "NO_GAME_SHADER_LIBRARIES");
    if (expectedCount !== undefined && report.gameLibraries.compiled !== expectedCount)
      issue(null, "EXPECTED_GAME_LIBRARY_COUNT_MISMATCH");
    const after = await file.stat({ bigint: true });
    requireValue(
      before.size === after.size &&
        before.mtimeNs === after.mtimeNs &&
        before.ctimeNs === after.ctimeNs,
      "IPA changed during validation; retry with a completed, unchanged file",
    );
    report.ok = report.issueCount === 0;
    return report;
  } finally {
    await file.close();
  }
}

async function main(args) {
  if (args.length === 0 || args.includes("--help")) {
    console.log(
      "Usage: node web/scripts/validate-ipa-shaders.mjs OUTPUT.ipa [--expected-count 7212]\n" +
        "Bun is also supported. Read-only, local STORE ZIP32/ZIP64 inspection.\n" +
        "7212 is optional reference data, not a universal count.\n" +
        "用法同上；仅本地只读检查本应用生成的 IPA，可用 bun 替代 node。\n" +
        "只验证归档 CRC、MTLB 结构及内置摘要，不证明语义、Xcode 等价或设备兼容。",
    );
    return;
  }
  requireValue(
    args.length === 1 || (args.length === 3 && args[1] === "--expected-count"),
    "Use OUTPUT.ipa [--expected-count N], or --help",
  );
  const expectedCount = args.length === 3 && /^\d+$/.test(args[2]) ? Number(args[2]) : undefined;
  requireValue(
    args.length !== 3 || expectedCount !== undefined,
    "Expected count must be a positive integer",
  );
  const controller = new AbortController();
  const cancel = () => controller.abort(new Error("Validation cancelled"));
  process.once("SIGINT", cancel);
  try {
    const report = await validateIpaShaderFile(args[0], {
      expectedCount,
      signal: controller.signal,
      onProgress(progress) {
        if (progress.phase === "archive-integrity")
          console.error("Checking archive layout and CRCs / 正在检查归档布局与 CRC…");
        else console.error(`Shader libraries / 着色器库: ${progress.checked}/${progress.total}`);
      },
    });
    console.log(JSON.stringify(report, null, 2));
    process.exitCode = report.ok ? 0 : 1;
  } finally {
    process.removeListener("SIGINT", cancel);
  }
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main(process.argv.slice(2)).catch((error) => {
    console.error(`Validation failed / 检查失败: ${error?.message ?? "Unknown error"}`);
    process.exitCode = 1;
  });
}
