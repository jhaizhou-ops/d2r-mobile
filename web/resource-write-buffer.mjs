// 固定 1 MiB 文件写缓冲；输入仍限 64 KiB，不并发写入，不修改来源视图。
// Fixed 1 MiB filesystem write buffer; inputs stay capped at 64 KiB, writes are serial, source views are unchanged.
export const RESOURCE_WRITE_BUFFER_BYTES = 1024 * 1024;
export function createResourceWriteBuffer({ write, written, check }) {
  let buffer,
    used = 0,
    disposed = false,
    busy = false;
  const ensure = () => {
    check();
    if (disposed) throw new Error("RESOURCE_WRITE_BUFFER_DISPOSED");
  };
  async function serial(operation) {
    ensure();
    if (busy) throw new Error("RESOURCE_WRITE_BUFFER_BUSY");
    busy = true;
    try {
      return await operation();
    } finally {
      busy = false;
    }
  }
  async function flush() {
    ensure();
    if (!used) return;
    const count = used;
    try {
      await write(buffer.subarray(0, count));
      written(count);
      check();
    } finally {
      // 未使用的尾部从初始化或上次刷新起就保持为零。
      // The unused tail is already zero from initialization or the previous flush.
      buffer.fill(0, 0, count);
      used = 0;
    }
  }
  return Object.freeze({
    append(bytes) {
      return serial(async () => {
        ensure();
        if (!(bytes instanceof Uint8Array) || bytes.length > 64 * 1024)
          throw new Error("RESOURCE_WRITE_BUFFER_INPUT");
        if (!bytes.length) return;
        buffer ??= new Uint8Array(RESOURCE_WRITE_BUFFER_BYTES);
        if (bytes.length > buffer.length - used) await flush();
        buffer.set(bytes, used);
        used += bytes.length;
        if (used === buffer.length) await flush();
        check();
      });
    },
    flush: () => serial(flush),
    dispose() {
      if (busy) throw new Error("RESOURCE_WRITE_BUFFER_BUSY");
      buffer?.fill(0);
      buffer = undefined;
      used = 0;
      disposed = true;
    },
  });
}
