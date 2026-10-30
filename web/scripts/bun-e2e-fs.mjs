// 为 Bun 测试提供基于磁盘的文件与目录句柄；依据 web/scripts 的模块与调用约定。
// Provide disk-backed file and directory handles for Bun tests using the module and calling contracts in web/scripts.
import {
  mkdir,
  readdir,
  stat,
  open,
  rename,
  rm,
  writeFile,
  statfs,
  lstat,
  rmdir,
} from "node:fs/promises";
import { join, basename } from "node:path";

function missing() {
  return new DOMException("Entry not found", "NotFoundError");
}
function safe(name) {
  if (
    typeof name !== "string" ||
    !name ||
    name === "." ||
    name === ".." ||
    name.includes("/") ||
    name.includes("\\")
  )
    throw new DOMException("Invalid entry name", "TypeMismatchError");
  return name;
}
async function kind(path) {
  try {
    return (await stat(path)).isDirectory() ? "directory" : "file";
  } catch (error) {
    if (error.code === "ENOENT") throw missing();
    throw error;
  }
}

// 捕获条目身份而非只比较路径，防止清理误删同路径替换项。
// Capture entry identity rather than just paths so cleanup preserves replacements.
const identities = new WeakMap();
const identityAt = (path) => lstat(path, { bigint: true }).catch(() => null);
const sameIdentity = (a, b) => a !== null && b !== null && a.dev === b.dev && a.ino === b.ino;
async function sameEntry(left, right) {
  if (!identities.has(right) || left.kind !== right.kind) return false;
  const [a, b, currentA, currentB] = await Promise.all([
    identities.get(left),
    identities.get(right),
    identityAt(left.path),
    identityAt(right.path),
  ]);
  return sameIdentity(a, b) && sameIdentity(a, currentA) && sameIdentity(b, currentB);
}

export class FileHandle {
  kind = "file";
  constructor(path) {
    this.path = path;
    this.name = basename(path);
    identities.set(this, identityAt(path));
  }
  async isSameEntry(other) {
    return sameEntry(this, other);
  }
  async queryPermission() {
    return "granted";
  }
  async getFile() {
    if ((await kind(this.path)) !== "file")
      throw new DOMException("Not a file", "TypeMismatchError");
    const file = Bun.file(this.path);
    Object.defineProperty(file, "name", { value: this.name });
    return file;
  }
  async createWritable({ keepExistingData = false } = {}) {
    await identities.get(this);
    const owner = this;
    const temporary = `${this.path}.bun-write-${crypto.randomUUID()}`;
    if (keepExistingData) throw new Error("keepExistingData is unsupported in this CLI");
    const file = await open(temporary, "wx");
    let state = "open",
      position = 0,
      fileClosed = false;
    return {
      async write(bytes) {
        if (state !== "open" || !(bytes instanceof Uint8Array)) throw new Error("Invalid write");
        for (let at = 0; at < bytes.length;) {
          const { bytesWritten } = await file.write(bytes, at, bytes.length - at, position + at);
          if (!bytesWritten) throw new Error("Short write");
          at += bytesWritten;
        }
        position += bytes.length;
      },
      async close() {
        if (state !== "open") throw new Error("Writer closed");
        state = "closing";
        await file.close();
        fileClosed = true;
        await rename(temporary, owner.path);
        // 自有原子提交更新身份；外部替换仍不能通过身份检查。
        // Refresh identity after our atomic commit; external replacements still fail comparison.
        identities.set(owner, identityAt(owner.path));
        await identities.get(owner);
        state = "closed";
      },
      async abort() {
        if (state === "closed" || state === "aborted") return;
        if (!fileClosed) {
          await file.close();
          fileClosed = true;
        }
        await rm(temporary, { force: true });
        state = "aborted";
      },
    };
  }
}

export class DirectoryHandle {
  kind = "directory";
  constructor(path) {
    this.path = path;
    this.name = basename(path);
    identities.set(this, identityAt(path));
  }
  async isSameEntry(other) {
    return sameEntry(this, other);
  }
  async queryPermission() {
    return "granted";
  }
  async *entries() {
    for (const item of await readdir(this.path, { withFileTypes: true })) {
      const path = join(this.path, item.name);
      yield [item.name, item.isDirectory() ? new DirectoryHandle(path) : new FileHandle(path)];
    }
  }
  async *keys() {
    for await (const [name] of this.entries()) yield name;
  }
  async getDirectoryHandle(name, { create = false } = {}) {
    const path = join(this.path, safe(name));
    if (create) await mkdir(path, { recursive: true });
    if ((await kind(path)) !== "directory")
      throw new DOMException("Not a directory", "TypeMismatchError");
    return new DirectoryHandle(path);
  }
  async getFileHandle(name, { create = false } = {}) {
    const path = join(this.path, safe(name));
    if (create) {
      try {
        await writeFile(path, new Uint8Array(), { flag: "wx" });
      } catch (error) {
        if (error.code !== "EEXIST") throw error;
      }
    }
    if ((await kind(path)) !== "file") throw new DOMException("Not a file", "TypeMismatchError");
    return new FileHandle(path);
  }
  async removeEntry(name, { recursive = false } = {}) {
    const path = join(this.path, safe(name));
    if ((await kind(path)) === "directory" && !recursive) {
      try {
        await rmdir(path);
      } catch (error) {
        if (error.code === "ENOTEMPTY" || error.code === "EEXIST")
          throw new DOMException("Directory is not empty", "InvalidModificationError");
        throw error;
      }
      return;
    }
    await rm(path, { recursive, force: false });
  }
}

export async function storageAt(path) {
  await mkdir(path, { recursive: true });
  const volume = await statfs(path);
  const available = volume.bavail * volume.bsize;
  return {
    async getDirectory() {
      return new DirectoryHandle(path);
    },
    async estimate() {
      return { quota: available, usage: 0 };
    },
  };
}
