// 将按钮和提示绑定到同一份就绪状态；文件夹扫描成功不代表可选设置已完整。
// Bind the button and guidance to one readiness result; a successful folder scan does not validate optional settings.
/**
 * @param {{ ready: boolean, busy: boolean, finished: boolean, hasDownload: boolean, hasFolder: boolean, inputMode: "packages" | "extracted" | null, packageCount: number, customizationError: string, signing: boolean, signingReady: boolean, identitySelected: boolean, profileSelected: boolean }} state
 */
export function getGenerationReadiness(state) {
  const blocked = (messageKey = null, detail = "") => ({ canGenerate: false, messageKey, detail });
  // 加载、操作和终止状态沿用实时进度消息，不用旧的文件夹提示覆盖它。
  // Loading, active and terminal states retain their live status instead of stale folder guidance.
  if (!state.ready || state.busy || state.finished || state.hasDownload) return blocked();
  if (
    !state.hasFolder ||
    !state.inputMode ||
    (state.inputMode === "packages" && state.packageCount === 0)
  )
    return blocked("dmWebGenerateChooseFolder");
  if (state.customizationError) return blocked("dmWebCustomCorrect", state.customizationError);
  if (state.signing) {
    if (!state.signingReady) return blocked("dmWebGenerateSigningUnavailable");
    if (!state.identitySelected && !state.profileSelected)
      return blocked("dmWebGenerateSigningFilesNeeded");
    if (!state.identitySelected) return blocked("dmWebGenerateSigningIdentityNeeded");
    if (!state.profileSelected) return blocked("dmWebGenerateSigningProfileNeeded");
  }
  return { canGenerate: true, messageKey: "dmWebGenerateReady", detail: "" };
}
