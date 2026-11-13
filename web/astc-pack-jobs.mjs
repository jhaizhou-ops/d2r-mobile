// 认证门面保留原始 WeakMap 能力；工作线程只导入独立内核，不复制认证图。
// The authenticated facade retains original WeakMap capabilities; workers import the separate core, never a duplicate authentication graph.
import {
  enumerateAuthenticatedAstcRecords,
  openAuthenticatedAstcRecord,
} from "./extraction-job-adapter.mjs";
import { readAuthenticatedCascVfs } from "./casc-overlay-writer.mjs";
import { createAstcPackJobsCore } from "./astc-pack-jobs-core.mjs";
import { stageArchiveRecord } from "./archive-stream-reader.mjs";
import { transcodeTextureStream } from "./toolchain/astc-encoder-wasm/texture-stream.mjs";
import { transformTextureDescriptorCache } from "./toolchain/astc-encoder-wasm/descriptor-cache.mjs";
export { createAstcPackJobsCore, ASTC_JOB_CLEANUP_WAIT_MS } from "./astc-pack-jobs-core.mjs";
const fail = (code) => {
  throw new Error(`ASTC_PACK_JOBS_${code}`);
};
const integer = (n, max) => Number.isSafeInteger(n) && n > 0 && n <= max;

// 依赖注入仅供受信任接线或合成测试；认证入口不可替换。
// Dependency injection is for trusted wiring or synthetic tests only; authentication entry points cannot be replaced.
export function createAstcPackJobCoordinator({
  stageRecord = stageArchiveRecord,
  transcodeTexture = transcodeTextureStream,
  transformDescriptor = transformTextureDescriptorCache,
} = {}) {
  if ([stageRecord, transcodeTexture, transformDescriptor].some((fn) => typeof fn !== "function"))
    fail("DEPENDENCIES");
  return async function createAuthenticatedAstcPackJobs(extracted, selection, options = {}) {
    const { root, maxEncodedBytes, maxDecodedBytes, batchBlocks, encode, signal } = options;
    if (
      !root ||
      !integer(maxEncodedBytes, 0xffffffff) ||
      !integer(maxDecodedBytes, 0xffffffff) ||
      !integer(batchBlocks, 65536) ||
      typeof encode !== "function" ||
      (signal !== undefined && !(signal instanceof AbortSignal))
    )
      fail("OPTIONS");
    signal?.throwIfAborted();
    const selected = await enumerateAuthenticatedAstcRecords(extracted, selection, { signal });
    if (!selected.descriptor) fail("DESCRIPTOR_MISSING");
    const vfs = await readAuthenticatedCascVfs(extracted, selection, { signal });
    return createAstcPackJobsCore(
      { ...options, selected, vfs, openRecord: openAuthenticatedAstcRecord },
      { stageRecord, transcodeTexture, transformDescriptor },
    );
  };
}
export const createAuthenticatedAstcPackJobs = createAstcPackJobCoordinator();
