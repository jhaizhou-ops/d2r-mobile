// 根据 STAGED_OUTPUT_SCOPE.md 和 ZIP 校验器契约，在浏览器 OPFS 中暂存、校验并事务性提交归档。
// Stage, validate, and transactionally commit archives in browser OPFS based on STAGED_OUTPUT_SCOPE.md and the ZIP validator contract.
import { fileSource, readRange } from "./preflight.mjs";
import { planStagedZip, validateStagedZip, STAGED_ZIP_LIMITS } from "./staged-zip-validator.mjs";

const error = (code) => new Error(code);
const assert = (value, code) => {
  if (!value) throw error(code);
};
const sameManifest = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const snapshot = (manifest) => manifest.map(({ path, size, mode }) => ({ path, size, mode }));
// 仅用于不创建资源的查询；取消等待后仍观察迟到的结果与拒绝。
// Only for queries that acquire no resources; observe late results/rejections after cancelling the wait.
async function awaitLookup(read, signal) {
  signal.throwIfAborted();
  let cancel;
  try {
    const value = await Promise.race([
      read(),
      new Promise((_, reject) => {
        cancel = () => reject(signal.reason);
        signal.addEventListener("abort", cancel, { once: true });
        if (signal.aborted) cancel();
      }),
    ]);
    signal.throwIfAborted();
    return value;
  } finally {
    if (cancel) signal.removeEventListener("abort", cancel);
  }
}

/**
 * 仅在本地使用 OPFS 的暂存适配器。调用者通过用户操作获得 destinationHandle，
 * 并在创建输出之前授予读写权限。本模块从不弹出提示、上传、选择目标或操作输入句柄。
 * 只允许删除自己创建的唯一 OPFS 临时文件，不扫描清理文件，也不清空整个存储。
 * 崩溃可能遗留该文件；绝不能删除未知文件。
 *
 * archiveFormat/maxOutputBytes 必须与 runPackagingJob 选项一致。预算包含元数据在内的所有字节。
 * 配额估算仅供参考；磁盘写满等错误仍会阻止提交。浏览器 API 不提供用户目标磁盘的可用空间。
 * 传输时可能同时存在两个本地副本。不会将整个归档加载为 Blob/ArrayBuffer。
 *
 * Local-only OPFS staging adapter. The caller obtains destinationHandle from a
 * user gesture and grants readwrite permission BEFORE creating the output. This
 * module never prompts, uploads, chooses a destination, or touches input handles.
 * A unique temporary OPFS file is the only entry it may remove. No scavenging or
 * global storage clearing. Crashes can leave that file; never delete unknown files.
 *
 * Match archiveFormat/maxOutputBytes to runPackagingJob options. Budget covers
 * ALL bytes, including metadata. Quota estimates are advisory; disk/full errors
 * still fail closed. User destination disk space is not exposed by browser APIs.
 * Two local copies may coexist during transfer. No whole-archive Blob/ArrayBuffer.
 */
export function createBrowserStagedOutputAdapters({
  destinationHandle,
  archiveFormat,
  maxOutputBytes,
  storage = globalThis.navigator?.storage,
  secureContext = globalThis.isSecureContext,
  randomUUID = () => globalThis.crypto.randomUUID(),
} = {}) {
  const owned = new WeakMap();
  return {
    async createOutput({ manifest, signal: callerSignal }) {
      const cancellation = new AbortController();
      const signal = callerSignal
        ? AbortSignal.any([callerSignal, cancellation.signal])
        : cancellation.signal;
      signal.throwIfAborted();
      const captured = snapshot(manifest);
      const plan = planStagedZip(captured, { archiveFormat, maxOutputBytes });
      assert(
        secureContext === true && typeof storage?.getDirectory === "function",
        "OUTPUT_OPFS_UNAVAILABLE",
      );
      assert(
        destinationHandle?.kind === "file" &&
          typeof destinationHandle.createWritable === "function" &&
          typeof destinationHandle.queryPermission === "function",
        "OUTPUT_DESTINATION_UNAVAILABLE",
      );
      assert(
        (await awaitLookup(
          () => destinationHandle.queryPermission({ mode: "readwrite" }),
          signal,
        )) === "granted",
        "OUTPUT_DESTINATION_PERMISSION_REQUIRED",
      );
      signal?.throwIfAborted();
      // 配额估算仅供 UI 参考；实际 OPFS 写入和关闭操作才是存储可用性的依据。
      // Quota estimates belong to the UI advisory; actual OPFS writes and closes determine availability.
      const root = await awaitLookup(() => storage.getDirectory(), signal);
      signal?.throwIfAborted();
      const uuid = randomUUID();
      assert(
        typeof uuid === "string" &&
          /^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/i.test(uuid),
        "OUTPUT_RANDOM_ID_UNAVAILABLE",
      );
      const name = `d2rm-output-${uuid}.tmp`;
      // OPFS 没有独占创建操作。加密随机 UUID 配合存在性检查可避免复用已有条目；
      // 同源脚本仍被视为可信，因为它们可以独立修改任何 OPFS 文件。
      // OPFS has no exclusive-create primitive. A cryptographic UUID plus an
      // existence check avoids reusing existing entries; same-origin scripts
      // remain trusted, as they can modify any OPFS file independently.
      try {
        await awaitLookup(() => root.getFileHandle(name), signal);
        throw error("OUTPUT_TEMP_COLLISION");
      } catch (cause) {
        if (cause?.name !== "NotFoundError") throw cause;
      }
      signal?.throwIfAborted();
      let stageHandle, stageWriter, destinationWriter, pending, abortPending;
      let state = "opening",
        bytesWritten = 0,
        removed = false;
      const remove = async () => {
        if (!stageHandle || removed) return;
        try {
          await root.removeEntry(name);
        } catch (cause) {
          if (cause?.name !== "NotFoundError") throw cause;
        }
        removed = true;
      };
      const release = async () => {
        const failures = [];
        for (const kind of ["stage", "destination"]) {
          const stream = kind === "stage" ? stageWriter : destinationWriter;
          if (stream) {
            try {
              await stream.abort();
              if (kind === "stage") stageWriter = undefined;
              else destinationWriter = undefined;
            } catch (cause) {
              // 保留清理失败的流，以便稍后重试。
              // Keep failed streams available for a later cleanup retry.
              failures.push(cause);
            }
          }
        }
        // 暂存写入器终止失败时保留文件，避免仍存活的写入器与删除操作竞争。
        // Retain the stage after a failed writer abort; a live writer must not race file removal.
        if (!stageWriter) {
          try {
            await remove();
          } catch (cause) {
            failures.push(cause);
          }
        }
        if (failures.length) throw error("OUTPUT_CLEANUP_FAILED");
      };
      const run = async (operation) => {
        assert(!pending, "OUTPUT_CONCURRENT_OPERATION");
        const promise = operation();
        pending = promise;
        try {
          return await promise;
        } finally {
          pending = undefined;
        }
      };
      try {
        stageHandle = await root.getFileHandle(name, { create: true });
        signal?.throwIfAborted();
        stageWriter = await stageHandle.createWritable({ keepExistingData: false });
        signal?.throwIfAborted();
        state = "writing";
      } catch (cause) {
        try {
          await release();
        } catch (cleanup) {
          // 创建失败尚未返回可重试的输出对象；必须同时报告原始失败与清理失败。
          // Setup has not returned a retryable output object; report both the original and cleanup failures.
          throw new AggregateError([cause, cleanup], "OUTPUT_SETUP_CLEANUP_FAILED", { cause });
        }
        throw cause;
      }
      const output = Object.freeze({
        write(bytes) {
          return run(async () => {
            assert(state === "writing", "OUTPUT_NOT_WRITABLE");
            signal?.throwIfAborted();
            assert(
              bytes instanceof Uint8Array && bytes.byteLength <= STAGED_ZIP_LIMITS.maxChunkBytes,
              "OUTPUT_INVALID_CHUNK",
            );
            assert(bytes.byteLength <= plan.totalBytes - bytesWritten, "OUTPUT_BYTE_LIMIT");
            await stageWriter.write(bytes);
            bytesWritten += bytes.byteLength;
            signal?.throwIfAborted();
          });
        },
        close() {
          return run(async () => {
            assert(state === "validated", "OUTPUT_NOT_VALIDATED");
            signal?.throwIfAborted();
            state = "transferring";
            try {
              destinationWriter = await destinationHandle.createWritable({
                keepExistingData: false,
              });
              signal?.throwIfAborted();
              const source = fileSource(await stageHandle.getFile());
              assert(source.size === plan.totalBytes, "OUTPUT_STAGE_CHANGED");
              // 重新校验实际复制的快照，防止它在首次校验和最终传输之间被悄然替换。
              // Revalidate the snapshot actually being copied. It cannot be
              // silently replaced between initial validation and final transfer.
              await validateStagedZip(source, captured, { archiveFormat, maxOutputBytes, signal });
              for (let offset = 0; offset < source.size;) {
                const length = Math.min(STAGED_ZIP_LIMITS.maxChunkBytes, source.size - offset);
                const bytes = await readRange(source, offset, length, signal);
                await destinationWriter.write(bytes);
                offset += length;
                signal?.throwIfAborted();
              }
              // 清理错误必须发生在不可逆的提交边界之前。
              // Cleanup failures happen before the irreversible commit boundary.
              await remove();
              signal?.throwIfAborted();
              await destinationWriter.close();
              destinationWriter = undefined;
              state = "committed"; // 关闭后不再检查取消。 | No post-close cancellation check.
            } catch (cause) {
              state = "failed";
              try {
                await release();
              } catch {
                // abort() 可以重试清理流以及删除自有条目。
                // abort() can retry stream cleanup and owned-entry removal.
              }
              throw cause;
            }
          });
        },
        abort() {
          cancellation.abort();
          if (abortPending) return abortPending;
          abortPending = (async () => {
            // 等待进行中的浏览器操作结束后，再释放其资源。
            // Drain an in-flight browser operation before disposing its resources.
            if (pending) {
              try {
                await pending;
              } catch {
                // 保留调用者的错误。
                // Preserve caller error.
              }
            }
            if (state === "committed" || state === "aborted") return;
            state = "failed";
            await release();
            state = "aborted";
          })().finally(() => {
            abortPending = undefined;
          });
          return abortPending;
        },
      });
      owned.set(output, {
        async validate(nextManifest, nextSignal) {
          return run(async () => {
            assert(
              state === "writing" && sameManifest(captured, snapshot(nextManifest)),
              "OUTPUT_MANIFEST_MISMATCH",
            );
            signal?.throwIfAborted();
            nextSignal?.throwIfAborted();
            assert(bytesWritten === plan.totalBytes, "OUTPUT_SHORT_ARCHIVE");
            state = "validating";
            await stageWriter.close();
            stageWriter = undefined;
            const source = fileSource(await stageHandle.getFile());
            const validationSignal = nextSignal ? AbortSignal.any([signal, nextSignal]) : signal;
            const result = await validateStagedZip(source, captured, {
              archiveFormat,
              maxOutputBytes,
              signal: validationSignal,
            });
            nextSignal?.throwIfAborted();
            state = "validated";
            return result;
          });
        },
      });
      return output;
    },
    async validateArchive({ output, manifest, signal }) {
      const owner = owned.get(output);
      assert(owner, "OUTPUT_NOT_OWNED");
      return owner.validate(manifest, signal);
    },
  };
}
