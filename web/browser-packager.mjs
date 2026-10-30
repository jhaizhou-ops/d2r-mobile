// 依据生产配置、密封运行时及打包适配器契约组装浏览器入口。
// Assemble the browser entry point from production profiles, sealed runtimes and packaging adapter contracts.
/**
 * 生产入口点。引导过程仅包含公开字节。
 * The production entry point. Bootstrap contains public bytes only.
 */
import { prepareSealedRuntime } from "./sealed-runtime/clients.mjs";
import {
  prepareAppCustomization,
  customizeUnsignedIpa,
  APP_CUSTOMIZATION_ERROR_CODES,
} from "./app-customization.mjs";
import {
  loadProductionProfile,
  bindProductionAstcEntries,
  inspectProductionAstcEntries,
} from "./production-profile.mjs";
import { resolveShellProfile, BUNDLED_SHELL_PROFILE_IDS } from "./shell-profiles.mjs";
import { astcOutputPaths } from "./astc-output-validator.mjs";
import {
  createExtractionJobAdapter,
  readAuthenticatedProgramModules,
  readAuthenticatedAssets,
  readInputContentPolicy,
} from "./extraction-job-adapter.mjs";
import { createProgramRewriterAdapter } from "./program-rewriter-adapter.mjs";
import { createProductionAssetAdapter } from "./production-assets.mjs";
import { createProductionShaderAdapter } from "./production-shaders.mjs";
import { createLocalOutputCache } from "./local-output-cache.mjs";
import { createBrowserBlobOutputAdapters, IPA_DOWNLOAD_LIMITS } from "./browser-blob-output.mjs";
import { planStagedZip } from "./staged-zip-validator.mjs";
import { exportRomfsResources, ResourceExportError } from "./resource-folder-export.mjs";
import { exportLegacyDevicePack, LegacyPackExportError } from "./legacy-device-pack-export.mjs";
import { deriveD2rRuntimeContentMode } from "./d2r-runtime-content-mode.mjs";
import { runPackagingJob, PackagingJobError } from "./packaging-job.mjs";
import {
  sanitizeResourceOutcome,
  sanitizeResourceProgress,
  sanitizeShaderRecordProgress,
  resourceProgressLabel,
  packagingActivity,
  isLegacyAstcDiagnosticCode,
} from "./packaging-diagnostics.mjs";
import { inspectGuestTarget, inspectGuestPlist } from "./guest-target-stream.mjs";
// 检查必需 API 和 gzip 格式；权限与实际存储可用性在使用时验证，配额估算仅供参考。
// Check required APIs and gzip format; permissions and actual storage usability are checked at use, not inferred from quota estimates.
export function assertBrowserPackagerCapabilities(environment = globalThis) {
  if (
    !environment.isSecureContext ||
    typeof environment.navigator?.storage?.getDirectory !== "function" ||
    typeof environment.navigator?.locks?.request !== "function" ||
    typeof environment.Worker !== "function" ||
    typeof environment.DecompressionStream !== "function" ||
    !environment.crypto?.subtle ||
    typeof environment.crypto?.randomUUID !== "function" ||
    typeof environment.AbortSignal?.any !== "function" ||
    typeof environment.AbortSignal?.prototype?.throwIfAborted !== "function"
  )
    throw new Error("BROWSER_UNSUPPORTED");
  // 所有密封运行时均为 gzip；构造空流探测格式，不下载或读取任何输入。
  // All sealed runtimes use gzip; construct an empty stream to probe the format without downloads or reads.
  try {
    new environment.DecompressionStream("gzip");
  } catch {
    throw new Error("BROWSER_UNSUPPORTED");
  }
}

/**
 * 可编辑界面在公开引导阶段预载全部选项，私有输入开始后不再联网。
 * Editable UI preloads every option during public bootstrap; no networking after private inputs begin.
 * @param {{manifestSha256?: string, signal?: AbortSignal, onProgress?: (event: any) => void, profileId?: string, legacyAstc?: boolean, signing?: boolean, astcPreset?: "thorough"|"fast", editableOptions?: boolean}} [options]
 */
export async function prepareBrowserPackager({
  manifestSha256,
  signal,
  onProgress,
  profileId,
  legacyAstc: initialLegacyAstc = false,
  signing = false,
  astcPreset: initialAstcPreset = "thorough",
  editableOptions = false,
} = {}) {
  validateGenerationOptions(initialLegacyAstc, initialAstcPreset);
  if (typeof editableOptions !== "boolean") throw new Error("EDITABLE_OPTIONS_OPTION");
  if (typeof signing !== "boolean") throw new Error("SIGNING_OPTION");
  const selectedProfile = resolveShellProfile(profileId);
  const preparedProfileIds = editableOptions
    ? [...new Set([selectedProfile.id, ...BUNDLED_SHELL_PROFILE_IDS])]
    : [selectedProfile.id];
  const legacyReady = initialLegacyAstc || editableOptions;
  assertBrowserPackagerCapabilities();
  const runtime = await prepareSealedRuntime({
    baseUrl: "./runtime/",
    profileId: selectedProfile.id,
    manifestSha256,
    preloadProfileIds: preparedProfileIds,
    deferOptionalInitialization: editableOptions,
    optionalGroups: [
      ...(legacyReady ? ["legacy-astc"] : []),
      ...(signing || editableOptions ? ["ipa-signing"] : []),
    ],
    signal,
    onProgress,
  });
  let profile, program, assets, shaders, shellVariantBytes, signingFactory, outputCache;
  let closed = false;
  let deferRuntimeClose = false;
  let closePromise, wholeClosePromise;
  const closeConversion = () => {
    if (closed) return closePromise;
    closed = true;
    let failure,
      failed = false;
    const pending = [];
    // 即使某项清理失败，也继续释放其余运行时及输出缓冲区。
    // Release remaining runtimes and output buffers even if one cleanup fails.
    for (const release of [
      () => program?.dispose(),
      () => assets?.dispose(),
      () => shaders?.dispose(),
      () => outputCache?.close(),
      () => profile?.close(),
      () => {
        if (!deferRuntimeClose) return runtime.close();
      },
      () => shellVariantBytes?.fill(0),
    ]) {
      try {
        const released = release();
        if (released && typeof released.then === "function") pending.push(released);
      } catch (error) {
        if (!failed) {
          failure = error;
          failed = true;
        }
      }
    }
    shellVariantBytes = profile = program = assets = shaders = undefined;
    if (pending.length) {
      closePromise = Promise.allSettled(pending).then((outcomes) => {
        const failures = [
          ...(failed ? [failure] : []),
          ...outcomes.filter((value) => value.status === "rejected").map((value) => value.reason),
        ];
        if (failures.length) throw new AggregateError(failures, "PACKAGER_CLEANUP_FAILED");
      });
      closePromise.catch(() => undefined);
      return closePromise;
    }
    if (failed) throw failure;
  };
  // 转换清理不能释放仍用于最终 IPA 的独立签名线程；显式关闭释放全部所有权。
  // Conversion cleanup preserves the independent signer; explicit close releases both owners.
  const close = () => {
    if (wholeClosePromise) return wholeClosePromise;
    const signer = signingFactory;
    signingFactory = undefined;
    if (!signer) return closeConversion();
    wholeClosePromise = cleanupPackagingAdapters(() => signer.close(), closeConversion);
    // 卸载时可以有意忽略清理结果；等待清理的调用方仍会收到原始拒绝原因。
    // Unmount may intentionally ignore cleanup; awaiters still receive the original rejection.
    wholeClosePromise.catch(() => undefined);
    return wholeClosePromise;
  };
  try {
    if (signing || editableOptions) signingFactory = runtime.takeIpaSigningFactory();
    if (legacyReady && typeof runtime.createAstcProducerClient !== "function")
      throw new Error("LEGACY_ASTC_UNAVAILABLE");
    profile = await loadProductionProfile(runtime, { signal, profileId: selectedProfile.id });
    let used = false;
    const allowed = (path) =>
      profile.trustedShell.trustedRecipe.requiredPaths.some((p) =>
        p.endsWith("/") ? path.startsWith(p) : path === p,
      ) || profile.astcOutput?.paths.includes(path);
    return Object.freeze({
      ready: true,
      realGameVerified: false,
      signingReady: Boolean(signingFactory),
      async prepareSigning(options) {
        if (!signingFactory) throw new Error("SIGNING_CLOSED");
        return signingFactory.prepare(options);
      },
      // 未签名 Blob 仍归调用方所有，签名失败时绝不替换它。
      // The unsigned Blob remains caller-owned and is never replaced on signing failure.
      async sign(options) {
        if (!signingFactory) throw new Error("SIGNING_CLOSED");
        return signingFactory.sign(options);
      },
      /**
       * 仅允许明确跳过原始资源导出；验证及补充包导出保持不变。
       * Allow only explicit original-resource export skipping; validation and supplemental export stay unchanged.
       * Omitted inputMode detects the input route; pass a discovered mode to pin a rescan.
       * @param {{input: any, selectedNspNames?: any, inputMode?: "packages"|"extracted", signal?: AbortSignal, onProgress?: (event: any) => void, exportResources?: boolean, appCustomization?: any, profileId?: string, legacyAstc?: boolean, astcPreset?: "thorough"|"fast", ignoreCache?: boolean}} options
       */
      async generate({
        input,
        selectedNspNames,
        inputMode,
        signal,
        onProgress,
        exportResources = true,
        appCustomization,
        profileId = selectedProfile.id,
        legacyAstc = initialLegacyAstc,
        astcPreset = initialAstcPreset,
        ignoreCache = false,
      }) {
        if (typeof ignoreCache !== "boolean") throw new Error("IGNORE_CACHE_OPTION");
        validateGenerationOptions(legacyAstc, astcPreset);
        if (!preparedProfileIds.includes(resolveShellProfile(profileId).id))
          throw new Error("PROFILE_NOT_PRELOADED");
        if (legacyAstc && !legacyReady) throw new Error("LEGACY_ASTC_NOT_PRELOADED");
        validateResourceExportOption(exportResources);
        const customization = await prepareAppCustomization(appCustomization, { signal });
        if (closed || used) throw new Error("RELOAD_REQUIRED");
        if (
          input?.kind !== "directory" ||
          typeof input.queryPermission !== "function" ||
          (await input.queryPermission({ mode: "readwrite" })) !== "granted"
        )
          throw new Error("INPUT_FOLDER_WRITE_PERMISSION_REQUIRED");
        // 权限查询期间可能已关闭或被另一调用占用；恢复后重新检查单次作业状态。
        // Permission checks can outlive closure or another caller; recheck the single-use state on resumption.
        if (closed || used) throw new Error("RELOAD_REQUIRED");
        used = true;
        const cacheContext = { runtimeManifestSha256: manifestSha256 };
        let extraction;
        try {
          // 仅使用已验证内存中的公开字节；选项在整个作业期间固定。
          // Use verified in-memory public bytes only; options are fixed for the entire job.
          if (profile.profileId !== profileId) {
            profile.close();
            const replacement = await loadProductionProfile(runtime, { signal, profileId });
            if (closed) {
              replacement.close();
              throw new Error("RELOAD_REQUIRED");
            }
            profile = replacement;
          }
          if (legacyAstc) await runtime.prepareAstcProducer({ signal });
          signal?.throwIfAborted();
          if (closed) throw new Error("RELOAD_REQUIRED");
          const preparedCache = await createLocalOutputCache({
            directory: input,
            ignoreCache,
            signal,
            onWarning: onProgress,
          });
          if (closed || signal?.aborted) {
            preparedCache.close();
            signal?.throwIfAborted();
            throw new Error("RELOAD_REQUIRED");
          }
          outputCache = preparedCache;
          extraction = createExtractionJobAdapter({
            createWorker: runtime.createExtractionWorker,
            contentMode: profile.contentMode,
            // 默认路径已对照 CNMT 检查完整 NCA 摘要；显式快速路径
            // 则跳过完整性扫描，以便先尝试提取。
            // The default path checks whole-NCA digests against CNMT. Explicit quick
            // mode skips full-content integrity scans to attempt extraction sooner.
            verifyRomfs: false,
          });
          program = createProgramRewriterAdapter({
            profileId: profile.profileId,
            verifyProvenance: readAuthenticatedProgramModules,
            createWorker: runtime.createProgramWorker,
            workerOutputOwnership: "owned",
          });
          if (!legacyAstc)
            assets = createProductionAssetAdapter({ runtime, profile, outputCache, cacheContext });
          shaders = createProductionShaderAdapter({
            runtime,
            catalog: profile.catalog,
            outputCache,
            cacheContext,
            trustedShell: {
              ...profile.shell.contract,
              trustedRecipe: profile.trustedShell.trustedRecipe,
            },
          });
        } catch (error) {
          await cleanupPackagingAdapters(() => {
            throw error;
          }, close);
        }
        let authenticated,
          selected,
          output,
          resources,
          resourceOutcome,
          finalizedPack,
          legacyPack,
          legacyPackOutcome;
        if (!exportResources) resources = resourceOutcome = RESOURCE_EXPORT_SKIPPED;
        const producer = legacyAstc ? runtime.createAstcProducerClient() : undefined;
        deferRuntimeClose = legacyAstc;
        const producerLease = producer
          ? {
              async close() {
                const failures = [];
                try {
                  await producer.close();
                } catch (error) {
                  failures.push(error);
                }
                try {
                  await runtime.close();
                } catch (error) {
                  failures.push(error);
                }
                if (failures.length)
                  throw new AggregateError(failures, "LEGACY_ASTC_RELEASE_FAILED");
              },
            }
          : undefined;
        const archiveFormat = "zip32";
        const maxOutputBytes = IPA_DOWNLOAD_LIMITS.maxOutputBytes;
        const adapters = {
          ...extraction,
          async extractSelected(context) {
            authenticated = await extraction.extractSelected(context);
            selected = context.selection;
            return authenticated;
          },
          async createOutput(context) {
            planStagedZip(context.manifest, { archiveFormat, maxOutputBytes });
            const { romfs } = readAuthenticatedAssets(authenticated, selected, { signal });
            try {
              resources = await runOptionalResourceExport(exportResources, () =>
                exportRomfsResources({
                  romfs,
                  // 仅根目录发现绝不遍历生成的资源文件夹。
                  // 仅新建 data；现有 data 逐字节校验后复用，绝不覆写源文件。
                  // Root-only discovery never traverses generated resource folders.
                  // Create data or verify/reuse it read-only, never overwrite source files.
                  outputDirectoryHandle: input,
                  signal,
                  onProgress: (progress) => {
                    const event = {
                      stage: "resources",
                      // 只有导出器完成标记写入后才报告完成，不能仅依据字节计数。
                      // Completion follows the exporter's marker write, not merely the byte count.
                      state: progress.state === "complete" ? "complete" : "running",
                      resourceProgress: sanitizeResourceProgress(progress),
                    };
                    return onProgress?.(
                      Object.freeze({
                        ...event,
                        label: resourceProgressLabel(event) ?? packagingActivity(event),
                      }),
                    );
                  },
                }),
              );
              resourceOutcome = exportResources ? sanitizeResourceOutcome(resources) : resources;
            } catch (error) {
              if (error instanceof ResourceExportError)
                resourceOutcome = sanitizeResourceOutcome(error.report);
              throw error;
            }
            if (finalizedPack) {
              try {
                legacyPack = await exportLegacyDevicePack({
                  finalizedPack,
                  outputDirectoryHandle: input,
                  signal,
                  onProgress: (progress) => onProgress?.({ stage: "legacy-pack", ...progress }),
                });
                legacyPackOutcome = legacyPack;
              } catch (error) {
                if (error instanceof LegacyPackExportError) legacyPackOutcome = error.report;
                throw error;
              }
            }
            output = createBrowserBlobOutputAdapters({ archiveFormat, maxOutputBytes });
            return output.createOutput(context);
          },
          validateArchive: (context) => output.validateArchive(context),
          async generateProgram(context) {
            const entries = await program.generateProgram(context);
            // 已完成的程序阶段不再使用 WASM；保留其他转换及签名线程。
            // The completed program stage no longer needs WASM; preserve other workers and signing.
            runtime.retireProgramWorker();
            return entries.filter((e) => allowed(e.path));
          },
          async generateAssets(context) {
            if (producer) {
              const root = await navigator.storage.getDirectory();
              context.signal.throwIfAborted();
              finalizedPack = await producer.produce(context.extracted, context.selection, {
                root,
                ...LEGACY_ASTC_PRODUCTION_LIMITS,
                preset: astcPreset,
                signal: context.signal,
                onProgress: createLegacyAstcDiagnosticObserver(onProgress, context.signal),
              });
              context.signal.throwIfAborted();
              assets = createProductionAssetAdapter({
                runtime,
                profile,
                outputCache,
                cacheContext,
                prebuiltPack: { pack: finalizedPack.pack, records: finalizedPack.records },
              });
            }
            const entries = await assets.generateAssets(context);
            const binding = assets.getAstcOutputBinding?.();
            const astcEntries = selectProductionAstcEntries(entries, binding);
            if (binding !== undefined)
              profile = await bindProductionAstcEntries(profile, astcEntries, binding, {
                signal: context.signal,
              });
            // 绑定完成后不再使用资产或纹理计算线程；保留提取及补充包输出租约。
            // Binding is complete; release asset/texture compute while retaining extraction and pack output leases.
            runtime.retireAssetWorkers();
            return entries.filter((entry) => allowed(entry.path));
          },
          async generateShaders(context) {
            const entries = await shaders.generateShaders({
              ...context,
              onProgress: createShaderDiagnosticObserver(onProgress, context.signal),
            });
            // 完整着色器输出已独立持有，不再依赖翻译器或 Cross 线程。
            // Completed shader outputs are independently owned and no longer need translator/Cross workers.
            runtime.retireShaderWorkers({ releasePublicWasm: true });
            return entries;
          },
          async loadShell({ signal }) {
            // 包认证与已提取来源分开；文件夹模式不宣称 DLC 元数据已认证。
            // Distinguish package authentication from local folders without claiming authenticated DLC metadata.
            const inputPolicy = readInputContentPolicy(authenticated, selected, { signal });
            const path = "Payload/DmIOS.app/DmIOS";
            const original = profile.shell.entries.find((entry) => entry.path === path);
            if (!original) throw new Error("SHELL_EXECUTABLE_MISSING");
            const bytes = new Uint8Array(original.size);
            let offset = 0;
            try {
              for await (const chunk of original.chunks()) {
                if (offset + chunk.length > bytes.length) throw new Error("SHELL_SIZE");
                bytes.set(chunk, offset);
                offset += chunk.length;
              }
              if (offset !== bytes.length) throw new Error("SHELL_SIZE");
              const variant = await deriveD2rRuntimeContentMode(bytes, {
                profileId: profile.profileId,
                contentMode: inputPolicy.contentMode,
              });
              shellVariantBytes = variant.bytes;
              return {
                ...profile.shell,
                contentModeReceipt:
                  inputPolicy.inputProvenance === "local-extracted"
                    ? Object.freeze({
                        ...variant.receipt,
                        inputProvenance: inputPolicy.inputProvenance,
                        dlcMetadata: inputPolicy.dlcMetadata,
                      })
                    : variant.receipt,
                entries: profile.shell.entries.map((entry) =>
                  entry.path !== path
                    ? entry
                    : {
                        ...entry,
                        sha256: variant.receipt.sha256,
                        async *chunks() {
                          signal.throwIfAborted();
                          if (!shellVariantBytes) throw new Error("SHELL_CLOSED");
                          yield shellVariantBytes;
                        },
                      },
                ),
              };
            } finally {
              bytes.fill(0);
            }
          },
          async validateGenerated({ generatedEntries, shell, signal }) {
            signal.throwIfAborted();
            if (!shell.contentModeReceipt) throw new Error("SHELL_CONTENT_MODE_UNBOUND");
            const astcEntries = selectProductionAstcEntries(generatedEntries, profile.astcOutput);
            if (profile.astcOutput)
              await inspectProductionAstcEntries(astcEntries, profile.astcOutput, { signal });
            const receiptBytes = new TextEncoder().encode(
              JSON.stringify(shell.contentModeReceipt) + "\n",
            );
            generatedEntries.push({
              path: "dm/content_mode.json",
              size: receiptBytes.length,
              mode: 0o644,
              async *chunks() {
                yield receiptBytes;
              },
            });
            const guest = generatedEntries.find(
              (e) => e.path === "Frameworks/Guest.framework/Guest",
            );
            return {
              valid: true,
              observedOutputTargetOS: await inspectGuestTarget(guest, {
                signal,
                expectedSdkVersion: profile.guestTarget.sdkVersion,
                minimumOSVersion: (
                  await inspectGuestPlist(
                    generatedEntries.find(
                      (e) => e.path === "Frameworks/Guest.framework/Info.plist",
                    ),
                    { profileId: profile.profileId, signal },
                  )
                ).minimumVersion,
              }),
            };
          },
          async cleanup(context) {
            await cleanupPackagingAdapters(() => extraction.cleanup(context), closeConversion);
          },
        };
        const pack = new TextEncoder().encode(
          `PRODUCT=${profile.trustedShell.trustedRecipe.product}\n`,
        );
        const generateProgram = adapters.generateProgram;
        adapters.generateProgram = async (context) => [
          ...(await generateProgram(context)),
          {
            path: "dm/pack_info.txt",
            size: pack.length,
            mode: 0o644,
            async *chunks() {
              yield pack;
            },
          },
        ];
        let result;
        try {
          result = await withLegacyAstcProducerLease(
            producerLease,
            () =>
              runPackagingJob({
                input,
                selectedNspNames,
                inputMode,
                adapters,
                signal,
                onProgress,
                archiveFormat,
                maxOutputBytes,
                selectionOptions: { contentMode: profile.contentMode },
                secretLifetime: "archive",
                concurrency: 1,
              }),
            () => output.getDownload(),
          );
        } catch (error) {
          if (
            error instanceof PackagingJobError &&
            (resourceOutcome || legacyPackOutcome || legacyAstc)
          ) {
            const reported = new PackagingJobError(
              error.cause,
              Object.freeze({
                ...error.report,
                ...(legacyAstc && legacyAstcFailureDetailCode(error)
                  ? { detailCode: legacyAstcFailureDetailCode(error) }
                  : {}),
                ...(resourceOutcome ? { resources: resourceOutcome } : {}),
                ...(legacyPackOutcome ? { legacyPack: legacyPackOutcome } : {}),
              }),
            );
            if (error.cleanupErrors) reported.cleanupErrors = error.cleanupErrors;
            const recovery = takeCommittedDownload(error);
            if (recovery) committedDownloads.set(reported, recovery);
            throw reported;
          }
          throw error;
        }
        let download = output.getDownload();
        let customized;
        try {
          customized = await customizeUnsignedIpa(download.blob, customization, { signal });
          if (customized.changed) download = Object.freeze({ ...download, blob: customized.blob });
        } catch (cause) {
          const error = new PackagingJobError(
            cause,
            Object.freeze({
              status: "failed",
              code: "STAGE_FAILED",
              stage: "customization",
              detailCode: APP_CUSTOMIZATION_ERROR_CODES.includes(cause?.message)
                ? cause.message
                : null,
              committed: true,
              cleanupCodes: Object.freeze([]),
              ...(resourceOutcome ? { resources: resourceOutcome } : {}),
              ...(legacyPackOutcome ? { legacyPack: legacyPackOutcome } : {}),
            }),
          );
          // 外观失败时只恢复原始未签名 IPA；绝不报告定制成功或把它静默交给签名。
          // Recover only the original unsigned IPA on customization failure; never claim success or silently sign it.
          committedDownloads.set(error, download);
          throw error;
        }
        return {
          ...result,
          ...(customized.changed
            ? {
                manifest: customized.manifest,
                entries: customized.manifest.length,
                bytesWritten: customized.blob.size,
                customization: { changed: true, iconMode: customized.iconMode },
              }
            : {}),
          resources,
          ...(legacyPack ? { legacyPack, encoding: finalizedPack.encoding } : {}),
          download,
        };
      },
      close,
    });
  } catch (error) {
    try {
      await close();
    } catch (cleanup) {
      throw new AggregateError([error, cleanup], "PACKAGER_SETUP_CLEANUP_FAILED", { cause: error });
    }
    throw error;
  }
}

function validateGenerationOptions(legacyAstc, astcPreset) {
  if (astcPreset !== "thorough" && astcPreset !== "fast") throw new Error("ASTC_PRESET_OPTION");
  if (typeof legacyAstc !== "boolean") throw new Error("LEGACY_ASTC_OPTION");
}

// 不允许过滤器悄悄丢弃未绑定 ASTC、补充包数据或索引；这些不是 IPA 文件。
// Never let filtering silently drop unbound ASTC, external supplemental data or index files; those are not IPA entries.
export function selectProductionAstcEntries(entries, binding) {
  if (!Array.isArray(entries)) throw new Error("PRODUCTION_ASTC_INVENTORY");
  const selected = [];
  for (const entry of entries) {
    const path = entry?.path;
    if (typeof path !== "string" || path.length > 512) throw new Error("PRODUCTION_ASTC_PATH");
    const basename = path.split("/").at(-1);
    if (
      basename === "pack_index.tsv" ||
      /^data\/.*\.data$/.test(path) ||
      path.startsWith("Legacy Device Pack") ||
      (binding?.pack?.name && basename === binding.pack.name)
    )
      throw new Error("PRODUCTION_EXTERNAL_PACK_IN_IPA");
    if (path.startsWith("romfs_overlay_astc")) selected.push(entry);
  }
  if (binding === undefined && selected.length) throw new Error("PRODUCTION_ASTC_UNBOUND");
  if (binding !== undefined) {
    if (!binding) throw new Error("PRODUCTION_ASTC_INVENTORY");
    const expected = astcOutputPaths(binding);
    if (JSON.stringify(selected.map((entry) => entry.path).sort()) !== JSON.stringify(expected))
      throw new Error("PRODUCTION_ASTC_INVENTORY");
  }
  return selected;
}

// 显式限制单记录和暂存包大小；补充包始终独立于 IPA。
// Explicit per-record and staged-pack limits; supplemental data always stays outside the IPA.
export const LEGACY_ASTC_PRODUCTION_LIMITS = Object.freeze({
  maxEncodedBytes: 256 * 1024 * 1024,
  maxDecodedBytes: 256 * 1024 * 1024,
  maxPackBytes: 8 * 1024 * 1024 * 1024,
  batchBlocks: 4096,
});

// 作业清理结束后才释放文件租约；清理错误不能掩盖原始失败。
// Release the File lease only after job cleanup; cleanup failures must not hide the primary failure.
// 恢复对象只存在于弱映射中，绝不进入错误属性、报告或 JSON 日志。
// Recovery lives only in a weak map, never in error properties, reports or JSON logs.
const committedDownloads = new WeakMap();
export function takeCommittedDownload(error) {
  const download = committedDownloads.get(error);
  committedDownloads.delete(error);
  return download;
}
export async function withLegacyAstcProducerLease(producer, operation, getCommittedDownload) {
  let result, primary;
  let failed = false;
  try {
    result = await operation();
  } catch (error) {
    primary = error;
    failed = true;
  }
  try {
    await producer?.close();
  } catch (cleanup) {
    if (!failed) {
      const error = new PackagingJobError(
        cleanup,
        Object.freeze({
          status: "failed",
          code: "STAGE_FAILED",
          stage: "cleanup",
          committed: result?.committed === true,
          cleanupCodes: Object.freeze(["LEGACY_ASTC_CLEANUP_FAILED"]),
        }),
      );
      error.cleanupErrors = Object.freeze([cleanup]);
      // 只有本次成功返回的作业及其已提交输出可恢复；错误报告中的标志不能授权恢复。
      // Only this successful job and its committed output permit recovery; report flags cannot authorize it.
      if (result?.committed === true && typeof getCommittedDownload === "function") {
        try {
          const download = getCommittedDownload();
          if (download?.blob instanceof Blob && typeof download.fileName === "string")
            committedDownloads.set(
              error,
              Object.freeze({ blob: download.blob, fileName: download.fileName }),
            );
        } catch {
          // 输出不可取回时保留原清理错误，不提供未完成的下载。
          // Preserve the cleanup error without offering an unavailable output.
        }
      }
      throw error;
    }
    const combined = new AggregateError([primary, cleanup], "LEGACY_ASTC_CLEANUP_FAILED", {
      cause: primary,
    });
    if (primary instanceof PackagingJobError) {
      const error = new PackagingJobError(
        primary.cause,
        Object.freeze({
          ...primary.report,
          cleanupCodes: Object.freeze([
            ...(primary.report.cleanupCodes ?? []),
            "LEGACY_ASTC_CLEANUP_FAILED",
          ]),
        }),
      );
      error.cleanupErrors = Object.freeze([cleanup]);
      throw error;
    }
    throw combined;
  }
  if (failed) throw primary;
  return result;
}

// 只转发封闭阶段及实际工作计数；多遍处理字节不是总进度，观察者不能改变转换结果。
// Forward only closed phases and actual work counts; multi-pass bytes are not total progress and observers cannot affect conversion.
export function createLegacyAstcDiagnosticObserver(onDiagnostic, signal) {
  return (progress) => {
    if (signal?.aborted || typeof onDiagnostic !== "function") return;
    if (!progress || !["preparing", "converting", "complete"].includes(progress.phase)) return;
    const count = (value) => Number.isSafeInteger(value) && value >= 0;
    if (
      !count(progress.processedBytes) ||
      !count(progress.completedBlocks) ||
      (progress.completedRecords !== undefined && !count(progress.completedRecords))
    )
      return;
    const diagnostic = Object.freeze({
      kind: "legacy-astc",
      phase: progress.phase,
      processedBytes: progress.processedBytes,
      completedBlocks: progress.completedBlocks,
      ...(progress.completedRecords === undefined
        ? {}
        : { completedRecords: progress.completedRecords }),
    });
    try {
      Promise.resolve(
        onDiagnostic(Object.freeze({ stage: "assets", state: "running", diagnostic })),
      ).catch(() => undefined);
    } catch {
      /* 观察者错误不影响任务。 | Observer failures do not affect the job. */
    }
  };
}

// 只从生产器的直接原因提升封闭诊断，不递归搜索或渲染原始错误。
// Promote closed diagnostics only from the producer's direct cause; never recursively search or render raw errors.
export function legacyAstcFailureDetailCode(error) {
  return error instanceof PackagingJobError && isLegacyAstcDiagnosticCode(error.cause?.detailCode)
    ? error.cause.detailCode
    : undefined;
}

// 跳过不表示已有资源经过验证，也不表示本次导出完成。
// Skipping neither verifies existing resources nor claims an export completed this run.
const RESOURCE_EXPORT_SKIPPED = Object.freeze({ status: "skipped", reason: "user-requested" });
function validateResourceExportOption(value) {
  if (typeof value !== "boolean") throw new Error("RESOURCE_EXPORT_OPTION");
}
export async function runOptionalResourceExport(exportResources = true, exportOriginalResources) {
  validateResourceExportOption(exportResources);
  return exportResources ? await exportOriginalResources() : RESOURCE_EXPORT_SKIPPED;
}

// 两项清理都必须尝试；保留首个原始错误，额外失败不能覆盖它。
// Attempt both cleanup operations; preserve the first original error without hiding subsequent failures.
export async function cleanupPackagingAdapters(cleanupExtraction, closeAdapters) {
  const failures = [];
  for (const release of [cleanupExtraction, closeAdapters]) {
    try {
      await release();
    } catch (error) {
      failures.push(error);
    }
  }
  if (failures.length === 1) throw failures[0];
  if (failures.length > 1)
    throw new AggregateError(failures, "PACKAGING_ADAPTER_CLEANUP_FAILED", { cause: failures[0] });
}

// 仅投影已验证的有界记录数，观察者异常不改变作业结果。
// Project only validated bounded record counts; observer errors cannot change the job result.
export function createShaderDiagnosticObserver(onProgress, signal) {
  return (value) => {
    if (signal?.aborted || typeof onProgress !== "function") return;
    const progress = sanitizeShaderRecordProgress(value);
    if (!progress) return;
    try {
      Promise.resolve(
        onProgress(
          Object.freeze({
            stage: "shaders",
            state: "running",
            diagnostic: Object.freeze({ kind: "shader-records", ...progress }),
          }),
        ),
      ).catch(() => {});
    } catch {
      // 不转发观察者错误或任意私有字段。
      // Never forward observer errors or arbitrary private fields.
    }
  };
}
