// 为 Bun 端到端测试提供文件读取兼容层；依据 web/scripts 的模块与调用约定。
// Provide file-reader compatibility for Bun end-to-end tests using the module and calling contracts in web/scripts.
import { openSync, readSync, closeSync } from "node:fs";

const originalSlice = Blob.prototype.slice;
Blob.prototype.slice = function (start = 0, end = this.size, type) {
  const sliced = originalSlice.call(this, start, end, type);
  const path =
    this.__bunSourcePath ??
    (typeof this.name === "string" && this.name.startsWith("/") ? this.name : undefined);
  if (path) {
    const offset = start < 0 ? Math.max(0, this.size + start) : Math.min(this.size, start);
    Object.defineProperty(sliced, "__bunSourcePath", { value: path });
    Object.defineProperty(sliced, "__bunSourceOffset", {
      value: (this.__bunSourceOffset ?? 0) + offset,
    });
  }
  return sliced;
};

globalThis.FileReaderSync = class {
  readAsArrayBuffer(blob) {
    const path =
      blob.__bunSourcePath ??
      (typeof blob.name === "string" && blob.name.startsWith("/") ? blob.name : undefined);
    if (!path) throw new Error("BUN_FILE_READER_SOURCE_UNKNOWN");
    const offset = blob.__bunSourceOffset ?? 0;
    const bytes = Buffer.allocUnsafe(blob.size);
    const fd = openSync(path, "r");
    try {
      for (let at = 0; at < bytes.length;) {
        const count = readSync(fd, bytes, at, bytes.length - at, offset + at);
        if (count === 0) throw new Error("BUN_FILE_READER_SHORT_READ");
        at += count;
      }
      return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    } finally {
      closeSync(fd);
    }
  }
};
