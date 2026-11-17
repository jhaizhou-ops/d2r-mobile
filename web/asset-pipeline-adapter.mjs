// 依据原生资源生成器、已认证提取及 d2rm_pack.py 目标路径连接资源流水线。
// Wire the asset pipeline from native generators, authenticated extraction and d2rm_pack.py destination paths.
import { readAuthenticatedAssets } from "./extraction-job-adapter.mjs";
import { generateNativeOverlay, NATIVE_OVERLAY_GENERATED } from "./native-overlay-generator.mjs";
import { generateRemainingArt } from "./remaining-art-generator.mjs";
import { generateMercenaryArt } from "./mercenary-art-generator.mjs";
import { generateMapGoal } from "./map-goal-generator.mjs";
import { writeCascOverlay } from "./casc-overlay-writer.mjs";
import { validateAstcOutputs, validateLegacyPackMetadata } from "./astc-output-validator.mjs";
import { RELEASE_ART_REVISION, requireArtRevision } from "./toolchain/art-recipes-js/revision.mjs";
import {
  RELEASE_OVERLAY_REVISION,
  requireOverlayRevision,
} from "./toolchain/overlay-recipe-js/revision.mjs";
import { validateEncodeRequest } from "./toolchain/texture-spike/browser-texture-encoder.mjs";

export const ASSET_PIPELINE_LIMITS = Object.freeze({
  textures: 32,
  sourceBytes: 8 * 1024 * 1024,
  replacementBytes: 48 * 1024 * 1024,
  outputBytes: 80 * 1024 * 1024,
  chunkBytes: 64 * 1024,
});
class AssetPipelineError extends Error {}
const fail = (code) => {
  throw new AssetPipelineError(code);
};
const path = (value) => {
  if (
    typeof value !== "string" ||
    value.length > 512 ||
    !/^data\/[a-z0-9_./ -]+$/.test(value) ||
    value.split("/").some((p) => !p || p === "." || p === "..")
  )
    fail("ASSET_PIPELINE_RECIPE");
  return value;
};
const view = (value) =>
  value instanceof Uint8Array ? value : value instanceof ArrayBuffer ? new Uint8Array(value) : null;

// 仅快照受信任调用方的有界元数据；不接受包载荷，也不认证哈希或来源。
// Snapshot bounded metadata from a trusted caller only; accept no pack payload and authenticate neither hashes nor provenance.
export function snapshotPrebuiltPackMetadata(value) {
  if (value === undefined) return undefined;
  if (
    !value ||
    typeof value !== "object" ||
    Array.isArray(value) ||
    Object.keys(value).sort().join(",") !== "pack,records" ||
    !Array.isArray(value.records) ||
    !value.records.length ||
    value.records.length > 32768
  )
    fail("ASSET_PIPELINE_PREBUILT_METADATA");
  const pack = validateLegacyPackMetadata(value.pack);
  if (pack.size === 0) fail("ASSET_PIPELINE_PREBUILT_METADATA");
  let totalPathBytes = 0;
  const textEncoder = new TextEncoder();
  const records = value.records.map((record) => {
    const pathBytes =
      typeof record?.path === "string" && record.path.length <= 4096
        ? textEncoder.encode(record.path).length
        : Infinity;
    if (
      !record ||
      typeof record !== "object" ||
      Array.isArray(record) ||
      Object.keys(record).sort().join(",") !== "decodedSize,encodedSize,newEkey,oldEkey,path" ||
      typeof record.path !== "string" ||
      !record.path.length ||
      record.path.length > 4096 ||
      pathBytes > 4096 ||
      (totalPathBytes += pathBytes) > 8 * 1024 * 1024 ||
      typeof record.oldEkey !== "string" ||
      !/^[0-9a-f]{32}$/.test(record.oldEkey) ||
      typeof record.newEkey !== "string" ||
      !/^[0-9a-f]{32}$/.test(record.newEkey) ||
      [record.encodedSize, record.decodedSize].some(
        (size) => !Number.isSafeInteger(size) || size < 0 || size > 0xffffffff,
      )
    )
      fail("ASSET_PIPELINE_PREBUILT_METADATA");
    return Object.freeze({
      path: record.path,
      oldEkey: record.oldEkey,
      newEkey: record.newEkey,
      encodedSize: record.encodedSize,
      decodedSize: record.decodedSize,
    });
  });
  return Object.freeze({ pack, records: Object.freeze(records) });
}

/**
 * 仅用于受信任宿主接线。这里组合真实原语，而非完整游戏配置。纹理变换由源码派生函数显式提供；不虚构布局或替换图像。应用接受任务输入之前，工厂必须返回已预加载并密封的客户端。原生目标：d2rm_pack.py 暂存 mapgoal/ 和 romfs_overlay/。此适配器仅输出覆盖增量文件，绝不克隆整个 RomFS。
 * Trusted host wiring only. This composes real primitives, not a complete game
 * profile. Texture transforms are explicitly supplied source-derived functions;
 * no layout or replacement image is invented. Factories must return preloaded,
 * sealed clients before job inputs are accepted by the application.
 *
 * Native destinations: d2rm_pack.py stages mapgoal/ and romfs_overlay/. This
 * adapter emits only overlay delta files, never clones the bulk RomFS.
 */
export function createAssetPipelineAdapter({
  artRecipeRevision = RELEASE_ART_REVISION,
  overlayRecipeRevision = RELEASE_OVERLAY_REVISION,
  mapGoal = false,
  mercenaryArt = false,
  remainingArt = false,
  nativeOverlayGenerated = null,
  outputCache,
  cacheContext,
  prebuiltPack,
  textureReplacements = [],
  createAssetWorker,
  recycleAssetWorker,
  createTextureClient,
  createTextureEncoder,
} = {}) {
  requireArtRevision(artRecipeRevision);
  requireOverlayRevision(overlayRecipeRevision);
  if (
    typeof mapGoal !== "boolean" ||
    typeof mercenaryArt !== "boolean" ||
    typeof remainingArt !== "boolean" ||
    (remainingArt && nativeOverlayGenerated === null) ||
    (prebuiltPack !== undefined && nativeOverlayGenerated === null) ||
    !Array.isArray(textureReplacements) ||
    textureReplacements.length > ASSET_PIPELINE_LIMITS.textures ||
    (!mapGoal && !mercenaryArt && !textureReplacements.length && nativeOverlayGenerated === null) ||
    (nativeOverlayGenerated !== null &&
      (typeof nativeOverlayGenerated !== "object" ||
        Array.isArray(nativeOverlayGenerated) ||
        textureReplacements.length ||
        Object.keys(nativeOverlayGenerated).some(
          (name) => !NATIVE_OVERLAY_GENERATED.includes(name),
        ))) ||
    typeof createAssetWorker !== "function" ||
    (recycleAssetWorker !== undefined && typeof recycleAssetWorker !== "function") ||
    typeof createTextureClient !== "function" ||
    ((textureReplacements.length || mercenaryArt || remainingArt) &&
      typeof createTextureEncoder !== "function")
  )
    fail("ASSET_PIPELINE_OPTIONS");
  prebuiltPack = snapshotPrebuiltPackMetadata(prebuiltPack);
  let astcBinding;
  const destinations = new Set();
  const recipes = textureReplacements.map((recipe) => {
    const input = path(recipe?.input),
      replacement = path(recipe?.replacement);
    if (
      !/^[a-f0-9]{64}$/.test(recipe.sha256) ||
      typeof recipe.transform !== "function" ||
      typeof recipe.add !== "boolean" ||
      destinations.has(replacement)
    )
      fail("ASSET_PIPELINE_RECIPE");
    destinations.add(replacement);
    return Object.freeze({
      input,
      replacement,
      sha256: recipe.sha256,
      transform: recipe.transform,
      add: recipe.add,
    });
  });
  let used = false,
    disposed = false,
    cleanupFailed = false;
  const clients = [],
    outputs = [],
    releaseEntries = [];
  let internalAbort;
  const closeClients = () => {
    for (const client of clients.splice(0)) {
      try {
        client.close();
      } catch {
        cleanupFailed = true;
      }
    }
  };
  const wipe = () => {
    try {
      for (const bytes of outputs) bytes.fill(0);
    } finally {
      outputs.length = 0;
      // 已返回的条目仍可存活；显式解除它们捕获的输出引用。
      // Returned entries may remain alive; explicitly release their captured output references.
      for (const release of releaseEntries.splice(0)) release();
    }
  };
  const dispose = () => {
    disposed = true;
    astcBinding = undefined;
    prebuiltPack = undefined;
    try {
      internalAbort?.abort();
      closeClients();
    } finally {
      // 仅解除借用对象引用，不修改调用方对象或字节。
      // Drop only our borrowed reference, without mutating the caller's object or bytes.
      nativeOverlayGenerated = null;
      wipe();
    }
    if (cleanupFailed) fail("ASSET_PIPELINE_CLEANUP");
  };
  async function generateAssets({ extracted, selection, signal } = {}) {
    astcBinding = undefined;
    if (used || disposed) fail("ASSET_PIPELINE_SINGLE_USE");
    if (signal?.aborted) fail("ASSET_PIPELINE_CANCELLED");
    used = true;
    internalAbort = new AbortController();
    const combined = signal
      ? AbortSignal.any([signal, internalAbort.signal])
      : internalAbort.signal;
    const check = () => {
      if (combined.aborted || disposed) fail("ASSET_PIPELINE_CANCELLED");
    };
    const abort = () => closeClients();
    combined.addEventListener("abort", abort, { once: true });
    const owned = new Set();
    const entries = [];
    let outputBytes = 0,
      pendingAstcBinding,
      completedEntries;
    const own = (value) => {
      const bytes = view(value);
      if (!bytes) fail("ASSET_PIPELINE_BYTES");
      owned.add(bytes);
      return bytes;
    };
    const release = (bytes) => {
      bytes.fill(0);
      owned.delete(bytes);
    };
    const wipeLate = (result) => {
      const direct = view(result);
      if (direct) direct.fill(0);
      for (const value of Object.values(result?.files ?? {})) view(value)?.fill(0);
      for (const key of ["pixels", "rgba", "sprite", "linearBC3"]) view(result?.[key])?.fill(0);
    };
    const run = async (action) => {
      check();
      let cancel;
      const interrupted = new Promise((_, reject) => {
        cancel = () => reject(new Error("ASSET_PIPELINE_CANCELLED"));
        combined.addEventListener("abort", cancel, { once: true });
      });
      try {
        return await Promise.race([
          Promise.resolve()
            .then(() => {
              check();
              return action();
            })
            .then((result) => {
              if (combined.aborted || disposed) {
                wipeLate(result);
                check();
              }
              return result;
            }),
          interrupted,
        ]);
      } finally {
        combined.removeEventListener("abort", cancel);
      }
    };
    const client = (factory, method) => {
      const result = factory();
      if (result && typeof result.close === "function") clients.push(result);
      if (!result || typeof result.close !== "function" || typeof result[method] !== "function")
        fail("ASSET_PIPELINE_CLIENT");
      return result;
    };
    const append = (name, value, adopt = false) => {
      const bytes = view(value);
      if (!bytes?.length || (outputBytes += bytes.length) > ASSET_PIPELINE_LIMITS.outputBytes)
        fail("ASSET_PIPELINE_OUTPUT_LIMIT");
      if (adopt) {
        // 仅接管内部原生覆盖生成器的独占完整缓冲区，其他调用保持快照语义。
        // Adopt only exclusive full buffers from the internal native overlay generator.
        if (
          !owned.has(bytes) ||
          Object.getPrototypeOf(bytes) !== Uint8Array.prototype ||
          !(bytes.buffer instanceof ArrayBuffer) ||
          Object.getPrototypeOf(bytes.buffer) !== ArrayBuffer.prototype ||
          bytes.buffer.resizable === true ||
          bytes.byteOffset !== 0 ||
          bytes.byteLength !== bytes.buffer.byteLength
        )
          fail("ASSET_PIPELINE_BYTES");
      }
      let stored = adopt ? bytes : new Uint8Array(bytes);
      const size = stored.length;
      outputs.push(stored);
      if (adopt) owned.delete(bytes);
      releaseEntries.push(() => {
        stored = undefined;
      });
      entries.push(
        Object.freeze({
          path: name,
          size,
          mode: 0o100644,
          async *chunks() {
            check();
            for (let offset = 0; offset < size; offset += ASSET_PIPELINE_LIMITS.chunkBytes) {
              check();
              if (!stored) fail("ASSET_PIPELINE_CANCELLED");
              yield stored.slice(offset, offset + ASSET_PIPELINE_LIMITS.chunkBytes);
            }
          },
        }),
      );
    };
    try {
      // 构造任何运行时或读取输入之前，先检查来源证明。
      // Provenance is checked before constructing any runtime or reading input.
      const assets = readAuthenticatedAssets(extracted, selection, { signal: combined });
      if (!assets?.logicalAssets || !assets?.romfs) fail("ASSET_PIPELINE_PROVENANCE");
      const worker = client(createAssetWorker, "run");
      const decoder = client(createTextureClient, "decodeAtlas");
      const decodeAtlas = async (bytes, options) => {
        const result = await decoder.decodeAtlas(bytes, options);
        return { ...result, frames: result.frameCount };
      };
      if (mapGoal) {
        const generated = await run(() =>
          generateMapGoal({
            recipeRevision: artRecipeRevision,
            logicalAssets: assets.logicalAssets,
            assetWorker: worker,
            decodeAtlas,
            signal: combined,
            outputCache,
            cacheContext,
          }),
        );
        check();
        for (const [name, bytes] of Object.entries(generated.files)) {
          const generatedBytes = own(bytes);
          append(`mapgoal/${name}`, generatedBytes);
          release(generatedBytes);
        }
      }
      const generatedArt = {};
      if (nativeOverlayGenerated !== null) {
        let bytes = 0;
        for (const [name, value] of Object.entries(nativeOverlayGenerated)) {
          const source = view(value);
          if (
            !source?.length ||
            source.length > 16 * 1024 * 1024 ||
            (bytes += source.length) > 48 * 1024 * 1024
          )
            fail("ASSET_PIPELINE_GENERATED_ART");
          generatedArt[name] = own(source.slice());
        }
      }
      let encoder;
      const getEncoder = () => (encoder ??= client(createTextureEncoder, "encodeAndPack"));
      if (mercenaryArt) {
        const generated = await run(() =>
          generateMercenaryArt({
            logicalAssets: assets.logicalAssets,
            assetWorker: worker,
            decodeAtlas,
            textureEncoder: getEncoder(),
            signal: combined,
          }),
        );
        check();
        for (const [name, bytes] of Object.entries(generated.files)) {
          const generatedBytes = own(bytes);
          append(`mercslots/${name}`, generatedBytes);
          if (nativeOverlayGenerated !== null && name === "HirelingInventoryBG.sprite") {
            if (generatedArt[`NativeHUD/mercslots/${name}`])
              fail("ASSET_PIPELINE_GENERATED_COLLISION");
            generatedArt[`NativeHUD/mercslots/${name}`] = generatedBytes;
          } else release(generatedBytes);
        }
      }
      if (remainingArt) {
        const generated = await run(() =>
          generateRemainingArt({
            logicalAssets: assets.logicalAssets,
            assetWorker: worker,
            decodeAtlas,
            textureEncoder: getEncoder(),
            signal: combined,
          }),
        );
        check();
        for (const [name, value] of Object.entries(generated.files)) {
          const data = own(value);
          if (name === "NativeHUD/widestash/VaultBottom.png") {
            release(data);
            continue;
          }
          if (!NATIVE_OVERLAY_GENERATED.includes(name) || generatedArt[name])
            fail("ASSET_PIPELINE_GENERATED_COLLISION");
          generatedArt[name] = data;
        }
      }
      if (recipes.length) {
        const encoder = getEncoder();
        const replacements = [];
        let replacementBytes = 0;
        for (const recipe of recipes) {
          const source = own(
            await run(() =>
              assets.logicalAssets.read(recipe.input, {
                maxBytes: ASSET_PIPELINE_LIMITS.sourceBytes,
                signal: combined,
              }),
            ),
          );
          if (!source.length || source.length > ASSET_PIPELINE_LIMITS.sourceBytes)
            fail("ASSET_PIPELINE_INPUT_LIMIT");
          const digest = new Uint8Array(await run(() => crypto.subtle.digest("SHA-256", source)));
          if (Array.from(digest, (b) => b.toString(16).padStart(2, "0")).join("") !== recipe.sha256)
            fail("ASSET_PIPELINE_SOURCE_HASH");
          const templateHeader = own(source.slice(0, 40));
          const decoded = await run(() => decoder.decodeAtlas(source, { signal: combined }));
          const pixels = own(decoded.pixels);
          if (
            decoded.frameCount !== 1 ||
            decoded.pixelFormat !== 70 ||
            decoded.bytesPerPixel !== 4 ||
            !Number.isSafeInteger(decoded.width) ||
            !Number.isSafeInteger(decoded.height) ||
            decoded.width < 1 ||
            decoded.height < 1 ||
            decoded.width * decoded.height > 4194304 ||
            pixels.length !== decoded.width * decoded.height * 4
          )
            fail("ASSET_PIPELINE_TEXTURE_LAYOUT");
          const transformed = await run(() =>
            recipe.transform({
              pixels,
              width: decoded.width,
              height: decoded.height,
              frameWidth: decoded.frameWidth,
              frameCount: decoded.frameCount,
              signal: combined,
            }),
          );
          const rgba = own(transformed?.rgba);
          validateEncodeRequest(templateHeader, transformed.width, transformed.height);
          if (rgba.length !== transformed.width * transformed.height * 4)
            fail("ASSET_PIPELINE_RGBA");
          const encoded = await run(() =>
            encoder.encodeAndPack(
              { templateHeader, rgba, width: transformed.width, height: transformed.height },
              { signal: combined },
            ),
          );
          const sprite = own(encoded?.sprite);
          if (encoded.linearBC3) release(own(encoded.linearBC3));
          if (
            encoded.width !== transformed.width ||
            encoded.height !== transformed.height ||
            sprite.length < 40 ||
            sprite.length > 16 * 1024 * 1024 ||
            (replacementBytes += sprite.length) > ASSET_PIPELINE_LIMITS.replacementBytes
          )
            fail("ASSET_PIPELINE_ENCODE_RESULT");
          replacements.push({ path: recipe.replacement, add: recipe.add, bytes: sprite });
          release(source);
          release(pixels);
          release(rgba);
          release(templateHeader);
        }
        const overlay = await run(() =>
          writeCascOverlay({ romfs: assets.romfs, replacements, worker, signal: combined }),
        );
        check();
        for (const [name, bytes] of Object.entries(overlay.files))
          append(`romfs_overlay/${name}`, own(bytes));
      }
      if (nativeOverlayGenerated !== null) {
        const overlay = await run(() =>
          generateNativeOverlay({
            recipeRevision: overlayRecipeRevision,
            extracted,
            selection,
            generated: generatedArt,
            prebuiltPack,
            worker,
            recycleWorker: recycleAssetWorker,
            signal: combined,
          }),
        );
        check();
        // 验证只持有同一字节视图的11项映射；不再复制整份生成文件映射或载荷。
        // Validation holds an eleven-entry map of the same byte views, without another generated-file-map or payload copy.
        const generatedFiles = Object.entries(overlay.files).map(([name, value]) => [
          name,
          own(value),
        ]);
        const astcFiles = Object.fromEntries(
          generatedFiles.filter(([name]) => name.startsWith("romfs_overlay_astc")),
        );
        if (prebuiltPack) {
          if (overlay.astc?.count !== prebuiltPack.records.length)
            fail("ASSET_PIPELINE_ASTC_BINDING");
          const checked = validateAstcOutputs({
            files: astcFiles,
            recipeRevision: overlayRecipeRevision,
            pack: overlay.astc.pack,
            overlayArchives: overlay.astc.overlayArchives,
          });
          if (
            checked.pack.name !== prebuiltPack.pack.name ||
            checked.pack.size !== prebuiltPack.pack.size ||
            checked.pack.sha256 !== prebuiltPack.pack.sha256
          )
            fail("ASSET_PIPELINE_ASTC_BINDING");
          pendingAstcBinding = Object.freeze({
            count: overlay.astc.count,
            recipeRevision: overlayRecipeRevision,
            pack: checked.pack,
            overlayArchives: Object.freeze({ ...overlay.astc.overlayArchives }),
          });
        } else if (overlay.astc !== undefined || Object.keys(astcFiles).length)
          fail("ASSET_PIPELINE_ASTC_BINDING");
        for (const [name, value] of generatedFiles) append(name, value, true);
      }
      check();
      closeClients();
      if (cleanupFailed) fail("ASSET_PIPELINE_CLEANUP");
      completedEntries = entries.sort((a, b) => a.path.localeCompare(b.path, "en"));
    } catch (error) {
      astcBinding = undefined;
      wipe();
      closeClients();
      const safe = new Error(
        combined.aborted || disposed
          ? "ASSET_PIPELINE_CANCELLED"
          : error instanceof AssetPipelineError
            ? error.message
            : "ASSET_PIPELINE_FAILED",
      );
      if (cleanupFailed) safe.cleanupCode = "ASSET_PIPELINE_CLEANUP";
      throw safe;
    } finally {
      for (const bytes of owned) bytes.fill(0);
      combined.removeEventListener("abort", abort);
    }
    check();
    astcBinding = pendingAstcBinding;
    return completedEntries;
  }
  return Object.freeze({
    generateAssets,
    getAstcOutputBinding: () => (disposed ? undefined : astcBinding),
    dispose,
    capabilities: Object.freeze({
      completeAssetPipeline: false,
      productionConversion: false,
      authenticatedAssetInputs: true,
      mapGoal,
      bc3OverlayRecipes: recipes.length,
    }),
  });
}
