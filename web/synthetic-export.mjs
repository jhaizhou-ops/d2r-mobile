// 将固定生成的测试输入导出为不可安装的合成归档。
// 依据 Guest 框架生成器与 ZIP 写入器的接口约定。
// Exports fixed generated test inputs as a non-installable synthetic archive.
// Based on the Guest framework generator and ZIP writer interface contracts.

import { buildGuestFramework, guestInfoPlist, PAGE_SIZE } from "./guest-framework.mjs";
import { writeZip } from "./zip-writer.mjs";

export const SYNTHETIC_FILENAME = "d2r-packager-synthetic-test.zip";
export const SYNTHETIC_CHUNK_BYTES = 64 * 1024;
const encoder = new TextEncoder();
const readme = `NOT INSTALLABLE — SYNTHETIC TEST ONLY
This ZIP is NOT an IPA and contains NO GAME CONTENT.
Guest.framework/Guest contains generated test bytes, not a game executable.
No app shell, assets, shaders, entitlements or installation support is included.
This archive tests local Guest framework generation and streamed ZIP output only.
Do not rename this archive to .ipa. It cannot be installed or played.
`;

// 仅使用固定的微小生成输入。绝不接受用户文件或模块。
// Fixed tiny generated input only. Never accepts user files or modules.
export function syntheticModules() {
  return ["main", "sdk"].map((name, module) => ({
    name,
    image: Uint8Array.from({ length: 32 }, (_, i) => (module * 53 + i * 7) & 255),
    textEnd: 32,
    patchOff: PAGE_SIZE,
  }));
}
function entry(path, bytes, mode = 0o644) {
  return {
    path,
    size: bytes.length,
    mode,
    chunks: async function* () {
      for (let i = 0; i < bytes.length; i += SYNTHETIC_CHUNK_BYTES) {
        yield bytes.subarray(i, i + SYNTHETIC_CHUNK_BYTES);
      }
    },
  };
}

/**
 * 接管已打开的输出接收器，包括 writeZip 开始前发生的失败。
 * 浏览器可写流在 close 前暂存写入；abort 丢弃暂存数据。
 * 保存选择器仍可能创建空占位文件。绝不承诺删除。
 * close 是提交边界；过晚的取消无法撤销已完成的输出。
 * Owns an already-open output sink, including failures before writeZip starts.
 * Browser writable streams stage writes until close; abort discards staged data.
 * A save picker can still create an empty placeholder. Never promises deletion.
 * close is the commit boundary; late cancellation cannot undo completed output.
 * @param {{write(bytes: Uint8Array<ArrayBuffer>): Promise<void>, close(): Promise<void>, abort(reason?: unknown): Promise<void>}} sink
 * @param {{signal?: AbortSignal, crypto?: Crypto, onProgress?: (progress: {phase: string, bytesWritten: number}) => void}} [options]
 */
export async function exportSyntheticArchive(
  sink,
  {
    signal,
    crypto = globalThis.crypto,
    onProgress = (_progress) => {
      void _progress;
    },
  } = {},
) {
  let aborted = false,
    closed = false,
    bytesWritten = 0;
  const output = {
    async write(bytes) {
      await sink.write(bytes);
      bytesWritten += bytes.length;
      onProgress({ phase: "saving", bytesWritten });
    },
    async close() {
      onProgress({ phase: "finishing", bytesWritten });
      await sink.close();
      closed = true;
    },
    async abort(error) {
      if (aborted || closed) return;
      aborted = true;
      await sink.abort(error);
    },
  };
  try {
    signal?.throwIfAborted();
    onProgress({ phase: "generating", bytesWritten });
    const guest = await buildGuestFramework(syntheticModules(), { signal, crypto });
    signal?.throwIfAborted();
    const entries = [
      entry("README-NOT-INSTALLABLE.txt", encoder.encode(readme)),
      entry("Guest.framework/Guest", guest, 0o755),
      entry("Guest.framework/Info.plist", encoder.encode(guestInfoPlist("local.synthetic.guest"))),
    ];
    return await writeZip(entries, output, { signal });
  } catch (error) {
    try {
      await output.abort(error);
    } catch {
      /**
       * 保留原始错误。
       * Preserve the original error.
       */
    }
    throw error;
  }
}
