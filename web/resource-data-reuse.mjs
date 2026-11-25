// 校验可复用资源目录的索引与数据；依据 ./romfs-tree.mjs 的模块与调用约定。
// Verify indexes and data in reusable resource directories using the module and calling contracts in ./romfs-tree.mjs.
import { ROMFS_LIMITS } from "./romfs-tree.mjs";

// 标记、调用方哈希、大小或时间戳均不能授权复用；只读地将目标每个字节与当前实时源能力逐一比对。
// No marker, caller hash, size or timestamp can authorize reuse. Compare every
// destination byte with the current live source capability without writing.
export async function verifyResourceData({
  directory,
  entries,
  romfs,
  markerName,
  signal,
  onProgress,
}) {
  const fail = () => {
    throw new Error("RESOURCE_EXISTING_DATA_UNVERIFIED");
  };
  const check = () => signal?.throwIfAborted();
  const expected = new Map(entries.map((entry) => [entry.path, entry]));
  const expectedDirectories = new Set();
  for (const entry of entries) {
    const parts = entry.path.split("/");
    for (let i = 1; i < parts.length; i++) expectedDirectories.add(parts.slice(0, i).join("/"));
  }
  const files = new Map(),
    directories = [],
    stack = [{ handle: directory, path: "", depth: 0 }];
  let members = 0,
    pathBytes = 0,
    filesVerified = 0,
    bytesVerified = 0n;
  const encoder = new TextEncoder();
  while (stack.length) {
    check();
    const node = stack.pop();
    if (typeof node.handle.entries !== "function") fail();
    for await (const [name, handle] of node.handle.entries()) {
      check();
      if (typeof name !== "string" || !name || /[/\\]/.test(name) || name === "." || name === "..")
        fail();
      const path = node.path ? `${node.path}/${name}` : name;
      const length = encoder.encode(path).length;
      pathBytes += length;
      if (
        ++members > ROMFS_LIMITS.members + 1 ||
        length > ROMFS_LIMITS.pathBytes ||
        pathBytes > ROMFS_LIMITS.totalPathBytes ||
        typeof handle?.isSameEntry !== "function"
      )
        fail();
      // 既不解析也不修改旧收据，其内容不构成证明。
      // The old receipt is neither parsed nor modified. Its contents are not proof.
      if (path === markerName && handle.kind === "file") continue;
      const item = { parent: node.handle, name, handle, path };
      if (handle.kind === "directory") {
        if (node.depth >= ROMFS_LIMITS.depth || !expectedDirectories.has(path)) fail();
        directories.push(item);
        stack.push({ ...item, depth: node.depth + 1 });
      } else if (handle.kind === "file" && expected.has(path) && !files.has(path))
        files.set(path, item);
      else fail();
    }
  }
  if (files.size !== expected.size) fail();
  for (const entry of entries) {
    check();
    const item = files.get(entry.path),
      source = romfs.getFile(entry.sourcePath);
    const file = await item.handle.getFile();
    if (
      !source ||
      source.size !== entry.size ||
      typeof source.readAt !== "function" ||
      !Number.isSafeInteger(file.size) ||
      BigInt(file.size) !== entry.size ||
      typeof file.slice !== "function"
    )
      fail();
    item.snapshot = file;
    for (let offset = 0n; offset < entry.size;) {
      check();
      const count = Number(
        entry.size - offset < BigInt(ROMFS_LIMITS.readBytes)
          ? entry.size - offset
          : BigInt(ROMFS_LIMITS.readBytes),
      );
      let actual, wanted;
      try {
        wanted = await source.readAt(offset, count, { signal });
        check();
        actual = new Uint8Array(
          await file.slice(Number(offset), Number(offset) + count).arrayBuffer(),
        );
        check();
        if (!(wanted instanceof Uint8Array) || wanted.length !== count || actual.length !== count)
          fail();
        for (let i = 0; i < count; i++) if (actual[i] !== wanted[i]) fail();
        offset += BigInt(count);
        bytesVerified += BigInt(count);
        onProgress?.({ filesVerified, bytesVerified });
      } finally {
        if (wanted instanceof Uint8Array) wanted.fill(0);
        actual?.fill(0);
      }
    }
    filesVerified++;
    onProgress?.({ filesVerified, bytesVerified });
  }
  // 除字节差异外，还检测句柄替换及普通并发编辑；这不是针对任意外部文件系统写入者的原子快照。
  // Detect replaced handles and ordinary concurrent edits as well as byte mismatches.
  // This is not an atomic snapshot against arbitrary external filesystem writers.
  for (const item of [...directories, ...files.values()]) {
    check();
    const current = await (item.handle.kind === "directory"
      ? item.parent.getDirectoryHandle(item.name)
      : item.parent.getFileHandle(item.name));
    if (!(await item.handle.isSameEntry(current))) fail();
    if (item.snapshot) {
      const now = await current.getFile();
      if (now.size !== item.snapshot.size || now.lastModified !== item.snapshot.lastModified)
        fail();
    }
  }
  check();
  return { filesVerified, bytesVerified };
}
