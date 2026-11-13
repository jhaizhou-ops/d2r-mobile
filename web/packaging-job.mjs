// 依据显式适配器契约及 ZIP 写入器提交边界协调本地打包事务。
// Coordinate local packaging transactions from explicit adapter contracts and ZIP writer commit boundaries.
import { isExtractionDiagnosticCode, extractionProgressLabel } from "./extraction-errors.mjs";
import { shaderFailureDiagnostic } from "./shader-diagnostics.mjs";
import { isAirCompilerDiagnosticCode } from "./air-compiler-contract.mjs";
import { isProgramCompatibilityDiagnosticCode } from "./program-compatibility.mjs";
import { discoverGameFiles, discoverGameFolder } from "./game-input.mjs";
import { resolveContentSelection } from "./content-selection.mjs";
import { validateShellContract, validateAssemblyInventory } from "./shell-contract.mjs";
import { writeZip } from "./zip-writer.mjs";
import { writeZip64, planZip64 } from "./zip64-writer.mjs";

/**
 * 显式能力声明，不是实现，也不是支持真实转换的证据。
 * Explicit capabilities, not implementations or evidence of real conversion support.
 */
export const REQUIRED_PACKAGING_ADAPTERS = Object.freeze([
  "openSecretSession",
  "inspectContents",
  "extractSelected",
  "generateProgram",
  "generateAssets",
  "generateShaders",
  "loadShell",
  "validateGenerated",
  "createOutput",
  "validateArchive",
  "cleanup",
]);
const labels = Object.freeze({
  capabilities: "Checking local conversion capabilities",
  discovery: "Finding local package and key files",
  secrets: "Reading local keys and verifying package contents",
  inspection: "Inspecting package content identities",
  selection: "Selecting base, update and additional content",
  extraction: "Extracting selected content locally",
  disposal: "Closing the isolated key session",
  program: "Generating the executable framework",
  assets: "Generating game assets",
  shaders: "Generating shader assets",
  shell: "Validating the application shell",
  validation: "Validating generated application content",
  assembly: "Checking the complete assembly inventory",
  output: "Writing a staged unsigned archive",
  archive: "Validating the staged archive",
  cleanup: "Releasing temporary conversion resources",
  commit: "Finalizing the unsigned archive",
});

/**
 * 输出条目类型。
 * Output entry type.
 * @typedef {{path:string,size:number,mode:number,signature?:{kind:string,offset:number,size:number},chunks:()=>AsyncIterable<Uint8Array>}} OutputEntry
 */
/**
 * 输出接收端类型。
 * Output sink type.
 * @typedef {{write:(bytes:Uint8Array)=>Promise<void>,close:()=>Promise<void>,abort:(error:Error)=>Promise<void>}} OutputSink
 */
/**
 * 适配器上下文类型。
 * Adapter context type.
 * @typedef {{signal:AbortSignal, inputs?:object, session?:object, selection?:object, extracted?:unknown}} AdapterContext
 */
/**
 * 适配器是受信任的本地能力，必须遵守 signal，避免记录或缓存秘密，且只能通过返回的 sink.close() 提交输出。openSecretSession 失败时，必须先清理自身部分创建的工作线程再拒绝。
 * Adapters are trusted local capabilities. They must honor signal, avoid logging/caching
 * secrets, and never commit output except through the returned sink.close(). A failed
 * openSecretSession must clean up its own partially-created worker before rejecting.
 * @typedef {object} PackagingAdapters
 * @property {(context:AdapterContext)=>Promise<{dispose:()=>Promise<void>}>} openSecretSession
 * @property {(context:AdapterContext)=>Promise<{records:object[],inventory:object[]}>} inspectContents
 * @property {(context:AdapterContext)=>Promise<unknown>} extractSelected
 * @property {(context:AdapterContext)=>Promise<OutputEntry[]>} generateProgram
 * @property {(context:AdapterContext)=>Promise<OutputEntry[]>} generateAssets
 * @property {(context:AdapterContext)=>Promise<OutputEntry[]>} generateShaders
 * @property {(context:AdapterContext)=>Promise<{contract:object,entries:OutputEntry[]}>} loadShell
 * @property {(context:object)=>Promise<{valid:boolean,observedOutputTargetOS:object}>} validateGenerated
 * @property {(context:object)=>Promise<OutputSink>} createOutput
 * @property {(context:object)=>Promise<{valid:boolean}>} validateArchive
 * @property {(context:{signal:AbortSignal})=>Promise<void>} cleanup
 */
/**
 * 打包进度类型。
 * Packaging progress type.
 * @typedef {{stage:string,state:string,label:string}} PackagingProgress
 */
/**
 * @typedef {object} PackagingJobOptions
 * @property {File[]|FileList|FileSystemDirectoryHandle} input
 * @property {PackagingAdapters} adapters
 * @property {AbortSignal} [signal]
 * @property {object} [selectionOptions]
 * @property {"packages"|"extracted"} [inputMode]
 * @property {string[]} [selectedNspNames] 所选文件夹中要使用的包文件名。 | Package filenames to use from the selected folder.
 * @property {"extraction"|"archive"} [secretLifetime] 默认为 extraction；显式 archive 模式允许隔离的惰性明文读取器存活至写入结束。 | Default extraction; explicit archive mode permits isolated lazy plaintext readers until writing finishes.
 * @property {"zip32"|"zip64"} [archiveFormat] ZIP64 需显式选择，不代表 iOS 兼容性。 | ZIP64 is explicit opt-in, not an iOS compatibility claim.
 * @property {number} [maxOutputBytes] ZIP64 必需的正安全整数归档预算。 | Required positive safe-integer archive budget for ZIP64.
 * @property {number} [concurrency] 最大并发生成器数（1–3，默认 2）。 | Maximum concurrent generators (1–3; default 2).
 * @property {(progress:PackagingProgress)=>void} [onProgress] 仅作观察，忽略异常。 | Observer only; exceptions ignored.
 */
export class PackagingJobError extends Error {
  constructor(cause, report) {
    super("Local packaging did not complete.", { cause });
    this.name = "PackagingJobError";
    this.report = report;
  }
}
const assertion = (condition) => {
  if (!condition) throw new Error("PACKAGING_CONTRACT");
};
const comparePaths = (a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0);

// 程序诊断先限定阶段，防止已提取输入的诊断白名单绕过阶段边界。
// Scope program diagnostics before the extraction allowlist can admit local-input codes.
function failureDetailCode(stage, code) {
  if (isProgramCompatibilityDiagnosticCode(code))
    return stage === "program" ||
      (stage === "secrets" &&
        ["PROGRAM_UNSUPPORTED_VERSION", "PROGRAM_MODULE_RELEASE_MISMATCH"].includes(code))
      ? code
      : null;
  return isExtractionDiagnosticCode(code) ||
    (stage === "shaders" && isAirCompilerDiagnosticCode(code))
    ? code
    : null;
}

/**
 * 不集成 UI，也不提供转换回退。仅在完整验证、临时资源清理和 sink.close 完成后兑现。原始错误作为 cause 供受信任调用方使用，绝不可用于 UI 或日志；只有 report 可安全展示给用户。
 * No UI integration or conversion fallback. Resolves only after full validation,
 * temporary-resource cleanup and sink.close. Original errors are available as cause
 * for trusted callers, NEVER suitable for UI/logging; only report is user-safe.
 * @param {PackagingJobOptions} options
 */
export async function runPackagingJob(options) {
  const controller = new AbortController();
  const signal = controller.signal;
  const abort = () => controller.abort(); // 绝不复制调用方提供的取消原因。 | Never copy caller-supplied abort reasons.
  options.signal?.addEventListener("abort", abort, { once: true });
  if (options.signal?.aborted) abort();
  let stage = "capabilities",
    session,
    output,
    committed = false,
    commitStarted = false,
    outputStarted = false;
  let cleanupDone = false,
    outputAborted = false;
  const cleanupCodes = [];
  const adapters = options.adapters;
  const emit = (name, state) => {
    try {
      // 不等待观察者；异步拒绝不能逸出为进程级错误。
      // Never await observers; async rejection must not escape as a process-level error.
      Promise.resolve(
        options.onProgress?.(Object.freeze({ stage: name, state, label: labels[name] })),
      ).catch(() => {});
    } catch {
      // 观察者不能改变执行、取消任务或暴露回调错误。
      // Observers cannot change execution, cancel the job or expose callback errors.
    }
  };
  const diagnosticObserver = (name) => (diagnostic) => {
    if (signal.aborted || stage !== name) return;
    const label = extractionProgressLabel(diagnostic);
    if (!label) return;
    try {
      Promise.resolve(
        options.onProgress?.(Object.freeze({ stage: name, state: "running", label, diagnostic })),
      ).catch(() => {});
    } catch {
      // 只读进度观察者不能影响任务。
      // Read-only progress observers cannot affect the job.
    }
  };
  const step = async (name, action) => {
    signal.throwIfAborted();
    stage = name;
    emit(name, "running");
    try {
      const result = await action();
      if (!committed) signal.throwIfAborted();
      emit(name, "complete");
      return result;
    } catch (error) {
      emit(name, "failed");
      throw error;
    }
  };
  const dispose = async () => {
    if (!session) return;
    const current = session;
    session = undefined; // 即使失败，也只尝试释放一次。 | Disposal is attempted exactly once, including on failure.
    try {
      await current.dispose();
    } catch (error) {
      cleanupCodes.push("SECRET_DISPOSAL_FAILED");
      throw error;
    }
  };
  const cleanup = async () => {
    if (cleanupDone) return;
    cleanupDone = true;
    try {
      await adapters.cleanup({ signal });
    } catch (error) {
      cleanupCodes.push("RESOURCE_CLEANUP_FAILED");
      throw error;
    }
  };
  const abortOutput = async () => {
    if (!output || outputAborted || committed) return;
    outputAborted = true;
    try {
      await output.abort(new Error("PACKAGING_ABORTED"));
    } catch {
      cleanupCodes.push("OUTPUT_ABORT_FAILED");
    }
  };
  let capabilitiesReady = false;
  try {
    // 在发现文件、打开密钥或创建输出之前就以关闭方式拒绝不满足条件的操作。
    // Fail closed BEFORE even discovering files, opening keys or creating output.
    assertion(
      adapters && REQUIRED_PACKAGING_ADAPTERS.every((key) => typeof adapters[key] === "function"),
    );
    assertion(
      options.concurrency === undefined ||
        (Number.isInteger(options.concurrency) &&
          options.concurrency >= 1 &&
          options.concurrency <= 3),
    );
    assertion(
      options.archiveFormat === undefined || ["zip32", "zip64"].includes(options.archiveFormat),
    );
    assertion(
      options.archiveFormat !== "zip64" ||
        (Number.isSafeInteger(options.maxOutputBytes) && options.maxOutputBytes > 0),
    );
    assertion(
      options.secretLifetime === undefined ||
        ["extraction", "archive"].includes(options.secretLifetime),
    );
    capabilitiesReady = true;
    const discovered = await step("discovery", () =>
      options.input?.kind === "directory"
        ? discoverGameFolder(options.input, { signal, inputMode: options.inputMode })
        : discoverGameFiles(options.input, { inputMode: options.inputMode }),
    );
    let inputs = discovered;
    if (options.selectedNspNames !== undefined) {
      assertion(discovered.inputMode === "packages");
      const names = options.selectedNspNames;
      assertion(
        Array.isArray(names) &&
          names.length > 0 &&
          names.every((name) => typeof name === "string" && name.length > 0),
      );
      const selected = new Set(names.map((name) => name.toLowerCase()));
      assertion(selected.size === names.length);
      const nsps = discovered.nsps.filter((file) => selected.has(file.name.toLowerCase()));
      assertion(nsps.length === selected.size);
      inputs = { ...discovered, nsps };
    }
    const context = { inputs, signal };
    await step("secrets", async () => {
      session = await adapters.openSecretSession({
        ...context,
        onDiagnostic: diagnosticObserver("secrets"),
      });
      assertion(session && typeof session.dispose === "function");
    });
    const inspected = await step("inspection", () =>
      adapters.inspectContents({ ...context, session }),
    );
    const selection = await step("selection", () =>
      typeof adapters.selectContents === "function"
        ? adapters.selectContents({
            ...context,
            session,
            inspected,
            selectionOptions: options.selectionOptions,
          })
        : resolveContentSelection(inspected.records, inspected.inventory, options.selectionOptions),
    );
    const extracted = await step("extraction", () =>
      adapters.extractSelected({
        ...context,
        session,
        selection,
        onDiagnostic: diagnosticObserver("extraction"),
      }),
    );
    if (options.secretLifetime !== "archive") await step("disposal", dispose);
    // 生成器不会收到输入或密钥文件句柄，也不会收到秘密会话引用。
    // 显式 archive 模式仅保留隔离会话，并通过受信任提取提供的有界、
    // 已认证明文读取能力访问它。
    // Generators receive no input/key-file handles or secret-session reference.
    // Explicit archive mode retains only the isolated session behind bounded,
    // authenticated plaintext read capabilities supplied by trusted extraction.
    const generationContext = { extracted, selection, signal };
    const tasks = [
      ["program", "generateProgram"],
      ["assets", "generateAssets"],
      ["shaders", "generateShaders"],
    ];
    const results = new Array(tasks.length);
    let next = 0,
      firstFailure;
    const worker = async () => {
      while (!firstFailure && next < tasks.length && !signal.aborted) {
        const index = next++;
        const [name, capability] = tasks[index];
        try {
          results[index] = await step(name, () => adapters[capability](generationContext));
          assertion(Array.isArray(results[index]));
        } catch (error) {
          if (!firstFailure) firstFailure = { error, stage: name };
          controller.abort();
        }
      }
    };
    // 只有这个有界池会并发运行。清理前先等待所有已启动工作结束。
    // Only this bounded pool runs concurrently. Drain all started work before cleanup.
    await Promise.all(Array.from({ length: options.concurrency ?? 2 }, worker));
    if (firstFailure) {
      stage = firstFailure.stage;
      throw firstFailure.error;
    }
    signal.throwIfAborted();
    const generatedEntries = results.flat().sort(comparePaths);
    const shell = await step("shell", async () => {
      const loaded = await adapters.loadShell({ signal });
      validateShellContract(loaded.contract);
      return loaded;
    });
    const validated = await step("validation", async () => {
      const result = await adapters.validateGenerated({
        generatedEntries,
        selection,
        shell,
        signal,
      });
      assertion(result?.valid === true);
      return result;
    });
    const assembly = await step("assembly", () =>
      validateAssemblyInventory({
        shell: shell.contract,
        generatedEntries,
        observedOutputTargetOS: validated.observedOutputTargetOS,
      }),
    );
    const entries = [
      ...shell.entries,
      ...generatedEntries.map((entry) => ({ ...entry, path: assembly.base + entry.path })),
    ].sort(comparePaths);
    // 将实际流式条目绑定到已验证元数据，不允许未经检查的额外外壳文件。
    // Bind actual streamed entries to validated metadata; no unchecked shell extras.
    const metadata = (list) =>
      list.map(({ path, size, mode }) => ({ path, size, mode })).sort(comparePaths);
    assertion(JSON.stringify(metadata(entries)) === JSON.stringify(metadata(assembly.entries)));
    const manifest = Object.freeze(metadata(entries).map(Object.freeze));
    const archiveEntries = entries.map((entry) => ({ ...entry, mode: entry.mode & 0o777 }));
    const zip64 = options.archiveFormat === "zip64";
    // 打开任何暂存目标之前，拒绝超出预算的 ZIP64 元数据。
    // Reject over-budget ZIP64 metadata before opening any staged destination.
    if (zip64) planZip64(archiveEntries, { maxTotalBytes: options.maxOutputBytes });
    const result = await step("output", async () => {
      outputStarted = true;
      output = await adapters.createOutput({ manifest, signal });
      assertion(
        output && ["write", "close", "abort"].every((key) => typeof output[key] === "function"),
      );
      return (zip64 ? writeZip64 : writeZip)(
        archiveEntries,
        {
          write: (bytes) => output.write(bytes),
          abort: abortOutput,
          close: async () => {
            // 所有输出条目生产器都已结束，因而不再需要
            // 惰性提取；在验证或提交之前释放秘密。
            // All output entry producers have drained. Lazy extraction can no
            // longer be needed; dispose secrets before validating or committing.
            if (session) await step("disposal", dispose);
            await step("archive", async () => {
              const checked = await adapters.validateArchive({ output, manifest, signal });
              assertion(checked?.valid === true);
            });
            await step("cleanup", cleanup);
            signal.throwIfAborted();
            stage = "commit";
            emit("commit", "running");
            commitStarted = true;
            // 特意不在 close 之后检查取消：close 就是提交边界。
            // Deliberately no post-close cancellation check: close is the commit boundary.
            await output.close();
            committed = true;
            emit("commit", "complete");
          },
        },
        { signal, maxTotalBytes: options.maxOutputBytes },
      );
    });
    return Object.freeze({
      status: "complete",
      unsigned: true,
      installable: false,
      committed,
      manifest,
      ...result,
    });
  } catch (cause) {
    const failedStage = stage;
    // close() 仍可能在提交前校验及复制；只把同一取消原因识别为取消，不能掩盖并发的磁盘错误。
    // close() can still validate and copy before commit; match the exact abort reason without masking a racing disk failure.
    const cancelled = !committed && signal.aborted && (!commitStarted || cause === signal.reason);
    controller.abort();
    try {
      await dispose();
    } catch {
      /* 已在上方收集，保留主要原因。 | Collected above; preserve primary cause. */
    }
    if (capabilitiesReady) {
      try {
        await cleanup();
      } catch {
        /* 已在上方收集，保留主要原因。 | Collected above; preserve primary cause. */
      }
    }
    await abortOutput();
    throw new PackagingJobError(
      cause,
      Object.freeze({
        status: "failed",
        code:
          failedStage === "capabilities"
            ? "CAPABILITY_UNAVAILABLE"
            : cancelled && options.signal?.aborted
              ? "CANCELLED"
              : "STAGE_FAILED",
        stage: failedStage,
        detailCode: failureDetailCode(failedStage, cause?.code),
        ...(failedStage === "shaders" ? { shaderDiagnostic: shaderFailureDiagnostic(cause) } : {}),
        outputStarted,
        committed,
        commitStarted,
        cleanupCodes: Object.freeze([...cleanupCodes]),
      }),
    );
  } finally {
    options.signal?.removeEventListener("abort", abort);
  }
}
