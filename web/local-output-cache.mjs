// 可丢弃的本地输出缓存。目录字节只有与本源的独立账本一致才可信；账本在调用方验证新生成输出后填充。
// Disposable local output cache. Folder bytes are trusted only against this origin's
// independent ledger, populated after a caller has validated a freshly generated output.
const ROOT = "D2R-cache";
const VERSION = "v1";
const LEDGER = "d2r-generated-output-ledger-v1-";
const PREFIX = "/__d2r_generated_output_ledger__/v1/";
const MAX_FILE = 16 * 1024 * 1024;
const MAX_TOTAL = 2 * 1024 * 1024 * 1024;
const MAX_ENTRIES = 32768;
const HASH = /^[a-f0-9]{64}$/;
const KIND = /^[a-z][a-z0-9-]{0,31}$/;
const UUID = /^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/;
const encoder = new TextEncoder();
const hash = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (n) =>
    n.toString(16).padStart(2, "0"),
  ).join("");

// 不接受含糊的 undefined、特殊对象、循环引用或无界元数据。
// No ambiguous undefined values, exotic objects, cycles, or unbounded metadata.
export async function cacheKeyFor(value) {
  let nodes = 0,
    total = 0;
  const textSize = (value) => {
    if (value.length > 1024 * 1024) throw new Error("CACHE_KEY_LIMIT");
    total += encoder.encode(JSON.stringify(value)).length;
    if (total > 2 * 1024 * 1024) throw new Error("CACHE_KEY_LIMIT");
  };
  const visit = (item, depth) => {
    if (++nodes > 65536 || depth > 32 || (total += 16) > 2 * 1024 * 1024)
      throw new Error("CACHE_KEY_LIMIT");
    if (item === null || typeof item === "boolean") return item;
    if (typeof item === "string") {
      textSize(item);
      return item;
    }
    if (typeof item === "number" && Number.isFinite(item)) return item;
    if (Array.isArray(item)) {
      if (
        item.length > 65536 ||
        Object.keys(item).length !== item.length ||
        !Array.from({ length: item.length }, (_, i) => Object.hasOwn(item, i)).every(Boolean)
      )
        throw new Error("CACHE_KEY_INVALID");
      return item.map((child) => visit(child, depth + 1));
    }
    if (item && Object.getPrototypeOf(item) === Object.prototype) {
      const result = Object.create(null);
      for (const key of Object.keys(item).sort()) {
        textSize(key);
        result[key] = visit(item[key], depth + 1);
      }
      return result;
    }
    throw new Error("CACHE_KEY_INVALID");
  };
  const bytes = encoder.encode(JSON.stringify(visit(value, 0)));
  if (bytes.length > 2 * 1024 * 1024) throw new Error("CACHE_KEY_LIMIT");
  try {
    return await hash(bytes);
  } finally {
    bytes.fill(0);
  }
}

/**
 * 可选的纯本地存储，不应包含凭据或原始输入。read 返回自有快照或 null；write 只接受已经验证的新输出。忽略缓存只跳过读取，仍更新新缓存。存储失败只禁用本实例，不终止生成任务；取消仍会终止操作。最后提交账本，未完成文件不能命中。
 *
 * Optional, local-only storage. No credentials or raw inputs belong here.
 * read returns an owned snapshot, or null. write must only receive an already
 * accepted fresh output. Ignore cache skips reads, not fresh cache refreshes.
 * Storage failures disable this instance, never the generation job. Cancellation
 * remains fatal. The ledger is committed last; incomplete files are never hits.
 */
export async function createLocalOutputCache({
  directory,
  ignoreCache = false,
  signal,
  onWarning,
  environment = globalThis,
  timeoutMs = 10000,
} = {}) {
  if (typeof ignoreCache !== "boolean") throw new Error("IGNORE_CACHE_OPTION");
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 30000)
    throw new Error("CACHE_TIMEOUT_INVALID");
  let closed = false,
    disabled = false,
    warned = false,
    folder,
    ledger,
    origin,
    totalBytes = 0,
    entryCount = 0,
    writing = false,
    releaseLock,
    activeOperations = 1;
  const records = new Map(),
    directories = new Map(),
    pendingIo = new Set();
  const lifetime = new AbortController();
  const checkUser = (requestSignal) => {
    signal?.throwIfAborted();
    requestSignal?.throwIfAborted();
  };
  const check = (requestSignal) => {
    checkUser(requestSignal);
    lifetime.signal.throwIfAborted();
  };
  const maybeRelease = () => {
    if ((closed || disabled) && activeOperations === 0 && pendingIo.size === 0) {
      releaseLock?.();
      releaseLock = undefined;
    }
  };
  const track = (promise) => {
    pendingIo.add(promise);
    const settled = () => {
      pendingIo.delete(promise);
      queueMicrotask(maybeRelease);
    };
    promise.then(settled, settled);
    return promise;
  };
  // 文件系统和浏览器存储的 Promise 不总能取消；及时停止等待，观察晚到结果并释放晚到的自有资源。
  // File-system and browser-storage promises are not always cancellable. Stop
  // waiting promptly, observe late outcomes, and dispose late owned results.
  const io = async (action, requestSignal, onLate) => {
    check(requestSignal);
    let timer,
      abandoned = false,
      resolved = false,
      value,
      cleaned = false;
    const cancellations = [];
    const cleanup = () => {
      if (!resolved || cleaned || !onLate) return;
      cleaned = true;
      try {
        track(Promise.resolve(onLate(value))).catch(() => {});
      } catch {
        /* 尽力处理。 */
        /* best effort */
      }
    };
    const pending = track(
      Promise.resolve().then(() => {
        check(requestSignal);
        return action();
      }),
    );
    pending.then(
      (result) => {
        resolved = true;
        value = result;
        if (abandoned) cleanup();
      },
      () => {},
    );
    try {
      const result = await Promise.race([
        pending,
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error("CACHE_STORAGE_TIMEOUT")), timeoutMs);
          for (const source of new Set([signal, requestSignal, lifetime.signal].filter(Boolean))) {
            const abort = () => reject(source.reason ?? new DOMException("Aborted", "AbortError"));
            source.addEventListener("abort", abort, { once: true });
            cancellations.push(() => source.removeEventListener("abort", abort));
            if (source.aborted) abort();
          }
        }),
      ]);
      check(requestSignal);
      return result;
    } catch (error) {
      abandoned = true;
      cleanup();
      throw error;
    } finally {
      clearTimeout(timer);
      for (const cancel of cancellations) cancel();
    }
  };
  const cleanup = async (action) => {
    let timer;
    try {
      await Promise.race([
        track(Promise.resolve().then(action)),
        new Promise((resolve) => {
          timer = setTimeout(resolve, 1000);
        }),
      ]);
    } catch {
      /* 清理不能阻塞生成或取消。 */
      /* Cleanup cannot block generation or cancellation. */
    } finally {
      clearTimeout(timer);
    }
  };
  const removeOwned = async (parent, name, handle, emptyOnly = false) => {
    const current = await parent.getFileHandle(name);
    if (typeof handle.isSameEntry !== "function" || !(await handle.isSameEntry(current))) return;
    if (emptyOnly && (await current.getFile()).size !== 0) return;
    await parent.removeEntry(name);
  };
  const unavailable = () => {
    disabled = true;
    if (warned) return;
    warned = true;
    try {
      Promise.resolve(onWarning?.({ phase: "cache", status: "unavailable" })).catch(() => {});
    } catch {
      /* 仅作建议参考。 */
      /* Advisory only. */
    }
  };
  const live = (requestSignal) => {
    checkUser(requestSignal);
    return !closed && !disabled;
  };
  const parse = (url) => {
    const parsed = new URL(url);
    if (
      parsed.origin !== origin ||
      parsed.search ||
      parsed.hash ||
      !parsed.pathname.startsWith(PREFIX)
    )
      throw new Error("CACHE_LEDGER_INVALID");
    const [kind, key, sha256, sizeText, id, ...extra] = parsed.pathname
      .slice(PREFIX.length)
      .split("/");
    const size = Number(sizeText);
    if (
      extra.length ||
      !KIND.test(kind) ||
      !HASH.test(key) ||
      !HASH.test(sha256) ||
      !UUID.test(id) ||
      !/^[1-9][0-9]*$/.test(sizeText) ||
      !Number.isSafeInteger(size) ||
      size > MAX_FILE
    )
      throw new Error("CACHE_LEDGER_INVALID");
    return { kind, key, sha256, size, id, url, file: `${key}-${id}.bin` };
  };
  const parameters = (kind, key, maxBytes) => {
    if (
      !KIND.test(kind) ||
      !HASH.test(key) ||
      !Number.isSafeInteger(maxBytes) ||
      maxBytes < 1 ||
      maxBytes > MAX_FILE
    )
      throw new Error("CACHE_ENTRY_INVALID");
  };
  const child = async (kind, create, requestSignal) => {
    if (directories.has(kind)) return directories.get(kind);
    const handle = await io(() => folder.getDirectoryHandle(kind, { create }), requestSignal);
    check(requestSignal);
    if (create) directories.set(kind, handle);
    return handle;
  };
  try {
    check();
    if (
      !environment.caches?.open ||
      !environment.crypto?.randomUUID ||
      !environment.navigator?.locks?.request ||
      !directory?.getDirectoryHandle
    )
      throw new Error("CACHE_STORAGE_UNAVAILABLE");
    // 每个源最多一个可选缓存写入会话；其他标签页仍正常生成，无需等待，也不争用账本大小统计或标记写入。
    // One optional cache writer session per origin. Other tabs still generate
    // normally, without waiting or racing ledger size/accounting and marker writes.
    const locked = await io(
      () =>
        new Promise((resolve, reject) => {
          Promise.resolve(
            environment.navigator.locks.request(
              "d2r-generated-output-cache-v1",
              { mode: "exclusive", ifAvailable: true },
              async (lock) => {
                if (!lock || signal?.aborted || closed || disabled) {
                  resolve(false);
                  return;
                }
                const held = new Promise((release) => {
                  releaseLock = release;
                });
                resolve(true);
                await held;
              },
            ),
          ).catch(reject);
        }),
    );
    if (!locked) throw new Error("CACHE_BUSY");
    origin = new URL(environment.location.href).origin;
    const root = await io(() => directory.getDirectoryHandle(ROOT, { create: true }));
    check();
    folder = await io(() => root.getDirectoryHandle(VERSION, { create: true }));
    check();
    let id;
    try {
      const marker = await io(() => folder.getFileHandle("cache.json"));
      const file = await io(() => marker.getFile());
      check();
      if (file.size > 256) throw new Error("CACHE_MARKER_INVALID");
      const data = JSON.parse(await io(() => file.text()));
      check();
      if (data?.schemaVersion !== 1 || !UUID.test(data.id) || Object.keys(data).length !== 2)
        throw new Error("CACHE_MARKER_INVALID");
      id = data.id;
    } catch (error) {
      check();
      if (error?.name !== "NotFoundError") throw error;
      id = environment.crypto.randomUUID();
      if (!UUID.test(id)) throw new Error("CACHE_MARKER_INVALID");
      const handle = await io(
        () => folder.getFileHandle("cache.json", { create: true }),
        undefined,
        (value) => cleanup(() => removeOwned(folder, "cache.json", value, true)),
      );
      let writer,
        markerComplete = false;
      try {
        check();
        writer = await io(
          () => handle.createWritable({ keepExistingData: false }),
          undefined,
          (value) => value.abort(),
        );
        check();
        await io(() => writer.write(encoder.encode(JSON.stringify({ schemaVersion: 1, id }))));
        check();
        await io(() => writer.close());
        writer = undefined;
        markerComplete = true;
      } finally {
        if (writer) await cleanup(() => writer.abort());
        if (!markerComplete) await cleanup(() => removeOwned(folder, "cache.json", handle, true));
      }
    }
    check();
    ledger = await io(() => environment.caches.open(LEDGER + id));
    check();
    const entries = await io(() => ledger.keys());
    check();
    if (entries.length > MAX_ENTRIES) throw new Error("CACHE_LEDGER_LIMIT");
    for (const entry of entries) {
      const record = parse(entry.url);
      totalBytes += record.size;
      if (totalBytes > MAX_TOTAL) throw new Error("CACHE_LEDGER_LIMIT");
      entryCount++;
      records.set(`${record.kind}:${record.key}`, record);
    }
  } catch {
    if (signal?.aborted) {
      disabled = true;
      checkUser();
    }
    unavailable();
  } finally {
    activeOperations--;
    maybeRelease();
  }
  return Object.freeze({
    async read({ kind, key, maxBytes, signal: requestSignal } = {}) {
      if (!live(requestSignal) || ignoreCache) return null;
      activeOperations++;
      let bytes;
      try {
        parameters(kind, key, Math.min(maxBytes, MAX_FILE));
        const record = records.get(`${kind}:${key}`);
        if (!record) return null;
        if (record.size > maxBytes) return null;
        const parent = await child(kind, false, requestSignal);
        const handle = await io(() => parent.getFileHandle(record.file), requestSignal);
        check(requestSignal);
        const file = await io(() => handle.getFile(), requestSignal);
        check(requestSignal);
        if (file.size !== record.size || file.size > maxBytes) return null;
        bytes = new Uint8Array(
          await io(
            () => file.arrayBuffer(),
            requestSignal,
            (value) => new Uint8Array(value).fill(0),
          ),
        );
        check(requestSignal);
        if (
          bytes.length !== record.size ||
          (await io(() => hash(bytes), requestSignal)) !== record.sha256
        )
          return null;
        check(requestSignal);
        if (closed || disabled) return null;
        const result = bytes;
        bytes = undefined;
        return result;
      } catch (error) {
        checkUser(requestSignal);
        if (error?.name !== "NotFoundError" && !closed) unavailable();
        return null;
      } finally {
        bytes?.fill(0);
        activeOperations--;
        maybeRelease();
      }
    },
    async write({ kind, key, bytes, signal: requestSignal } = {}) {
      if (!live(requestSignal) || writing) return false;
      writing = true;
      activeOperations++;
      let snapshot,
        writer,
        createdHandle,
        createdParent,
        createdName,
        committed = false,
        publishedUrl;
      try {
        if (bytes?.length > MAX_FILE) return false;
        parameters(kind, key, bytes?.length);
        if (
          !(bytes instanceof Uint8Array) ||
          !(bytes.buffer instanceof ArrayBuffer) ||
          bytes.buffer.resizable
        )
          throw new Error("CACHE_ENTRY_INVALID");
        if (entryCount >= MAX_ENTRIES || bytes.length > MAX_TOTAL - totalBytes) return false;
        snapshot = new Uint8Array(bytes);
        const sha256 = await io(() => hash(snapshot), requestSignal);
        check(requestSignal);
        if (closed) return false;
        const parent = await child(kind, true, requestSignal);
        const id = environment.crypto.randomUUID();
        if (!UUID.test(id)) throw new Error("CACHE_ENTRY_INVALID");
        const record = parse(`${origin}${PREFIX}${kind}/${key}/${sha256}/${snapshot.length}/${id}`);
        // 随机且不可变的叶文件避免修改现有缓存读取器正在使用的文件。
        // Random immutable leaves avoid mutating an existing cache reader's file.
        try {
          await io(() => parent.getFileHandle(record.file), requestSignal);
          throw new Error("CACHE_DESTINATION_COLLISION");
        } catch (error) {
          if (error?.name !== "NotFoundError") throw error;
        }
        check(requestSignal);
        const handle = await io(
          () => parent.getFileHandle(record.file, { create: true }),
          requestSignal,
          (value) => cleanup(() => removeOwned(parent, record.file, value)),
        );
        createdHandle = handle;
        createdParent = parent;
        createdName = record.file;
        writer = await io(
          () => handle.createWritable({ keepExistingData: false }),
          requestSignal,
          (value) => value.abort(),
        );
        check(requestSignal);
        for (let offset = 0; offset < snapshot.length; offset += 1024 * 1024) {
          await io(
            () => writer.write(snapshot.subarray(offset, offset + 1024 * 1024)),
            requestSignal,
          );
          check(requestSignal);
        }
        await io(() => writer.close(), requestSignal);
        writer = undefined;
        check(requestSignal);
        if (closed) return false;
        // 完成凭据是可信源记录，不是目录内的任何内容。
        // The trusted-origin record, not anything in the folder, is completion authority.
        publishedUrl = record.url;
        await io(
          () => ledger.put(record.url, new Response(null)),
          requestSignal,
          () => cleanup(() => ledger.delete(record.url)),
        );
        check(requestSignal);
        committed = true;
        const previous = records.get(`${kind}:${key}`);
        records.set(`${kind}:${key}`, record);
        totalBytes += record.size;
        entryCount++;
        check(requestSignal);
        // 只删除自有可信账本指明的过期叶文件，绝不递归删除。
        // Remove only a superseded leaf named by our own trusted ledger. Never recurse.
        if (previous) {
          try {
            const old = await io(() => parent.getFileHandle(previous.file), requestSignal);
            const file = await io(() => old.getFile(), requestSignal);
            let oldBytes;
            try {
              if (file.size === previous.size && file.size <= MAX_FILE) {
                oldBytes = new Uint8Array(
                  await io(
                    () => file.arrayBuffer(),
                    requestSignal,
                    (value) => new Uint8Array(value).fill(0),
                  ),
                );
                const digest = await io(() => hash(oldBytes), requestSignal);
                const current = await io(() => parent.getFileHandle(previous.file), requestSignal);
                if (
                  digest === previous.sha256 &&
                  typeof old.isSameEntry === "function" &&
                  (await io(() => old.isSameEntry(current), requestSignal))
                ) {
                  check(requestSignal);
                  await io(() => parent.removeEntry(previous.file), requestSignal);
                  await io(() => ledger.delete(previous.url), requestSignal);
                  totalBytes -= previous.size;
                  entryCount--;
                }
              }
            } finally {
              oldBytes?.fill(0);
            }
          } catch {
            check(requestSignal);
          }
        }
        return true;
      } catch {
        checkUser(requestSignal);
        if (!closed) unavailable();
        return false;
      } finally {
        if (writer) await cleanup(() => writer.abort());
        if (!committed && publishedUrl) await cleanup(() => ledger.delete(publishedUrl));
        if (!committed && createdHandle)
          await cleanup(() => removeOwned(createdParent, createdName, createdHandle));
        snapshot?.fill(0);
        writing = false;
        activeOperations--;
        maybeRelease();
      }
    },
    close() {
      closed = true;
      lifetime.abort(new Error("CACHE_CLOSED"));
      records.clear();
      directories.clear();
      maybeRelease();
    },
  });
}
