// 依据公开合成夹具检查浏览器内 WASM 读取、完整性与清理边界。
// Check in-browser WASM read, integrity and cleanup boundaries using public synthetic fixtures.
import factory from "./dist/hactool-boundary.mjs";
import { createBoundary } from "./browser-boundary.mjs";
const assert = (ok) => {
  if (!ok) throw new Error("Synthetic assertion failed");
};
function fixture(name, payload) {
  const names = new TextEncoder().encode(`${name}\0`);
  const header = new Uint8Array(40 + names.length);
  header.set([80, 70, 83, 48]);
  const view = new DataView(header.buffer);
  view.setUint32(4, 1, true);
  view.setUint32(8, names.length, true);
  view.setBigUint64(24, BigInt(payload.length), true);
  header.set(names, 40);
  return new Blob([header, payload]);
}
function ambiguousFixture(duplicate) {
  const names = new TextEncoder().encode(duplicate ? "a\0a\0" : "a\0b\0");
  const bytes = new Uint8Array(16 + 48 + names.length + 6);
  bytes.set([80, 70, 83, 48]);
  const view = new DataView(bytes.buffer);
  view.setUint32(4, 2, true);
  view.setUint32(8, names.length, true);
  view.setBigUint64(24, 3n, true);
  view.setBigUint64(40, duplicate ? 3n : 1n, true);
  view.setBigUint64(48, 3n, true);
  view.setUint32(56, 2, true);
  bytes.set(names, 64);
  return new Blob([bytes]);
}
async function independentDigest(bytes) {
  return Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (byte) =>
    byte.toString(16).padStart(2, "0"),
  ).join("");
}
let stage = "module-initialization";
try {
  let wasm;
  const boundary = await createBoundary(async (options) => {
    wasm = await factory(options);
    return wasm;
  });
  // 代码已加载；操作期间不允许再使用任何网络 API。
  // Code has loaded. No further network API is permitted during operations.
  globalThis.fetch = () => {
    throw new Error("Unexpected network operation");
  };
  stage = "crypto-fixture";
  assert(boundary.cryptoFixture() === 0);
  stage = "ivfc-hierarchy-fixture";
  assert(boundary.ivfcFixture() === 0);
  stage = "bktr-merged-crypto-fixture";
  assert(boundary.bktrFixture() === 0);
  const prod = new TextEncoder().encode(`header_key = ${"0123456789abcdef".repeat(4)}\n`);
  const title = new TextEncoder().encode(`${"11".repeat(16)} = ${"22".repeat(16)}\n`);
  stage = "explicit-key-loading";
  boundary.loadKeys(prod, title);
  assert(prod.every((b) => b === 0) && title.every((b) => b === 0));
  assert(!boundary.secretFilesPresent());
  stage = "blob-pfs0-mount";
  const payload = new Uint8Array(3 * 65536 + 17);
  for (let i = 0; i < payload.length; i++) payload[i] = i % 251;
  const syntheticContainer = fixture("synthetic.cnmt", payload);
  boundary.mount([syntheticContainer]);
  stage = "bounded-input-sha256";
  assert(
    boundary.hashInput(0) === (await independentDigest(await syntheticContainer.arrayBuffer())),
  );
  assert(boundary.open(0, "synthetic-pfs0") === 1);
  stage = "bounded-entry-sha256";
  assert(boundary.hashEntry(0) === (await independentDigest(payload)));
  assert(boundary.entry(0).size === BigInt(payload.length));
  stage = "bounded-output";
  let received = 0,
    calls = 0;
  await boundary.streamEntry(0, async (chunk) => {
    assert(chunk.length <= 65536);
    for (let i = 0; i < chunk.length; i++) assert(chunk[i] === payload[received + i]);
    received += chunk.length;
    calls++;
    await new Promise((resolve) => setTimeout(resolve, 1));
  });
  assert(received === payload.length && calls === 4);
  let rejected = false;
  try {
    boundary.read(0, 0n, 65537);
  } catch {
    rejected = true;
  }
  assert(rejected);
  const controller = new AbortController();
  controller.abort();
  rejected = false;
  try {
    await boundary.streamEntry(0, async () => {}, controller.signal);
  } catch {
    rejected = true;
  }
  assert(rejected);
  // 稀疏的合成只读文件系统节点用于检查实际 WASM off_t 在 4 GiB 之外的定位，
  // 无需分配或下载 5 GiB Blob。上面的常规路径
  // 单独检查真实的 Blob/WORKERFS 读取。
  // A sparse synthetic read-only FS node exercises actual WASM off_t seeks
  // beyond 4 GiB without allocating or downloading a 5 GiB Blob. The regular
  // path above separately exercises real Blob/WORKERFS reads.
  stage = "sparse-64bit-seek";
  const sparse = new Uint8Array(await fixture("sparse.bin", new Uint8Array(1)).arrayBuffer());
  const sparseOffset = 5n * 1024n ** 3n;
  new DataView(sparse.buffer).setBigUint64(16, sparseOffset, true);
  boundary.mount([new Blob([sparse])]);
  const node = wasm.FS.lookupPath("/input/input-0").node;
  const payloadAt = BigInt(sparse.length - 1) + sparseOffset;
  node.size = Number(payloadAt + 1n);
  node.stream_ops = {
    ...node.stream_ops,
    read(stream, buffer, offset, length, position) {
      void stream;
      const count = Math.min(length, node.size - position);
      for (let i = 0; i < count; i++)
        buffer[offset + i] =
          position + i < sparse.length - 1
            ? sparse[position + i]
            : BigInt(position + i) === payloadAt
              ? 173
              : 0;
      return count;
    },
  };
  assert(boundary.open(0, "synthetic-pfs0") === 1);
  assert(boundary.entry(0).offset === sparseOffset);
  assert(boundary.read(0, 0n, 1)[0] === 173);
  // 生产 Meta 入口必须拒绝未签名的合成头部。
  // 该入口没有测试密钥覆盖选项或跳过签名的标志。
  // The production Meta entrypoint must reject an unsigned synthetic header.
  // There is no test-key override or skip-signature flag in that entrypoint.
  stage = "unsigned-meta-rejection";
  const unsignedHeader = new Uint8Array(0xc00);
  unsignedHeader.set([78, 67, 65, 51], 0x200);
  unsignedHeader[0x205] = 1;
  new DataView(unsignedHeader.buffer).setBigUint64(0x208, 0xc00n, true);
  boundary.mount([new Blob([unsignedHeader])]);
  assert(boundary.inspect(0).fixedSignatureValid === false);
  rejected = false;
  try {
    boundary.open(0, "meta");
  } catch {
    rejected = true;
  }
  assert(rejected);
  stage = "unknown-unsigned-meta-rejection";
  rejected = false;
  try {
    boundary.open(0, "warlock-meta");
  } catch {
    rejected = true;
  }
  assert(rejected);
  stage = "unsigned-program-rejection";
  unsignedHeader[0x205] = 0;
  boundary.mount([new Blob([unsignedHeader])]);
  rejected = false;
  try {
    boundary.open(0, "program-exefs");
  } catch {
    rejected = true;
  }
  assert(rejected);
  stage = "unsigned-romfs-rejection";
  rejected = false;
  try {
    boundary.openRomfs(0);
  } catch {
    rejected = true;
  }
  assert(rejected);
  stage = "unsigned-patch-base-rejection";
  rejected = false;
  try {
    boundary.openPatchedRomfs(0, 0);
  } catch {
    rejected = true;
  }
  assert(rejected);
  stage = "ambiguous-directory-rejection";
  for (const duplicate of [true, false]) {
    boundary.mount([ambiguousFixture(duplicate)]);
    rejected = false;
    try {
      boundary.open(0, "synthetic-pfs0");
    } catch {
      rejected = true;
    }
    assert(rejected);
    rejected = false;
    try {
      boundary.entry(0);
    } catch {
      rejected = true;
    }
    assert(rejected);
  }
  stage = "path-rejection";
  boundary.mount([fixture("../unsafe", new Uint8Array(1))]);
  rejected = false;
  try {
    boundary.open(0, "synthetic-pfs0");
  } catch {
    rejected = true;
  }
  assert(rejected);
  stage = "disposal";
  boundary.dispose();
  self.postMessage({
    ok: true,
    checks: [
      "WASM crypto fixture",
      "six-level IVFC padding/tamper/bounds fixture",
      "BKTR counter/merged read/IVFC tamper fixture",
      "unsigned patch base rejected",
      "unsigned RomFS header rejected",
      "explicit key paths and wiping",
      "WORKERFS synthetic PFS0",
      "bounded output/backpressure",
      "WASM sparse >4GiB seek",
      "unsigned Meta header rejected",
      "unsigned Program header rejected",
      "independent input/entry SHA256 parity",
      "duplicate/overlapping members rejected",
      "bounds/traversal/cancellation",
    ],
  });
} catch {
  // 绝不显示密钥处理上下文中的异常消息或堆栈。
  // Never surface exception messages or stacks from a key-handling context.
  self.postMessage({ ok: false, error: "Synthetic WASM browser smoke failed", stage });
}
