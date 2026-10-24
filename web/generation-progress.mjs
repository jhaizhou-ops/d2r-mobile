// 仅统计本次固定作业的已完成阶段；计数器只描述当前阶段，不估算耗时。
// Count completed stages in one frozen job; counters describe only the current stage, never elapsed time.
import {
  sanitizeResourceProgress,
  sanitizeShaderRecordProgress,
} from "./packaging-diagnostics.mjs";
import { isExtractionProgress } from "./extraction-errors.mjs";

const stages = Object.freeze({
  discovery: "input",
  secrets: "input",
  inspection: "input",
  selection: "input",
  extraction: "input",
  program: "program",
  assets: "assets",
  shaders: "shaders",
  shell: "validation",
  validation: "validation",
  assembly: "validation",
  resources: "resources",
  "legacy-pack": "legacy-pack",
  output: "finalize",
  disposal: "finalize",
  archive: "finalize",
  cleanup: "finalize",
  commit: "finalize",
});
const completionEvents = new Set([
  "extraction",
  "program",
  "assets",
  "shaders",
  "assembly",
  "resources",
  "legacy-pack",
]);
const signingStages = ["inspect", "sign", "package", "verify"];
const count = (value) => Number.isSafeInteger(value) && value >= 0;
const readyBlob = (value) => value?.blob instanceof Blob && value.blob.size > 0;

export function createGenerationProgress(options = {}) {
  const included = Object.freeze({
    legacyAstc: options.legacyAstc === true,
    signing: options.signing === true,
    exportResources: options.exportResources !== false,
  });
  const plan = Object.freeze([
    "preparing",
    "input",
    "program",
    "assets",
    "shaders",
    "validation",
    ...(included.exportResources ? ["resources"] : []),
    ...(included.legacyAstc ? ["legacy-pack"] : []),
    "finalize",
    ...(included.signing ? ["signing"] : []),
    "download",
  ]);
  const completed = new Set();
  let status = "running",
    stage = "preparing",
    detail = null;
  let generated = false,
    signed = false;
  const complete = (name) => {
    if (plan.includes(name)) completed.add(name);
  };
  const selectNext = () => {
    stage = plan.find((name) => !completed.has(name)) ?? "download";
    detail = null;
  };
  const snapshot = () =>
    Object.freeze({
      status,
      stage,
      detail,
      completedStages: completed.size,
      totalStages: plan.length,
      // 即使以后阶段数量变化，也绝不将未完成任务向上取整到 100。
      // Never round an incomplete job up to 100, even if future stage counts change.
      percent:
        status === "complete"
          ? 100
          : Math.min(99, Math.floor((completed.size * 100) / plan.length)),
    });
  return Object.freeze({
    included,
    plan,
    snapshot,
    prepared() {
      if (status !== "running") return;
      complete("preparing");
      selectNext();
    },
    observe(event) {
      if (status !== "running" || generated || !Object.hasOwn(stages, event?.stage)) return;
      const mapped = stages[event.stage];
      if (!plan.includes(mapped) || completed.has(mapped)) return;
      // 阶段失败会终止遥测；取消由调用方单独分类。
      // A stage failure is terminal for telemetry. The caller classifies cancellation separately.
      if (event.state === "failed") {
        status = "failed";
        stage = mapped;
        detail = null;
        return;
      }
      stage = mapped;
      detail = null;
      if (event.state === "complete" && completionEvents.has(event.stage)) {
        complete(mapped);
        selectNext();
        return;
      }
      if (event.state !== "running") return;
      if (event.stage === "shaders" && event.diagnostic?.kind === "shader-records") {
        const progress = sanitizeShaderRecordProgress(event.diagnostic);
        if (progress) detail = Object.freeze({ kind: "shaders", ...progress });
      } else if (event.stage === "resources") {
        const progress = sanitizeResourceProgress(event.resourceProgress);
        if (progress) detail = Object.freeze({ kind: "resources", ...progress });
      } else if (
        event.stage === "legacy-pack" &&
        count(event.bytesWritten) &&
        count(event.totalBytes) &&
        event.totalBytes > 0 &&
        event.bytesWritten <= event.totalBytes
      ) {
        detail = Object.freeze({
          kind: "legacy-pack",
          bytesWritten: event.bytesWritten,
          totalBytes: event.totalBytes,
        });
      } else if (
        included.legacyAstc &&
        event.stage === "assets" &&
        event.diagnostic?.kind === "legacy-astc" &&
        count(event.diagnostic.completedBlocks) &&
        count(event.diagnostic.processedBytes)
      ) {
        detail = Object.freeze({
          kind: "legacy-astc",
          completedBlocks: event.diagnostic.completedBlocks,
          processedBytes: event.diagnostic.processedBytes,
        });
      } else if (
        ["secrets", "extraction"].includes(event.stage) &&
        isExtractionProgress(event.diagnostic) &&
        ["romfs-base", "romfs-merged"].includes(event.diagnostic.stage)
      ) {
        detail = Object.freeze({
          kind: "romfs",
          phase: event.diagnostic.stage === "romfs-base" ? "base" : "merged",
          percent: event.diagnostic.completed,
        });
      }
    },
    generated(result) {
      // 仅有提交事件不够，自定义、导出和返回 Blob 都必须成功。
      // A commit event alone is insufficient: customization, exports and the returned Blob must succeed.
      if (
        !["running", "cancelling"].includes(status) ||
        !readyBlob(result?.download) ||
        (included.exportResources && result.resources?.status !== "complete") ||
        (included.legacyAstc && result.legacyPack?.status !== "complete")
      )
        return false;
      generated = true;
      for (const name of plan) if (!["signing", "download"].includes(name)) complete(name);
      status = "running";
      selectNext();
      return true;
    },
    startSigning() {
      if (!generated || !included.signing || signed || status === "complete") return false;
      status = "running";
      stage = "signing";
      detail = null;
      return true;
    },
    observeSigning(event) {
      if (status !== "running" || stage !== "signing") return;
      const index = signingStages.indexOf(event?.stage);
      if (
        index < 0 ||
        !count(event.completed) ||
        !count(event.total) ||
        event.completed > event.total
      )
        return;
      detail = Object.freeze({ kind: "signing", step: index + 1, total: signingStages.length });
    },
    signed(result) {
      if (
        !generated ||
        !included.signing ||
        !readyBlob(result) ||
        !["running", "cancelling"].includes(status)
      )
        return false;
      signed = true;
      complete("signing");
      status = "running";
      selectNext();
      return true;
    },
    signingFailed(cancelled = false) {
      if (!generated || signed || !included.signing) return;
      status = cancelled ? "signing-cancelled" : "signing-failed";
      stage = "signing";
      detail = null;
    },
    downloadPrepared(available) {
      // 未签名回退或恢复的原始 IPA 不能满足已请求的签名输出。
      // An unsigned fallback or recovered original IPA must never satisfy the requested output.
      if (
        !generated ||
        (included.signing && !signed) ||
        !["running", "download-pending", "complete"].includes(status)
      )
        return;
      stage = "download";
      detail = null;
      if (available) {
        complete("download");
        status = "complete";
      } else if (status !== "complete") status = "download-pending";
    },
    cancel() {
      if (status !== "running") return;
      status = "cancelling";
      detail = null;
    },
    fail(cancelled = false) {
      if (status === "complete") return;
      status = cancelled ? "cancelled" : "failed";
      detail = null;
    },
  });
}
