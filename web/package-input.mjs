// 依据内容魔数分派包容器；后续 NCA/CNMT 认证保持不变。
// Dispatch package containers by content magic; downstream NCA/CNMT authentication is unchanged.
import { inspectNsp, NspInputError } from "./nsp-input.mjs";
import { inspectXci } from "./xci-input.mjs";
export async function inspectGamePackage(file) {
  if (!(file instanceof Blob) || !Number.isSafeInteger(file.size) || file.size < 16)
    throw new NspInputError("PACKAGE_FORMAT", "Package header is invalid.");
  const bytes = new Uint8Array(await file.slice(0, Math.min(file.size, 0x104)).arrayBuffer());
  if (bytes.length !== Math.min(file.size, 0x104))
    throw new NspInputError("PACKAGE_FORMAT", "Package header is truncated.");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(0, true) === 0x30534650) return inspectNsp(file);
  if (bytes.length >= 0x104 && view.getUint32(0x100, true) === 0x44414548) return inspectXci(file);
  throw new NspInputError("PACKAGE_FORMAT", "Expected NSP/PFS0 or XCI/HEAD input.");
}
