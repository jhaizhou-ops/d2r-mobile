// 在现有包输入之外提供有界的本地已提取目录选择，不宣称 NCA 认证。
// Add bounded local extracted-directory selection without claiming NCA authentication.
import {
  discoverNspFolder,
  discoverNspFiles,
  NspInputError,
  NSP_INPUT_LIMITS,
} from "./nsp-input.mjs";
import { ROMFS_LIMITS } from "./romfs-tree.mjs";
import { indexFiles, REQUIRED_NSO_MODULES, safeRelativePath } from "./preflight.mjs";

export const GAME_INPUT_MODES = Object.freeze(["packages", "extracted"]);
const encoder = new TextEncoder();
const fail = (code) => {
  throw new NspInputError(code, code);
};
// 只识别单独的附加内容布局；合并资源中的 DLC 无法由目录名判定。
// Recognize separate add-on layouts only; merged resources cannot establish DLC presence/absence.
export function isSeparateDlcLayout(value) {
  const first = value.split("/")[0].toLowerCase();
  return (
    first.includes("0100726014353001") ||
    /(?:^|[ ._[\]-])(?:dlc|aoc)(?:[ ._[\]-]|$)/u.test(first) ||
    /^(?:dlc|aoc|add[ _-]?on(?:[ _-]?content)?|warlock|reign[ _-]*of[ _-]*the[ _-]*warlock)(?:[ ._-]|$)/u.test(
      first,
    )
  );
}
function mode(value) {
  if (!GAME_INPUT_MODES.includes(value)) fail("INPUT_MODE");
}
function path(value) {
  try {
    safeRelativePath(value);
  } catch {
    fail("EXTRACTED_NAME");
  }
  if (
    /[<>"|?*\p{C}]/u.test(value) ||
    value.split("/").some((part) => encoder.encode(part).length > ROMFS_LIMITS.nameBytes)
  )
    fail("EXTRACTED_NAME");
  if (
    value.split("/").length > ROMFS_LIMITS.depth ||
    encoder.encode(value).length > ROMFS_LIMITS.pathBytes
  )
    fail("EXTRACTED_LIMIT");
  return value;
}
function filesResult(records) {
  const exefs = [],
    romfs = [];
  for (const record of records) {
    path(record.path);
    if (
      !(record.file instanceof Blob) ||
      !Number.isSafeInteger(record.file.size) ||
      record.file.size < 0
    )
      fail("EXTRACTED_FILE");
    const [root, ...rest] = record.path.split("/");
    if (!rest.length) continue;
    const relative = rest.join("/");
    if (isSeparateDlcLayout(relative)) fail("EXTRACTED_DLC_UNSUPPORTED");
    if (root.toLowerCase() === "exefs")
      exefs.push(Object.freeze({ path: relative, file: record.file }));
    else if (root.toLowerCase() === "romfs")
      romfs.push(Object.freeze({ path: relative, file: record.file }));
  }
  try {
    indexFiles(
      records.map(({ path, file }) => ({ path, size: file.size })),
      { maxPathDepth: ROMFS_LIMITS.depth + 1, maxPathNodes: ROMFS_LIMITS.members },
    );
  } catch {
    fail("EXTRACTED_NAME");
  }
  if (
    !REQUIRED_NSO_MODULES.every((name) => exefs.some((record) => record.path === name)) ||
    !romfs.some((record) => record.path === "data/build_manifest.psv")
  )
    fail("EXTRACTED_MISSING_INPUT");
  return Object.freeze({
    inputMode: "extracted",
    exefs: Object.freeze(exefs),
    romfs: Object.freeze(romfs),
    nsps: Object.freeze([]),
  });
}
function observer(onProgress, inputMode, extractedFileCount = 0, event = {}) {
  try {
    Promise.resolve(
      onProgress?.({
        nspCount: 0,
        prodKeysFound: false,
        titleKeysFound: false,
        ...event,
        inputMode,
        extractedFileCount,
      }),
    ).catch(() => {});
  } catch {
    /* 观察器不能影响验证。 | Observers cannot affect validation. */
  }
}
// 取消也结束卡住的目录枚举；迟到的读取结果只被观察，不保留。
// Cancellation also ends a stalled enumeration; late reads are observed, not retained.
async function cancellable(action, signal) {
  signal?.throwIfAborted();
  if (!signal) return action();
  let abort;
  try {
    return await Promise.race([
      Promise.resolve().then(action),
      new Promise((_, reject) => {
        abort = () => reject(new DOMException("Folder check cancelled.", "AbortError"));
        signal.addEventListener("abort", abort, { once: true });
        if (signal.aborted) abort();
      }),
    ]);
  } finally {
    signal.removeEventListener("abort", abort);
  }
}
// 只枚举根目录来选择输入路径；任何包文件都优先于已提取目录。
// Choose the input route from root entries only; any package takes precedence over extracted data.
async function detectFolder(directory, signal) {
  signal?.throwIfAborted();
  if (directory?.kind !== "directory" || typeof directory.entries !== "function") fail("DIRECTORY");
  const entries = [],
    iterator = directory.entries()[Symbol.asyncIterator]();
  let exhausted = false;
  /** @type {"packages"|"extracted"} */
  let inputMode = "extracted";
  try {
    for (;;) {
      const item = await cancellable(() => iterator.next(), signal);
      signal?.throwIfAborted();
      if (item.done) {
        exhausted = true;
        break;
      }
      if (entries.length >= NSP_INPUT_LIMITS.directoryEntries) fail("DIRECTORY_LIMIT");
      const [name, handle] = item.value;
      if (handle?.kind === "file" && /\.(?:nsp|xci)$/iu.test(name)) inputMode = "packages";
      entries.push(item.value);
    }
  } finally {
    if (!exhausted)
      Promise.resolve()
        .then(() => iterator.return?.())
        .catch(() => {});
  }
  return {
    inputMode,
    directory: {
      kind: "directory",
      async *entries() {
        yield* entries;
      },
    },
  };
}
/**
 * @param {any} directory
 * @param {{inputMode?: "packages"|"extracted", signal?: AbortSignal, onProgress?: (event: {inputMode: string, extractedFileCount: number, nspCount: number, prodKeysFound: boolean, titleKeysFound: boolean, scannedEntries?: number}) => void}} [options]
 */
export async function discoverGameFolder(directory, { inputMode, signal, onProgress } = {}) {
  if (inputMode === undefined) ({ directory, inputMode } = await detectFolder(directory, signal));
  mode(inputMode);
  if (inputMode === "packages") {
    const files = await discoverNspFolder(directory, {
      signal,
      onProgress: (event) => observer(onProgress, inputMode, 0, event),
    });
    return { ...files, inputMode };
  }
  if (directory?.kind !== "directory" || typeof directory.entries !== "function") fail("DIRECTORY");
  const records = [],
    seen = new Set();
  const stack = [{ directory, prefix: "" }];
  let count = 0,
    pathBytes = 0;
  observer(onProgress, inputMode);
  while (stack.length) {
    const { directory: current, prefix } = stack.pop();
    const iterator = current.entries()[Symbol.asyncIterator]();
    let exhausted = false;
    try {
      for (;;) {
        const item = await cancellable(() => iterator.next(), signal);
        signal?.throwIfAborted();
        if (item.done) {
          exhausted = true;
          break;
        }
        const [name, handle] = item.value;
        if (++count > ROMFS_LIMITS.members) fail("EXTRACTED_LIMIT");
        path(name);
        if (name.includes("/")) fail("EXTRACTED_NAME");
        const relative = path(prefix ? `${prefix}/${name}` : name),
          folded = relative.toLowerCase();
        pathBytes += encoder.encode(relative).length;
        if (pathBytes > ROMFS_LIMITS.totalPathBytes) fail("EXTRACTED_LIMIT");
        if (seen.has(folded)) fail("EXTRACTED_NAME");
        seen.add(folded);
        if (!prefix && isSeparateDlcLayout(name)) fail("EXTRACTED_DLC_UNSUPPORTED");
        if (!prefix && !["exefs", "romfs"].includes(folded)) continue;
        if (handle?.kind === "directory") stack.push({ directory: handle, prefix: relative });
        else if (handle?.kind === "file" && prefix) {
          const file = await cancellable(() => handle.getFile(), signal);
          signal?.throwIfAborted();
          records.push({ path: relative, file });
          observer(onProgress, inputMode, records.length);
        } else fail("EXTRACTED_MISSING_INPUT");
      }
    } finally {
      if (!exhausted)
        Promise.resolve()
          .then(() => iterator.return?.())
          .catch(() => {});
    }
  }
  signal?.throwIfAborted();
  return filesResult(records);
}
/** @param {Iterable<File>} files
 * @param {{inputMode?: "packages"|"extracted"}} [options] */
export function discoverGameFiles(files, { inputMode } = {}) {
  if (inputMode === undefined) {
    const selected = [];
    inputMode = "extracted";
    for (const file of files) {
      if (selected.length >= ROMFS_LIMITS.members) fail("EXTRACTED_LIMIT");
      const parts = (file.webkitRelativePath || "").split("/");
      if (parts.length === 2 && /\.(?:nsp|xci)$/iu.test(parts[1])) inputMode = "packages";
      if (inputMode === "packages" && selected.length >= NSP_INPUT_LIMITS.directoryEntries)
        fail("DIRECTORY_LIMIT");
      selected.push(file);
    }
    files = selected;
  }
  mode(inputMode);
  if (inputMode === "packages") return { ...discoverNspFiles(files), inputMode };
  const records = [],
    seen = new Set();
  let root,
    count = 0,
    pathBytes = 0;
  for (const file of files) {
    if (++count > ROMFS_LIMITS.members) fail("EXTRACTED_LIMIT");
    const parts = path(file.webkitRelativePath || "").split("/");
    if (parts.length < 2 || file.name !== parts.at(-1)) fail("EXTRACTED_NAME");
    if (root === undefined) root = parts[0];
    if (parts[0] !== root) fail("DIRECTORY");
    const relative = parts.slice(1).join("/");
    if (isSeparateDlcLayout(relative)) fail("EXTRACTED_DLC_UNSUPPORTED");
    pathBytes += encoder.encode(relative).length;
    if (pathBytes > ROMFS_LIMITS.totalPathBytes) fail("EXTRACTED_LIMIT");
    if (seen.has(relative.toLowerCase())) fail("EXTRACTED_NAME");
    seen.add(relative.toLowerCase());
    if (["exefs", "romfs"].includes(parts[1].toLowerCase())) records.push({ path: relative, file });
  }
  return filesResult(records);
}
