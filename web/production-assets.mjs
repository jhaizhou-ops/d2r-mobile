// 依据原生资源生成器及提取能力契约组合生产资源阶段。
// Compose production asset stages from native asset generators and extraction capability contracts.
/**
 * 原生资源顺序；所有读取器均来自已认证提取。
 * Native asset ordering; all readers come from authenticated extraction.
 */
import { resolveShellProfile } from "./shell-profiles.mjs";
import { readAuthenticatedAssets } from "./extraction-job-adapter.mjs";
import { generateNativeArt } from "./native-art-generator.mjs";
import { generateHud } from "./hud-generator.mjs";
import { generateProductionDerived } from "./production-derived.mjs";
import {
  createAssetPipelineAdapter,
  snapshotPrebuiltPackMetadata,
} from "./asset-pipeline-adapter.mjs";
import { FRONTEND_PANEL_SOURCE, transformFrontendPanel } from "./frontend-panel-transform.mjs";

// 工作线程传回 ArrayBuffer；只建立字节视图，不复制生成文件。
// Workers return ArrayBuffers; create byte views without copying generated files.
const byteView = (value) => {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  throw new Error("PRODUCTION_ASSET_OUTPUT_TYPE");
};

const failCleanup = () => {
  throw new Error("PRODUCTION_ASSET_CLEANUP");
};

export function createProductionAssetAdapter({
  runtime,
  profile,
  prebuiltPack,
  outputCache,
  cacheContext,
}) {
  if (typeof profile?.profileId !== "string") throw new Error("PRODUCTION_ASSET_PROFILE");
  const { assetRecipe } = resolveShellProfile(profile.profileId);
  const assetCacheContext = Object.freeze({
    runtimeManifestSha256: cacheContext?.runtimeManifestSha256 ?? null,
    profileId: profile.profileId,
    artRevision: assetRecipe.artRevision,
    overlayRevision: assetRecipe.overlayRevision,
  });
  prebuiltPack = snapshotPrebuiltPackMetadata(prebuiltPack);
  let astcBinding;
  const owned = new Set(),
    releaseEntries = new Set();
  let pipeline,
    disposed = false;
  return {
    async generateAssets({ extracted, selection, signal }) {
      if (disposed) throw new Error("PRODUCTION_ASSET_DISPOSED");
      astcBinding = undefined;
      const { logicalAssets } = readAuthenticatedAssets(extracted, selection, { signal });
      let worker, texture, encoder;
      let completed = false,
        pendingAstcBinding,
        generationFailed = false,
        generationError;
      const generated = new Set(),
        emitted = new Set(),
        generationReleases = new Set();
      const own = (value) => {
        const bytes = byteView(value);
        owned.add(bytes);
        generated.add(bytes);
        return bytes;
      };
      const fileViews = (files) => {
        const views = [];
        let invalid = false;
        // 错误批次中的其他有效缓冲区也必须接管并擦除。
        // Adopt valid siblings in a rejected batch before reporting its invalid output type.
        for (const [path, value] of Object.entries(files)) {
          if (!(value instanceof Uint8Array) && !(value instanceof ArrayBuffer)) {
            invalid = true;
            continue;
          }
          views.push([path, own(value)]);
        }
        if (invalid) throw new Error("PRODUCTION_ASSET_OUTPUT_TYPE");
        return Object.fromEntries(views);
      };
      const check = () => {
        signal?.throwIfAborted();
        if (disposed) throw new Error("PRODUCTION_ASSET_DISPOSED");
      };
      const entry = (path, bytes) => {
        bytes = own(bytes);
        emitted.add(bytes);
        const release = () => {
          bytes = undefined;
        };
        releaseEntries.add(release);
        generationReleases.add(release);
        return {
          path,
          size: bytes.length,
          mode: 0o644,
          async *chunks() {
            if (disposed || !bytes) throw new Error("PRODUCTION_ASSET_DISPOSED");
            yield bytes;
          },
        };
      };
      try {
        check();
        worker = runtime.createAssetWorker();
        texture = runtime.createTextureClient();
        encoder = runtime.createTextureEncoder();
        const decodeAtlas = (bytes) => texture.decodeAtlas(bytes, { signal });
        const art = await generateNativeArt({
          logicalAssets,
          assetWorker: worker,
          decodeAtlas,
          metadata: profile.nativeArt,
          metadataSha256: profile.nativeArtSha256,
          signal,
          outputCache,
          cacheContext: assetCacheContext,
        });
        const artFiles = fileViews(art.files);
        check();
        const hud = await generateHud({
          logicalAssets,
          assetWorker: worker,
          decodeAtlas,
          nativeArtFiles: artFiles,
          metadata: profile.hud,
          metadataSha256: profile.hudSha256,
          mode: "switch-upscaled",
          recipeRevision: assetRecipe.artRevision,
          signal,
          outputCache,
          cacheContext: assetCacheContext,
        });
        const hudFiles = fileViews(hud.files);
        check();
        const derived = await generateProductionDerived({
          logicalAssets,
          assetWorker: worker,
          nativeArtFiles: artFiles,
          signal,
        });
        const derivedFiles = fileViews(derived.files ?? derived);
        check();
        for (const [tag, expected] of Object.entries(profile.derivedCounts)) {
          if (derived.counts?.[tag] !== expected)
            throw new Error("SHELL_DERIVED_CAPACITY_MISMATCH");
        }
        let source = await logicalAssets.read(FRONTEND_PANEL_SOURCE.input, {
          maxBytes: 8 * 1024 * 1024,
          signal,
        });
        let decoded, transformed, encoded, panelSprite;
        try {
          decoded = await decodeAtlas(source);
          transformed = transformFrontendPanel({ ...decoded, signal });
          encoded = await encoder.encodeAndPack(
            { templateHeader: source.slice(0, 40), ...transformed },
            { signal },
          );
          panelSprite = own(encoded.sprite);
          check();
        } finally {
          source.fill(0);
          decoded?.pixels.fill(0);
          transformed?.rgba.fill(0);
          encoded?.linearBC3?.fill(0);
          // 擦除后解除临时缓冲区引用，避免后续流水线等待期间保留它们。
          // Drop wiped temporary buffers before awaiting the subsequent pipeline.
          source = decoded = transformed = encoded = undefined;
        }
        const overlayInputs = {
          "NativeHUD/v4/base.png": hudFiles["base.png"],
          "NativeHUD/v4/hud.json": hudFiles["hud.json"],
          "NativeHUD/v4/hud_v4_ui/ui.json": hudFiles["hud_v4_ui/ui.json"],
          "NativeHUD/touch/fe_rightpanel.sprite": panelSprite,
        };
        pipeline = createAssetPipelineAdapter({
          mapGoal: true,
          artRecipeRevision: assetRecipe.artRevision,
          overlayRecipeRevision: assetRecipe.overlayRevision,
          mercenaryArt: true,
          remainingArt: true,
          nativeOverlayGenerated: overlayInputs,
          prebuiltPack,
          outputCache,
          cacheContext: assetCacheContext,
          createAssetWorker: runtime.createAssetWorker,
          recycleAssetWorker: runtime.recycleAssetWorker,
          createTextureClient: runtime.createTextureClient,
          createTextureEncoder: runtime.createTextureEncoder,
        });
        const entries = await pipeline.generateAssets({ extracted, selection, signal });
        for (const [path, bytes] of Object.entries(artFiles)) {
          if (path !== "dm_touch_native_art.c") entries.push(entry(path, bytes));
        }
        for (const [path, bytes] of Object.entries(hudFiles))
          if (path.startsWith("hud_v4_ui/")) entries.push(entry(path, bytes));
        for (const [path, bytes] of Object.entries(derivedFiles)) entries.push(entry(path, bytes));
        entries.push(
          entry("native_art_manifest.json", new TextEncoder().encode(profile.nativeArt.manifest)),
        );
        check();
        pendingAstcBinding = pipeline.getAstcOutputBinding();
        completed = true;
        return entries;
      } catch (error) {
        generationFailed = true;
        generationError = error;
        throw error;
      } finally {
        // 工厂或单个关闭失败也必须释放其他已创建客户端，且不覆盖原始生成错误。
        // Release every created client even if a factory or close fails; preserve the generation error.
        let cleanupFailed = false;
        for (const client of [worker, texture, encoder]) {
          try {
            client?.close();
          } catch {
            cleanupFailed = true;
          }
        }
        // 立即接管每批生成结果；失败时擦除全部，成功时仅保留待打包条目。
        // Own each returned batch before the next await; retain only package entries after success.
        const retained = completed && !cleanupFailed && !disposed && !signal?.aborted;
        for (const bytes of generated) {
          if (retained && emitted.has(bytes)) continue;
          bytes.fill(0);
          owned.delete(bytes);
        }
        if (!retained) {
          for (const release of generationReleases) {
            release();
            releaseEntries.delete(release);
          }
        }
        if (cleanupFailed) {
          if (!generationFailed) failCleanup();
          try {
            if (generationError instanceof Error)
              generationError.cleanupCode = "PRODUCTION_ASSET_CLEANUP";
          } catch {
            // 原始错误可能被冻结或包含只读属性；清理标记不能覆盖它。
            // The primary error may be frozen or read-only; annotation must not replace it.
          }
        }
        if (completed) {
          check();
          astcBinding = pendingAstcBinding;
        }
      }
    },
    getAstcOutputBinding() {
      return disposed ? undefined : astcBinding;
    },
    dispose() {
      disposed = true;
      astcBinding = undefined;
      prebuiltPack = undefined;
      try {
        pipeline?.dispose();
      } finally {
        try {
          for (const bytes of owned) bytes.fill(0);
        } finally {
          owned.clear();
          // 清理时解除输出闭包引用，不改变暂存前可重复读取的行为。
          // Release output closure references at cleanup, preserving repeatable reads before staging.
          for (const release of releaseEntries) release();
          releaseEntries.clear();
        }
      }
    },
  };
}
