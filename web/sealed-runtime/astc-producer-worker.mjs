// 从预验证公开字节初始化固定编码器，然后串行生成私有补充包；仅供已密封加载器使用。
// Initialize fixed codecs from preverified public bytes, then serially produce a private pack for the sealed loader.
import { createArchiveReadRpcClient } from "../archive-read-rpc.mjs";
import { createAstcPackJobsCore } from "../astc-pack-jobs-core.mjs";
import { createRawFileStage } from "../raw-file-stage.mjs";
import { writeCascPack } from "../casc-pack-writer.mjs";
import { encodeWithModule } from "../toolchain/astc-encoder-wasm/contract.mjs";
import { createPngCodec } from "../toolchain/png-codec-exact/codec.mjs";
import { createSha256FileHasher } from "../toolchain/sha256-stream-wasm/adapter.mjs";
const fail = () => {
  throw new Error("ASTC_PRODUCER_WORKER_FAILED");
};
// 仅白名单诊断可跨线程，不传播原始错误文本、路径或键。
// Only allowlisted diagnostics cross threads, never raw errors, paths or keys.
export function astcProducerFailureCode(error) {
  if (error?.name === "QuotaExceededError") return "STORAGE_QUOTA";
  if (
    [
      "CASC_PACK_PACK_SIZE",
      "CASC_PACK_ENCODED_SIZE",
      "CASC_PACK_METADATA_LIMIT",
      "CASC_PACK_OFFSET_RANGE",
      "RAW_STAGE_BYTE_LIMIT",
      "ARCHIVE_STREAM_RECORD",
      "ARCHIVE_STREAM_BLOCK_BOUNDS",
      "ARCHIVE_STREAM_RAW_BOUNDS",
      "ARCHIVE_STREAM_DECODED_SIZE",
      "ASTC_PACK_JOBS_DESCRIPTOR_SIZE",
      "ASTC_PACK_JOBS_PATH_BUDGET",
    ].includes(error?.message)
  )
    return "SIZE_LIMIT";
  if (
    [
      "ARCHIVE_STREAM_EKEY",
      "ARCHIVE_STREAM_CKEY",
      "ARCHIVE_STREAM_BLOCK_MD5",
      "ARCHIVE_STREAM_SOURCE_CHANGED",
    ].includes(error?.message)
  )
    return "INTEGRITY_FAILED";
  return "CODEC_FAILED";
}
const names = ["astc-encoder.mjs", "astc-encoder.wasm", "png-codec.wasm", "sha256.wasm"];
export async function initializeAstcProducerCodecs(entries) {
  if (!Array.isArray(entries) || entries.length !== names.length) fail();
  const assets = new Map(entries);
  if (
    assets.size !== names.length ||
    names.some(
      (name) =>
        !(assets.get(name) instanceof ArrayBuffer) ||
        assets.get(name).resizable ||
        !assets.get(name).byteLength ||
        assets.get(name).byteLength > 32 * 1024 * 1024,
    )
  )
    fail();
  const urls = [];
  const blob = (bytes, type) => {
    const url = URL.createObjectURL(new Blob([bytes], { type }));
    urls.push(url);
    return url;
  };
  try {
    const wasm = new Uint8Array(assets.get("astc-encoder.wasm"));
    const wasmUrl = blob(wasm, "application/wasm");
    const { default: factory } = await import(
      blob(assets.get("astc-encoder.mjs"), "text/javascript")
    );
    let instantiations = 0;
    const module = await factory({
      wasmBinary: wasm,
      locateFile(name) {
        if (name !== "astc-encoder.wasm") fail();
        return wasmUrl;
      },
      instantiateWasm(imports, receive) {
        const compiled = new WebAssembly.Module(wasm),
          instance = new WebAssembly.Instance(compiled, imports);
        instantiations++;
        receive(instance, compiled);
        return instance.exports;
      },
      print() {},
      printErr() {},
    });
    if (
      instantiations !== 1 ||
      typeof module._astc_encoder_abi_version !== "function" ||
      module._astc_encoder_abi_version() !== 2 ||
      !(module.HEAPU8 instanceof Uint8Array) ||
      ["_malloc", "_free", "_astc_encode_rgba"].some((name) => typeof module[name] !== "function")
    )
      fail();
    const codec = await createPngCodec(new Uint8Array(assets.get("png-codec.wasm")));
    const hasher = await createSha256FileHasher(new Uint8Array(assets.get("sha256.wasm")));
    // 初始化完成后也不允许代码通过网络回退；这不是同源恶意脚本的安全沙箱。
    // Forbid network fallback after initialization too; this is not a sandbox against malicious same-origin scripts.
    return {
      encode: (input) => encodeWithModule(module, input),
      codec,
      hashFile: hasher.hashFile.bind(hasher),
    };
  } finally {
    for (const url of urls) URL.revokeObjectURL(url);
    assets.clear();
  }
}
/**
 * 注入接口仅供合成生命周期测试；真实入口使用上述固定编码器与固定内核。
 * Injection is only for synthetic lifecycle tests; the actual entry uses fixed codecs and core above.
 */
export function createAstcProducerWorkerHandler({
  send,
  initialize = initializeAstcProducerCodecs,
  core = createAstcPackJobsCore,
  writePack = writeCascPack,
} = {}) {
  let state = "new",
    codecs,
    rpc,
    rememberedFailure;
  const controller = new AbortController();
  return async function receive(data) {
    try {
      if (data?.type === "initialize" && state === "new") {
        state = "initializing";
        codecs = await initialize(data.assets);
        if (state !== "initializing") fail();
        state = "ready";
        send({ type: "initialized", version: 1, astc: true, png: true, sha: true });
        return;
      }
      if (data?.type !== "produce" || state !== "ready") fail();
      state = "running";
      const preset = data.preset;
      if (preset !== "thorough" && preset !== "fast") fail();
      if (
        !Array.isArray(data.records) ||
        data.records.length < 1 ||
        data.records.length > 32768 ||
        !(data.vfs instanceof ArrayBuffer) ||
        data.vfs.resizable ||
        data.vfs.byteLength > 16 * 1024 * 1024 ||
        data.root?.kind !== "directory"
      )
        fail();
      rpc = createArchiveReadRpcClient(data.port, { signal: controller.signal });
      const opened = new Map();
      const tokens = data.records.map((entry) => {
        if (
          typeof entry?.path !== "string" ||
          entry.path.length > 4096 ||
          typeof entry.record?.offset !== "bigint"
        )
          fail();
        const token = Object.freeze({ path: entry.path });
        opened.set(token, { source: rpc.open(entry.source), record: entry.record });
        return token;
      });
      const descriptor = tokens.at(-1);
      if (descriptor.path !== "data/hd/texture_desc_cache.json") fail();
      let sequence = 0,
        workedBytes = 0,
        workedBlocks = 0,
        lastReport = 0;
      const progress = (kind, count) => {
        if (!count) return;
        if (kind === "blocks") workedBlocks += count;
        else workedBytes += count;
        const now = performance.now();
        if (kind !== "blocks" && now - lastReport < 250) return;
        lastReport = now;
        send({ type: "progress", sequence: ++sequence, bytes: workedBytes, blocks: workedBlocks });
      };
      const createStage = async (options) => {
        let stage;
        const remember = (error) => {
          const code = astcProducerFailureCode(error);
          if (code !== "CODEC_FAILED") rememberedFailure = code;
          throw error;
        };
        try {
          stage = await createRawFileStage(options);
        } catch (error) {
          remember(error);
        }
        return {
          ...stage,
          async finalize() {
            try {
              return await stage.finalize();
            } catch (error) {
              remember(error);
            }
          },
          async append(bytes) {
            try {
              await stage.append(bytes);
            } catch (error) {
              remember(error);
            }
            progress("bytes", bytes.length);
          },
          async backpatch(at, bytes) {
            try {
              await stage.backpatch(at, bytes);
            } catch (error) {
              remember(error);
            }
            progress("bytes", bytes.length);
          },
        };
      };
      const jobs = core({
        selected: { textures: tokens.slice(0, -1), descriptor },
        vfs: new Uint8Array(data.vfs),
        openRecord: (token) => {
          const value = opened.get(token);
          if (!value) fail();
          return value;
        },
        root: data.root,
        maxEncodedBytes: data.maxEncodedBytes,
        maxDecodedBytes: data.maxDecodedBytes,
        batchBlocks: data.batchBlocks,
        encode: async (input) => {
          const output = await codecs.encode({ ...input, preset });
          progress("blocks", (input.width * input.height) / 16);
          return output;
        },
        createStage,
        signal: controller.signal,
      });
      let pack;
      try {
        pack = await writePack({
          jobs: jobs.jobs,
          root: data.root,
          location: data.location,
          locationAvailable: (name) => name === data.packName,
          maxPackBytes: data.maxPackBytes,
          maxEntries: jobs.maxEntries,
          codec: codecs.codec,
          createStage,
          hashFile: async (file, options) => {
            let hashed = 0;
            return codecs.hashFile(file, {
              ...options,
              onProgress: ({ bytesHashed }) => {
                progress("bytes", bytesHashed - hashed);
                hashed = bytesHashed;
              },
            });
          },
          signal: controller.signal,
        });
      } finally {
        await jobs.dispose();
        opened.clear();
      }
      controller.signal.throwIfAborted();
      state = "complete";
      rpc.close();
      // File 用结构化克隆传递而非 arrayBuffer；暂存目录由父级保留至导出结束。
      // Structured-clone the File, never arrayBuffer it; parent retains the staging directory through export.
      send({
        type: "complete",
        preset,
        file: pack.source.file,
        pack: pack.pack,
        records: pack.records,
        index: pack.index,
        count: pack.count,
      });
    } catch (error) {
      state = "failed";
      controller.abort();
      rpc?.close();
      send({ type: "failure", code: rememberedFailure ?? astcProducerFailureCode(error) });
    }
  };
}
if (
  typeof WorkerGlobalScope !== "undefined" &&
  globalThis instanceof globalThis.WorkerGlobalScope
) {
  // 只使用已提供字节，初始化期间和之后均禁止网络请求。
  // Use supplied bytes only; reject networking during and after initialization.
  globalThis.fetch = () => Promise.reject(new Error("ASTC_PRODUCER_NETWORK_FORBIDDEN"));
  globalThis.XMLHttpRequest = class {
    constructor() {
      fail();
    }
  };
  const receive = createAstcProducerWorkerHandler({ send: (data) => self.postMessage(data) });
  self.addEventListener("message", ({ data }) => {
    void receive(data);
  });
}
