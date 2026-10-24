// 浏览器本地打包界面：依据打包进度协议与浏览器专用中英文字表。
// Local browser packaging UI based on the packaging progress protocol and browser-owned Chinese/English table.
import { catalog } from "../i18n-catalog.mjs";
import { t, browserLanguage, setBrowserLanguage, subscribeLanguage } from "../i18n.mjs";
import { FolderOpenIcon, PackageIcon, ShieldCheckIcon } from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState, useSyncExternalStore } from "react";
import { Button } from "@/components/ui/button";
import { Progress } from "@/components/ui/progress";
import {
  Accordion,
  AccordionItem,
  AccordionTrigger,
  AccordionContent,
} from "@/components/ui/accordion";
import { InstallationGuide } from "@/components/InstallationGuide";
import { PreparationFaq } from "@/components/PreparationFaq";
import { PostGenerationGuide } from "@/components/PostGenerationGuide";
import { createPostGenerationGuide } from "../post-generation-guide.mjs";
import { GenerationProgress } from "@/components/GenerationProgress";
import { createGenerationProgress } from "../generation-progress.mjs";
import { Checkbox } from "@/components/ui/checkbox";
import { Input } from "@/components/ui/input";
import { NativeSelect, NativeSelectOption } from "@/components/ui/native-select";
import {
  Field,
  FieldSet,
  FieldLegend,
  FieldGroup,
  FieldContent,
  FieldLabel,
  FieldDescription,
} from "@/components/ui/field";
import {
  AppCustomizationFields,
  type AppCustomizationDraft,
} from "@/components/AppCustomizationFields";
import { prepareAppCustomization } from "../app-customization.mjs";
import {
  normalizeAppCustomizationDraft,
  resolveAppCustomizationDefaults,
} from "../app-customization-defaults.mjs";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { prepareBrowserPackager, takeCommittedDownload } from "../browser-packager.mjs";
import { DEFAULT_SHELL_PROFILE_ID, IOS18_SHELL_PROFILE_ID } from "../shell-profiles.mjs";
import { discoverGameFolder } from "../game-input.mjs";
import { PROGRAM_COMPATIBILITY } from "../program-compatibility.mjs";
import { createDownloadPreparation, attemptAutomaticDownload } from "../download-preparation.mjs";
import { signWithLocalFiles } from "../signing-input.mjs";
import { IPA_SIGNING_LIMITS } from "../ipa-signing-contract.mjs";
import { createUiOperationGate } from "../ui-operation-gate.mjs";
import { getGenerationReadiness } from "../generation-readiness.mjs";
import { readOpfsStorageAdvisory } from "../opfs-storage-advisory.mjs";
import {
  packagingActivity,
  packagingFailureMessage,
  packagingFailureSummary,
  shaderFailureDetails,
  committedCleanupWarning,
  resourceProgressLabel,
  shaderProgressLabel,
  gameFolderFailureMessage,
  appCustomizationFailureMessage,
} from "../packaging-diagnostics.mjs";

type LocalPickers = typeof globalThis & {
  showDirectoryPicker?: (options: {
    mode: "read" | "readwrite";
  }) => Promise<FileSystemDirectoryHandle>;
};
const browser = globalThis as LocalPickers;
const supportsDirectoryPicker = typeof browser.showDirectoryPicker === "function";
type Packager = Awaited<ReturnType<typeof prepareBrowserPackager>>;
type LoadingProgress = {
  phase: string;
  loadedBytes: number;
  totalBytes: number;
  downloadedBytes: number;
  cachedBytes: number;
  completedFiles: number;
  totalFiles: number;
  label: string;
};
type InputMode = "packages" | "extracted";
type FolderProgress = {
  inputMode?: string;
  extractedFileCount?: number;
  nspCount: number;
  prodKeysFound: boolean;
  titleKeysFound: boolean;
};
type ActivityEntry = { id: number; text: string };
type GenerationTracker = ReturnType<typeof createGenerationProgress>;
const folderSummary = (progress: FolderProgress) =>
  progress.inputMode === "extracted"
    ? t("dmWebExtractedSummary", progress.extractedFileCount ?? 0)
    : progress.inputMode === "packages"
      ? t(
          "dmWebFolderSummary",
          progress.nspCount,
          t(progress.prodKeysFound ? "dmWebFound" : "dmWebMissing"),
          t(progress.titleKeysFound ? "dmWebFoundOptional" : "dmWebNotProvided"),
        )
      : t("dmWebDetectingInput");
const megabytes = (bytes: number) => t("dmWebMegabytes", (bytes / 1_000_000).toFixed(1));
const loadingLabel = (phase: string) =>
  t(phase === "download" ? "dmWebDownloading" : "dmWebPreparing");
export default function App() {
  const language = useSyncExternalStore(subscribeLanguage, browserLanguage, browserLanguage);
  const [target, setTarget] = useState(() =>
    new URLSearchParams(window.location.search).get("target") === "ios18" ? "ios18" : "ios26",
  );
  const [legacyAstc, setLegacyAstc] = useState(
    () => new URLSearchParams(window.location.search).get("legacyAstc") === "1",
  );
  const [astcPreset, setAstcPreset] = useState<"thorough" | "fast">(() =>
    new URLSearchParams(window.location.search).get("astcPreset") === "fast" ? "fast" : "thorough",
  );
  const [signing, setSigning] = useState(
    () => new URLSearchParams(window.location.search).get("signing") === "1",
  );
  const [signingReady, setSigningReady] = useState(false);
  const [signingStatus, setSigningStatus] = useState("");
  const [signingRetryAvailable, setSigningRetryAvailable] = useState(false);
  const [openOptions, setOpenOptions] = useState<string[]>([]);
  const unsignedForSigning = useRef<{ blob: Blob; fileName: string } | null>(null);
  const identityInput = useRef<HTMLInputElement>(null);
  const profileInput = useRef<HTMLInputElement>(null);
  const passwordInput = useRef<HTMLInputElement>(null);
  const [signingFiles, setSigningFiles] = useState({ identity: false, profile: false });
  const credentialsSelected = signingFiles.identity && signingFiles.profile;
  const [legacyStatus, setLegacyStatus] = useState("");
  const [skipResources, setSkipResources] = useState(false);
  const [ignoreCache, setIgnoreCache] = useState(false);
  const [resourceOutcome, setResourceOutcome] = useState("");
  const [storageAdvisory, setStorageAdvisory] = useState("");
  const profileId = target === "ios18" ? IOS18_SHELL_PROFILE_ID : DEFAULT_SHELL_PROFILE_ID;
  const supportedVersions = PROGRAM_COMPATIBILITY.releases
    .filter((release) =>
      PROGRAM_COMPATIBILITY.shellProfiles
        .find((profile) => profile.id === profileId)
        ?.programReleaseIds.includes(release.id),
    )
    .map((release) => release.gameVersion)
    .join(", ");
  const [inputMode, setInputMode] = useState<InputMode | null>(null);
  const [appCustomization, setAppCustomization] = useState<AppCustomizationDraft>({});
  const appCustomizationDefaults = useMemo(
    () => resolveAppCustomizationDefaults(profileId),
    [profileId],
  );
  const [customizationError, setCustomizationError] = useState("");
  const customizationDraftValidation = useMemo(() => {
    try {
      return {
        options: normalizeAppCustomizationDraft(appCustomization, appCustomizationDefaults),
        error: null,
      };
    } catch (error) {
      return { options: undefined, error };
    }
  }, [appCustomization, appCustomizationDefaults]);
  // 在渲染期间翻译当前校验结果，不在缓存的规范化过程中翻译。
  // Translate the current validation result during render, not inside the cached normalization.
  const customizationValidation = {
    options: customizationDraftValidation.options,
    error:
      customizationDraftValidation.error === null
        ? ""
        : appCustomizationFailureMessage(customizationDraftValidation.error),
  };
  useEffect(() => {
    document.documentElement.lang = language;
    document.title = t("dmWebTitle");
  }, [language]);
  const [activity, setActivity] = useState<ActivityEntry[]>([]);
  const [copyStatus, setCopyStatus] = useState("");
  const lastActivity = useRef("");
  const activityBuffer = useRef<ActivityEntry[]>([]);
  const activityRevision = useRef(0);
  const renderedActivityRevision = useRef(0);
  const copiedActivityRevision = useRef<number | null>(null);
  const copyRequest = useRef(0);
  const activityTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pendingGenerationStatus = useRef<string | null>(null);
  const generationTracker = useRef<GenerationTracker | null>(null);
  const pendingOverallProgress = useRef<{
    tracker: GenerationTracker;
    snapshot: ReturnType<GenerationTracker["snapshot"]>;
  } | null>(null);
  const [overallProgress, setOverallProgress] = useState<ReturnType<
    GenerationTracker["snapshot"]
  > | null>(null);
  const publishProgress = useCallback((tracker: GenerationTracker | null) => {
    if (mounted.current && tracker && generationTracker.current === tracker) {
      pendingOverallProgress.current = null;
      setOverallProgress(tracker.snapshot());
    }
  }, []);
  const [loading, setLoading] = useState<LoadingProgress | null>(null);
  const [loadFailed, setLoadFailed] = useState(false);
  const [status, setStatus] = useState(t("dmWebLoading"));
  const [ready, setReady] = useState(false),
    [busy, setBusy] = useState(false);
  const [generating, setGenerating] = useState(false);
  const [cancelling, setCancelling] = useState(false);
  const [folder, setFolder] = useState<FileSystemDirectoryHandle | null>(null);
  const [nspNames, setNspNames] = useState<string[]>([]);
  const [download, setDownload] = useState<{ url: string; fileName: string } | null>(null);
  const [completedOutput, setCompletedOutput] =
    useState<ReturnType<typeof createPostGenerationGuide>>(null);
  const signedDownload = download?.fileName === "D2RMobile-signed.ipa";
  const [finished, setFinished] = useState(false);
  const downloadPreparation = useRef(createDownloadPreparation());
  const [downloadPreparationFailed, setDownloadPreparationFailed] = useState(false);
  const [originalIdentityDownload, setOriginalIdentityDownload] = useState(false);
  const generationReadiness = getGenerationReadiness({
    ready,
    busy,
    finished,
    hasDownload: download !== null,
    hasFolder: folder !== null,
    inputMode,
    packageCount: nspNames.length,
    customizationError: customizationValidation.error || customizationError,
    signing,
    signingReady,
    identitySelected: signingFiles.identity,
    profileSelected: signingFiles.profile,
  });
  const generationReadinessMessage = generationReadiness.messageKey
    ? [t(generationReadiness.messageKey), generationReadiness.detail].filter(Boolean).join(" ")
    : "";
  const mounted = useRef(true);
  const operationGate = useRef(createUiOperationGate());
  const flushActivity = useCallback(() => {
    if (activityTimer.current !== null) {
      clearTimeout(activityTimer.current);
      activityTimer.current = null;
    }
    if (!mounted.current) return;
    if (renderedActivityRevision.current !== activityRevision.current) {
      renderedActivityRevision.current = activityRevision.current;
      setActivity([...activityBuffer.current]);
      if (copiedActivityRevision.current !== activityRevision.current) setCopyStatus("");
    }
    if (pendingGenerationStatus.current !== null) {
      setStatus(pendingGenerationStatus.current);
      pendingGenerationStatus.current = null;
    }
    const pendingProgress = pendingOverallProgress.current;
    pendingOverallProgress.current = null;
    if (pendingProgress && generationTracker.current === pendingProgress.tracker)
      setOverallProgress(pendingProgress.snapshot);
  }, []);
  const appendActivity = useCallback(
    (text: string | null, force = false) => {
      if (!mounted.current) return;
      if (text && (force || text !== lastActivity.current)) {
        lastActivity.current = text;
        activityRevision.current += 1;
        // 删除最旧记录时保留其他行的身份，避免按移动后的索引重新挂载。
        // Preserve surviving row identities when trimming, instead of remounting by shifted indexes.
        activityBuffer.current.push({
          id: activityRevision.current,
          text: `${new Date().toLocaleString()} ${text}`,
        });
        if (activityBuffer.current.length > 200) activityBuffer.current.shift();
      }
      // 保留有界日志中的每条记录，但合并短暂的界面更新。
      // 终止事件立即刷新此前的记录。
      // Preserve every bounded log entry, but coalesce transient UI updates.
      // Terminal events flush the preceding entries immediately.
      if (force) flushActivity();
      else if (activityTimer.current === null) {
        activityTimer.current = setTimeout(flushActivity, 250);
      }
    },
    [flushActivity],
  );
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      if (activityTimer.current !== null) clearTimeout(activityTimer.current);
      activityTimer.current = null;
      pendingGenerationStatus.current = null;
      pendingOverallProgress.current = null;
    };
  }, []);
  const packager = useRef<Packager | null>(null),
    cancellation = useRef<AbortController | null>(null);
  useEffect(() => {
    const preparedDownload = downloadPreparation.current;
    const operations = operationGate.current;
    mounted.current = true;
    const controller = new AbortController();
    cancellation.current = controller;
    const digest = document.querySelector<HTMLMetaElement>(
      'meta[name="runtime-manifest-sha256"]',
    )?.content;
    if (!supportsDirectoryPicker) {
      setStatus(t("dmWebFolderAccessUnsupported"));
      return;
    }
    if (!digest || !/^[a-f0-9]{64}$/.test(digest)) {
      setStatus(t("dmWebUnsupported"));
      return;
    }
    appendActivity(t("dmWebLoadingVerified"), true);
    appendActivity(t("dmWebManifest", digest), true);
    let previousPhase = "";
    void prepareBrowserPackager({
      manifestSha256: digest,
      editableOptions: true,
      signal: controller.signal,
      onProgress: (event: LoadingProgress) => {
        if (!controller.signal.aborted) {
          setLoading(event);
          setStatus(loadingLabel(event.phase));
          if (event.phase !== previousPhase) {
            previousPhase = event.phase;
            appendActivity(
              event.phase === "download" ? t("dmWebDownloading") : t("dmWebPreparing"),
              true,
            );
          }
        }
      },
    })
      .then((value) => {
        if (controller.signal.aborted) {
          value.close();
          return;
        }
        packager.current = value;
        setLoading(null);
        setReady(true);
        setSigningReady(value.signingReady);
        // 空间估计不预留空间，也不阻止继续；忽略取消或卸载后的结果。
        // Estimates neither reserve space nor block continuation; ignore cancelled or unmounted results.
        void readOpfsStorageAdvisory({ signal: controller.signal }).then((estimate) => {
          if (controller.signal.aborted || !mounted.current || estimate.status === "cancelled")
            return;
          setStorageAdvisory(
            estimate.status === "estimated" && estimate.availableBytes !== null
              ? t("dmWebStorageEstimate", megabytes(estimate.availableBytes))
              : t("dmWebStorageUnknown"),
          );
        });
        setStatus(t("dmWebReady"));
        appendActivity(t("dmWebApplicationReady"), true);
      })
      .catch((error: unknown) => {
        if (!controller.signal.aborted) {
          setLoadFailed(true);
          appendActivity(t("dmWebSetupFailed"), true);
          setStatus(
            t(
              "dmWebSetupError",
              String(error instanceof Error ? error.message : error).slice(0, 1400),
            ),
          );
        }
      });
    return () => {
      mounted.current = false;
      controller.abort();
      operations.cancel();
      cancellation.current?.abort();
      packager.current?.close();
      preparedDownload.dispose();
      unsignedForSigning.current = null;
    };
  }, [appendActivity]);
  useEffect(() => {
    // URL 仅镜像公开选项；状态变化不会重新初始化运行时或清除文件与下载。
    // URL mirrors public options only; changes never restart runtimes or clear files/downloads.
    const url = new URL(window.location.href);
    for (const [key, value] of [
      ["target", target === "ios18" ? "ios18" : null],
      ["legacyAstc", legacyAstc ? "1" : null],
      ["astcPreset", astcPreset === "fast" ? "fast" : null],
      ["signing", signing ? "1" : null],
    ] as const) {
      if (value === null) url.searchParams.delete(key);
      else url.searchParams.set(key, value);
    }
    window.history.replaceState(window.history.state, "", url.href);
  }, [target, legacyAstc, astcPreset, signing]);
  async function choose() {
    if (!ready || busy || !browser.showDirectoryPicker) return;
    const operation = operationGate.current.begin();
    if (!operation) return;
    const current = () => mounted.current && operation.isCurrent();
    const previousStatus = status;
    const jobPackager = packager.current;
    const controller = operation.controller;
    cancellation.current = controller;
    let selectedName = "";
    let progress: FolderProgress = {
      nspCount: 0,
      prodKeysFound: false,
      titleKeysFound: false,
    };
    setBusy(true);
    setCancelling(false);
    setStatus(t("dmWebWaitingFolder"));
    try {
      const selected = await browser.showDirectoryPicker({ mode: "readwrite" });
      controller.signal.throwIfAborted();
      if (!current()) return;
      setSkipResources(false);
      selectedName = selected.name;
      setFolder(null);
      setNspNames([]);
      setInputMode(null);
      setStatus(t("dmWebChecking", selected.name));
      const files = await discoverGameFolder(selected, {
        signal: controller.signal,
        onProgress: (event: FolderProgress) => {
          if (controller.signal.aborted || !current()) return;
          progress = event;
          setStatus(t("dmWebCheckingProgress", selected.name, folderSummary(event)));
        },
      });
      controller.signal.throwIfAborted();
      if (!current()) return;
      const names = files.nsps.map((file) => file.name);
      setNspNames(names);
      setInputMode(files.inputMode);
      setFolder(selected);
      appendActivity(
        files.inputMode === "extracted"
          ? t("dmWebExtractedChecked")
          : t(
              "dmWebFolderChecked",
              progress.nspCount,
              t(progress.titleKeysFound ? "dmWebProvided" : "dmWebNotProvided"),
            ),
        true,
      );
      setStatus(
        t(
          files.inputMode === "extracted" ? "dmWebExtractedReady" : "dmWebPackagesReady",
          selected.name,
          folderSummary({ ...progress, inputMode: files.inputMode }),
        ),
      );
    } catch (error) {
      if (!current()) return;
      if (
        controller.signal.aborted ||
        (error instanceof DOMException && error.name === "AbortError")
      ) {
        setStatus(controller.signal.aborted ? t("dmWebCancelledReload") : previousStatus);
      } else {
        setFolder(null);
        setNspNames([]);
        setInputMode(null);
        appendActivity(t("dmWebFolderFailed"), true);
        const code = (error as { code?: string })?.code;
        const permissionError =
          error instanceof DOMException &&
          ["NotAllowedError", "SecurityError"].includes(error.name);
        const reason = permissionError
          ? t("dmWebFolderPermission")
          : gameFolderFailureMessage(code);
        setStatus(`${selectedName ? `${selectedName}: ${folderSummary(progress)} ` : ""}${reason}`);
      }
    } finally {
      // 显式取消后必须重载，立即关闭已预加载的运行时；仅关闭选取器仍允许重试。
      // Explicit cancellation requires reload: release preloaded runtimes now; picker dismissal stays retryable.
      if (controller.signal.aborted && current()) {
        try {
          await jobPackager?.close();
        } catch {
          if (current()) appendActivity(t("dmWebCleanupWarning"), true);
        }
      }
      if (current()) {
        setBusy(false);
        setCancelling(false);
        if (controller.signal.aborted) setFinished(true);
      }
      operation.finish();
    }
  }
  function prepareDownload(automatic = !originalIdentityDownload) {
    const prepared = downloadPreparation.current.prepare();
    const tracker = generationTracker.current;
    tracker?.downloadPrepared(prepared.status === "ready");
    publishProgress(tracker);
    if (prepared.status !== "ready") {
      setDownloadPreparationFailed(true);
      appendActivity(t("dmWebDownloadPreparationFailed"), true);
      setStatus(t("dmWebDownloadPreparationFailed"));
      return;
    }
    setDownload({ url: prepared.url, fileName: prepared.fileName });
    setDownloadPreparationFailed(false);
    const requestedOutputComplete = tracker?.snapshot().status === "complete";
    if (tracker?.included.signing && requestedOutputComplete) {
      setSigningStatus(t("dmWebSigningComplete"));
      setStatus(t("dmWebSigningComplete"));
    }
    // 签名作业的未签名恢复产物只能手动下载，不能冒充已完成的请求产物。
    // Unsigned recovery from a signing job is manual-only, never the completed requested output.
    if (automatic && (!tracker?.included.signing || requestedOutputComplete))
      attemptAutomaticDownload(prepared);
  }
  function clearSigningInputs() {
    if (identityInput.current) identityInput.current.value = "";
    if (profileInput.current) profileInput.current.value = "";
    if (passwordInput.current) passwordInput.current.value = "";
    if (mounted.current) setSigningFiles({ identity: false, profile: false });
  }
  async function runSigning(
    jobPackager: Packager,
    unsigned: { blob: Blob; fileName: string },
    controller: AbortController,
    current: () => boolean,
  ): Promise<boolean> {
    const tracker = generationTracker.current;
    let acceptingSigningProgress = true;
    pendingGenerationStatus.current = null;
    setDownload(null);
    setDownloadPreparationFailed(false);
    tracker?.startSigning();
    publishProgress(tracker);
    setSigningRetryAvailable(false);
    setSigningStatus(t("dmWebSigningWorking"));
    setStatus(t("dmWebSigningWorking"));
    let attempt: Awaited<ReturnType<Packager["prepareSigning"]>> | undefined;
    try {
      // 先初始化独立线程，再读取凭据；重试不再运行转换或资源导出。
      // Initialize the independent worker before reading credentials; retries never rerun conversion or resource export.
      attempt = await jobPackager.prepareSigning({ signal: controller.signal });
      controller.signal.throwIfAborted();
      if (!current()) return false;
      const signed = await signWithLocalFiles({
        unsigned,
        sign: attempt.sign,
        identityFile: identityInput.current?.files?.[0],
        profileFile: profileInput.current?.files?.[0],
        password: passwordInput.current?.value ?? "",
        signal: controller.signal,
        onProgress: (event: { stage: string; completed: number; total: number }) => {
          if (!acceptingSigningProgress || !current() || controller.signal.aborted) return;
          tracker?.observeSigning(event);
          publishProgress(tracker);
        },
      });
      acceptingSigningProgress = false;
      controller.signal.throwIfAborted();
      if (!current()) return false;
      pendingGenerationStatus.current = null;
      downloadPreparation.current.retain(signed);
      tracker?.signed(signed);
      publishProgress(tracker);
      unsignedForSigning.current = null;
      setSigningStatus("");
      prepareDownload();
      return false;
    } catch (signingError) {
      if (!current()) return false;
      // 固定消息不包含凭据；失败不改变未签名 IPA 或已完成的资源导出。
      // Fixed messages contain no credentials; failure does not change the unsigned IPA or completed resource exports.
      const signingMessage = t(
        controller.signal.aborted
          ? "dmWebSigningCancelled"
          : signingError instanceof Error &&
              signingError.message === "SIGNING_APPLICATION_ID_TOO_LONG"
            ? "dmWebSigningApplicationIdTooLong"
            : signingError instanceof Error && signingError.message === "SIGNING_PROFILE_MISMATCH"
              ? "dmWebSigningProfileMismatch"
              : "dmWebSigningFailed",
      );
      tracker?.signingFailed(controller.signal.aborted);
      publishProgress(tracker);
      pendingGenerationStatus.current = null;
      setSigningStatus(signingMessage);
      appendActivity(signingMessage, true);
      setStatus(signingMessage);
      setSigningRetryAvailable(true);
      prepareDownload(false);
      return true;
    } finally {
      acceptingSigningProgress = false;
      attempt?.close();
      clearSigningInputs();
    }
  }
  async function retrySigning() {
    const unsigned = unsignedForSigning.current;
    if (
      !signingRetryAvailable ||
      !unsigned ||
      !packager.current ||
      !signingReady ||
      !identityInput.current?.files?.[0] ||
      !profileInput.current?.files?.[0]
    )
      return;
    const operation = operationGate.current.begin();
    if (!operation) return;
    const jobPackager = packager.current;
    const current = () => mounted.current && operation.isCurrent();
    cancellation.current = operation.controller;
    setBusy(true);
    setGenerating(true);
    setCancelling(false);
    pendingGenerationStatus.current = null;
    let keepSigningFactory = false;
    try {
      keepSigningFactory = await runSigning(jobPackager, unsigned, operation.controller, current);
    } finally {
      try {
        if (!keepSigningFactory || !current()) await jobPackager.close();
      } catch {
        if (current()) {
          const warning = t("dmWebCleanupWarning");
          appendActivity(warning, true);
          setStatus((previous: string) => `${previous} ${warning}`);
        }
      }
      if (current()) {
        setBusy(false);
        setGenerating(false);
        setCancelling(false);
      }
      operation.finish();
    }
  }
  async function generate() {
    if (!generationReadiness.canGenerate || !packager.current || !folder || !inputMode) return;
    if (
      signing &&
      (!signingReady || !identityInput.current?.files?.[0] || !profileInput.current?.files?.[0])
    )
      return;
    const operation = operationGate.current.begin();
    if (!operation) return;
    const jobOptions = Object.freeze({
      profileId,
      legacyAstc,
      signing,
      astcPreset,
      ignoreCache,
      exportResources: !skipResources,
      inputMode,
    });
    const tracker = createGenerationProgress(jobOptions);
    generationTracker.current = tracker;
    publishProgress(tracker);
    const jobPackager = packager.current;
    const current = () => mounted.current && operation.isCurrent();
    const controller = operation.controller;
    cancellation.current = controller;
    setBusy(true);
    setGenerating(true);
    setCancelling(false);
    setCustomizationError("");
    let preparedCustomization;
    try {
      // 先检查本地选项；验证失败不消耗一次性打包器，允许更正后重试。
      // Check local options first; invalid input does not consume the one-shot packager and can be corrected.
      setStatus(t("dmWebCustomChecking"));
      preparedCustomization = await prepareAppCustomization(customizationValidation.options, {
        signal: controller.signal,
      });
      controller.signal.throwIfAborted();
      if (!current()) {
        operation.finish();
        return;
      }
    } catch (error) {
      if (current()) {
        tracker.fail(controller.signal.aborted);
        publishProgress(tracker);
        if (controller.signal.aborted) {
          setReady(false);
          setFinished(true);
          setStatus(t("dmWebCancelledReload"));
          if (jobOptions.signing) clearSigningInputs();
          try {
            await jobPackager.close();
          } catch {
            if (current()) appendActivity(t("dmWebCleanupWarning"), true);
          }
        } else {
          setCustomizationError(appCustomizationFailureMessage(error));
          setStatus(t("dmWebCustomCorrect"));
        }
        if (current()) {
          setBusy(false);
          setGenerating(false);
          setCancelling(false);
        }
      }
      operation.finish();
      return;
    }
    tracker.prepared();
    publishProgress(tracker);
    setResourceOutcome("");
    setCompletedOutput(null);
    setCancelling(false);
    setReady(false);
    setStatus(t("dmWebStarting"));
    if (jobOptions.legacyAstc) setLegacyStatus(t("dmWebLegacyPreparing"));
    appendActivity(t("dmWebGenerationStarted"), true);
    if (jobOptions.ignoreCache) appendActivity(t("dmWebIgnoreCacheActive"), true);
    if (jobOptions.inputMode === "packages") appendActivity(t("dmWebSkipHashActive"), true);
    else appendActivity(t("dmWebExtractedChecked"), true);
    let acceptingProgress = true;
    let generationCompleted = false;
    let keepSigningFactory = false;
    try {
      const result = await jobPackager.generate({
        profileId: jobOptions.profileId,
        legacyAstc: jobOptions.legacyAstc,
        astcPreset: jobOptions.astcPreset,
        ignoreCache: jobOptions.ignoreCache,
        input: folder,
        inputMode: jobOptions.inputMode,
        ...(Object.keys(preparedCustomization).length
          ? { appCustomization: preparedCustomization }
          : {}),
        ...(jobOptions.inputMode === "packages" ? { selectedNspNames: nspNames } : {}),
        exportResources: jobOptions.exportResources,
        signal: controller.signal,
        onProgress: (event: {
          phase?: string;
          status?: string;
          label?: string;
          stage?: string;
          state?: string;
          bytesWritten?: number;
          totalBytes?: number;
          directoryName?: string;
          diagnostic?: {
            stage?: string;
            completed?: number;
            kind?: string;
            processedBytes?: number;
            completedBlocks?: number;
            completedRecords?: number;
            totalRecords?: number;
          };
          resourceProgress?: {
            filesWritten: number;
            totalFiles: number;
            bytesWritten: number;
            totalBytes: number;
          };
        }) => {
          if (!acceptingProgress || controller.signal.aborted || !current()) return;
          if (event.phase === "cache") {
            const cacheLabel = packagingActivity(event);
            if (cacheLabel) {
              pendingGenerationStatus.current = cacheLabel;
              appendActivity(cacheLabel, true);
            }
            return;
          }
          tracker.observe(event);
          pendingOverallProgress.current = { tracker, snapshot: tracker.snapshot() };
          appendActivity(null);
          const shaderLabel = shaderProgressLabel(event);
          if (shaderLabel) {
            // 记录计数只更新合并后的状态行，不逐条写入活动日志。
            // Record counts update only the coalesced status line, not per-record activity logs.
            pendingGenerationStatus.current = shaderLabel;
            appendActivity(null);
            return;
          }
          // 补充包导出单独显示，不把它的完成状态当作 IPA 完成。
          // Show the supplemental export separately; its completion is not IPA completion.
          if (
            jobOptions.legacyAstc &&
            event.stage === "assets" &&
            event.diagnostic?.kind === "legacy-astc"
          ) {
            const blocks = event.diagnostic.completedBlocks;
            const bytes = event.diagnostic.processedBytes;
            if (
              typeof blocks === "number" &&
              Number.isSafeInteger(blocks) &&
              blocks >= 0 &&
              typeof bytes === "number" &&
              Number.isSafeInteger(bytes) &&
              bytes >= 0
            )
              setLegacyStatus(t("dmWebLegacyConverting", blocks, (bytes / 1_000_000).toFixed(1)));
            return;
          }
          if (jobOptions.legacyAstc && event.stage === "legacy-pack") {
            setLegacyStatus(
              event.state === "complete"
                ? t("dmWebLegacyComplete", event.directoryName ?? "")
                : t(
                    "dmWebLegacySaving",
                    megabytes(event.bytesWritten ?? 0),
                    megabytes(event.totalBytes ?? 0),
                  ),
            );
            return;
          }
          const activityLabel = packagingActivity(event);
          pendingGenerationStatus.current =
            resourceProgressLabel(event) ?? activityLabel ?? t("dmWebInProgress");
          appendActivity(activityLabel, event.state === "failed" || event.state === "complete");
        },
      });
      if (!current()) return;
      acceptingProgress = false;
      pendingGenerationStatus.current = null;
      generationCompleted = true;
      setCompletedOutput(
        createPostGenerationGuide(
          result,
          preparedCustomization.displayName ?? appCustomizationDefaults.displayName,
        ),
      );
      tracker.generated(result);
      publishProgress(tracker);
      if (jobOptions.legacyAstc && result.legacyPack?.status === "complete")
        setLegacyStatus(t("dmWebLegacyComplete", result.legacyPack.directoryName));
      downloadPreparation.current.retain(result.download);
      if (result.resources.status === "skipped") {
        setResourceOutcome(t("dmWebResourcesSkipped"));
      } else if (result.resources.status === "complete") {
        setResourceOutcome(t("dmWebResourcesSaved", result.resources.directoryName));
      } else {
        setResourceOutcome(t("dmWebResultIncomplete"));
      }
      // 请求签名时先保留未签名数据；签名完成前不发布下载链接或就绪消息。
      // Retain unsigned bytes when signing is requested; publish no download or ready message before signing finishes.
      if (jobOptions.signing) {
        unsignedForSigning.current = result.download;
        keepSigningFactory = await runSigning(jobPackager, result.download, controller, current);
      } else {
        appendActivity(t("dmWebUnsignedReady"), true);
        setStatus(
          result.resources.status === "complete"
            ? t("dmWebDownloadReady", folder.name, result.resources.directoryName)
            : t("dmWebUnsignedReady"),
        );
        prepareDownload();
      }
    } catch (error) {
      if (!current()) return;
      if (generationCompleted) {
        tracker.downloadPrepared(false);
        publishProgress(tracker);
        setDownloadPreparationFailed(true);
        appendActivity(t("dmWebDownloadPreparationFailed"), true);
        setStatus(t("dmWebDownloadPreparationFailed"));
        return;
      }
      tracker.fail(controller.signal.aborted);
      publishProgress(tracker);
      const report = (
        error as {
          report?: {
            stage?: string;
            code?: string;
            detailCode?: string;
            shaderDiagnostic?: unknown;
            outputStarted?: boolean;
            legacyPack?: { status?: string; directoryName?: string };
            resources?: { status?: string; directoryName?: string };
          };
        }
      )?.report;
      if (report?.resources?.status === "skipped") setResourceOutcome(t("dmWebResourcesSkipped"));
      if (jobOptions.legacyAstc)
        setLegacyStatus(
          report?.legacyPack?.status === "complete"
            ? t("dmWebLegacyComplete", report.legacyPack.directoryName ?? "")
            : t("dmWebLegacyIncomplete"),
        );
      const recovered = takeCommittedDownload(error);
      if (recovered) {
        if (report?.resources?.status === "complete")
          setResourceOutcome(t("dmWebResourcesSaved", report.resources.directoryName ?? ""));
        downloadPreparation.current.retain(recovered);
        // 生成成功返回前恢复的都是原始 IPA；清理也可能在自定义开始前失败。
        // Every recovery before successful generation is the original IPA; cleanup can fail before customization starts.
        const originalIdentity = Object.keys(preparedCustomization).length > 0;
        setOriginalIdentityDownload(originalIdentity);
        setCompletedOutput(createPostGenerationGuide(report, appCustomizationDefaults.displayName));
        const warning = originalIdentity
          ? `${report?.stage === "customization" ? "" : `${committedCleanupWarning()} `}${t("dmWebCustomOriginalRetained")}`
          : committedCleanupWarning();
        appendActivity(warning, true);
        setStatus(warning);
        // 自定义失败只提供明确标注的原始下载，不自动下载或签名。
        // Customization failures offer a labeled original download, without automatic download or signing.
        prepareDownload(!originalIdentity && !jobOptions.signing);
        return;
      }
      appendActivity(packagingFailureSummary(report), true);
      for (const detail of shaderFailureDetails(report)) appendActivity(detail, true);
      setStatus(packagingFailureMessage(report));
    } finally {
      acceptingProgress = false;
      if (current()) {
        if (jobOptions.signing) clearSigningInputs();
        pendingGenerationStatus.current = null;
      }
      // 仅签名失败保留公开签名工厂；转换失败或签名成功关闭全部所有权。
      // Only signing failure retains the public signing factory; conversion failure or signing success closes every owner.
      try {
        if (!keepSigningFactory || !current()) await jobPackager.close();
      } catch {
        if (current()) {
          const warning = t("dmWebCleanupWarning");
          appendActivity(warning, true);
          setStatus((previous: string) => `${previous} ${warning}`);
        }
      }
      if (current()) {
        pendingGenerationStatus.current = null;
        setBusy(false);
        setGenerating(false);
        setCancelling(false);
        setFinished(true);
      }
      operation.finish();
    }
  }
  return (
    <main className="mx-auto flex min-h-screen max-w-3xl items-start px-4 py-8 sm:items-center sm:px-6 sm:py-12">
      <Card className="w-full gap-5 overflow-hidden border-primary/15 pt-0 shadow-sm">
        <CardHeader className="gap-3 border-b border-primary/10 bg-linear-to-br from-primary/10 via-card to-card py-6">
          <Field orientation="horizontal" className="ml-auto w-fit gap-2">
            <FieldLabel htmlFor="ui-language">
              {catalog.dmWebLanguageLabel[0]} / {catalog.dmWebLanguageLabel[1]}
            </FieldLabel>
            <NativeSelect
              id="ui-language"
              value={language}
              onChange={(event) => setBrowserLanguage(event.target.value)}
            >
              <NativeSelectOption value="en" lang="en">
                {catalog.dmWebLanguageEnglish[0]}
              </NativeSelectOption>
              <NativeSelectOption value="zh-CN" lang="zh-CN">
                {catalog.dmWebLanguageChinese[1]}
              </NativeSelectOption>
            </NativeSelect>
          </Field>
          <div className="flex items-center gap-3">
            <span className="flex size-11 shrink-0 items-center justify-center rounded-xl bg-primary text-primary-foreground">
              <PackageIcon className="size-6" aria-hidden="true" />
            </span>
            <CardTitle role="heading" aria-level={1} className="text-2xl tracking-tight">
              {t("dmWebTitle")}
            </CardTitle>
          </div>
          {!supportsDirectoryPicker && (
            <p
              role="alert"
              className="rounded-lg border border-destructive/30 bg-destructive/5 p-3 text-sm text-foreground"
            >
              {t("dmWebFolderAccessUnsupported")}
            </p>
          )}
          <CardDescription className="font-medium text-foreground">
            {t("dmWebBrowserOnly")}
          </CardDescription>
          <CardDescription>{t("dmWebAutoInstructions")}</CardDescription>
          <CardDescription className="text-xs">
            {t(
              "dmWebOutputCompatibility",
              t(target === "ios18" ? "dmWebTarget18" : "dmWebTarget26"),
              t(legacyAstc ? "dmWebLegacyTextures" : "dmWebBcTextures"),
            )}
          </CardDescription>
          <CardDescription className="text-xs">{t("dmWebOptionsBeforeFolder")}</CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          <InstallationGuide />
          <AppCustomizationFields
            section="identity"
            value={appCustomization}
            defaults={appCustomizationDefaults}
            disabled={!ready || busy || finished || download !== null}
            error={customizationValidation.error || customizationError}
            onChange={(value) => {
              if (operationGate.current.busy || !ready || busy || finished || download !== null)
                return;
              setAppCustomization(value);
              setCustomizationError("");
            }}
          />
          <FieldSet className="gap-3 border-t pt-4">
            <FieldLegend variant="label">{t("dmWebSigningOptions")}</FieldLegend>
            <Field orientation="horizontal">
              <Checkbox
                id="signing-enabled"
                checked={signing}
                disabled={busy || generating || finished || download !== null}
                onCheckedChange={(checked) => {
                  if (
                    operationGate.current.busy ||
                    finished ||
                    busy ||
                    generating ||
                    download !== null
                  )
                    return;
                  if (checked !== true) clearSigningInputs();
                  setSigning(checked === true);
                }}
              />
              <FieldContent>
                <FieldLabel htmlFor="signing-enabled">{t("dmWebSigningOption")}</FieldLabel>
                <FieldDescription className="text-xs">{t("dmWebSigningHelp")}</FieldDescription>
              </FieldContent>
            </Field>
            <FieldSet
              className="max-w-xl gap-3"
              disabled={
                !signing ||
                !signingReady ||
                busy ||
                generating ||
                ((!ready || finished || download !== null) && !signingRetryAvailable)
              }
            >
              <FieldLegend className="sr-only">{t("dmWebSigningCredentials")}</FieldLegend>
              <FieldGroup className="gap-3">
                <Field className="gap-1.5 sm:grid sm:grid-cols-[11rem_minmax(0,1fr)] sm:items-center">
                  <FieldLabel htmlFor="signing-identity" className="font-normal">
                    {t("dmWebSigningIdentity")}
                  </FieldLabel>
                  <Input
                    id="signing-identity"
                    ref={identityInput}
                    type="file"
                    accept=".p12,.pfx"
                    onChange={() => {
                      if (!operationGate.current.busy)
                        setSigningFiles({
                          identity: Boolean(identityInput.current?.files?.[0]),
                          profile: Boolean(profileInput.current?.files?.[0]),
                        });
                    }}
                  />
                </Field>
                <Field className="gap-1.5 sm:grid sm:grid-cols-[11rem_minmax(0,1fr)] sm:items-center">
                  <FieldLabel htmlFor="signing-password" className="font-normal">
                    {t("dmWebSigningPassword")}
                  </FieldLabel>
                  <Input
                    id="signing-password"
                    ref={passwordInput}
                    type="password"
                    autoComplete="off"
                    maxLength={IPA_SIGNING_LIMITS.passwordCharacters}
                  />
                </Field>
                <Field className="gap-1.5 sm:grid sm:grid-cols-[11rem_minmax(0,1fr)] sm:items-center">
                  <FieldLabel htmlFor="signing-profile" className="font-normal">
                    {t("dmWebSigningProfile")}
                  </FieldLabel>
                  <Input
                    id="signing-profile"
                    ref={profileInput}
                    type="file"
                    accept=".mobileprovision"
                    onChange={() => {
                      if (!operationGate.current.busy)
                        setSigningFiles({
                          identity: Boolean(identityInput.current?.files?.[0]),
                          profile: Boolean(profileInput.current?.files?.[0]),
                        });
                    }}
                  />
                </Field>
              </FieldGroup>
            </FieldSet>
          </FieldSet>
          <div className="grid gap-3 sm:grid-cols-2">
            <Button
              variant="outline"
              className="h-auto min-h-20 justify-start gap-3 rounded-xl border-primary/25 bg-primary/5 p-4 text-left whitespace-normal text-primary hover:bg-primary/10 hover:text-primary"
              onClick={() => void choose()}
              disabled={!ready || busy}
            >
              <span
                className="flex size-7 shrink-0 items-center justify-center rounded-full border border-current text-xs"
                aria-hidden="true"
              >
                1
              </span>
              <FolderOpenIcon aria-hidden="true" />
              {t("dmWebChoose")}
            </Button>
            <Button
              className="h-auto min-h-20 justify-start gap-3 rounded-xl p-4 text-left whitespace-normal"
              onClick={() => void generate()}
              disabled={!generationReadiness.canGenerate}
              aria-describedby={
                generationReadinessMessage ? "generation-readiness" : "packager-status"
              }
              aria-busy={generating}
            >
              <span
                className="flex size-7 shrink-0 items-center justify-center rounded-full border border-current/40 text-xs"
                aria-hidden="true"
              >
                2
              </span>
              {!generating && <PackageIcon aria-hidden="true" />}
              {generating && (
                <span
                  className="size-4 animate-spin rounded-full border-2 border-current border-t-transparent motion-reduce:animate-none"
                  aria-hidden="true"
                />
              )}
              {generating ? t("dmWebGenerating") : t("dmWebGenerate")}
            </Button>
            {busy && (
              <Button
                variant="outline"
                className="sm:col-span-2"
                disabled={cancelling}
                onClick={() => {
                  setReady(false);
                  setCancelling(true);
                  generationTracker.current?.cancel();
                  publishProgress(generationTracker.current);
                  cancellation.current?.abort();
                  pendingGenerationStatus.current = null;
                  setStatus(t("dmWebCancellingWait"));
                  appendActivity(t("dmWebCancellationRequested"), true);
                }}
              >
                {cancelling ? t("dmWebCancelling") : t("dmWebCancel")}
              </Button>
            )}
          </div>
          <p className="text-xs leading-relaxed text-muted-foreground">
            {t("dmWebGenerationExpectation")}
          </p>
          {generationReadinessMessage && (
            <p
              id="generation-readiness"
              className="text-sm text-muted-foreground"
              role="status"
              aria-live="polite"
            >
              {generationReadinessMessage}
            </p>
          )}
          <GenerationProgress
            progress={overallProgress}
            statusMessages={[status, signingStatus, resourceOutcome]}
            statusId="packager-status"
          />
          {!overallProgress && (
            <p
              id="packager-status"
              className="text-sm text-muted-foreground"
              role="status"
              aria-live="polite"
            >
              {status}
            </p>
          )}
          {folder && (
            <div className="space-y-2">
              <p className="break-all text-sm font-medium">{folder.name}</p>
              {inputMode === "extracted" ? (
                <p className="text-xs text-muted-foreground">{t("dmWebExtractedInstructions")}</p>
              ) : (
                <Accordion type="single" collapsible className="text-muted-foreground">
                  <AccordionItem value="packages" className="border-0">
                    <AccordionTrigger className="min-h-11 py-2 text-xs">
                      {t("dmWebIncludedPackages", nspNames.length)}
                    </AccordionTrigger>
                    <AccordionContent className="pb-0 text-xs">
                      <ul className="space-y-1 break-all">
                        {nspNames.map((name) => (
                          <li key={name}>{name}</li>
                        ))}
                      </ul>
                    </AccordionContent>
                  </AccordionItem>
                </Accordion>
              )}
            </div>
          )}
          {!overallProgress && signingStatus && (
            <p className="text-sm text-muted-foreground" role="status">
              {signingStatus}
            </p>
          )}
          {!overallProgress && resourceOutcome && (
            <p className="text-sm text-muted-foreground" role="status">
              {resourceOutcome}
            </p>
          )}
          {downloadPreparationFailed && (
            <div className="space-y-2">
              {!overallProgress && (
                <p className="text-sm text-muted-foreground" role="status">
                  {t("dmWebDownloadPreparationFailed")}
                </p>
              )}
              <Button onClick={() => prepareDownload()}>
                {t("dmWebRetryDownloadPreparation")}
              </Button>
            </div>
          )}
          {signingRetryAvailable && (
            <Button onClick={retrySigning} disabled={busy || !credentialsSelected}>
              {t("dmWebRetrySigning")}
            </Button>
          )}
          {download && !busy && (
            <div className="space-y-2">
              <Button asChild>
                <a href={download.url} download={download.fileName}>
                  {t(
                    originalIdentityDownload
                      ? "dmWebCustomOriginalDownload"
                      : signedDownload
                        ? "dmWebSignedDownload"
                        : signing
                          ? "dmWebUnsignedFallbackDownload"
                          : "dmWebDownload",
                  )}
                </a>
              </Button>
              <p className="text-xs text-muted-foreground">
                {t(
                  originalIdentityDownload
                    ? "dmWebCustomOriginalRetained"
                    : signedDownload
                      ? "dmWebSignedHelp"
                      : signing
                        ? "dmWebUnsignedFallbackHelp"
                        : "dmWebDownloadHelp",
                )}
              </p>
            </div>
          )}
          {download && completedOutput && !busy && (!signing || signedDownload) && (
            <PostGenerationGuide
              output={completedOutput}
              downloadFileName={download.fileName}
              signed={signedDownload}
            />
          )}
          {legacyAstc && legacyStatus && (
            <p className="text-sm text-muted-foreground" role="status" aria-live="polite">
              {legacyStatus}
            </p>
          )}
          <Accordion
            type="multiple"
            value={openOptions}
            onValueChange={setOpenOptions}
            className="rounded-lg border px-3 [&_[data-slot=accordion-content][data-state=closed]]:hidden"
          >
            <AccordionItem value="advanced">
              <AccordionTrigger className="min-h-11 py-3">
                {t("dmWebAdvancedOptions")}
              </AccordionTrigger>
              <AccordionContent forceMount className="space-y-4">
                <FieldGroup className="gap-4">
                  <Field orientation="horizontal">
                    <Checkbox
                      id="ignore-cache"
                      checked={ignoreCache}
                      aria-describedby="ignore-cache-help"
                      disabled={busy || generating || finished || download !== null}
                      onCheckedChange={(checked) => {
                        if (
                          operationGate.current.busy ||
                          finished ||
                          busy ||
                          generating ||
                          download !== null
                        )
                          return;
                        setIgnoreCache(checked === true);
                      }}
                    />
                    <FieldContent>
                      <FieldLabel htmlFor="ignore-cache">{t("dmWebIgnoreCache")}</FieldLabel>
                      <FieldDescription id="ignore-cache-help" className="text-xs">
                        {t("dmWebIgnoreCacheHelp")}
                      </FieldDescription>
                    </FieldContent>
                  </Field>
                  <Field className="gap-2 [&>[data-slot=native-select-wrapper]]:w-full [&>[data-slot=native-select-wrapper]]:max-w-xs">
                    <FieldLabel htmlFor="ios-target">{t("dmWebTarget")}</FieldLabel>
                    <NativeSelect
                      id="ios-target"
                      aria-describedby="ios-target-help"
                      value={target}
                      disabled={busy || generating || finished || download !== null}
                      onChange={(event) => {
                        const next = event.target.value;
                        if (
                          (next !== "ios18" && next !== "ios26") ||
                          next === target ||
                          operationGate.current.busy ||
                          finished ||
                          busy ||
                          generating ||
                          download !== null
                        )
                          return;
                        setTarget(next);
                      }}
                    >
                      <NativeSelectOption value="ios26">{t("dmWebTarget26")}</NativeSelectOption>
                      <NativeSelectOption value="ios18">{t("dmWebTarget18")}</NativeSelectOption>
                    </NativeSelect>
                    <FieldDescription id="ios-target-help" className="text-xs">
                      {t("dmWebTargetReload")}
                    </FieldDescription>
                    <FieldDescription className="text-xs">
                      {t("dmWebSupportedVersions", supportedVersions)}
                    </FieldDescription>
                  </Field>
                  <Field orientation="horizontal">
                    <Checkbox
                      id="legacy-textures"
                      checked={legacyAstc}
                      aria-describedby="legacy-option-help"
                      disabled={busy || generating || finished || download !== null}
                      onCheckedChange={(checked) => {
                        if (
                          operationGate.current.busy ||
                          finished ||
                          busy ||
                          generating ||
                          download !== null
                        )
                          return;
                        setLegacyAstc(checked === true);
                      }}
                    />
                    <FieldContent>
                      <FieldLabel htmlFor="legacy-textures">{t("dmWebLegacyOption")}</FieldLabel>
                      <FieldDescription id="legacy-option-help" className="text-xs">
                        {t("dmWebLegacyHelp")}
                      </FieldDescription>
                    </FieldContent>
                  </Field>
                  {legacyAstc && (
                    <Field className="gap-2 [&>[data-slot=native-select-wrapper]]:w-full [&>[data-slot=native-select-wrapper]]:max-w-xs">
                      <FieldLabel htmlFor="astc-preset">{t("dmWebAstcPreset")}</FieldLabel>
                      <NativeSelect
                        id="astc-preset"
                        value={astcPreset}
                        aria-describedby="astc-preset-help"
                        disabled={busy || generating || finished || download !== null}
                        onChange={(event) => {
                          if (
                            operationGate.current.busy ||
                            finished ||
                            busy ||
                            generating ||
                            download !== null
                          )
                            return;
                          const next = event.target.value;
                          if (next === "fast" || next === "thorough") setAstcPreset(next);
                        }}
                      >
                        <NativeSelectOption value="thorough">
                          {t("dmWebAstcThorough")}
                        </NativeSelectOption>
                        <NativeSelectOption value="fast">{t("dmWebAstcFast")}</NativeSelectOption>
                      </NativeSelect>
                      <FieldDescription id="astc-preset-help" className="text-xs">
                        {t("dmWebAstcPresetHelp")}
                      </FieldDescription>
                    </Field>
                  )}
                  {folder && (
                    <Field orientation="horizontal">
                      <Checkbox
                        id="skip-resources"
                        checked={skipResources}
                        disabled={!ready || busy}
                        aria-describedby={skipResources ? "skip-resources-help" : undefined}
                        onCheckedChange={(checked) => {
                          if (operationGate.current.busy) return;
                          setSkipResources(checked === true);
                        }}
                      />
                      <FieldContent>
                        <FieldLabel htmlFor="skip-resources">{t("dmWebSkipResources")}</FieldLabel>
                        {skipResources && (
                          <FieldDescription id="skip-resources-help" className="text-xs">
                            {t(
                              inputMode === "extracted"
                                ? "dmWebExtractedSkipResourcesHelp"
                                : "dmWebSkipResourcesHelp",
                            )}
                            {legacyAstc ? ` ${t("dmWebSkipKeepsLegacy")}` : ""}
                          </FieldDescription>
                        )}
                      </FieldContent>
                    </Field>
                  )}
                </FieldGroup>
                <AppCustomizationFields
                  section="advanced"
                  value={appCustomization}
                  defaults={appCustomizationDefaults}
                  disabled={!ready || busy || finished || download !== null}
                  error={customizationValidation.error || customizationError}
                  onChange={(value) => {
                    if (
                      operationGate.current.busy ||
                      !ready ||
                      busy ||
                      finished ||
                      download !== null
                    )
                      return;
                    setAppCustomization(value);
                    setCustomizationError("");
                  }}
                />
              </AccordionContent>
            </AccordionItem>
          </Accordion>
          <PreparationFaq />
          {storageAdvisory && <p className="text-xs text-muted-foreground">{storageAdvisory}</p>}
          <Accordion type="single" collapsible className="text-muted-foreground">
            <AccordionItem value="activity" className="border-0">
              <AccordionTrigger className="min-h-11 py-2 text-xs">
                {t("dmWebLogCount", activity.length)}
              </AccordionTrigger>
              <AccordionContent className="space-y-2 text-xs">
                <p>{t("dmWebLogHelp")}</p>
                <ol
                  className="max-h-64 overflow-auto break-words space-y-1"
                  aria-label={t("dmWebLog")}
                >
                  {activity.map((line) => (
                    <li key={line.id}>{line.text}</li>
                  ))}
                </ol>
                <Button
                  variant="outline"
                  size="sm"
                  onClick={async () => {
                    const request = ++copyRequest.current;
                    const revision = activityRevision.current;
                    copiedActivityRevision.current = revision;
                    setCopyStatus("");
                    try {
                      await navigator.clipboard.writeText(
                        activityBuffer.current.map((line) => line.text).join("\n"),
                      );
                      if (!mounted.current || request !== copyRequest.current) return;
                      setCopyStatus(
                        revision === activityRevision.current
                          ? t("dmWebCopied")
                          : t("dmWebCopiedNew"),
                      );
                    } catch {
                      if (mounted.current && request === copyRequest.current) {
                        setCopyStatus(t("dmWebCopyFailed"));
                      }
                    }
                  }}
                >
                  {t("dmWebCopy")}
                </Button>
                {copyStatus && <p role="status">{copyStatus}</p>}
              </AccordionContent>
            </AccordionItem>
          </Accordion>
          {loading && !loadFailed && (
            <div className="space-y-2 text-xs text-muted-foreground">
              {loading.phase === "download" &&
              Number.isFinite(loading.loadedBytes) &&
              Number.isFinite(loading.totalBytes) &&
              loading.totalBytes > 0 ? (
                <>
                  <Progress
                    className="[&_[data-slot=progress-indicator]]:motion-reduce:transition-none"
                    aria-label={t("dmWebFilesLoaded")}
                    aria-describedby="runtime-loading-details"
                    value={Math.min(
                      100,
                      Math.max(0, (loading.loadedBytes / loading.totalBytes) * 100),
                    )}
                  />
                  <p id="runtime-loading-details">
                    {t(
                      "dmWebFilesReady",
                      megabytes(loading.loadedBytes),
                      megabytes(loading.totalBytes),
                      loading.completedFiles,
                      loading.totalFiles,
                    )}
                  </p>
                  <p>
                    {t("dmWebDownloaded", megabytes(loading.downloadedBytes))}
                    {loading.cachedBytes > 0
                      ? t("dmWebCached", megabytes(loading.cachedBytes))
                      : ""}
                  </p>
                </>
              ) : (
                <Progress
                  className="bg-primary/30 motion-safe:animate-pulse"
                  aria-label={loadingLabel(loading.phase)}
                  value={null}
                />
              )}
            </div>
          )}
          {(loadFailed || finished) && (
            <Button variant="outline" onClick={() => window.location.reload()}>
              {loadFailed ? t("dmWebReloadRetry") : t("dmWebReload")}
            </Button>
          )}
          <p className="flex items-start gap-2 text-xs text-muted-foreground">
            <ShieldCheckIcon className="size-4 shrink-0 text-primary" aria-hidden="true" />
            {t("dmWebPrivacy")}
          </p>
        </CardContent>
      </Card>
    </main>
  );
}
