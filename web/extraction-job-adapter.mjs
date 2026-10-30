// 依据提取工作线程协议与已认证结果，将提取能力接入打包任务。
// Connect extraction capabilities to packaging jobs using the extraction worker protocol and authenticated results.
import { openExtractedInput } from "./extracted-input.mjs";
import { resolveContentSelection } from "./content-selection.mjs";
import { isExtractionDiagnosticCode } from "./extraction-errors.mjs";
import {
  createExtractionWorkerClient,
  ExtractionClientError,
} from "./extraction-worker-client.mjs";
import { findUniqueLogicalPathSuffixes } from "./logical-asset-suffixes.mjs";
import { indexRomfs } from "./romfs-tree.mjs";
import { createCascLogicalAssets, createCascAstcRecords } from "./casc-logical-reader.mjs";
import { REQUIRED_NSO_MODULES } from "./preflight.mjs";
import { resolveD2rExpansionSelection, D2R_CONTENT_MODES } from "./d2r-expansion-policy.mjs";

export const EXTRACTION_ADAPTER_CAPABILITIES = Object.freeze({
  authenticatedSelection: true,
  inputModes: Object.freeze(["packages", "extracted"]),
  extractedDlcMetadataAuthenticated: false,
  lazyPlaintextReaders: true,
  requiresArchiveSecretLifetime: true,
  productionConversion: false,
  dlcExtraction: false,
  // 资料片是否必需由内容模式决定；生产配置允许仅基础包和更新。
  // Expansion requirements depend on content mode; production allows base plus update only.
  defaultContentMode: "warlock",
  contentModes: D2R_CONTENT_MODES,
  offlineRuntimeProven: false,
});
const fail = (code) => {
  throw new ExtractionClientError(code);
};
// 异步读取完成后来源可能已撤销；拒绝交付时清零本次独占的明文字节。
// An async read may finish after revocation; wipe its owned plaintext if delivery is rejected.
function checkedReadResult(bytes, check) {
  try {
    check();
    return bytes;
  } catch (error) {
    bytes.fill(0);
    throw error;
  }
}
const canonical = (value) =>
  JSON.stringify(structuredClone(value), (_, child) => {
    if (typeof child === "bigint") return `${child}n`;
    if (child && typeof child === "object" && !Array.isArray(child))
      return Object.fromEntries(
        Object.keys(child)
          .sort()
          .map((key) => [key, child[key]]),
      );
    return child;
  });
// 注册仅属于已完成的认证提取。复制的结果、
// 调用方布尔值或调用方提供的元数据都不能建立来源证明。
// Registration is private to authenticated extraction or the separately measured local-folder route. A copied result,
// a caller boolean, or caller-supplied metadata can never create provenance.
const authenticatedPrograms = new WeakMap();
/**
 * 受信任的生成器接线注入此验证器；独立密封的程序运行时无需导入提取模块。重写前必须复制借用字节，并按精确且受信任的整文件摘要进行验证。
 * Trusted generator wiring injects this verifier; no extraction imports are
 * required in a separately sealed program runtime. Borrowed bytes MUST be copied
 * and verified against an exact trusted full-file digest before rewriting.
 */
export function readAuthenticatedProgramModules(extracted, selection, { signal } = {}) {
  if (signal?.aborted) fail("EXTRACTION_CANCELLED");
  try {
    const proof = authenticatedPrograms.get(extracted);
    if (!proof || proof.selection !== canonical(selection)) fail("EXTRACTION_PROVENANCE_REJECTED");
    proof.check();
    return proof.modules;
  } catch {
    fail("EXTRACTION_PROVENANCE_REJECTED");
  }
}

/**
 * 已认证且仍有效的资源能力。调用方持有的副本或信任标志都不能注册来源；选择绑定到同一个已验证提取结果。
 * Source-bound, live asset capabilities. Package inputs authenticate NCAs; local
 * folders have measured module provenance, not a signed RomFS envelope. No copy or trust flag can
 * register provenance; selection is bound to the same verified extraction result.
 */
export function readAuthenticatedAssets(extracted, selection, { signal } = {}) {
  if (signal?.aborted) fail("EXTRACTION_CANCELLED");
  try {
    const proof = authenticatedPrograms.get(extracted);
    if (!proof || proof.selection !== canonical(selection)) fail("EXTRACTION_PROVENANCE_REJECTED");
    proof.check();
    return proof.assets;
  } catch {
    fail("EXTRACTION_PROVENANCE_REJECTED");
  }
}

// 内容模式绑定到私有来源登记；文件夹模式并不证明 DLC 不存在。
// Bind runtime mode to private source registration; folder mode does not prove DLC absence.
export function readInputContentPolicy(extracted, selection, options) {
  readAuthenticatedAssets(extracted, selection, options);
  return authenticatedPrograms.get(extracted).inputContentPolicy;
}

// 只有认证结果才能铸造记录令牌；令牌副本或清单行不能重新建立来源证明。
// Only authenticated results mint record tokens; token copies or manifest rows cannot recreate provenance.
const authenticatedAstcRecords = new WeakMap();
export async function enumerateAuthenticatedAstcRecords(extracted, selection, { signal } = {}) {
  const assets = readAuthenticatedAssets(extracted, selection, { signal });
  const proof = authenticatedPrograms.get(extracted);
  const records = await createCascAstcRecords(assets.romfs, { signal });
  proof.check();
  signal?.throwIfAborted();
  const textures = [];
  let descriptor = null;
  for (const path of records.entries()) {
    const token = Object.freeze({ path });
    authenticatedAstcRecords.set(token, { records, path, check: proof.check, signal });
    if (path === "data/hd/texture_desc_cache.json") descriptor = token;
    else textures.push(token);
  }
  return Object.freeze({ textures: Object.freeze(textures), descriptor });
}
export function openAuthenticatedAstcRecord(token, { signal } = {}) {
  if (signal?.aborted) fail("EXTRACTION_CANCELLED");
  const proof = authenticatedAstcRecords.get(token);
  if (!proof) fail("EXTRACTION_PROVENANCE_REJECTED");
  proof.check();
  proof.signal?.throwIfAborted();
  const opened = proof.records.open(proof.path, { signal });
  return Object.freeze({
    record: opened.record,
    source: Object.freeze({
      size: opened.source.size,
      async readAt(offset, count, options) {
        proof.check();
        const bytes = await opened.source.readAt(offset, count, options);
        return checkedReadResult(bytes, () => proof.check());
      },
    }),
  });
}

async function finishExtracted(programModules, romfs, selection, check, signal) {
  // 在任何异步初始化前，只捕获内部签发的来源凭据。
  // Capture only internally issued provenance before any asynchronous initialization.
  const selectionKey = canonical(selection);
  const inputContentPolicy = Object.freeze(
    selection.inputKind === "extracted"
      ? {
          contentMode: "base-update-only",
          inputProvenance: "local-extracted",
          dlcMetadata: "not-authenticated",
        }
      : {
          contentMode: selection.dlc.length ? "warlock" : "base-update-only",
          inputProvenance: "authenticated-package",
          dlcMetadata: "authenticated",
        },
  );
  const logical = await createCascLogicalAssets(romfs, { signal });
  const logicalAssets = Object.freeze({
    lookup(path) {
      check();
      return logical.lookup(path);
    },
    itemSpriteManifest() {
      check();
      const lines = [];
      let size = 0;
      for (const row of logical.entries()) {
        check();
        const path = row[9].toLowerCase();
        if (!/^data:data\/hd\/global\/ui\/items\/.*\.sprite$/.test(path)) continue;
        const line = `${"|".repeat(9)}${path}|\n`;
        size += new TextEncoder().encode(line).length;
        if (size > 4 * 1024 * 1024) throw new Error("EXTRACTION_ITEM_MANIFEST_LIMIT");
        lines.push(line);
      }
      return lines.join("");
    },
    findUniquePathSuffixes(suffixes) {
      return findUniqueLogicalPathSuffixes(logical, suffixes, check);
    },
    findUniquePathSuffix(suffix) {
      check();
      if (
        typeof suffix !== "string" ||
        suffix.length > 512 ||
        !/^\/[a-z0-9_/.-]+$/.test(suffix) ||
        suffix.split("/").some((part) => part === "." || part === "..")
      )
        throw new Error("EXTRACTION_ASSET_SUFFIX");
      let match;
      for (const row of logical.entries()) {
        check();
        if (!row[9].toLowerCase().endsWith(suffix)) continue;
        if (match !== undefined || logical.hasDuplicatePath(row[9]))
          throw new Error("EXTRACTION_ASSET_AMBIGUOUS");
        match = row[9];
      }
      if (match === undefined) throw new Error("EXTRACTION_ASSET_MISSING");
      return match;
    },
    async read(path, options) {
      check(options?.signal);
      const bytes = await logical.read(path, options);
      return checkedReadResult(bytes, () => check(options?.signal));
    },
  });
  const extracted = Object.freeze({
    programModules: Object.freeze(programModules),
    romfs,
    logicalAssets,
  });
  authenticatedPrograms.set(extracted, {
    selection: selectionKey,
    inputContentPolicy,
    modules: extracted.programModules,
    assets: Object.freeze({ romfs, logicalAssets }),
    check: () => check(),
  });
  return extracted;
}

/**
 * 只有受信任的应用代码提供 createWorker。将这三个任务方法与独立受信任的生成器和输出适配器组合，组合清理逻辑，并显式运行 runPackagingJob({secretLifetime:"archive", ...})。不提供工作线程 URL、真实构建允许列表、UI 启用或网络回退。
 * Only trusted application code supplies createWorker. Combine these three job
 * methods with independently trusted generators/output adapters, compose cleanup,
 * and explicitly runPackagingJob({secretLifetime:"archive", ...}).
 * No worker URL, real build allowlist, UI enablement or network fallback is provided.
 */
export function createExtractionJobAdapter({
  createWorker,
  contentMode = "warlock",
  verifyRomfs = true,
  maxProgramBytes = 32 * 1024 * 1024,
  timeoutMs,
  operationTimeoutMs,
  maxQueue,
} = {}) {
  if (
    typeof createWorker !== "function" ||
    !D2R_CONTENT_MODES.includes(contentMode) ||
    typeof verifyRomfs !== "boolean" ||
    !Number.isSafeInteger(maxProgramBytes) ||
    maxProgramBytes < 1 ||
    maxProgramBytes > 64 * 1024 * 1024
  )
    fail("EXTRACTION_ADAPTER_OPTIONS");
  const sessions = new WeakMap();
  const owned = [];
  let used = false,
    current,
    cleaned = false;
  const localSessions = [];
  function lookup(session) {
    const state = sessions.get(session);
    if (!state || state.closed || cleaned) fail("EXTRACTION_DISPOSED");
    return state;
  }
  async function cleanup() {
    cleaned = true;
    try {
      if (current) await current.dispose();
    } finally {
      for (const local of localSessions.splice(0)) local.dispose();
      for (const bytes of owned.splice(0)) bytes.fill(0);
    }
  }
  return Object.freeze({
    async openSecretSession({ inputs, signal, onDiagnostic } = {}) {
      if (used || cleaned) fail("EXTRACTION_SINGLE_USE");
      used = true;
      signal?.throwIfAborted();
      if (inputs?.inputMode === "extracted") {
        const state = {
          closed: false,
          extracted: false,
          local: null,
          inspected: null,
          extractionSignal: null,
          abort: null,
        };
        const openingLifetime = new AbortController();
        const localSignal = signal
          ? AbortSignal.any([signal, openingLifetime.signal])
          : openingLifetime.signal;
        const abort = () => {
          state.closed = true;
          openingLifetime.abort();
          state.local?.dispose();
        };
        state.abort = abort;
        const session = Object.freeze({
          async dispose() {
            signal?.removeEventListener("abort", abort);
            state.extractionSignal?.removeEventListener("abort", abort);
            state.closed = true;
            openingLifetime.abort();
            state.local?.dispose();
          },
        });
        current = session;
        sessions.set(session, state);
        signal?.addEventListener("abort", abort, { once: true });
        try {
          state.local = await openExtractedInput(inputs, {
            signal: localSignal,
            contentMode,
            maxProgramBytes,
          });
          localSessions.push(state.local);
          lookup(session);
          state.inspected = { inputKind: "extracted", selection: state.local.selection };
          return session;
        } catch (error) {
          await session.dispose();
          fail(
            signal?.aborted
              ? "EXTRACTION_CANCELLED"
              : isExtractionDiagnosticCode(error?.code)
                ? error.code
                : "EXTRACTION_FAILED",
          );
        }
      }
      const client = createExtractionWorkerClient({
        createWorker,
        timeoutMs,
        operationTimeoutMs,
        maxQueue,
      });
      const state = { client, inspected: null, closed: false, extracted: false };
      const abort = () => {
        state.closed = true;
        state.inspected = null;
        client.close();
      };
      const session = Object.freeze({
        async dispose() {
          signal?.removeEventListener("abort", abort);
          if (state.closed) return;
          state.closed = true;
          state.inspected = null;
          try {
            await client.dispose();
          } finally {
            // 强制终止也会撤销超过时限仍卡住的工作线程。
            // Hard termination also revokes a worker stuck beyond its timeout.
            client.close();
          }
        },
      });
      current = session;
      sessions.set(session, state);
      signal?.addEventListener("abort", abort, { once: true });
      try {
        const inspected = await client.initialize(inputs, { signal, onProgress: onDiagnostic });
        if (signal?.aborted) fail("EXTRACTION_CANCELLED");
        // 初始化回复兑现期间可能已清理会话；不能重新填充已撤销的缓存。
        // Cleanup may win after initialization settles; never repopulate a revoked session cache.
        lookup(session);
        state.inspected = inspected;
        return session;
      } catch (error) {
        try {
          await session.dispose();
        } catch {
          // 即使拆卸也失败，仍保留安全的主要诊断。
          // Preserve the safe primary diagnosis even if teardown also fails.
        }
        fail(
          signal?.aborted
            ? "EXTRACTION_CANCELLED"
            : isExtractionDiagnosticCode(error?.code)
              ? error.code
              : "EXTRACTION_FAILED",
        );
      }
    },
    async inspectContents({ session, signal } = {}) {
      signal?.throwIfAborted();
      const state = lookup(session);
      // 跨公共协调器边界传递快照，而非可变的缓存记录。
      // Snapshot across the public coordinator boundary, not mutable cached records.
      return structuredClone(state.inspected);
    },
    async selectContents({ session, selectionOptions, signal } = {}) {
      signal?.throwIfAborted();
      const state = lookup(session);
      return state.local
        ? structuredClone(state.local.selection)
        : resolveContentSelection(
            state.inspected.records,
            state.inspected.inventory,
            selectionOptions,
          );
    },
    async extractSelected({ session, selection, signal, onDiagnostic } = {}) {
      const state = lookup(session);
      const phase = (stage) => {
        try {
          // 本地阶段通知与工作线程进度一样隔离观察者，不等待其异步完成。
          // Isolate local phase observers like worker progress, without awaiting their completion.
          Promise.resolve(onDiagnostic?.({ stage, completed: 0, total: 0, bytes: 0 })).catch(
            () => {},
          );
        } catch {
          // 观察者错误不能撤销已认证提取。 | Observer errors cannot revoke authenticated extraction.
        }
      };
      try {
        signal?.throwIfAborted();
        if (state.local) {
          if (state.extracted || canonical(selection) !== canonical(state.local.selection))
            fail("EXTRACTION_SELECTION_MISMATCH");
          state.extracted = true;
          // 提取阶段可以使用独立信号；取消同样撤销已借出的模块和资源读取器。
          // Extraction may use its own signal; cancellation also revokes borrowed modules and resource readers.
          state.extractionSignal = signal;
          signal?.addEventListener("abort", state.abort, { once: true });
          const check = (readSignal) => {
            lookup(session);
            state.local.check(readSignal);
          };
          phase("logical-index");
          return await finishExtracted(
            state.local.programModules,
            state.local.romfs,
            state.local.selection,
            check,
            signal,
          );
        }
        if (
          state.extracted ||
          !selection ||
          !Array.isArray(selection.dlc) ||
          selection.dlc.length > 1
        )
          fail("EXTRACTION_SELECTION_REJECTED");
        state.extracted = true;
        const options = {
          contentMode,
          baseVersion: selection.base.version,
          patchVersion: selection.patch?.version,
          dlcVersions: Object.fromEntries(
            selection.dlc.map((row) => [row.titleId.toString(16).padStart(16, "0"), row.version]),
          ),
        };
        // 只有保留的边界证据才能满足此本地包检查条件。
        // 调用方提供的选择布尔值、清单或 DLC 对象都不是证明。
        // Only retained boundary evidence may satisfy this local package gate.
        // Caller-supplied selection booleans, inventory or DLC objects are not proof.
        const authenticated = resolveD2rExpansionSelection(
          state.inspected.records,
          state.inspected.inventory,
          options,
        );
        const expectedSelection = canonical(authenticated);
        if (expectedSelection !== canonical(selection)) fail("EXTRACTION_SELECTION_MISMATCH");
        const selected = await state.client.select(options, { signal, onProgress: onDiagnostic });
        if (canonical(selected.selection) !== expectedSelection)
          fail("EXTRACTION_SELECTION_MISMATCH");
        const names = new Set();
        const modules = new Map();
        for (let i = 0; i < selected.entries.length; i++) {
          const entry = selected.entries[i];
          if (
            !entry ||
            entry.index !== i ||
            typeof entry.name !== "string" ||
            names.has(entry.name) ||
            typeof entry.size !== "bigint" ||
            entry.size < 0n
          )
            fail("EXTRACTION_EXEFS_INVALID");
          names.add(entry.name);
          if (REQUIRED_NSO_MODULES.includes(entry.name)) modules.set(entry.name, entry);
        }
        if (modules.size !== REQUIRED_NSO_MODULES.length) fail("EXTRACTION_PROGRAM_MISSING");
        let total = 0n;
        for (const entry of modules.values()) {
          if (!entry.size) fail("EXTRACTION_PROGRAM_MISSING");
          total += entry.size;
        }
        if (total > BigInt(maxProgramBytes)) fail("EXTRACTION_PROGRAM_BUDGET");
        const programModules = Object.create(null);
        phase("exefs-read");
        for (const name of REQUIRED_NSO_MODULES) {
          const entry = modules.get(name),
            bytes = new Uint8Array(Number(entry.size));
          owned.push(bytes);
          for (let offset = 0; offset < bytes.length; offset += 65536) {
            const part = await state.client.readExefs(
              {
                token: selected.token,
                index: entry.index,
                offset: BigInt(offset),
                count: Math.min(65536, bytes.length - offset),
              },
              { signal },
            );
            try {
              bytes.set(part, offset);
            } finally {
              part.fill(0);
            }
          }
          programModules[name] = bytes;
        }
        // 这会撤销 ExeFS 生成；先暂存所有必需模块。
        // This revokes the ExeFS generation; all required modules are staged first.
        phase("romfs-open");
        const opened = await state.client.openRomfs(selected.token, {
          signal,
          onProgress: onDiagnostic,
          verifyRomfs,
        });
        const check = (readSignal) => {
          lookup(session);
          signal?.throwIfAborted();
          readSignal?.throwIfAborted();
        };
        const source = Object.freeze({
          size: opened.size,
          async readAt(offset, count, options = {}) {
            check(options.signal);
            if (
              typeof offset !== "bigint" ||
              offset < 0n ||
              offset > opened.size ||
              !Number.isInteger(count) ||
              count < 0 ||
              count > 65536 ||
              BigInt(count) > opened.size - offset
            )
              fail("EXTRACTION_READ_BOUNDS");
            const combined =
              signal && options.signal
                ? AbortSignal.any([signal, options.signal])
                : (signal ?? options.signal);
            const result = await state.client.readRomfs(
              { token: opened.token, offset, count },
              { signal: combined },
            );
            return checkedReadResult(result, () => check(options.signal));
          },
        });
        phase("romfs-index");
        const indexed = await indexRomfs(source, { signal });

        // 暴露有界只读能力，而非元数据行、令牌、密钥文件、
        // 原始源句柄、会话或传输客户端。
        // Expose bounded read-only capabilities, not metadata rows, tokens, key files,
        // raw source handles, sessions or the transport client.
        const romfs = Object.freeze({
          // 索引阶段已强制执行 ROMFS_LIMITS.files/path/depth。只暴露
          // 不可变清单，绝不暴露原始镜像偏移或源句柄。
          // Indexing already enforces ROMFS_LIMITS.files/path/depth. Expose only
          // immutable inventory, never raw image offsets or source handles.
          files: Object.freeze(
            indexed.files.map(({ path, size }) => Object.freeze({ path, size })),
          ),
          getFile(path) {
            check();
            const file = indexed.getFile(path);
            if (!file) return undefined;
            return Object.freeze({
              size: file.size,
              async readAt(offset, count, options) {
                check(options?.signal);
                const cancel = () => {
                  state.closed = true;
                  state.inspected = null;
                  state.client.close();
                };
                options?.signal?.addEventListener("abort", cancel, { once: true });
                try {
                  const bytes = await file.readAt(offset, count, options);
                  return checkedReadResult(bytes, () => check(options?.signal));
                } finally {
                  options?.signal?.removeEventListener("abort", cancel);
                }
              },
            });
          },
        });
        phase("logical-index");
        return await finishExtracted(programModules, romfs, selected.selection, check, signal);
      } catch (error) {
        try {
          await session.dispose();
        } catch {
          /* 保留已净化的提取失败信息。 | Preserve sanitized extraction failure. */
        }
        for (const bytes of owned.splice(0)) bytes.fill(0);
        fail(
          signal?.aborted
            ? "EXTRACTION_CANCELLED"
            : isExtractionDiagnosticCode(error?.code)
              ? error.code
              : "EXTRACTION_FAILED",
        );
      }
    },
    cleanup,
  });
}
