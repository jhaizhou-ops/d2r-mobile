// 依据 boundary.c 的有界接口封装专用 Worker 的 WASM 元数据桥接。
// Wrap the dedicated Worker WASM metadata bridge based on the bounded boundary.c interface.
import { extractionDiagnosticError } from "../../extraction-errors.mjs";
// 必须在新建的专用 Worker 中运行。这是元数据优先的桥接层，
// 并非应用的生产 NSP 输入校验器或 IPA 构建器。
// Must run in a fresh dedicated Worker. This is a metadata-first bridge, not
// the application's production NSP input validator or an IPA builder.
const MAX_KEYS = 1024 * 1024;
const CHUNK = 64 * 1024;
const messages = new Map([
  [-1, "Extractor state or input is invalid"],
  [-2, "Container metadata exceeds supported bounds"],
  [-3, "Required key or content integrity verification failed"],
  [-4, "This content format is not supported by the metadata boundary"],
  [-5, "Local file read failed"],
]);
const check = (result) => {
  if (result === -6) throw extractionDiagnosticError("TITLE_KEY_REQUIRED");
  if (result < 0) throw new Error(messages.get(result) || "Extraction failed");
  return result;
};
export async function createBoundary(factory) {
  if (typeof FileReaderSync === "undefined")
    throw new Error("A dedicated browser Worker is required");
  let activeVerificationProgress = null;
  let nativeProgressSeen = false;
  // 连运行时诊断也要抑制；异常绝不暴露上游文本。
  // Suppress even runtime diagnostics; exceptions never expose upstream text.
  const module = await factory({
    print() {},
    printErr() {},
    onAbort() {},
    onVerificationProgress(phase, completed, total) {
      if (
        !activeVerificationProgress ||
        ![1, 2].includes(phase) ||
        ![completed, total].every(Number.isSafeInteger) ||
        total <= 0 ||
        completed < 0 ||
        completed > total
      )
        return;
      nativeProgressSeen = true;
      try {
        activeVerificationProgress({
          phase: phase === 1 ? "base" : "merged",
          completed,
          total,
        });
      } catch {
        // 进度观察者不能中断认证校验。
        // Progress observers cannot interrupt authenticated verification.
      }
    },
  });
  module.FS.mkdir("/input");
  module.FS.mkdir("/secret");
  let mounted = false,
    disposed = false,
    generation = 0;
  const guard = (fn) => {
    if (disposed) throw new Error("Extractor has been disposed");
    try {
      return fn();
    } catch (error) {
      if (error?.code === "TITLE_KEY_REQUIRED")
        throw extractionDiagnosticError("TITLE_KEY_REQUIRED");
      throw new Error("Local extraction operation failed");
    }
  };
  const wipeFile = (path) => {
    try {
      const node = module.FS.lookupPath(path).node;
      if (node.contents?.fill) node.contents.fill(0);
      module.FS.unlink(path);
    } catch {
      /* 文件可能尚未创建。绝不记录解析器状态。
       * File may not have been created. Never log parser state. */
    }
  };
  const withBuffer = (size, fn) => {
    const ptr = module._malloc(size);
    if (!ptr) throw new Error("Extractor memory limit reached");
    try {
      return fn(ptr);
    } finally {
      module._nsp_zero(ptr, size);
      module._free(ptr);
    }
  };
  const path = (index) => {
    if (!Number.isSafeInteger(index) || index < 0) throw new Error("Invalid input index");
    return `/input/input-${index}`;
  };
  return {
    mount(blobs) {
      return guard(() => {
        generation++;
        module._nsp_close();
        if (mounted) {
          module.FS.unmount("/input");
          mounted = false;
        }
        if (
          !Array.isArray(blobs) ||
          blobs.length > 100000 ||
          blobs.some((blob) => !(blob instanceof Blob))
        )
          throw new Error("Invalid local input");
        module.FS.mount(
          module.WORKERFS,
          { blobs: blobs.map((data, i) => ({ name: `input-${i}`, data })) },
          "/input",
        );
        mounted = true;
      });
    },
    loadKeys(prod, title) {
      // 接收可变数组，而非不可变的已解码密钥字符串。调用方将 ArrayBuffer
      // 转移到此 Worker；所有路径都会擦除两个数组。
      // Take mutable arrays, not immutable decoded key strings. Caller transfers
      // ArrayBuffers into this Worker; both arrays are wiped in every path.
      try {
        return guard(() => {
          // 失败的替换也必须撤销旧读取器并擦除旧密钥。
          // Even a failed replacement revokes old readers and clears old keys.
          generation++;
          module._nsp_reset();
          try {
            if (
              !(prod instanceof Uint8Array) ||
              !(title instanceof Uint8Array) ||
              prod.length > MAX_KEYS ||
              title.length > MAX_KEYS ||
              !prod.length
            )
              throw new Error("Invalid key input");
            module.FS.writeFile("/secret/prod.keys", prod);
            module.FS.writeFile("/secret/title.keys", title);
            return check(module._nsp_load_keys());
          } catch (error) {
            // 原生加载也可能在已解析部分密钥后失败。
            // Native loading can fail after parsing some key material.
            module._nsp_reset();
            throw error;
          }
        });
      } finally {
        prod?.fill?.(0);
        title?.fill?.(0);
        wipeFile("/secret/prod.keys");
        wipeFile("/secret/title.keys");
      }
    },
    inspect(index) {
      return guard(() =>
        withBuffer(24, (ptr) => {
          generation++;
          check(
            module.ccall("nsp_inspect_nca", "number", ["string", "number"], [path(index), ptr]),
          );
          const view = new DataView(module.HEAPU8.buffer, ptr, 24);
          return {
            applicationId: view.getBigUint64(0, true).toString(16).padStart(16, "0"),
            size: view.getBigUint64(8, true),
            contentType: view.getUint32(16, true),
            fixedSignatureValid: view.getUint32(20, true) === 1,
          };
        }),
      );
    },
    open(index, kind = "meta") {
      return guard(() => {
        generation++;
        const operation =
          kind === "meta"
            ? "nsp_open_meta"
            : kind === "warlock-meta"
              ? "nsp_open_warlock_meta"
              : kind === "program-exefs"
                ? "nsp_open_exefs"
                : kind === "synthetic-pfs0"
                  ? "nsp_open_pfs0"
                  : null;
        if (!operation) throw new Error("Unsupported operation");
        check(module.ccall(operation, "number", ["string"], [path(index)]));
        return check(module._nsp_entry_count());
      });
    },
    // 读取器生命周期绑定到这个已认证 NCA。后续打开、检查、加载密钥、
    // 挂载或输入散列操作即使失败，也会使读取器失效。
    // Reader lifetime is bound to this exact authenticated NCA. A later open,
    // inspect, key load, mount or input hash invalidates it even if that fails.
    openPatchedRomfs(baseIndex, patchIndex, onReadProgress, verifyRomfs = true) {
      if (!Number.isSafeInteger(patchIndex) || patchIndex < 0) {
        ++generation;
        throw new Error("Invalid patch index");
      }
      return this.openRomfs(baseIndex, patchIndex, onReadProgress, verifyRomfs);
    },
    openRomfs(index, patchIndex, onReadProgress, verifyRomfs = true) {
      return guard(() => {
        const current = ++generation;
        if (typeof verifyRomfs !== "boolean") throw new Error("Invalid RomFS verification mode");
        activeVerificationProgress = onReadProgress;
        nativeProgressSeen = false;
        const streamOps = module.WORKERFS.stream_ops;
        const originalRead = streamOps.read;
        const reportStep = 64 * 1024 * 1024;
        let bytesRead = 0;
        let nextReport = reportStep;
        let lastReported = 0;
        const report = () => {
          if (nativeProgressSeen || bytesRead === lastReported) return;
          try {
            onReadProgress?.({ phase: "read", completed: bytesRead, total: 0 });
          } catch {
            // 进度观察者不能中断认证校验。
            // Progress observers cannot interrupt authenticated verification.
          }
          lastReported = bytesRead;
        };
        streamOps.read = (...args) => {
          const count = originalRead(...args);
          if (Number.isSafeInteger(count) && count > 0) {
            bytesRead += count;
            if (bytesRead >= nextReport) {
              report();
              nextReport = bytesRead + reportStep;
            }
          }
          return count;
        };
        let size;
        try {
          size = withBuffer(8, (ptr) => {
            check(
              patchIndex === undefined
                ? module.ccall(
                    verifyRomfs ? "nsp_open_romfs" : "nsp_open_romfs_hash_verified",
                    "number",
                    ["string", "number"],
                    [path(index), ptr],
                  )
                : module.ccall(
                    verifyRomfs ? "nsp_open_patched_romfs" : "nsp_open_patched_romfs_hash_verified",
                    "number",
                    ["string", "string", "number"],
                    [path(index), path(patchIndex), ptr],
                  ),
            );
            return new DataView(module.HEAPU8.buffer, ptr, 8).getBigUint64(0, true);
          });
          report();
        } finally {
          streamOps.read = originalRead;
          activeVerificationProgress = null;
        }
        return Object.freeze({
          size,
          readAt(offset, count) {
            return guard(() => {
              if (current !== generation) throw new Error("RomFS reader expired");
              if (
                typeof offset !== "bigint" ||
                offset < 0n ||
                offset > size ||
                !Number.isInteger(count) ||
                count < 0 ||
                count > CHUNK ||
                BigInt(count) > size - offset
              )
                throw new Error("Invalid bounded read");
              return withBuffer(Math.max(count, 1), (ptr) => {
                const length = check(
                  module._nsp_read_romfs(
                    Number(offset & 0xffffffffn),
                    Number(offset >> 32n),
                    ptr,
                    count,
                  ),
                );
                return module.HEAPU8.slice(ptr, ptr + length);
              });
            });
          },
        });
      });
    },
    // 整个 NCA 的输入摘要独立于头部或 CNMT 声明。
    // 计算输入散列会关闭当前成员目录；之后需要重新打开。
    // Whole-NCA input digest is independent of header/CNMT declarations.
    // Hashing an input closes the current member directory; reopen afterward.
    hashInput(index) {
      return guard(() =>
        withBuffer(32, (ptr) => {
          generation++;
          check(module.ccall("nsp_hash_input", "number", ["string", "number"], [path(index), ptr]));
          return Array.from(module.HEAPU8.subarray(ptr, ptr + 32), (byte) =>
            byte.toString(16).padStart(2, "0"),
          ).join("");
        }),
      );
    },
    hashEntry(index) {
      if (!Number.isInteger(index) || index < 0 || index > 0xffffffff)
        throw new Error("Invalid entry index");
      return guard(() =>
        withBuffer(32, (ptr) => {
          check(module._nsp_hash_entry(index, ptr));
          return Array.from(module.HEAPU8.subarray(ptr, ptr + 32), (byte) =>
            byte.toString(16).padStart(2, "0"),
          ).join("");
        }),
      );
    },
    entry(index) {
      if (!Number.isInteger(index) || index < 0 || index > 0xffffffff)
        throw new Error("Invalid entry index");
      return guard(() =>
        withBuffer(4096, (ptr) => {
          const length = check(module._nsp_entry_info(index, ptr, 4096));
          const view = new DataView(module.HEAPU8.buffer, ptr, length);
          const name = new TextDecoder("utf-8", { fatal: true }).decode(
            module.HEAPU8.subarray(ptr + 16, ptr + length - 1),
          );
          return { name, size: view.getBigUint64(8, true), offset: view.getBigUint64(0, true) };
        }),
      );
    },
    read(index, offset, count) {
      if (!Number.isInteger(index) || index < 0 || index > 0xffffffff)
        throw new Error("Invalid entry index");
      return guard(() => {
        if (
          typeof offset !== "bigint" ||
          offset < 0n ||
          offset > 0xffffffffffffffffn ||
          !Number.isInteger(count) ||
          count < 0 ||
          count > CHUNK
        )
          throw new Error("Invalid bounded read");
        return withBuffer(Math.max(count, 1), (ptr) => {
          const size = check(
            module._nsp_read_entry(
              index,
              Number(offset & 0xffffffffn),
              Number(offset >> 32n),
              ptr,
              count,
            ),
          );
          return module.HEAPU8.slice(ptr, ptr + size);
        });
      });
    },
    async streamEntry(index, sink, signal) {
      const { size } = this.entry(index);
      for (let offset = 0n; offset < size;) {
        if (signal?.aborted) throw new Error("Extraction cancelled");
        const count = Number(size - offset > BigInt(CHUNK) ? BigInt(CHUNK) : size - offset);
        const bytes = this.read(index, offset, count);
        try {
          await sink(bytes);
        } finally {
          bytes.fill(0);
        }
        offset += BigInt(count);
      }
    },
    cryptoFixture() {
      return guard(() => check(module._nsp_crypto_fixture()));
    },
    bktrFixture() {
      return guard(() => check(module._nsp_bktr_fixture()));
    },
    ivfcFixture() {
      return guard(() => check(module._nsp_ivfc_fixture()));
    },
    secretFilesPresent() {
      return module.FS.readdir("/secret").some((name) => name !== "." && name !== "..");
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      try {
        module._nsp_reset();
      } finally {
        wipeFile("/secret/prod.keys");
        wipeFile("/secret/title.keys");
        try {
          if (mounted) module.FS.unmount("/input");
        } finally {
          // 即使销毁或卸载失败，也擦除持有的线性内存视图。所有者随后必须终止
          // 这个一次性 Worker；不要再次调用 C。
          // Even if teardown or unmount fails, erase the owned linear-memory view. The owner
          // must terminate this one-shot Worker next; do not call C again.
          module.HEAPU8.fill(0);
        }
      }
    },
  };
}
