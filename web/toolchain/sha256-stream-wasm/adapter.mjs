// 依据固定 SHA-256 ABI 顺序读取本地 File/Blob 小片段，取消不会影响其他散列所有者。
// Read bounded local File/Blob slices through the pinned SHA-256 ABI; cancellation cannot affect another owner.
export const SHA256_CHUNK_BYTES = 65536;
export const SHA256_MEMORY_BYTES = 262144;
const check = (signal) => signal?.throwIfAborted();
const wipeFileBuffer = (buffer) => {
  if (buffer instanceof ArrayBuffer && buffer.byteLength) new Uint8Array(buffer).fill(0);
};
async function cancellable(read, signal) {
  check(signal);
  let pending,
    delivered = false;
  let remove = () => {};
  try {
    pending = Promise.resolve().then(read);
    const buffer = await (signal
      ? Promise.race([
          pending,
          new Promise((_, reject) => {
            const abort = () => reject(signal.reason);
            signal.addEventListener("abort", abort, { once: true });
            remove = () => signal.removeEventListener("abort", abort);
            if (signal.aborted) abort();
          }),
        ])
      : pending);
    delivered = true;
    return buffer;
  } finally {
    remove();
    // 已取消的等待不转移文件缓冲区所有权；迟到结果必须擦除且拒绝必须被观察。
    // Cancelled waits transfer no File-buffer ownership; wipe late results and observe rejections.
    if (!delivered && pending) pending.then(wipeFileBuffer).catch(() => {});
  }
}

/**
 * 低层 ABI 测试接缝；生产方应使用验证零导入模块的 createSha256FileHasher。
 * Low-level ABI test seam; production callers should use the zero-import-checking module factory.
 */
export function createSha256FileHasherFromAbi(wasm) {
  for (const name of [
    "sha_input",
    "sha_output",
    "sha_begin",
    "sha_update",
    "sha_finish",
    "sha_abort",
  ])
    if (typeof wasm?.[name] !== "function") throw new TypeError("Missing SHA-256 ABI");
  if (
    !(wasm.memory instanceof WebAssembly.Memory) ||
    wasm.memory.buffer.byteLength !== SHA256_MEMORY_BYTES
  )
    throw new TypeError("Invalid fixed SHA-256 memory");
  let current = 0;
  const range = (pointer, length) => {
    if (
      !Number.isInteger(pointer) ||
      pointer < 0 ||
      pointer > wasm.memory.buffer.byteLength - length
    )
      throw new Error("Invalid SHA-256 ABI range");
    return new Uint8Array(wasm.memory.buffer, pointer, length);
  };
  return Object.freeze({
    async hashFile(file, { signal, onProgress } = {}) {
      if (!(file instanceof Blob)) throw new TypeError("Expected File or Blob");
      const size = file.size;
      if (!Number.isSafeInteger(size) || size < 0)
        throw new TypeError("Expected bounded File or Blob");
      if (signal !== undefined && !(signal instanceof AbortSignal))
        throw new TypeError("Expected AbortSignal");
      if (onProgress !== undefined && typeof onProgress !== "function")
        throw new TypeError("Expected progress observer");
      check(signal);
      if (current) throw new Error("SHA-256 instance is busy");
      const owner = wasm.sha_begin() >>> 0;
      if (!owner) throw new Error("Cannot begin SHA-256");
      current = owner;
      const release = () => {
        if (current !== owner) return;
        current = 0;
        try {
          wasm.sha_abort(owner);
        } catch {
          // 保留读错误或取消原因；不得向另一个所有者重试清理。
          // Preserve read/cancel failures; never retry cleanup against another owner.
        }
      };
      signal?.addEventListener("abort", release, { once: true });
      let bytesHashed = 0;
      try {
        check(signal);
        for (let offset = 0; offset < size;) {
          const length = Math.min(SHA256_CHUNK_BYTES, size - offset);
          const buffer = await cancellable(
            () => file.slice(offset, offset + length).arrayBuffer(),
            signal,
          );
          try {
            check(signal);
            if (current !== owner) throw new Error("SHA-256 ownership lost");
            if (
              !(buffer instanceof ArrayBuffer) ||
              buffer.resizable ||
              buffer.byteLength !== length
            )
              throw new Error("Invalid or short SHA-256 file slice");
            range(wasm.sha_input(), length).set(new Uint8Array(buffer));
            if (wasm.sha_update(owner, length) !== 0) throw new Error("SHA-256 update failed");
          } finally {
            // WASM 已复制输入；成功或失败后均擦除私有读取副本，不修改原始 Blob。
            // WASM has copied the input; wipe private read copies after success or failure, never the source Blob.
            wipeFileBuffer(buffer);
          }
          bytesHashed += length;
          offset += length;
          // 仅在实际散列更新完成后报告计数；观察者失败不影响散列所有权。
          // Report counts only after actual hash updates; observer failures never affect hash ownership.
          if (onProgress) {
            try {
              Promise.resolve(onProgress(Object.freeze({ bytesHashed, totalBytes: size }))).catch(
                () => {},
              );
            } catch {
              /* 保留散列状态。 | Preserve hash state. */
            }
          }
          // 给任务队列让出执行机会，使大文件取消可在有限块之间生效。
          // Yield to the task queue so large-file cancellation is observable between bounded chunks.
          if (bytesHashed % (SHA256_CHUNK_BYTES * 16) === 0)
            await new Promise((resolve) => setTimeout(resolve, 0));
        }
        check(signal);
        if (current !== owner || wasm.sha_finish(owner) !== 0)
          throw new Error("SHA-256 finish failed");
        const digest = range(wasm.sha_output(), 32);
        const sha256 = Array.from(digest, (byte) => byte.toString(16).padStart(2, "0")).join("");
        digest.fill(0);
        current = 0;
        return Object.freeze({ sha256, bytesHashed });
      } finally {
        signal?.removeEventListener("abort", release);
        release();
      }
    },
  });
}

/**
 * 模块字节的来源/摘要必须由发布加载器验证；此处验证导入闭包和固定内存。
 * Release loaders must verify module provenance/digest; this factory checks import closure and fixed memory.
 */
export async function createSha256FileHasher(bytes) {
  const module = bytes instanceof WebAssembly.Module ? bytes : await WebAssembly.compile(bytes);
  if (WebAssembly.Module.imports(module).length) throw new Error("Unexpected SHA-256 host imports");
  const { exports: wasm } = await WebAssembly.instantiate(module);
  wasm._initialize?.();
  if (
    !(wasm.memory instanceof WebAssembly.Memory) ||
    wasm.memory.buffer.byteLength !== SHA256_MEMORY_BYTES
  )
    throw new Error("Invalid fixed SHA-256 memory");
  let fixed = false;
  try {
    wasm.memory.grow(1);
  } catch {
    fixed = true;
  }
  if (!fixed) throw new Error("SHA-256 memory must not grow");
  return createSha256FileHasherFromAbi(wasm);
}
