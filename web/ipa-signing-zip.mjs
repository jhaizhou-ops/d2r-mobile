// 独立检查签名事务的规范 ZIP；不放宽未签名外壳合同。
// Independently checks the signing transaction's canonical ZIP, without relaxing the unsigned shell contract.
import { fileSource } from "./preflight.mjs";
import { planStagedZip, validateStagedZip } from "./staged-zip-validator.mjs";
import { writeZip } from "./zip-writer.mjs";
import { IPA_SIGNING_LIMITS, signingError, validSigningBlob } from "./ipa-signing-contract.mjs";

const options = (signal) => ({
  archiveFormat: "zip32",
  maxOutputBytes: IPA_SIGNING_LIMITS.ipaBytes,
  signal,
});
const requireValue = (value) => {
  if (!value) throw signingError("SIGNING_INPUT_INVALID");
};
const forbidden = (path) =>
  /(?:^|\/)(?:_CodeSignature|embedded\.mobileprovision|PlugIns|Watch)(?:\/|$)/i.test(path);

export async function inspectUnsignedSigningArchive(blob, { signal } = {}) {
  try {
    requireValue(validSigningBlob(blob));
    signal?.throwIfAborted();
    const end = new DataView(await blob.slice(-22).arrayBuffer());
    requireValue(end.getUint32(0, true) === 0x06054b50 && end.getUint16(20, true) === 0);
    const count = end.getUint16(10, true),
      length = end.getUint32(12, true),
      offset = end.getUint32(16, true);
    requireValue(
      count > 0 &&
        count <= 20000 &&
        length <= 8 * 1024 * 1024 &&
        offset + length === blob.size - 22,
    );
    const bytes = new Uint8Array(await blob.slice(offset, offset + length).arrayBuffer());
    const view = new DataView(bytes.buffer),
      manifest = [];
    let cursor = 0,
      appRoot;
    for (let i = 0; i < count; i++) {
      requireValue(cursor + 46 <= bytes.length && view.getUint32(cursor, true) === 0x02014b50);
      const nameLength = view.getUint16(cursor + 28, true);
      requireValue(
        nameLength > 0 && nameLength <= 4096 && cursor + 46 + nameLength <= bytes.length,
      );
      requireValue(
        view.getUint16(cursor + 30, true) === 0 && view.getUint16(cursor + 32, true) === 0,
      );
      const path = new TextDecoder("utf-8", { fatal: true }).decode(
        bytes.subarray(cursor + 46, cursor + 46 + nameLength),
      );
      const match = /^Payload\/([^/]+\.app)\//.exec(path);
      requireValue(match && !forbidden(path));
      const root = `Payload/${match[1]}/`;
      appRoot ??= root;
      requireValue(appRoot === root);
      const size = view.getUint32(cursor + 24, true),
        mode = view.getUint32(cursor + 38, true) >>> 16;
      requireValue(size <= IPA_SIGNING_LIMITS.memberBytes && [0o100644, 0o100755].includes(mode));
      manifest.push({ path, size, mode: mode & 0o777 });
      cursor += 46 + nameLength;
    }
    requireValue(cursor === bytes.length);
    // 清单由实际中央目录推导；独立验证器重新检查所有头、偏移、CRC 和尾部。
    // Derive the manifest from the actual directory; the independent validator rechecks every header, offset, CRC and trailer.
    await validateStagedZip(fileSource(blob), manifest, options(signal));
    const plan = planStagedZip(manifest, options(signal));
    const entries = plan.entries.map(({ path, size, mode, offset, name }) =>
      Object.freeze({
        path,
        size,
        mode,
        blob: blob.slice(offset + 30 + name.length, offset + 30 + name.length + size),
      }),
    );
    requireValue(entries.some((entry) => entry.path === appRoot + "Info.plist"));
    return Object.freeze({ appRoot, entries: Object.freeze(entries) });
  } catch (error) {
    signal?.throwIfAborted();
    if (error?.message === "SIGNING_INPUT_INVALID") throw error;
    throw signingError("SIGNING_INPUT_INVALID");
  }
}

export async function createSignedSigningArchive(entries, { signal } = {}) {
  const parts = [];
  let pendingPayload;
  try {
    requireValue(Array.isArray(entries) && entries.length > 0 && entries.length <= 20000);
    const snapshot = entries.map(({ path, size, mode, blob }) => {
      requireValue(
        blob instanceof Blob && blob.size === size && size <= IPA_SIGNING_LIMITS.memberBytes,
      );
      return { path, size, mode, blob };
    });
    planStagedZip(snapshot, options(signal));
    let total = 0,
      closed = false;
    await writeZip(
      snapshot.map((entry) => ({
        ...entry,
        async *chunks() {
          for (let offset = 0; offset < entry.size; offset += 65536) {
            signal?.throwIfAborted();
            const source = entry.blob.slice(offset, offset + 65536);
            const bytes = new Uint8Array(await source.arrayBuffer());
            try {
              requireValue(
                source.size === Math.min(65536, entry.size - offset) &&
                  bytes.length === source.size,
              );
              requireValue(!pendingPayload);
              pendingPayload = { bytes, source };
              yield bytes;
            } finally {
              pendingPayload = undefined;
              // 输出持有不可变 Blob 切片；CRC 临时缓冲区在消费或失败后不再需要。
              // Output owns immutable Blob slices; CRC scratch bytes are no longer needed after consumption or failure.
              bytes.fill(0);
            }
          }
        },
      })),
      {
        async write(bytes) {
          total += bytes.length;
          if (total > IPA_SIGNING_LIMITS.ipaBytes) throw signingError("SIGNING_SIZE_LIMIT");
          // 仅精确匹配当前 CRC 块时复用不可变切片；头和描述符仍独立快照。
          // Reuse an immutable slice only for the exact current CRC chunk; headers/descriptors remain independent snapshots.
          const payload = pendingPayload;
          pendingPayload = undefined;
          if (payload) {
            requireValue(payload.bytes === bytes);
            parts.push(payload.source);
          } else {
            parts.push(new Blob([bytes]));
          }
        },
        async close() {
          closed = true;
        },
        async abort() {
          pendingPayload = undefined;
          parts.length = 0;
        },
      },
      { signal },
    );
    requireValue(closed);
    const blob = new Blob(parts, { type: "application/octet-stream" });
    parts.length = 0;
    await validateStagedZip(fileSource(blob), snapshot, options(signal));
    signal?.throwIfAborted();
    return blob;
  } catch {
    pendingPayload = undefined;
    parts.length = 0;
    signal?.throwIfAborted();
    throw signingError("SIGNING_OUTPUT_INVALID");
  }
}
