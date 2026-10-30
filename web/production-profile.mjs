// 依据仓库中的外壳发布证据、资源来源记录及着色器目录加载可信生产配置。
// Load trusted production profiles from repository shell-release evidence, asset provenance and shader catalogs.
import {
  validateAstcOutputs,
  validateLegacyPackMetadata,
  validateCascArchiveName,
  astcOutputPaths,
  ASTC_OUTPUT_LIMITS,
} from "./astc-output-validator.mjs";
import { RELEASE_OVERLAY_REVISION } from "./toolchain/overlay-recipe-js/revision.mjs";
import { validateShellContract } from "./shell-contract.mjs";
import { resolveShellProfile } from "./shell-profiles.mjs";
import { loadShaderCatalog } from "./shader-catalog-adapter.mjs";
import { createShellLoader } from "./shell-loader.mjs";
import { createShellTargetInspector } from "./shell-target.mjs";
import artPins from "./toolchain/asset-generation-spike/native-art-provenance.json" with { type: "json" };
import hudPins from "./toolchain/asset-generation-spike/hud-provenance.json" with { type: "json" };
const text = (bytes) => new TextDecoder("utf-8", { fatal: true }).decode(bytes);
export async function loadProductionProfile(runtime, { signal, profileId } = {}) {
  const selected = resolveShellProfile(profileId);
  const { evidence, capacities, contentPin, guestTarget, releaseId } = selected;
  if (
    capacities.shellSha256 !== evidence.sha256 ||
    evidence.entries.find((entry) => entry.path === capacities.evidence.executablePath)?.sha256 !==
      capacities.evidence.executableSha256
  )
    throw new Error("SHELL_CAPACITY_BINDING");
  const read = (path) => runtime.readPublicAsset(path);
  let shellBytes = read(selected.archivePath);
  const sha = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", shellBytes)), (x) =>
    x.toString(16).padStart(2, "0"),
  ).join("");
  if (sha !== evidence.sha256 || shellBytes.length !== evidence.archiveBytes)
    throw new Error("PUBLIC_SHELL_INTEGRITY");
  const catalog = await loadShaderCatalog({
    map: read("catalog/shader_map.tsv"),
    facts: read("catalog/shader_facts.tsv"),
    conflicts: read("catalog/shader_map.tsv.conflicts.tsv"),
  });
  const trustedRecipe = {
    schemaVersion: 1,
    product: evidence.shellInfo.PRODUCT,
    chain: evidence.shellInfo.CHAIN,
    kind: "ios",
    shaderDigest: evidence.shellInfo.SHADER_SRC,
    spirvCrossVersion: evidence.shellInfo.SPIRV_CROSS,
    shaderKeys: catalog.keys,
    targetOS: { platform: "ios", minimumVersion: evidence.machOBuildVersions[0].minimumVersion },
    requiredPaths: evidence.requiredPaths,
    extraFiles: ["dm/pack_info.txt", "dm/content_mode.json"],
  };
  const trustedShell = {
    verified: true,
    releaseId,
    trustedRecipe,
    files: evidence.entries,
  };
  const loadShell = createShellLoader({
    source: {
      size: shellBytes.length,
      read: async (offset, length) => {
        if (!shellBytes) throw new Error("PROFILE_CLOSED");
        return shellBytes.slice(offset, offset + length);
      },
    },
    releaseId,
    resolveTrustedRelease: async () => trustedShell,
    inspectTargetOS: createShellTargetInspector({ executablePath: contentPin.executablePath }),
  });
  const shell = await loadShell({ signal });
  function metadata(names, pins) {
    const value = {},
      hashes = {};
    for (const [key, path] of Object.entries(names)) {
      value[key] = text(read("profile/" + path.split("/").at(-1)));
      hashes[key] = pins[path];
    }
    return [value, hashes];
  }
  const [nativeArt, nativeArtSha256] = metadata(
    {
      manifest: "dm/ios/Sources/TouchArt/native_art_manifest.json",
      geometry: "dm/ios/docs/native_art_v1_geometry.json",
      binding: "dm/ios/docs/touch_layout_spec_native_binding_v5.json",
      implementation: "dm/ios/tools/native_art/implementation.c.inc",
    },
    artPins,
  );
  const [hud, hudSha256] = metadata(
    {
      manifest: "dm/ios/Sources/TouchArt/native_art_manifest.json",
      patch: "dm/ios/tools/touchrectv1_patch.json",
      i18n: "dm/i18n/ui_text.tsv",
    },
    hudPins,
  );
  return {
    profileId: selected.id,
    assetRecipe: selected.assetRecipe,
    guestTarget,
    contentPin,
    contentMode: "optional-warlock",
    derivedCounts: capacities.counts,
    catalog,
    trustedShell,
    shell,
    nativeArt,
    nativeArtSha256,
    hud,
    hudSha256,
    close() {
      try {
        shellBytes?.fill(0);
      } finally {
        // 清除闭包中的缓冲区引用；已释放的条目不可再次读取。
        // Release the closure-held buffer; disposed entries cannot be read again.
        shellBytes = undefined;
      }
    },
  };
}

/**
 * 逐块检查真实条目，只保留验证所需的小前缀；不重建完整 ASTC 文件缓冲区。
 * Inspect actual streamed entries, retaining only tiny validation prefixes, never reconstructing whole ASTC file buffers.
 */
export async function inspectProductionAstcEntries(entries, binding, { signal } = {}) {
  signal?.throwIfAborted();
  if (!Array.isArray(entries) || !binding?.overlayArchives)
    throw new Error("PRODUCTION_ASTC_INVENTORY");
  const { recipeRevision = RELEASE_OVERLAY_REVISION } = binding;
  const expected = astcOutputPaths({ overlayArchives: binding.overlayArchives, recipeRevision });
  if (entries.length !== expected.length) throw new Error("PRODUCTION_ASTC_INVENTORY");
  const pack = validateLegacyPackMetadata(binding.pack);
  const overlayArchives = Object.freeze({
    base: validateCascArchiveName(binding.overlayArchives.base),
    item_notes: validateCascArchiveName(binding.overlayArchives.item_notes),
  });
  const files = Object.create(null);
  let totalBytes = 0;
  try {
    for (const entry of entries) {
      signal?.throwIfAborted();
      if (
        !entry ||
        typeof entry.path !== "string" ||
        entry.path.length > 512 ||
        Object.hasOwn(files, entry.path) ||
        !Number.isSafeInteger(entry.size) ||
        entry.size < 0 ||
        entry.size > ASTC_OUTPUT_LIMITS.fileBytes ||
        typeof entry.chunks !== "function" ||
        !Number.isInteger(entry.mode) ||
        entry.mode < 0 ||
        entry.mode > 0xffff ||
        ![0, 0o100000].includes(entry.mode & 0o170000) ||
        (entry.mode & 0o7777) !== 0o644 ||
        (totalBytes += entry.size) > ASTC_OUTPUT_LIMITS.totalBytes
      )
        throw new Error("PRODUCTION_ASTC_ENTRY");
      const metadata = entry.path === "romfs_overlay_astc/pack.json",
        marker = entry.path.endsWith("/xbox_glyphs"),
        manifest = entry.path.endsWith("/overlay.manifest");
      if (metadata && entry.size > ASTC_OUTPUT_LIMITS.packJsonBytes)
        throw new Error("PRODUCTION_ASTC_PACK_JSON");
      if (
        (marker && entry.size > ASTC_OUTPUT_LIMITS.markerBytes) ||
        (manifest && entry.size > ASTC_OUTPUT_LIMITS.manifestBytes)
      )
        throw new Error("PRODUCTION_ASTC_METADATA");
      const prefix = new Uint8Array(
        Math.min(
          entry.size,
          metadata
            ? ASTC_OUTPUT_LIMITS.packJsonBytes
            : marker
              ? ASTC_OUTPUT_LIMITS.markerBytes
              : manifest
                ? ASTC_OUTPUT_LIMITS.manifestBytes
                : entry.path.endsWith("/features.txt")
                  ? 11
                  : 1,
        ),
      );
      files[entry.path] = prefix;
      let count = 0;
      for await (const chunk of entry.chunks()) {
        signal?.throwIfAborted();
        if (
          !(chunk instanceof Uint8Array) ||
          !(chunk.buffer instanceof ArrayBuffer) ||
          chunk.buffer.resizable ||
          chunk.buffer.byteLength > 1024 * 1024 ||
          chunk.length > entry.size - count
        )
          throw new Error("PRODUCTION_ASTC_CHUNK");
        if (count < prefix.length)
          prefix.set(chunk.subarray(0, Math.min(chunk.length, prefix.length - count)), count);
        count += chunk.length;
      }
      signal?.throwIfAborted();
      if (count !== entry.size) throw new Error("PRODUCTION_ASTC_SIZE");
    }
    // 验证器只检查这些内容前缀；真实大小和总预算已在上面独立验证。
    // The validator examines only these content prefixes; actual sizes and total budgets were independently checked above.
    const validation = validateAstcOutputs({ files, pack, overlayArchives, recipeRevision });
    return Object.freeze({ ...validation, totalBytes, overlayArchives });
  } finally {
    for (const bytes of Object.values(files)) bytes.fill(0);
  }
}

/**
 * 生成完成后才扩展外壳契约，只加入与封闭配方匹配并通过清单、流边界和结构检查的精确文件名；这不是归档内容认证，原配置保持不变。
 * Extend the shell contract only after exact closed-recipe inventory, stream and structure checks; this does not authenticate archive contents and leaves the original profile unchanged.
 */
export async function bindProductionAstcEntries(profile, entries, binding, options) {
  if (profile.astcOutput) throw new Error("PRODUCTION_ASTC_ALREADY_BOUND");
  const expectedRevision =
    profile.profileId === undefined
      ? RELEASE_OVERLAY_REVISION
      : resolveShellProfile(profile.profileId).assetRecipe.overlayRevision;
  const recipeRevision =
    binding?.recipeRevision === undefined ? RELEASE_OVERLAY_REVISION : binding.recipeRevision;
  if (recipeRevision !== expectedRevision) throw new Error("PRODUCTION_ASTC_RECIPE_BINDING");
  const astcOutput = await inspectProductionAstcEntries(entries, binding, options);
  const trustedRecipe = Object.freeze({
    ...profile.trustedShell.trustedRecipe,
    extraFiles: Object.freeze([
      ...profile.trustedShell.trustedRecipe.extraFiles,
      ...astcOutput.paths,
    ]),
  });
  const contract = Object.freeze({ ...profile.shell.contract, trustedRecipe });
  validateShellContract(contract);
  return {
    ...profile,
    astcOutput,
    trustedShell: Object.freeze({ ...profile.trustedShell, trustedRecipe }),
    shell: Object.freeze({ ...profile.shell, contract }),
  };
}
