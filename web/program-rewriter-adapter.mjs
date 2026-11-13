// 依据受信任发布目录、提取来源证明和原始工作线程检查连接程序重写。
// Wire program rewriting from trusted release catalogs, extraction provenance and original worker checks.
import { buildGuestFramework, guestInfoPlist, planGuestFramework } from "./guest-framework.mjs";
import { resolveShellProfile } from "./shell-profiles.mjs";

import {
  PROGRAM_REWRITER_SOURCE,
  REGISTERED_PROGRAM_CATALOG,
  REGISTERED_PROGRAM_MODULE_NAMES,
  diagnoseRegisteredProgramDigests,
  PROGRAM_REWRITE_LIMITS,
} from "./program-release-catalog.mjs";
export { PROGRAM_REWRITER_SOURCE } from "./program-release-catalog.mjs";

export const PROGRAM_REWRITER_CAPABILITIES = Object.freeze({
  syntheticRewriteToGuest: true,
  productionProgramGeneration: false,
  realVersionCatalog: true,
  registeredReleaseGeneration: true,
  multiModuleGuest: true,
  realGameVerified: false,
});
const filesFor = (name, registered = false) =>
  [
    "build_src.sha256",
    ...[
      "patched.bin",
      "fixups.tsv",
      "gates.tsv",
      "sites.tsv",
      "cuts.tsv",
      "params.tsv",
      "obs.tsv",
    ].map((suffix) => `${name}.${suffix}`),
    ...(registered && name === "main" ? ["main.aim.tsv", "main.rules.tsv"] : []),
  ].sort();
const MAX_BYTES = 1024 * 1024;
const SHA = /^[0-9a-f]{64}$/;
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });
export class ProgramAdapterError extends Error {
  constructor(code) {
    super(code);
    this.name = "ProgramAdapterError";
    this.code = code;
  }
}
const fail = (code) => {
  throw new ProgramAdapterError(code);
};
const check = (signal) => {
  if (signal?.aborted) fail("PROGRAM_CANCELLED");
};
const sha256 = async (bytes) =>
  Array.from(new Uint8Array(await globalThis.crypto.subtle.digest("SHA-256", bytes)), (x) =>
    x.toString(16).padStart(2, "0"),
  ).join("");
function snapshot(bytes, maxBytes = MAX_BYTES) {
  if (
    !(bytes instanceof Uint8Array) ||
    !bytes.byteLength ||
    bytes.byteLength > maxBytes ||
    (globalThis.SharedArrayBuffer && bytes.buffer instanceof globalThis.SharedArrayBuffer)
  )
    fail("PROGRAM_INVALID_BYTES");
  return new Uint8Array(bytes);
}
function catalogFor(value) {
  if (value === REGISTERED_PROGRAM_CATALOG)
    return new Map(value.versions.map((release) => [release.id, { ...release, registered: true }]));
  if (
    !value ||
    ![1, 2].includes(value.schemaVersion) ||
    value.scope !== "synthetic-only" ||
    value.sourceSha256 !== PROGRAM_REWRITER_SOURCE ||
    !Array.isArray(value.versions) ||
    !value.versions.length ||
    value.versions.length > 4
  )
    fail("PROGRAM_TRUSTED_CATALOG_REQUIRED");
  const catalog = new Map();
  for (const entry of value.versions) {
    if (
      !entry ||
      typeof entry.id !== "string" ||
      catalog.has(entry.id) ||
      !SHA.test(entry.guestSha256) ||
      !SHA.test(entry.plistSha256)
    )
      fail("PROGRAM_INVALID_TRUSTED_VERSION");
    const modules =
      value.schemaVersion === 1 ? [{ ...entry, name: "main", caseName: entry.id }] : entry.modules;
    if (
      !Array.isArray(modules) ||
      (value.schemaVersion === 2 && modules.map((module) => module.name).join(",") !== "main,sdk")
    )
      fail("PROGRAM_INVALID_MODULE_SET");
    const checked = modules.map((module) => {
      if (
        !module ||
        !["main", "sdk"].includes(module.name) ||
        typeof module.caseName !== "string" ||
        module.buildId !== "534e5448455449430000000000000000" ||
        module.ruleset !== "native" ||
        !SHA.test(module.inputSha256) ||
        !module.files ||
        JSON.stringify(Object.keys(module.files).sort()) !==
          JSON.stringify(filesFor(module.name)) ||
        Object.values(module.files).some((hash) => !SHA.test(hash))
      )
        fail("PROGRAM_INVALID_TRUSTED_VERSION");
      return Object.freeze({ ...module, files: Object.freeze({ ...module.files }) });
    });
    catalog.set(
      entry.id,
      Object.freeze({ ...entry, modules: Object.freeze(checked), registered: false }),
    );
  }
  return catalog;
}
function layout(bytes, imageSize) {
  const values = Object.create(null);
  for (const line of decoder.decode(bytes).trim().split("\n")) {
    const match = /^# ([A-Z0-9_]+)\t(0x[0-9a-f]+|[0-9]+)$/.exec(line);
    if (!match || Object.hasOwn(values, match[1])) fail("PROGRAM_INVALID_FIXUPS");
    values[match[1]] = Number(match[2]);
  }
  // 早期 Apple 输出省略 TARGET；显式 Android/HarmonyOS 输出绝不能作为 iOS 输入。
  // Older Apple outputs omitted TARGET. Explicit Android/HarmonyOS output is never an iOS input.
  if (Object.hasOwn(values, "TARGET") && values.TARGET !== 0) fail("PROGRAM_INVALID_FIXUPS");
  for (const key of [
    "TEXT_END",
    "PATCH_OFF",
    "KEY_PAGE_OFF",
    "KEY_PAGE_SIZE",
    "RO_OFF",
    "DATA_OFF",
  ])
    if (!Number.isSafeInteger(values[key]) || values[key] < 0 || values[key] > imageSize)
      fail("PROGRAM_INVALID_FIXUPS");
  if (
    !values.TEXT_END ||
    values.TEXT_END % 4 ||
    values.PATCH_OFF % 65536 ||
    values.KEY_PAGE_OFF % 16384 ||
    values.KEY_PAGE_SIZE !== 16384 ||
    values.KEY_PAGE_OFF + values.KEY_PAGE_SIZE > values.PATCH_OFF ||
    values.TEXT_END > values.RO_OFF ||
    values.RO_OFF > values.DATA_OFF ||
    values.DATA_OFF > values.KEY_PAGE_OFF
  )
    fail("PROGRAM_INVALID_FIXUPS");
  return { textEnd: values.TEXT_END, patchOff: values.PATCH_OFF };
}
/**
 * 受信任的应用接线。已注册路线要求提取颁发的 verifyProvenance(extracted,selection,{signal}) 返回借用的 {main,sdk}。不接受调用方的认证声明或替代发布目录。合成目录必须来自独立受信任的 CI/发布注册表，绝不能来自提取内容或 UI。resolveInput(extracted, selection) 返回 {versionId, bytes}。createWorker() 必须返回 {run({bytes,caseName}): Promise<worker response>, close()}。原始工作线程的精确合成夹具检查仍为必需。
 * Trusted application wiring. The registered route requires extraction-issued
 * verifyProvenance(extracted,selection,{signal}) -> borrowed {main,sdk}.
 * No caller claims of authentication or alternate release catalogs are accepted.
 * Synthetic catalogs must come
 * from a separately trusted CI/release registry, NEVER extracted content or UI.
 * resolveInput(extracted, selection) -> {versionId, bytes}. createWorker() must
 * return {run({bytes,caseName}): Promise<worker response>, close()}.
 * 输出默认按借用处理；仅可信连接可选择 "owned"。每个响应文件必须交出独占、
 * 普通、覆盖完整缓冲区的 Uint8Array，底层 ArrayBuffer 不可共享或调整大小；
 * 晚到响应也遵守此约定。适配器可在失败、取消或销毁时擦除这些输出。
 * Outputs are borrowed by default. Only trusted wiring may opt into "owned":
 * every response file must relinquish a distinct, plain, full-span Uint8Array
 * backed by a nonshared, nonresizable ArrayBuffer, including late responses.
 * The adapter may wipe owned outputs on failure, cancellation, or disposal.
 * The original worker's exact synthetic fixture gate remains mandatory.
 */
export function createProgramRewriterAdapter({
  trustedCatalog = REGISTERED_PROGRAM_CATALOG,
  profileId,
  resolveInput,
  verifyProvenance,
  createWorker,
  workerOutputOwnership = "borrowed",
} = {}) {
  const shellProfile = resolveShellProfile(profileId);
  const versions = catalogFor(trustedCatalog);
  const registered = trustedCatalog === REGISTERED_PROGRAM_CATALOG;
  if (
    (registered ? typeof verifyProvenance !== "function" : typeof resolveInput !== "function") ||
    typeof createWorker !== "function" ||
    !["borrowed", "owned"].includes(workerOutputOwnership)
  )
    fail("PROGRAM_RUNTIME_REQUIRED");
  let used = false,
    disposed = false,
    activeWorker,
    rejectActive,
    acceptingOutputs = false;
  const owned = [],
    releaseEntries = [];
  const outputBuffers = new WeakSet();
  const own = (bytes) => {
    owned.push(bytes);
    return bytes;
  };
  const wipe = () => {
    try {
      for (const bytes of owned) bytes.fill(0);
    } finally {
      owned.length = 0;
      // 擦除后解除条目闭包引用；仅清空所有权列表不会释放输出缓冲区。
      // Release entry closure references after wiping; clearing ownership alone is insufficient.
      for (const release of releaseEntries.splice(0)) release();
    }
  };
  // 密封工作线程已转移独占缓冲区；默认自定义客户端仍保留借用语义。
  // Sealed workers transfer exclusive buffers; custom clients remain borrowed by default.
  function receiveOwnedOutput(result) {
    let invalid = false;
    for (const value of Object.values(result?.files ?? {})) {
      if (
        !(value instanceof Uint8Array) ||
        Object.getPrototypeOf(value) !== Uint8Array.prototype ||
        !(value.buffer instanceof ArrayBuffer) ||
        Object.getPrototypeOf(value.buffer) !== ArrayBuffer.prototype ||
        value.buffer.resizable === true ||
        value.byteOffset !== 0 ||
        value.byteLength !== value.buffer.byteLength
      ) {
        invalid = true;
        continue;
      }
      // 已分离的视图也报告零长度，但不能擦除；真正的空辅助文件仍有效。
      // Detached views also report zero lengths, but cannot be wiped. Empty sidecars are valid.
      if (value.byteLength === 0) {
        try {
          Uint8Array.prototype.subarray.call(value, 0, 0);
        } catch {
          invalid = true;
          continue;
        }
      }
      if (!acceptingOutputs || disposed) {
        value.fill(0);
      } else if (outputBuffers.has(value.buffer)) {
        invalid = true;
      } else {
        outputBuffers.add(value.buffer);
        own(value);
      }
    }
    // 拒绝前登记所有可接管文件，使清理也覆盖后面的条目。
    // Register every eligible file before rejecting, so cleanup covers later entries too.
    if (invalid && acceptingOutputs && !disposed) fail("PROGRAM_INVALID_OUTPUT");
    return result;
  }
  function close() {
    const worker = activeWorker;
    activeWorker = undefined;
    worker?.close();
  }
  // 清理没有异步步骤；同步抛错可让调用方汇总清理失败。
  // Cleanup has no asynchronous steps; synchronous errors let callers collect failures.
  function dispose() {
    disposed = true;
    rejectActive?.(new ProgramAdapterError("PROGRAM_DISPOSED"));
    try {
      close();
    } finally {
      wipe();
    }
  }
  async function generateProgram({ extracted, selection, signal } = {}) {
    check(signal);
    if (used || disposed) fail("PROGRAM_SINGLE_USE");
    used = true;
    acceptingOutputs = true;
    let abort;
    const interrupted = new Promise((_, reject) => {
      rejectActive = reject;
      abort = () => {
        try {
          close();
        } catch {
          /* 取消仍会拒绝，且不暴露工作线程细节。 | Cancellation still rejects without exposing worker details. */
        }
        reject(new ProgramAdapterError("PROGRAM_CANCELLED"));
      };
      signal?.addEventListener("abort", abort, { once: true });
    });
    const race = (action) => Promise.race([Promise.resolve().then(action), interrupted]);
    try {
      const input = registered
        ? { programModules: await race(() => verifyProvenance(extracted, selection, { signal })) }
        : await race(() => resolveInput(extracted, selection, { signal, maxBytes: MAX_BYTES }));
      check(signal);
      let version = registered ? undefined : versions.get(input?.versionId);
      if (!registered && !version) fail("PROGRAM_UNSUPPORTED_VERSION");
      const moduleNames = registered
        ? REGISTERED_PROGRAM_MODULE_NAMES
        : version.modules.map((module) => module.name);
      const inputModules =
        input.programModules ?? (moduleNames.length === 1 ? { main: input.bytes } : null);
      if (
        !inputModules ||
        Object.keys(inputModules).sort().join(",") !== [...moduleNames].sort().join(",")
      )
        fail("PROGRAM_INVALID_MODULE_SET");
      let inputTotal = 0,
        outputTotal = 0;
      const staged = [],
        digests = Object.create(null);
      // 同步快照两个借用数组。工作线程或异步
      // 哈希都不能观察到提取所拥有输入缓冲区的后续变更。
      // Snapshot both borrowed arrays synchronously. No worker or asynchronous
      // hashing can observe a later mutation of extraction-owned input buffers.
      for (const name of moduleNames) {
        const bytes = own(
          snapshot(
            inputModules[name],
            registered ? PROGRAM_REWRITE_LIMITS.moduleInputBytes : MAX_BYTES,
          ),
        );
        inputTotal += bytes.length;
        if (inputTotal > PROGRAM_REWRITE_LIMITS.totalInputBytes) fail("PROGRAM_INPUT_BUDGET");
        staged.push({ name, bytes });
      }
      for (const { name, bytes } of staged) {
        digests[name] = await race(() => sha256(bytes));
        check(signal);
      }
      if (registered) {
        const detected = diagnoseRegisteredProgramDigests(digests);
        if (detected.status !== "registered")
          fail(
            detected.status === "incompatible-modules"
              ? "PROGRAM_MODULE_RELEASE_MISMATCH"
              : "PROGRAM_UNSUPPORTED_VERSION",
          );
        version = versions.get(detected.releaseId);
        if (!version) fail("PROGRAM_UNSUPPORTED_VERSION");
        if (!shellProfile.programReleaseIds.includes(version.id))
          fail("PROGRAM_SHELL_RELEASE_MISMATCH");
      }
      for (const entry of staged) {
        const module = version.modules.find((candidate) => candidate.name === entry.name);
        entry.module = module;
        if (digests[entry.name] !== module.inputSha256) fail("PROGRAM_INPUT_DIGEST_MISMATCH");
        if (
          module.buildId &&
          Array.from(entry.bytes.subarray(0x40, 0x50), (x) => x.toString(16).padStart(2, "0")).join(
            "",
          ) !== module.buildId
        )
          fail("PROGRAM_BUILD_ID_MISMATCH");
        check(signal);
      }
      activeWorker = createWorker();
      if (
        !activeWorker ||
        typeof activeWorker.run !== "function" ||
        typeof activeWorker.close !== "function"
      )
        fail("PROGRAM_RUNTIME_REQUIRED");
      const modules = [],
        sidecars = Object.create(null);
      for (const { module, bytes } of staged) {
        const result = await race(() =>
          Promise.resolve(
            activeWorker.run({
              bytes,
              moduleName: module.name,
              ...(registered ? { releaseId: version.id } : { caseName: module.caseName }),
            }),
          ).then((result) =>
            workerOutputOwnership === "owned" ? receiveOwnedOutput(result) : result,
          ),
        );
        check(signal);
        const expected = filesFor(module.name, registered);
        if (
          !result?.ok ||
          result.status !== (registered ? "repository-registered-unverified" : "synthetic-only") ||
          !result.files ||
          JSON.stringify(Object.keys(result.files).sort()) !== JSON.stringify(expected)
        )
          fail("PROGRAM_REWRITE_REJECTED");
        const files = Object.create(null);
        for (const name of expected) {
          const value = result.files[name];
          if (
            !(value instanceof Uint8Array) ||
            value.byteLength > PROGRAM_REWRITE_LIMITS.totalOutputBytes ||
            (globalThis.SharedArrayBuffer && value.buffer instanceof globalThis.SharedArrayBuffer)
          )
            fail("PROGRAM_INVALID_OUTPUT");
          outputTotal += value.length;
          if (outputTotal > PROGRAM_REWRITE_LIMITS.totalOutputBytes) fail("PROGRAM_OUTPUT_BUDGET");
          files[name] = workerOutputOwnership === "owned" ? value : own(new Uint8Array(value));
          if (!registered && (await race(() => sha256(files[name]))) !== module.files[name])
            fail("PROGRAM_OUTPUT_DIGEST_MISMATCH");
          check(signal);
        }
        if (
          decoder.decode(files["build_src.sha256"]) !== `${PROGRAM_REWRITER_SOURCE}  patch_nso.py\n`
        )
          fail("PROGRAM_SOURCE_MISMATCH");
        const image = files[`${module.name}.patched.bin`];
        modules.push({
          name: module.name,
          image,
          ...layout(files[`${module.name}.fixups.tsv`], image.length),
        });
        for (const suffix of [
          "fixups.tsv",
          "gates.tsv",
          ...(module.name === "main" ? ["cuts.tsv"] : []),
        ]) {
          const name = `${module.name}.${suffix}`;
          sidecars[name] = files[name];
        }
        // 已注册输出没有虚构的基准哈希。断言原始
        // 规则清单仍标识固定的构建及输出镜像。
        // Registered outputs have no invented oracle hashes. Assert the original
        // rules manifest still identifies the pinned build and the emitted image.
        if (registered && module.name === "main") {
          const rules = decoder.decode(files["main.rules.tsv"]);
          if (
            !rules.startsWith(`# RULESET\t${version.ruleset}\n# BUILD_ID\t${module.buildId}\n`) ||
            !rules.includes(`# IMAGE_SHA256\t${await race(() => sha256(image))}\n`)
          )
            fail("PROGRAM_RULESET_MANIFEST_MISMATCH");
        }
      }
      const { signatureOffset, signatureSize } = planGuestFramework(modules);
      const guest = own(
        await race(() => buildGuestFramework(modules, { signal, profileId: shellProfile.id })),
      );
      const plist = own(encoder.encode(guestInfoPlist(undefined, { profileId: shellProfile.id })));
      if (
        !registered &&
        ((await race(() => sha256(guest))) !== version.guestSha256 ||
          (await race(() => sha256(plist))) !== version.plistSha256)
      )
        fail("PROGRAM_GUEST_DIGEST_MISMATCH");
      check(signal);
      close();
      // Guest 已复制镜像；只保留调用方会读取的输出，释放输入快照和临时重写文件。
      // Guest has copied the images; retain only streamed outputs, not snapshots or rewrite scratch.
      const retained = new Set([guest, plist, ...Object.values(sidecars)]);
      for (let i = owned.length - 1; i >= 0; i--) {
        if (!retained.has(owned[i])) {
          owned[i].fill(0);
          owned.splice(i, 1);
        }
      }
      staged.length = 0;
      modules.length = 0;
      const entry = (path, data, mode = 0o644, signature) => {
        const size = data.length;
        releaseEntries.push(() => {
          data = undefined;
        });
        return {
          path,
          size,
          mode,
          ...(signature ? { signature } : {}),
          async *chunks() {
            if (disposed) fail("PROGRAM_DISPOSED");
            for (let at = 0; at < size; at += 65536) {
              check(signal);
              if (disposed || !data) fail("PROGRAM_DISPOSED");
              yield data.slice(at, at + 65536);
            }
          },
        };
      };
      return [
        entry("Frameworks/Guest.framework/Guest", guest, 0o755, {
          kind: "ad-hoc",
          offset: signatureOffset,
          size: signatureSize,
        }),
        entry("Frameworks/Guest.framework/Info.plist", plist),
        ...Object.entries(sidecars).map(([name, value]) => entry(`dm/${name}`, value)),
      ];
    } catch (error) {
      wipe();
      if (error instanceof ProgramAdapterError) throw error;
      fail("PROGRAM_GENERATION_FAILED");
    } finally {
      acceptingOutputs = false;
      rejectActive = undefined;
      signal?.removeEventListener("abort", abort);
      try {
        close();
      } catch {
        wipe();
        fail("PROGRAM_WORKER_CLEANUP_FAILED");
      }
    }
  }
  return Object.freeze({ capabilities: PROGRAM_REWRITER_CAPABILITIES, generateProgram, dispose });
}
