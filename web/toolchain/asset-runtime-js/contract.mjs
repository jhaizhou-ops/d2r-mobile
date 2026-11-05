// 保留现有资产 worker 的操作限制与传输协议；来源为 pyodide-worker.mjs。
// Preserve existing asset worker operation limits and transfer protocol from pyodide-worker.mjs.
export const OPERATION_LIMITS = Object.freeze({
  "image-default-frame": [20, 4096],
  "experience-table": [20, 4096],
  "overlay-paths": [20, 4096],
  "derived-tables": [20, 4096],
  "vault-table": [20, 4096],
  "native-art": [20, 4096],
  "hud-v4": [64, 4096],
  "map-goal": [20, 4096],
  "casc-overlay": [64, 1048576],
  "casc-prebuilt-overlay": [64, 1048576],
  "mercenary-art": [20, 4096],
  "overlay-recipe": [144, 4096],
  "remaining-art": [20, 4096],
});
export function validateInitialization(data) {
  if (
    !Array.isArray(data?.assets) ||
    data.assets.length > 32 ||
    (data.probes !== undefined && (!Array.isArray(data.probes) || data.probes.length !== 0))
  )
    throw new Error("Invalid bounded asset initialization; network proof probes are unsupported");
  const assets = new Map();
  let total = 0;
  for (const entry of data.assets) {
    if (!Array.isArray(entry) || entry.length !== 2)
      throw new Error("Invalid bounded runtime asset");
    const [name, bytes] = entry;
    if (
      typeof name !== "string" ||
      !/^[a-zA-Z0-9_.+-]+$/.test(name) ||
      assets.has(name) ||
      !(bytes instanceof ArrayBuffer) ||
      !bytes.byteLength ||
      bytes.byteLength > 32 * 1024 * 1024
    )
      throw new Error("Invalid bounded runtime asset");
    total += bytes.byteLength;
    assets.set(name, new Uint8Array(bytes));
  }
  if (total > 96 * 1024 * 1024) throw new Error("Runtime cap exceeded");
  return assets;
}
export function validateRequest(data) {
  const limits = Object.hasOwn(OPERATION_LIMITS, data?.operation)
    ? OPERATION_LIMITS[data.operation]
    : null;
  if (
    !Number.isSafeInteger(data?.id) ||
    data.id < 1 ||
    !limits ||
    !(data.payload instanceof ArrayBuffer) ||
    data.payload.byteLength > limits[0] * 1024 * 1024 ||
    typeof data.options !== "object" ||
    data.options === null ||
    JSON.stringify(data.options).length > limits[1]
  )
    throw new Error("Invalid bounded asset request");
  // 外部引用只在最后的独立载荷段中传输；普通选项上限保持不变。
  // External references travel only in a separate final payload segment; ordinary option limits stay unchanged.
  if (data.operation === "casc-prebuilt-overlay") {
    const { parts, jobs, location } = data.options;
    if (
      Object.keys(data.options).some((key) => !["parts", "jobs", "location"].includes(key)) ||
      !Array.isArray(jobs) ||
      jobs.length > 4096 ||
      !Array.isArray(parts) ||
      parts.length !== 4 + jobs.length ||
      parts.some((n) => !Number.isSafeInteger(n) || n < 0) ||
      parts.at(-1) < 1 ||
      parts.at(-1) > 16 * 1024 * 1024 ||
      parts.reduce((sum, n) => sum + n, 0) !== data.payload.byteLength ||
      !Array.isArray(location) ||
      location.length !== 2 ||
      location.some((n) => !Number.isSafeInteger(n) || n < 0 || n > 255) ||
      new TextEncoder().encode(JSON.stringify(data.options)).length > 1048576
    )
      throw new Error("Invalid bounded external CASC references");
  }
  return {
    operation: data.operation,
    payload: new Uint8Array(data.payload),
    options: data.options,
  };
}
export function transferableResult(id, result) {
  if (!result || !result.files || typeof result.files !== "object" || Array.isArray(result.files))
    throw new Error("Invalid asset result");
  const files = {},
    transfers = [];
  for (const [name, bytes] of Object.entries(result.files)) {
    if (!(bytes instanceof Uint8Array)) throw new Error("Invalid asset result bytes");
    const copy = new Uint8Array(bytes);
    Object.defineProperty(files, name, { value: copy.buffer, enumerable: true });
    transfers.push(copy.buffer);
  }
  return { message: { id, ok: true, result: { ...result, files } }, transfers };
}
