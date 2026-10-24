// 在调用方提供的 OPFS 根目录中暂存有界原始文件，只清理自己创建的唯一子目录。
// Stage bounded raw files in a caller-provided OPFS root, cleaning only the unique child this module creates.
export const RAW_FILE_STAGE_CHUNK_BYTES = 1024 * 1024;
const fail = (code) => {
  throw new Error(code);
};
const validInteger = (value) => Number.isSafeInteger(value) && value >= 0;

/**
 * 不访问源文件夹、不联网、不回退到内存；maxBytes 必须是显式安全整数。
 * No source-folder access, networking or RAM fallback; maxBytes must be an explicit safe integer.
 */
export async function createRawFileStage({
  root,
  maxBytes,
  signal,
  randomUUID = () => globalThis.crypto.randomUUID(),
} = {}) {
  if (!validInteger(maxBytes)) fail("RAW_STAGE_INVALID_LIMIT");
  if (signal !== undefined && !(signal instanceof AbortSignal)) fail("RAW_STAGE_INVALID_SIGNAL");
  if (
    root?.kind !== "directory" ||
    typeof root.getDirectoryHandle !== "function" ||
    typeof root.removeEntry !== "function"
  )
    fail("RAW_STAGE_ROOT_REQUIRED");
  signal?.throwIfAborted();
  const uuid = randomUUID();
  if (
    typeof uuid !== "string" ||
    !/^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/i.test(uuid)
  )
    fail("RAW_STAGE_INVALID_ID");
  const name = `d2rm-raw-${uuid}`;
  // OPFS 没有独占创建；加密随机标识和存在性检查不能防御不可信的同源脚本。
  // OPFS has no exclusive creation; random IDs and existence checks do not defend against untrusted same-origin scripts.
  try {
    await root.getDirectoryHandle(name);
    fail("RAW_STAGE_COLLISION");
  } catch (cause) {
    if (cause?.name !== "NotFoundError") throw cause;
  }
  signal?.throwIfAborted();
  let child, handle, writer, snapshot, pending, disposal;
  let state = "opening",
    size = 0,
    removed = false,
    disposed = false;
  const check = () => {
    signal?.throwIfAborted();
    if (disposed) fail("RAW_STAGE_DISPOSED");
  };
  const release = async () => {
    const errors = [];
    snapshot = undefined;
    if (writer) {
      try {
        await writer.abort();
        writer = undefined;
      } catch (cause) {
        errors.push(cause);
      }
    }
    // 终止失败不能证明写入已停止；保留写入器和目录供 dispose 重试。
    // A failed abort does not prove writes stopped; retain writer and child for dispose retry.
    if (!writer && child && !removed) {
      try {
        await root.removeEntry(name, { recursive: true });
        removed = true;
        child = handle = undefined;
      } catch (cause) {
        if (cause?.name === "NotFoundError") {
          removed = true;
          child = handle = undefined;
        } else errors.push(cause);
      }
    }
    if (errors.length) throw new AggregateError(errors, "RAW_STAGE_CLEANUP_FAILED");
  };
  const detach = () => signal?.removeEventListener("abort", onAbort);
  function dispose() {
    disposed = true;
    detach();
    if (disposal) return disposal;
    disposal = (async () => {
      // 先等进行中的文件系统调用返回，再清理，避免关闭与写入发生竞争。
      // Drain any in-flight filesystem operation before cleanup to avoid racing writes with closure.
      if (pending) {
        try {
          await pending;
        } catch {
          /* 原调用者接收错误。 | The original caller receives the error. */
        }
      }
      state = "disposed";
      await release();
    })().finally(() => {
      disposal = undefined;
    });
    return disposal;
  }
  function onAbort() {
    // 保留失败的清理资源；调用者可通过 dispose 重试并观察清理错误。
    // Retain failed cleanup resources; callers can retry and observe cleanup errors through dispose.
    void dispose().catch(() => {});
  }
  const run = (operation) => {
    if (disposed)
      return Promise.reject(signal?.aborted ? signal.reason : new Error("RAW_STAGE_DISPOSED"));
    if (pending) return Promise.reject(new Error("RAW_STAGE_CONCURRENT_OPERATION"));
    const task = Promise.resolve().then(async () => {
      try {
        return await operation();
      } catch (cause) {
        state = "failed";
        detach();
        try {
          await release();
        } catch (cleanup) {
          throw new AggregateError([cause, cleanup], "RAW_STAGE_OPERATION_AND_CLEANUP_FAILED", {
            cause,
          });
        }
        throw cause;
      }
    });
    pending = task;
    return task.finally(() => {
      if (pending === task) pending = undefined;
    });
  };
  try {
    child = await root.getDirectoryHandle(name, { create: true });
    check();
    handle = await child.getFileHandle("data.bin", { create: true });
    check();
    writer = await handle.createWritable({ keepExistingData: false });
    check();
    state = "writing";
    signal?.addEventListener("abort", onAbort, { once: true });
    check();
  } catch (cause) {
    detach();
    try {
      await release();
    } catch (cleanup) {
      throw new AggregateError([cause, cleanup], "RAW_STAGE_OPERATION_AND_CLEANUP_FAILED", {
        cause,
      });
    }
    throw cause;
  }
  const validateWrite = (bytes) => {
    check();
    if (state !== "writing") fail("RAW_STAGE_NOT_WRITABLE");
    if (
      !(bytes instanceof Uint8Array) ||
      !(bytes.buffer instanceof ArrayBuffer) ||
      bytes.buffer.resizable === true ||
      bytes.buffer.byteLength > RAW_FILE_STAGE_CHUNK_BYTES ||
      bytes.byteLength > RAW_FILE_STAGE_CHUNK_BYTES
    )
      fail("RAW_STAGE_INVALID_CHUNK");
  };
  return Object.freeze({
    append(bytes) {
      return run(async () => {
        validateWrite(bytes);
        if (bytes.length > maxBytes - size) fail("RAW_STAGE_BYTE_LIMIT");
        await writer.write({ type: "write", position: size, data: bytes });
        check();
        size += bytes.length;
      });
    },
    backpatch(offset, bytes) {
      return run(async () => {
        validateWrite(bytes);
        if (!validInteger(offset) || offset > size || bytes.length > size - offset)
          fail("RAW_STAGE_PATCH_RANGE");
        await writer.write({ type: "write", position: offset, data: bytes });
        check();
      });
    },
    finalize() {
      return run(async () => {
        check();
        if (state !== "writing") fail("RAW_STAGE_NOT_WRITABLE");
        state = "closing";
        await writer.close();
        writer = undefined;
        check();
        snapshot = await handle.getFile();
        check();
        if (snapshot.size !== size || typeof snapshot.slice !== "function")
          fail("RAW_STAGE_SIZE_CHANGED");
        state = "finalized";
        return Object.freeze({
          file: snapshot,
          size,
          readAt(offset, length) {
            return run(async () => {
              check();
              if (state !== "finalized") fail("RAW_STAGE_NOT_READABLE");
              if (
                !validInteger(offset) ||
                !validInteger(length) ||
                length > RAW_FILE_STAGE_CHUNK_BYTES ||
                offset > size ||
                length > size - offset
              )
                fail("RAW_STAGE_READ_RANGE");
              const bytes = new Uint8Array(
                await snapshot.slice(offset, offset + length).arrayBuffer(),
              );
              try {
                check();
                if (bytes.length !== length) fail("RAW_STAGE_SHORT_READ");
                return bytes;
              } catch (cause) {
                // 成功返回前仍拥有读取副本；失败时清零，但不修改已交给调用方的缓冲区。
                // Own the read copy until successful return; wipe failures, never buffers already handed to callers.
                bytes.fill(0);
                throw cause;
              }
            });
          },
        });
      });
    },
    dispose,
  });
}
