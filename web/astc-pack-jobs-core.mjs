// 工作线程专用有界转换内核；认证与 UI 依赖保留在父级门面。
// Worker-only bounded conversion core; authentication and UI dependencies stay in the parent facade.
import { createCascPackStorageLookup } from "./toolchain/casc-overlay-js/overlay.mjs";
import { stageArchiveRecord } from "./archive-stream-reader.mjs";
import { transcodeTextureStream } from "./toolchain/astc-encoder-wasm/texture-stream.mjs";
import {
  transformTextureDescriptorCache,
  DESCRIPTOR_CACHE_LIMITS,
} from "./toolchain/astc-encoder-wasm/descriptor-cache.mjs";
const fail = (code) => {
  throw new Error(`ASTC_PACK_JOBS_${code}`);
};
const integer = (n, max) => Number.isSafeInteger(n) && n > 0 && n <= max;
const CHUNK = 65536;
export const ASTC_JOB_CLEANUP_WAIT_MS = 1000;
async function boundedCleanup(pending) {
  let timer;
  try {
    await Promise.race([
      pending,
      new Promise((_, reject) => {
        timer = setTimeout(
          () => reject(new Error("ASTC_PACK_JOBS_CLEANUP_TIMEOUT")),
          ASTC_JOB_CLEANUP_WAIT_MS,
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

// 工作线程内核不授予认证；只有受信任的父级通过绑定能力或 RPC 提供 openRecord 和 TVFS。
// The worker core grants no authentication; only trusted parents supply openRecord and TVFS through bound capabilities or RPC.
export function createAstcPackJobsCore(
  {
    selected,
    vfs,
    openRecord,
    root,
    maxEncodedBytes,
    maxDecodedBytes,
    batchBlocks,
    encode,
    signal,
    createStage,
  },
  {
    stageRecord = stageArchiveRecord,
    transcodeTexture = transcodeTextureStream,
    transformDescriptor = transformTextureDescriptorCache,
  } = {},
) {
  if (
    !selected?.descriptor ||
    !Array.isArray(selected.textures) ||
    selected.textures.length > 32767 ||
    !(vfs instanceof Uint8Array) ||
    vfs.length > 16 * 1024 * 1024 ||
    typeof openRecord !== "function" ||
    !root ||
    !integer(maxEncodedBytes, 0xffffffff) ||
    !integer(maxDecodedBytes, 0xffffffff) ||
    !integer(batchBlocks, 65536) ||
    typeof encode !== "function" ||
    (signal !== undefined && !(signal instanceof AbortSignal)) ||
    [stageRecord, transcodeTexture, transformDescriptor].some((fn) => typeof fn !== "function")
  )
    fail("CORE_OPTIONS");
  const tokens = [...selected.textures, selected.descriptor];
  let pathBytes = 0;
  for (const token of tokens) {
    if (typeof token?.path !== "string" || token.path.length > 4096) fail("PATH");
    pathBytes += new TextEncoder().encode(token.path).length;
    if (pathBytes > 8 * 1024 * 1024) fail("PATH_BUDGET");
  }
  const controller = new AbortController();
  signal = signal ? AbortSignal.any([signal, controller.signal]) : controller.signal;
  signal.throwIfAborted();
  // 只传递已选令牌数量，不复制清单或提前打开记录。
  // Pass only the selected token count, without copying a manifest or opening records early.
  let lookupStorage = createCascPackStorageLookup(vfs, { expectedQueries: tokens.length });
  const converted = new Map();
  let used = false,
    activeClose;
  const abort = () => {
    activeClose?.().catch(() => {});
    converted.clear();
    vfs.fill(0);
    lookupStorage = null;
  };
  signal.addEventListener("abort", abort, { once: true });
  async function dispose() {
    controller.abort();
    abort();
    signal.removeEventListener("abort", abort);
    if (activeClose) await boundedCleanup(activeClose());
  }

  async function* run() {
    if (used) fail("SINGLE_USE");
    used = true;
    try {
      for (const token of tokens) {
        signal?.throwIfAborted();
        const descriptor = token === selected.descriptor;
        const { source, record } = await openRecord(token, { signal });
        const [storage] = lookupStorage([{ path: token.path, oldEkey: record.ekey }]);
        if (
          storage.encodedSize !== record.encodedSize ||
          storage.decodedSize !== record.decodedSize
        )
          fail("STORAGE_SIZE");
        const stage = await stageRecord(source, record, {
          root,
          maxEncodedBytes,
          maxDecodedBytes: descriptor
            ? Math.min(maxDecodedBytes, DESCRIPTOR_CACHE_LIMITS.inputBytes)
            : maxDecodedBytes,
          signal,
          createStage,
        });
        let result,
          iterator,
          closed,
          complete = false,
          claimed = false;
        const close = () => {
          if (descriptor) result?.bytes?.fill(0);
          if (!closed)
            closed = Promise.all([
              Promise.resolve().then(() => iterator?.return?.()),
              Promise.resolve().then(() => stage.dispose()),
            ]).then(() => undefined);
          return closed;
        };
        activeClose = close;
        let primary;
        try {
          signal?.throwIfAborted();
          let first;
          if (descriptor) {
            if (!integer(stage.size, DESCRIPTOR_CACHE_LIMITS.inputBytes)) fail("DESCRIPTOR_SIZE");
            let input = new Uint8Array(stage.size);
            for (let at = 0; at < input.length; at += CHUNK) {
              signal?.throwIfAborted();
              const count = Math.min(CHUNK, input.length - at),
                bytes = await stage.readAt(at, count, { signal });
              if (!(bytes instanceof Uint8Array) || bytes.length !== count) fail("SHORT_READ");
              input.set(bytes, at);
            }
            try {
              result = await transformDescriptor(input, converted, {
                signal,
                batchBlocks,
                encode,
              });
            } finally {
              input.fill(0);
              input = null;
            }
            if (
              !(result?.bytes instanceof Uint8Array) ||
              result.bytes.length > DESCRIPTOR_CACHE_LIMITS.outputBytes
            )
              fail("DESCRIPTOR_OUTPUT");
          } else {
            iterator = transcodeTexture(stage, { signal, batchBlocks, encode })[
              Symbol.asyncIterator
            ]();
            first = await iterator.next();
            if (!first.done && first.value?.kind === "skip") continue;
            if (
              first.done ||
              first.value?.kind !== "header" ||
              !["linear", "srgb"].includes(first.value.profile)
            )
              fail("TEXTURE_HEADER");
            if (token.path.length > 1024 || !/^[\x20-\x7e]+$/.test(token.path))
              fail("DESCRIPTOR_PATH");
          }
          const size = descriptor ? result.bytes.length : stage.size;
          const jobSource = {
            async *[Symbol.asyncIterator]() {
              if (claimed) fail("SOURCE_SINGLE_USE");
              claimed = true;
              let count = 0,
                sourceFailure,
                sawComplete = false;
              try {
                if (descriptor) {
                  for (let at = 0; at < result.bytes.length; at += CHUNK) {
                    signal?.throwIfAborted();
                    yield result.bytes.slice(at, at + CHUNK);
                  }
                } else {
                  let event = first;
                  while (!event.done) {
                    signal?.throwIfAborted();
                    const value = event.value;
                    if (value.kind === "complete") {
                      sawComplete = true;
                      if (count !== size || value.size !== size) fail("TEXTURE_SIZE");
                    } else {
                      if (
                        !(value.bytes instanceof Uint8Array) ||
                        value.offset !== count ||
                        value.bytes.length > 1024 * 1024 ||
                        count + value.bytes.length > size
                      )
                        fail("TEXTURE_CHUNK");
                      count += value.bytes.length;
                      yield value.bytes;
                    }
                    event = await iterator.next();
                  }
                  if (count !== size || !sawComplete) fail("TEXTURE_SIZE");
                  converted.set(token.path, first.value.profile === "srgb");
                }
                complete = true;
              } catch (error) {
                sourceFailure = error;
                throw error;
              } finally {
                if (descriptor) result.bytes.fill(0);
                try {
                  await boundedCleanup(close());
                } catch (error) {
                  // 不覆盖已知转换失败。 | Do not overwrite a known conversion failure.
                  // eslint-disable-next-line no-unsafe-finally
                  if (!sourceFailure) throw error;
                }
              }
            },
          };
          yield Object.freeze({
            path: token.path,
            spec: storage.spec,
            decodedSize: size,
            source: jobSource,
          });
          if (!complete) fail("SOURCE_NOT_CONSUMED");
        } catch (error) {
          primary = error;
          throw error;
        } finally {
          try {
            await boundedCleanup(close());
          } catch (error) {
            // 不覆盖已知转换失败。 | Do not overwrite a known conversion failure.
            // eslint-disable-next-line no-unsafe-finally
            if (!primary) throw error;
          }
          activeClose = undefined;
        }
      }
    } finally {
      signal?.removeEventListener("abort", abort);
      converted.clear();
      vfs.fill(0);
      lookupStorage = null;
    }
  }
  return Object.freeze({
    jobs: { [Symbol.asyncIterator]: run },
    maxEntries: selected.textures.length + 1,
    dispose,
  });
}
