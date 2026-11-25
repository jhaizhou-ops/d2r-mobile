/* 仅使用仓库精确发布输入或生成的合成样例。 */
/* Exact repository release inputs or generated synthetic fixtures only. */
import createNative from "./capstone-lz4.mjs";
import { createProgramRewriter } from "./rewrite-core.mjs";
import { PROGRAM_REWRITER_SOURCE } from "./program-release-catalog.mjs";
import { SOURCE_SHA256 } from "./js/rule-data.mjs";
let initialized;
function initialize() {
  initialized ??= (async () => {
    if (SOURCE_SHA256 !== PROGRAM_REWRITER_SOURCE) throw new Error("Program source pin mismatch");
    const response = await fetch(new URL("fixtures/manifest.json", import.meta.url));
    if (!response.ok) throw new Error("Missing fixture manifest");
    const manifest = await response.json();
    if (manifest.sourceSha256 !== SOURCE_SHA256) throw new Error("Fixture source pin mismatch");
    return { native: await createNative(), manifest };
  })();
  return initialized;
}
export const rewriteProgram = createProgramRewriter(initialize);
let queue = Promise.resolve();
self.addEventListener("message", ({ data }) => {
  queue = queue.then(async () => {
    try {
      const result = await rewriteProgram(data);
      self.postMessage(
        { id: data.id, ok: true, ...result },
        Object.values(result.files).map((bytes) => bytes.buffer),
      );
    } catch (error) {
      self.postMessage({ id: data?.id, ok: false, error: String(error) });
    }
  });
});
