// 专用工作线程的常规文件挂载；资源读取不会把整个 IPA 放入 WASM 堆。
// Regular-file mount for the dedicated worker; resource reads never place the whole IPA in the WASM heap.
import { createSigningFileStore } from "./ipa-signing-fs.mjs";
import { signingError } from "./ipa-signing-contract.mjs";

export function mountSigningArchive(module, archive, options = {}) {
  const FS = module?.FS;
  if (
    !FS ||
    ["createNode", "mkdir", "mount", "unmount"].some((key) => typeof FS[key] !== "function")
  )
    throw signingError("SIGNING_INITIALIZATION_FAILED");
  const store = createSigningFileStore(archive, options),
    nodes = new Map(),
    initial = store.list();
  const appRoot = archive.appRoot;
  const internal = (path) => "Payload/App.app/" + path.slice(appRoot.length);
  const external = (path) => appRoot + path.slice("Payload/App.app/".length);
  const directories = new Set(["", "Payload", "Payload/App.app"]);
  const signatureDirectories = new Set(["Payload/App.app/_CodeSignature"]);
  for (const file of initial) {
    const path = internal(file.path),
      parts = path.split("/");
    for (let i = 1; i < parts.length; i++) directories.add(parts.slice(0, i).join("/"));
    if (path.endsWith(".framework/Info.plist"))
      signatureDirectories.add(path.slice(0, -11) + "/_CodeSignature");
  }
  let initializing = true,
    mounted = false,
    disposed = false;
  const error = (code) => {
    throw new FS.ErrnoError(code);
  };
  const guarded = (fn) => {
    try {
      return fn();
    } catch (e) {
      if (e instanceof FS.ErrnoError) throw e;
      error(29);
    }
  };
  const pathFor = (parent, name) => (parent.path ? parent.path + "/" + name : name);
  let nodeOps, streamOps;
  const create = (parent, name, mode, path) => {
    const node = FS.createNode(parent, name, mode, 0);
    node.path = path;
    node.timestamp = 0;
    node.size = 0;
    node.node_ops = nodeOps;
    node.stream_ops = streamOps;
    node.directory = (mode & 0o170000) === 0o040000;
    if (node.directory) node.contents = Object.create(null);
    if (parent) parent.contents[name] = node;
    nodes.set(path, node);
    return node;
  };
  nodeOps = {
    getattr(node) {
      return {
        dev: 1,
        ino: node.id,
        mode: node.mode,
        nlink: 1,
        uid: 0,
        gid: 0,
        rdev: 0,
        size: node.directory ? 4096 : node.size,
        atime: new Date(0),
        mtime: new Date(0),
        ctime: new Date(0),
        blksize: 4096,
        blocks: Math.ceil(node.size / 512),
      };
    },
    setattr(node, attr) {
      if (attr.size !== undefined)
        guarded(() => {
          if (node.directory) error(31);
          store.truncate(external(node.path), attr.size);
          node.size = attr.size;
        });
      if (attr.mode !== undefined) {
        // Emscripten open 在创建后回写 0644/0666；只消费一次并保持规范 0644。
        // Emscripten open finalizes a new file as 0644/0666; consume this once and retain canonical 0644.
        if (node.creationModePending && [0o100644, 0o100666].includes(attr.mode)) {
          node.creationModePending = false;
        } else if (attr.mode !== node.mode) error(63);
      }
    },
    lookup(parent, name) {
      return parent.contents[name] ?? error(44);
    },
    mknod(parent, name, mode) {
      return guarded(() => {
        const path = pathFor(parent, name),
          type = mode & 0o170000;
        if (parent.contents[name]) error(20);
        if (type === 0o040000) {
          if (!initializing && !signatureDirectories.has(path)) error(63);
        } else if (type === 0o100000) {
          store.truncate(external(path), 0);
        } else error(63);
        const node = create(parent, name, type | (type === 0o040000 ? 0o755 : 0o644), path);
        node.creationModePending = type === 0o100000;
        return node;
      });
    },
    rename(node, parent, name) {
      return guarded(() => {
        if (node.directory || parent.contents[name]?.directory) error(63);
        const previous = node.path,
          path = pathFor(parent, name);
        const mode = store.rename(external(previous), external(path));
        // 保持挂载 stat 与最终 ZIP 的原始可执行权限一致。
        // Keep mounted stat permissions consistent with the original executable mode in the final ZIP.
        node.mode = 0o100000 | mode;
        delete node.parent.contents[node.name];
        nodes.delete(previous);
        node.parent = parent;
        node.name = name;
        node.path = path;
        parent.contents[name] = node;
        nodes.set(path, node);
      });
    },
    unlink(parent, name) {
      return guarded(() => {
        const node = parent.contents[name];
        if (!node) error(44);
        if (node.directory) error(31);
        store.remove(external(node.path));
        nodes.delete(node.path);
        delete parent.contents[name];
      });
    },
    rmdir(parent, name) {
      const node = parent.contents[name];
      if (!node) error(44);
      if (!node.directory) error(54);
      if (Object.keys(node.contents).length) error(55);
      if (!signatureDirectories.has(node.path)) error(63);
      nodes.delete(node.path);
      delete parent.contents[name];
    },
    readdir(node) {
      return [".", "..", ...Object.keys(node.contents)];
    },
    symlink() {
      error(63);
    },
  };
  streamOps = {
    llseek(stream, offset, whence) {
      const position =
        whence === 0
          ? offset
          : whence === 1
            ? stream.position + offset
            : whence === 2
              ? stream.node.size + offset
              : NaN;
      if (!Number.isSafeInteger(position) || position < 0) error(28);
      return position;
    },
    read(stream, buffer, offset, length, position) {
      return guarded(() => {
        const node = stream.node;
        if (node.directory) error(31);
        const count = Math.min(length, Math.max(0, node.size - position));
        for (let done = 0; done < count; done += 65536)
          buffer.set(
            store.read(external(node.path), position + done, Math.min(65536, count - done)),
            offset + done,
          );
        return count;
      });
    },
    write(stream, buffer, offset, length, position) {
      return guarded(() => {
        const node = stream.node;
        if (node.directory) error(31);
        store.write(
          external(node.path),
          position,
          new Uint8Array(buffer.buffer, buffer.byteOffset + offset, length),
        );
        node.size = Math.max(node.size, position + length);
        return length;
      });
    },
  };
  try {
    FS.mkdir("/work");
    FS.mount(
      {
        mount(mountInfo) {
          const root = create(null, "/", 0o040755, "");
          // 子节点在构造时继承挂载标识，必须先设置以便 unmount 清理哈希表。
          // Children inherit mount identity at construction; set it first so unmount cleans the hash table.
          root.mount = mountInfo;
          for (const directory of [...directories]
            .filter(Boolean)
            .sort((a, b) => a.split("/").length - b.split("/").length)) {
            const slash = directory.lastIndexOf("/");
            create(
              nodes.get(slash < 0 ? "" : directory.slice(0, slash)),
              directory.slice(slash + 1),
              0o040755,
              directory,
            );
          }
          for (const file of initial) {
            const path = internal(file.path),
              slash = path.lastIndexOf("/");
            const node = create(
              nodes.get(path.slice(0, slash)),
              path.slice(slash + 1),
              0o100000 | file.mode,
              path,
            );
            node.size = file.size;
          }
          return root;
        },
      },
      {},
      "/work",
    );
    mounted = true;
    initializing = false;
  } catch {
    store.dispose();
    throw signingError("SIGNING_INITIALIZATION_FAILED");
  }
  return Object.freeze({
    collectEntries() {
      if (disposed) throw signingError("SIGNING_CLOSED");
      return store.collectEntries();
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      try {
        if (mounted) FS.unmount("/work");
      } finally {
        store.dispose();
        nodes.clear();
      }
    },
  });
}
