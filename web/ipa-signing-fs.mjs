// 只读 Blob 资源和有界写时复制覆盖层；不依赖特定 WASM ABI。
// Read-only Blob resources and a bounded copy-on-write overlay, independent of a particular WASM ABI.
import { indexFiles } from "./preflight.mjs";
import { signingError } from "./ipa-signing-contract.mjs";

export const SIGNING_FS_LIMITS = Object.freeze({
  readBytes: 65536,
  mutableFileBytes: 128 * 1024 * 1024,
  mutableTotalBytes: 256 * 1024 * 1024,
  metadataBytes: 8 * 1024 * 1024,
});
const requireValue = (value) => {
  if (!value) throw signingError("SIGNING_INPUT_INVALID");
};
const range = (offset, length) =>
  Number.isSafeInteger(offset) &&
  offset >= 0 &&
  Number.isSafeInteger(length) &&
  length >= 0 &&
  Number.isSafeInteger(offset + length);
const machMagic = (bytes) =>
  bytes.length === 4 &&
  [
    0xfeedface, 0xfeedfacf, 0xcefaedfe, 0xcffaedfe, 0xcafebabe, 0xbebafeca, 0xcafebabf, 0xbfbafeca,
  ].includes(new DataView(bytes.buffer, bytes.byteOffset, 4).getUint32(0));

export function createSigningFileStore(archive, { readBlobSync } = {}) {
  requireValue(
    archive &&
      Array.isArray(archive.entries) &&
      archive.entries.length <= 20000 &&
      typeof archive.appRoot === "string" &&
      /^Payload\/[^/]+\.app\/$/.test(archive.appRoot),
  );
  if (!readBlobSync) {
    requireValue(typeof globalThis.FileReaderSync === "function");
    const reader = new globalThis.FileReaderSync();
    readBlobSync = (blob) => new Uint8Array(reader.readAsArrayBuffer(blob));
  }
  requireValue(typeof readBlobSync === "function");
  const original = new Map(),
    files = new Map(),
    executables = new Set(),
    bundles = new Set([archive.appRoot.slice(0, -1)]);
  let allocated = 0,
    disposed = false;
  const check = () => {
    requireValue(!disposed);
  };
  const readBlob = (blob, offset, length) => {
    requireValue(
      range(offset, length) &&
        length <= SIGNING_FS_LIMITS.readBytes &&
        offset + length <= blob.size,
    );
    const bytes = readBlobSync(blob.slice(offset, offset + length));
    requireValue(bytes instanceof Uint8Array && bytes.length === length);
    return bytes;
  };
  try {
    indexFiles(archive.entries);
    let executableBytes = 0;
    for (const entry of archive.entries) {
      requireValue(
        entry.path.startsWith(archive.appRoot) &&
          entry.blob instanceof Blob &&
          entry.size === entry.blob.size &&
          [0o644, 0o755].includes(entry.mode),
      );
      original.set(entry.path, { ...entry });
      files.set(entry.path, { ...entry, data: null });
      if (entry.size >= 4 && machMagic(readBlob(entry.blob, 0, 4))) {
        executableBytes += entry.size;
        requireValue(
          entry.size <= SIGNING_FS_LIMITS.mutableFileBytes &&
            executableBytes <= SIGNING_FS_LIMITS.mutableTotalBytes,
        );
        executables.add(entry.path);
      }
      if (entry.path.endsWith(".framework/Info.plist")) bundles.add(entry.path.slice(0, -11));
    }
  } catch {
    throw signingError("SIGNING_INPUT_INVALID");
  }
  const temporary = (path) => {
    for (const executable of executables) {
      if (
        path === executable + ".fato" ||
        (path.startsWith(executable + ".archo.") &&
          /^\d{1,4}$/.test(path.slice(executable.length + 7)))
      )
        return true;
    }
    return false;
  };
  const allowed = (path) =>
    executables.has(path) ||
    temporary(path) ||
    [...bundles].some(
      (bundle) =>
        path === bundle + "/Info.plist" || path === bundle + "/_CodeSignature/CodeResources",
    ) ||
    path === archive.appRoot + "embedded.mobileprovision";
  const cap = (path) =>
    executables.has(path) || temporary(path)
      ? SIGNING_FS_LIMITS.mutableFileBytes
      : SIGNING_FS_LIMITS.metadataBytes;
  const writable = (path) => {
    check();
    requireValue(typeof path === "string" && allowed(path));
    let file = files.get(path);
    if (!file) {
      requireValue(files.size < 20000);
      file = { path, size: 0, mode: 0o644, blob: null, data: null };
      files.set(path, file);
    }
    return file;
  };
  const resize = (file, length, preserve = true) => {
    requireValue(Number.isSafeInteger(length) && length >= 0);
    if (length > cap(file.path)) throw signingError("SIGNING_SIZE_LIMIT");
    const oldCapacity = file.data?.length ?? 0;
    if (file.data && length <= oldCapacity) return;
    const capacity = Math.min(
      cap(file.path),
      Math.max(length, Math.min(oldCapacity * 2, cap(file.path))),
    );
    if (allocated - oldCapacity + capacity > SIGNING_FS_LIMITS.mutableTotalBytes)
      throw signingError("SIGNING_SIZE_LIMIT");
    const data = new Uint8Array(capacity);
    if (preserve) {
      if (file.data) data.set(file.data.subarray(0, Math.min(file.size, length)));
      else if (file.blob)
        for (let offset = 0; offset < Math.min(file.size, length); offset += 65536)
          data.set(
            readBlob(file.blob, offset, Math.min(65536, Math.min(file.size, length) - offset)),
            offset,
          );
    }
    file.data?.fill(0);
    file.data = data;
    allocated += capacity - oldCapacity;
  };
  const drop = (file) => {
    if (file?.data) {
      allocated -= file.data.length;
      file.data.fill(0);
    }
  };
  return Object.freeze({
    list() {
      check();
      return Object.freeze(
        [...files.values()].map(({ path, size, mode }) => Object.freeze({ path, size, mode })),
      );
    },
    read(path, offset, length) {
      check();
      const file = files.get(path);
      requireValue(
        file && range(offset, length) && length <= 65536 && offset + length <= file.size,
      );
      return file.data
        ? file.data.slice(offset, offset + length)
        : readBlob(file.blob, offset, length).slice();
    },
    write(path, offset, bytes) {
      requireValue(bytes instanceof Uint8Array && range(offset, bytes.length));
      const file = writable(path),
        length = Math.max(file.size, offset + bytes.length);
      resize(file, length);
      file.data.set(bytes, offset);
      file.size = length;
    },
    truncate(path, size) {
      const file = writable(path);
      requireValue(Number.isSafeInteger(size) && size >= 0);
      resize(file, size);
      if (size < file.size) file.data.fill(0, size);
      file.size = size;
    },
    remove(path) {
      const file = writable(path);
      drop(file);
      files.delete(path);
    },
    rename(from, to) {
      check();
      requireValue(
        temporary(from) &&
          executables.has(to) &&
          (from === to + ".fato" || from.startsWith(to + ".archo.")) &&
          files.has(from),
      );
      const file = files.get(from);
      requireValue(file.size <= cap(to));
      drop(files.get(to));
      files.delete(from);
      const mode = original.get(to)?.mode ?? file.mode;
      files.set(to, { ...file, path: to, mode });
      return mode;
    },
    collectEntries() {
      check();
      requireValue(
        [...original.keys()].every((path) => files.has(path)) && ![...files.keys()].some(temporary),
      );
      const entries = [...files.values()].map((file) =>
        Object.freeze({
          path: file.path,
          size: file.size,
          mode: file.mode,
          blob: file.data ? new Blob([file.data.subarray(0, file.size)]) : file.blob,
        }),
      );
      try {
        indexFiles(entries);
      } catch {
        throw signingError("SIGNING_OUTPUT_INVALID");
      }
      return Object.freeze(entries);
    },
    get allocatedBytes() {
      return allocated;
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      for (const file of files.values()) drop(file);
      files.clear();
      original.clear();
      executables.clear();
      bundles.clear();
    },
  });
}
