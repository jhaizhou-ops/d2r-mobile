// 根据生成状态显示阶段进度与结果；依据 @/components/ui/progress、../../i18n.mjs 的模块与调用约定。
// Render stage progress and results from generation state using the module and calling contracts in @/components/ui/progress、../../i18n.mjs.
import { useId } from "react";
import { Progress } from "@/components/ui/progress";
import { t } from "../../i18n.mjs";
import { compilerWorkerLabel } from "../../packaging-diagnostics.mjs";
import { createGenerationProgress } from "../../generation-progress.mjs";

type Snapshot = ReturnType<ReturnType<typeof createGenerationProgress>["snapshot"]>;
const stageKeys: Record<string, string> = {
  preparing: "dmWebOverallPreparing",
  input: "dmWebOverallInput",
  program: "dmWebStagesProgram",
  assets: "dmWebStagesAssets",
  shaders: "dmWebStagesShaders",
  validation: "dmWebStagesValidation",
  resources: "dmWebStagesResources",
  "legacy-pack": "dmWebOverallLegacy",
  finalize: "dmWebOverallFinalize",
  signing: "dmWebOverallSigning",
  download: "dmWebOverallDownload",
};
const statusKeys: Record<string, string> = {
  complete: "dmWebOverallReady",
  failed: "dmWebOverallFailed",
  cancelling: "dmWebWaitCleanup",
  cancelled: "dmWebOverallCancelled",
  "signing-failed": "dmWebOverallSigningIncomplete",
  "signing-cancelled": "dmWebSigningCancelled",
  "download-pending": "dmWebOverallDownloadPending",
};
const megabytes = (bytes: number) => (bytes / 1_000_000).toFixed(1);
function stageDetail(detail: Snapshot["detail"]) {
  if (!detail) return t("dmWebOverallUnmeasured");
  switch (detail.kind) {
    case "shaders":
      return (
        t("dmWebStagesShaders") +
        t("dmWebProgressFraction", detail.completedRecords, detail.totalRecords) +
        compilerWorkerLabel(detail.compilerWorkers)
      );
    case "resources":
      return t(
        "dmWebResourceProgress",
        detail.filesWritten,
        detail.totalFiles,
        megabytes(detail.bytesWritten),
        megabytes(detail.totalBytes),
      );
    case "legacy-pack":
      return t(
        "dmWebLegacySaving",
        t("dmWebMegabytes", megabytes(detail.bytesWritten)),
        t("dmWebMegabytes", megabytes(detail.totalBytes)),
      );
    case "legacy-astc":
      return `${t("dmWebLegacyConverting", detail.completedBlocks, megabytes(detail.processedBytes))} ${t("dmWebOverallUnmeasured")}`;
    case "romfs":
      return t(
        "dmWebVerificationPercent",
        t(detail.phase === "base" ? "dmWebBase" : "dmWebMerged"),
        detail.percent,
      );
    case "signing":
      return `${t("dmWebOverallSigningStage", detail.step, detail.total)}. ${t("dmWebOverallUnmeasured")}`;
    default:
      return t("dmWebOverallUnmeasured");
  }
}

export function GenerationProgress({
  progress,
  statusMessages = [],
  statusId,
}: {
  progress: Snapshot | null;
  statusMessages?: string[];
  statusId?: string;
}) {
  const labelId = useId(),
    descriptionId = useId();
  if (!progress) return null;
  const messages = [...new Set(statusMessages.filter(Boolean))];
  const summary =
    progress.status === "running" ? t(stageKeys[progress.stage]) : t(statusKeys[progress.status]);
  const count = t(
    "dmWebOverallCount",
    progress.percent,
    progress.completedStages,
    progress.totalStages,
  );
  return (
    <section
      className="space-y-2 rounded-lg border border-primary/20 bg-primary/5 p-3"
      aria-labelledby={labelId}
    >
      <div className="flex flex-wrap items-center justify-between gap-1 text-sm">
        <p id={labelId} className="font-medium">
          {t("dmWebOverallTitle")}
        </p>
        <p className="text-xs tabular-nums text-muted-foreground">{count}</p>
      </div>
      <Progress
        value={progress.percent}
        aria-labelledby={labelId}
        aria-describedby={descriptionId}
        aria-valuetext={`${count}. ${summary}`}
        className="[&_[data-slot=progress-indicator]]:motion-reduce:transition-none"
      />
      <p className="text-sm" role="status" aria-live="polite">
        {summary}
      </p>
      {messages.length > 0 && (
        <div id={statusId} className="space-y-1 text-sm" role="status" aria-live="polite">
          {messages.map((message) => (
            <p key={message}>{message}</p>
          ))}
        </div>
      )}
      {progress.status === "running" && (
        <p className="text-xs text-muted-foreground">{stageDetail(progress.detail)}</p>
      )}
      <p id={descriptionId} className="text-xs text-muted-foreground">
        {t("dmWebOverallEstimate")}
      </p>
    </section>
  );
}
