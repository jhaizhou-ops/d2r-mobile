// 构建时固定的本地签名入口；初始化后没有代码下载或任意模块加载。
// Build-owned local signing entry; no code downloads or arbitrary module loading after initialization.
import factory from "../toolchain/zsign-wasm/dist/zsign.mjs";
import { byteFactory } from "./wasm-factory.mjs";
import { createIpaSigningWorkerHandler } from "../ipa-signing-worker-core.mjs";
import { inspectUnsignedSigningArchive, createSignedSigningArchive } from "../ipa-signing-zip.mjs";
import { mountSigningArchive } from "../ipa-signing-mount.mjs";
const handle = createIpaSigningWorkerHandler({
  createNative: (buffer) => byteFactory(factory, buffer)(),
  inspectArchive: inspectUnsignedSigningArchive,
  mountArchive: mountSigningArchive,
  createArchive: createSignedSigningArchive,
  postMessage: (message) => self.postMessage(message),
  closeWorker: () => self.close(),
});
self.addEventListener("message", ({ data }) => {
  void handle(data);
});
