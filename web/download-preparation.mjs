// 保留已完成 IPA 的 Blob；下载链接准备失败不改变转换或导出结果。
// Retain a completed IPA Blob; download-link preparation failures do not change conversion or export outcomes.
export function createDownloadPreparation({
  createObjectURL = (blob) => URL.createObjectURL(blob),
  revokeObjectURL = (url) => URL.revokeObjectURL(url),
} = {}) {
  let retained, current;
  return Object.freeze({
    /** @param {{blob: Blob, fileName: string}} output */
    retain(output) {
      retained = { blob: output.blob, fileName: output.fileName };
    },
    prepare() {
      if (!retained) return Object.freeze({ status: "unavailable" });
      if (current?.blob === retained.blob && current.fileName === retained.fileName)
        return Object.freeze({ status: "ready", url: current.url, fileName: current.fileName });
      let url;
      try {
        url = createObjectURL(retained.blob);
      } catch {
        return Object.freeze({ status: "retryable" });
      }
      const previous = current;
      current = { ...retained, url };
      // 仅在新链接成功建立后释放旧链接；清理异常不否定新产物。
      // Release the old URL only after the new one exists; cleanup errors do not negate the new artifact.
      if (previous) {
        try {
          revokeObjectURL(previous.url);
        } catch {
          /* 新链接仍有效。 | The new link stays valid. */
        }
      }
      return Object.freeze({ status: "ready", url, fileName: retained.fileName });
    },
    dispose() {
      const previous = current;
      retained = current = undefined;
      if (previous) {
        try {
          revokeObjectURL(previous.url);
        } catch {
          /* 已丢弃引用，页面卸载仍可回收链接。 | References are dropped; page unload can reclaim the URL. */
        }
      }
    },
  });
}

// 自动点击只是便利功能；创建节点、点击或移除失败都不得撤销可见下载链接。
// Auto-click is only a convenience; node creation, click or removal failure must not revoke the visible download link.
export function attemptAutomaticDownload({ url, fileName }, documentObject = document) {
  let link;
  try {
    link = documentObject.createElement("a");
    link.href = url;
    link.download = fileName;
    documentObject.body.append(link);
    link.click();
  } catch {
    /* 用户仍可使用可见链接。 | The visible link remains available. */
  } finally {
    try {
      link?.remove();
    } catch {
      /* 不改变已完成产物。 | Do not change the completed artifact. */
    }
  }
}
