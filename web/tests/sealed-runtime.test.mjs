// Feature suite: sealed-runtime. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as runtimeAssetGroupsModule from "../runtime-asset-groups.mjs";
import * as promisesModule from "node:fs/promises";
import * as nodeOsModule from "node:os";
import * as nodePathModule from "node:path";
import * as nodeChild_processModule from "node:child_process";
import * as nodeUrlModule from "node:url";
import * as typescriptModule from "typescript";
import * as payloadPolicyModule from "../runtime-build/payload-policy.mjs";
import * as airCompilerRuntimeContractModule from "../air-compiler-runtime-contract.mjs";
import * as clientsModule from "../sealed-runtime/clients.mjs";
import * as shellProfilesModule from "../shell-profiles.mjs";
import * as packageModule from "../runtime-build/package.mjs";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as nodeBufferModule from "node:buffer";
import * as nodeCryptoModule from "node:crypto";
import * as nodeZlibModule from "node:zlib";
import * as transportModule from "../runtime-build/transport.mjs";
import * as nodeFsModule from "node:fs";
import * as nodeVmModule from "node:vm";
import * as shaderDiagnosticsModule from "../shader-diagnostics.mjs";
import * as nodeProcessModule from "node:process";

// runtime-asset-groups
{
  // 用合成公开记录验证可选编解码器闭包与内容归属；不构建或下载编解码器。
  // Verify optional codec closure and content ownership with synthetic public records; no codec builds or downloads.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const {
    OPTIONAL_RUNTIME_ASSET_GROUPS,
    runtimeContentKey,
    selectRuntimeAssetGroups,
    validateRuntimeAssetGroups,
    validateRuntimeGroupSelection,
    validateRuntimeTransportGroups,
  } = runtimeAssetGroupsModule;

  const common = { path: "common.mjs", bytes: 20, sha256: "a".repeat(64) };
  const optional = () =>
    OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"].map((path, i) => ({
      path,
      optionalGroup: "legacy-astc",
      bytes: i + 1,
      sha256: String(i).repeat(64),
    }));

  test("unmarked existing assets remain common and optional selection defaults off", () => {
    assert.deepEqual(selectRuntimeAssetGroups([common]), [common]);
    const assets = [common, ...optional()],
      before = structuredClone(assets);
    assert.deepEqual(selectRuntimeAssetGroups(assets), [common]);
    assert.deepEqual(selectRuntimeAssetGroups(assets, ["legacy-astc"]), assets);
    assert.deepEqual(assets, before);
    assert.equal(validateRuntimeAssetGroups(assets).get(runtimeContentKey(common)), "");
  });

  test("only complete exact compiled codec paths can be marked optional", () => {
    for (const mutate of [
      (assets) => assets.pop(),
      (assets) => assets.push({ ...assets[0] }),
      (assets) => delete assets[0].optionalGroup,
      (assets) => (assets[0].optionalGroup = "unknown"),
      (assets) => (assets[0].optionalGroup = null),
      (assets) => (assets[0].optionalGroup = ""),
      (assets) => (assets[0].path = "../escape"),
      (assets) => (assets[0].path = "toolchain/asset-runtime-js/dist/png-codec.wasm"),
      (assets) => assets.push({ ...common, optionalGroup: "legacy-astc" }),
      (assets) =>
        assets.push({ ...common, path: "profile/shell.zip", optionalGroup: "legacy-astc" }),
    ]) {
      const assets = optional();
      mutate(assets);
      assert.throws(() => validateRuntimeAssetGroups(assets), /optional runtime asset/);
    }
  });

  test("unknown, malformed, duplicate and unavailable requested groups reject", () => {
    for (const selection of [
      null,
      "legacy-astc",
      {},
      [null],
      [""],
      ["__proto__"],
      ["unknown"],
      ["legacy-astc", "legacy-astc"],
    ])
      assert.throws(() => selectRuntimeAssetGroups(optional(), selection), /group selection/);
    assert.throws(() => selectRuntimeAssetGroups([common], ["legacy-astc"]), /unavailable/);
  });

  test("trusted optional selection is snapshotted before asynchronous setup", () => {
    const requested = ["legacy-astc"],
      selected = validateRuntimeGroupSelection(requested);
    requested.length = 0;
    assert.deepEqual(selected, ["legacy-astc"]);
    assert.ok(Object.isFrozen(selected));
  });

  test("shared common content wins independently of asset order", () => {
    const assets = optional(),
      alias = { ...assets[2], path: "common/license.txt" };
    delete alias.optionalGroup;
    for (const inventory of [
      [alias, ...assets],
      [...assets, alias],
    ]) {
      const ownership = validateRuntimeAssetGroups(inventory);
      assert.equal(ownership.size, assets.length);
      assert.equal(ownership.get(runtimeContentKey(alias)), "");
      assert.equal(ownership.get(runtimeContentKey(assets[0])), "legacy-astc");
    }
  });

  test("mixed physical payloads reject, including when optional groups are not selected", () => {
    const assets = [common, ...optional()],
      entries = assets.map((asset, i) => ({ ...asset, file: `payloads/${i}.bin`, offset: 0 }));
    validateRuntimeTransportGroups(assets, entries);
    entries[1].file = entries[0].file;
    assert.throws(() => validateRuntimeTransportGroups(assets, entries), /payload is shared/);
    entries[1] = { ...entries[1], sha256: "f".repeat(64) };
    assert.throws(() => validateRuntimeTransportGroups(assets, entries), /Unknown/);
  });

  test("signing and legacy texture closures select independently and together", () => {
    const signing = OPTIONAL_RUNTIME_ASSET_GROUPS["ipa-signing"].map((path, i) => ({
      path,
      optionalGroup: "ipa-signing",
      bytes: i + 100,
      sha256: "b".repeat(64),
    }));
    const legacy = optional();
    const assets = [common, ...legacy, ...signing];
    assert.deepEqual(selectRuntimeAssetGroups(assets), [common]);
    assert.deepEqual(selectRuntimeAssetGroups(assets, ["ipa-signing"]), [common, ...signing]);
    assert.deepEqual(selectRuntimeAssetGroups(assets, ["legacy-astc"]), [common, ...legacy]);
    assert.deepEqual(selectRuntimeAssetGroups(assets, ["legacy-astc", "ipa-signing"]), assets);
    assert.throws(
      () => selectRuntimeAssetGroups([common, ...legacy, ...signing.slice(1)]),
      /Incomplete/,
    );
    assert.throws(() => selectRuntimeAssetGroups([common], ["ipa-signing"]), /unavailable/);
  });
}

// runtime-build-determinism
{
  // 用模拟工具验证可复现元数据输入，不执行真正的 Git 操作或编译器构建。
  // Verify reproducible metadata inputs with mock tools, without real Git operations or compiler builds.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { mkdtemp, mkdir, readFile, rm, writeFile } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { spawnSync } = nodeChild_processModule;
  const { fileURLToPath } = nodeUrlModule;

  const source = (path) => readFile(new URL(path, import.meta.url), "utf8");
  const commit = "f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7";

  async function mockBuild(t, epoch) {
    const directory = await mkdtemp(join(tmpdir(), "d2r-build-metadata-"));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const bin = join(directory, "bin"),
      upstream = join(directory, "source");
    await mkdir(bin);
    await mkdir(upstream);
    await writeFile(
      join(bin, "git"),
      `#!/bin/sh
case "$3 $4" in
  'rev-parse HEAD') printf '%s\\n' '${commit}' ;;
  'show -s') printf '%s\\n' "$D2R_MOCK_EPOCH" ;;
  *) exit 99 ;;
esac
`,
      { mode: 0o755 },
    );
    const capture = join(directory, "calls.jsonl");
    await writeFile(
      join(bin, "cmake"),
      `#!${process.execPath}
require('node:fs').appendFileSync(process.env.D2R_CAPTURE, JSON.stringify({args:process.argv.slice(2),epoch:process.env.SOURCE_DATE_EPOCH,tz:process.env.TZ})+'\\n');
`,
      { mode: 0o755 },
    );
    const result = spawnSync(
      "bash",
      [
        fileURLToPath(new URL("../toolchain/spirv-cross-spike/build.sh", import.meta.url)),
        "native",
        upstream,
        join(directory, "build"),
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          PATH: `${bin}:${process.env.PATH}`,
          SOURCE_DATE_EPOCH: "1",
          TZ: "Asia/Shanghai",
          D2R_MOCK_EPOCH: epoch,
          D2R_CAPTURE: capture,
        },
      },
    );
    const calls = await readFile(capture, "utf8").then(
      (text) =>
        text
          .trim()
          .split("\n")
          .map((line) => JSON.parse(line)),
      (error) => {
        if (error.code === "ENOENT") return [];
        throw error;
      },
    );
    return { result, calls };
  }

  test("verified upstream epoch and UTC override ambient build metadata", async (t) => {
    const { result, calls } = await mockBuild(t, "1700000000");
    assert.equal(result.status, 0, result.stderr);
    assert.equal(calls.length, 2);
    assert.equal(calls[0].args[0], "-S");
    assert.equal(calls[1].args[0], "--build");
    for (const call of calls) {
      assert.equal(call.epoch, "1700000000");
      assert.equal(call.tz, "UTC");
    }
  });

  test("invalid upstream epoch fails before configure or build", async (t) => {
    const { result, calls } = await mockBuild(t, "not-an-epoch");
    assert.equal(result.status, 1);
    assert.match(result.stderr, /Invalid upstream commit epoch/);
    assert.deepEqual(calls, []);
  });

  test("project-only revision opt-outs retain portable debug symbols and external provenance", async () => {
    const [project, build] = await Promise.all([
      source("../toolchain/shader-spike/ShaderSpike.csproj"),
      source("../runtime-build/build.mjs"),
    ]);
    assert.match(
      project,
      /<IncludeSourceRevisionInInformationalVersion>false<\/IncludeSourceRevisionInInformationalVersion>/,
    );
    assert.match(project, /<EnableSourceLink>false<\/EnableSourceLink>/);
    assert.match(project, /<DebugType>portable<\/DebugType>/);
    assert.match(project, /<DebugSymbols>true<\/DebugSymbols>/);
    assert.doesNotMatch(project, /<(?:EnableSourceControlManagerQueries|SourceRevisionId)>/);
    for (const field of ["sourceRevision", "sourceDirty", "crossCommit", "emscripten", "dotnet"])
      assert.match(build, new RegExp(`\\b${field}(?:\\s*:|,)`));
  });
}

// runtime-payload-policy
{
  // 验证已审计源码副本不会再次进入传输，同时保留真实字节加载入口和构建内的前端依赖。
  // Keep audited source copies out of transport while retaining byte-loader entries and frontend build dependencies.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: ts } = typescriptModule;
  const {
    RUNTIME_DIRECT_ASSETS,
    RUNTIME_SEALED_WORKERS,
    SOURCE_ONLY_RUNTIME_ASSETS,
    assertNoSourceOnlyRuntimeAsset,
  } = payloadPolicyModule;
  const { AIR_COMPILER_RUNTIME } = airCompilerRuntimeContractModule;
  const { OPTIONAL_RUNTIME_ASSET_GROUPS } = runtimeAssetGroupsModule;
  const source = (path) => readFile(new URL(path, import.meta.url), "utf8");

  // 检查真实静态值导入，不要求空格、引号样式或本地别名一致。
  // Check real static value imports without requiring whitespace, quote style or local aliases.
  function hasStaticValueImport(text, module, imported) {
    const file = ts.createSourceFile(
      "source.tsx",
      text,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.TSX,
    );
    return file.statements.some((statement) => {
      if (
        !ts.isImportDeclaration(statement) ||
        !ts.isStringLiteral(statement.moduleSpecifier) ||
        statement.moduleSpecifier.text !== module
      )
        return false;
      const clause = statement.importClause;
      if (
        !clause ||
        clause.isTypeOnly ||
        !clause.namedBindings ||
        !ts.isNamedImports(clause.namedBindings)
      )
        return false;
      return clause.namedBindings.elements.some(
        (binding) =>
          !binding.isTypeOnly && (binding.propertyName ?? binding.name).text === imported,
      );
    });
  }

  test("static import checks accept equivalent formatting but not comments, types or dynamic imports", () => {
    for (const text of [
      'import { prepareBrowserPackager } from "../browser-packager.mjs";',
      "import{prepareBrowserPackager as prepare}from'../browser-packager.mjs';",
      "import {\n /* value import */ prepareBrowserPackager,\n} from '../browser-packager.mjs'",
    ])
      assert.ok(hasStaticValueImport(text, "../browser-packager.mjs", "prepareBrowserPackager"));
    for (const text of [
      '// import { prepareBrowserPackager } from "../browser-packager.mjs";',
      'import type { prepareBrowserPackager } from "../browser-packager.mjs";',
      'import { type prepareBrowserPackager } from "../browser-packager.mjs";',
      'const { prepareBrowserPackager } = await import("../browser-packager.mjs");',
      'import { anotherValue } from "../browser-packager.mjs";',
      'import { prepareBrowserPackager } from "../other.mjs";',
    ])
      assert.equal(
        hasStaticValueImport(text, "../browser-packager.mjs", "prepareBrowserPackager"),
        false,
      );
  });

  test("only the 89 audited source-only assets are rejected; live assets and proof records remain", () => {
    assert.equal(SOURCE_ONLY_RUNTIME_ASSETS.length, 89);
    assert.equal(new Set(SOURCE_ONLY_RUNTIME_ASSETS).size, 89);
    assert.ok(Object.isFrozen(SOURCE_ONLY_RUNTIME_ASSETS));
    for (const path of SOURCE_ONLY_RUNTIME_ASSETS)
      assert.throws(() => assertNoSourceOnlyRuntimeAsset(path), /Source-only module/);
    for (const path of [
      ...RUNTIME_DIRECT_ASSETS,
      ...RUNTIME_SEALED_WORKERS,
      ...OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"],
      "toolchain/offline-runtime-spike/dist/worker.mjs",
      "toolchain/offline-runtime-spike/dist/texture.mjs",
      "toolchain/offline-runtime-spike/dist/browser-texture.mjs",
      "toolchain/offline-runtime-spike/dist/manifest.json",
      "toolchain/offline-runtime-spike/dotnet-dist/worker.mjs",
      "toolchain/offline-runtime-spike/dotnet-dist/manifest.json",
      "toolchain/asset-runtime-js/dist/licenses/hash-wasm-LICENSE.txt",
      "toolchain/shell-release-evidence/v2.0.0.json",
      "toolchain/shell-release-evidence/v2.0.0-ios18.json",
      "toolchain/shell-release-evidence/v2.0.0-derived-capacities.json",
      "toolchain/shell-release-evidence/v2.0.0-ios18-derived-capacities.json",
      "toolchain/program-rewrite-spike/dist/toolchain/shell-release-evidence/v2.0.0.json",
      "toolchain/program-rewrite-spike/dist/toolchain/shell-release-evidence/v2.0.0-ios18.json",
      "toolchain/program-rewrite-spike/dist/toolchain/shell-release-evidence/v2.0.0-derived-capacities.json",
      "toolchain/program-rewrite-spike/dist/toolchain/shell-release-evidence/v2.0.0-ios18-derived-capacities.json",
    ])
      assert.doesNotThrow(() => assertNoSourceOnlyRuntimeAsset(path));
  });

  test("the fixed added CI shaders cannot enter the public runtime payload", () => {
    for (const fixture of [
      "toolchain/metal-air-compiler/fixtures/msl-scalar-operations.metal",
      "toolchain/metal-air-compiler/fixtures/msl-fragment-atomic-add.metal",
      "toolchain/metal-air-compiler/fixtures/msl-internal-helper-function-constant.metal",
      "toolchain/metal-air-compiler/fixtures/msl-optimizer-bit-reversal.metal",
      "toolchain/metal-air-compiler/fixtures/msl-patch-entry-name.metal",
      "toolchain/metal-air-compiler/fixtures/constant-vector-pointer-reads.metal",
      "toolchain/metal-air-compiler/fixtures/direct-buffer-vector-pointer.metal",
      "toolchain/metal-air-compiler/fixtures/conditional-distinct-buffer-rows.metal",
      "toolchain/metal-air-compiler/fixtures/loop-carried-buffer-pointer.metal",
      "toolchain/metal-air-compiler/fixtures/retyped-buffer-pointer-load.metal",
      "toolchain/metal-air-compiler/fixtures/metal-constant-pointer-slots.ll",
      "toolchain/metal-air-compiler/fixtures/metal-canonical-gep-recasts.ll",
      "toolchain/metal-air-compiler/fixtures/scalar-i1-conversions.ll",
      "toolchain/metal-air-compiler/fixtures/scalar-i1-conversions.expected.ll",
      "toolchain/metal-air-compiler/fixtures/scalar-i1-conversions.json",
      "toolchain/metal-air-compiler/fixtures/msl-stage-record-threadgroup.metal",
      "toolchain/metal-air-compiler/fixtures/msl-stage-record-buffers.metal",
      "toolchain/metal-air-compiler/fixtures/msl-buffer-reflection-mixed.metal",
      "toolchain/metal-air-compiler/fixtures/msl-buffer-reflection-array.metal",
      "toolchain/metal-air-compiler/fixtures/msl-buffer-reflection-float3.metal",
      "toolchain/metal-air-compiler/fixtures/msl-buffer-reflection-nested.metal",
      "toolchain/metal-air-compiler/fixtures/msl-fragment-input-liveness.metal",
      "toolchain/metal-air-compiler/check-fragment-input-reflection.mjs",
    ]) {
      assert(SOURCE_ONLY_RUNTIME_ASSETS.includes(fixture));
      assert(!RUNTIME_DIRECT_ASSETS.includes(fixture));
      assert(!RUNTIME_SEALED_WORKERS.includes(fixture));
      assert.throws(() => assertNoSourceOnlyRuntimeAsset(fixture), /Source-only module/);
    }
  });

  test("frontend imports the shared client at build time, not through runtime byte loading", async () => {
    const [app, packager, build, loader, assembly] = await Promise.all([
      source("../src/App.tsx"),
      source("../browser-packager.mjs"),
      source("../scripts/build.mjs"),
      source("../sealed-runtime/clients.mjs"),
      source("../runtime-build/package.mjs"),
    ]);
    assert.ok(hasStaticValueImport(app, "../browser-packager.mjs", "prepareBrowserPackager"));
    assert.ok(
      hasStaticValueImport(packager, "./sealed-runtime/clients.mjs", "prepareSealedRuntime"),
    );
    assert.match(build, /entrypoints: \[resolve\(root, "index\.html"\)\]/);
    assert.doesNotMatch(build, /\bexternal\s*:/);
    for (const path of SOURCE_ONLY_RUNTIME_ASSETS) assert.ok(!loader.includes(`"${path}"`));
    assert.match(assembly, /assertNoSourceOnlyRuntimeAsset\(path\)/);
    assert.match(assembly, /for \(const path of RUNTIME_DIRECT_ASSETS\) await copy\(path\)/);
    assert.match(assembly, /await sealed\(`\$\{offline\}\/dist`, \{ bootstrap: false \}\)/);
    assert.match(assembly, /await sealed\(`\$\{offline\}\/dotnet-dist`, \{ bootstrap: false \}\)/);
    for (const path of RUNTIME_SEALED_WORKERS) {
      if (path === AIR_COMPILER_RUNTIME.entry) {
        assert.ok(
          hasStaticValueImport(
            loader,
            "../air-compiler-runtime-contract.mjs",
            "validateAirCompilerRuntimeComponent",
          ),
        );
        assert.match(loader, /validateAirCompilerRuntimeComponent\(manifest/);
        assert.match(assembly, /airCompilerRuntimeComponent\(airCompilerReport\)/);
        continue;
      }
      assert.ok(loader.includes(`"${path}"`));
      assert.ok(assembly.includes(`entry: "${path}"`));
    }
    assert.match(
      assembly,
      /translator: \{\s+sealedEntry: "sealed-runtime\/dotnet-worker\.mjs",\s+entry: "sealed-runtime\/dotnet-worker\.mjs"/,
    );
  });

  // 其他输入模块属于已打包的前端/线程闭包，不属于原始公开载荷。
  // Alternate input modules belong to bundled frontend/worker closures, not raw public payloads.
  test("alternate input source stays bundled and synthetic smoke server has complete imports", async () => {
    for (const path of [
      "game-input.mjs",
      "extracted-input.mjs",
      "package-input.mjs",
      "xci-input.mjs",
    ]) {
      assert(SOURCE_ONLY_RUNTIME_ASSETS.includes(path));
      assert.throws(() => assertNoSourceOnlyRuntimeAsset(path), /Source-only module/);
    }
    const smoke = await source("../toolchain/nsp-extract-spike/browser-smoke.mjs");
    for (const path of ["package-input.mjs", "xci-input.mjs", "extraction-errors.mjs"])
      assert(smoke.includes(`"/${path}"`));
    const sharedSource = smoke.slice(
      smoke.indexOf("const shared = ["),
      smoke.indexOf("].includes(pathname)"),
    );
    const shared = new Set(
      [...sharedSource.matchAll(/"\/([^"/]+\.mjs)"/g)].map((match) => match[1]),
    );
    const webRoot = new URL("../", import.meta.url);
    for (const path of shared) {
      const url = new URL(path, webRoot);
      const module = ts.createSourceFile(
        path,
        await readFile(url, "utf8"),
        ts.ScriptTarget.Latest,
        true,
        ts.ScriptKind.JS,
      );
      for (const statement of module.statements) {
        if (!ts.isImportDeclaration(statement) || !ts.isStringLiteral(statement.moduleSpecifier))
          continue;
        const specifier = statement.moduleSpecifier.text;
        if (!specifier.startsWith(".")) continue;
        const dependency = new URL(specifier, url).pathname.slice(webRoot.pathname.length);
        assert(shared.has(dependency), `${path} imports unmapped shared route ${dependency}`);
      }
    }
  });
}

// runtime-profile-selection
{
  // 以合成清单验证封闭外壳选择，不下载外壳、不读取游戏或密钥。
  // Verify closed shell selection using synthetic manifests, without shell downloads, game data or keys.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { validateRuntimeProfileSelection, prepareSealedRuntime } = clientsModule;
  const { resolveShellProfile, SHELL_PROFILE_IDS, BUNDLED_SHELL_PROFILE_IDS } = shellProfilesModule;
  const { OPTIONAL_RUNTIME_ASSET_GROUPS } = runtimeAssetGroupsModule;

  const common = ["assets", "texture", "translator", "extraction", "program", "spirvCross"].map(
    (name, i) => ({ path: `workers/${name}.mjs`, bytes: 32, sha256: String(i + 1).repeat(64) }),
  );
  function fixture({
    profileIds = BUNDLED_SHELL_PROFILE_IDS,
    packed = true,
    optional = false,
    shared = false,
  } = {}) {
    const profiles = profileIds.map(resolveShellProfile);
    const assets = [
      ...common.map((asset) => ({ ...asset })),
      ...profiles.map((profile) => ({
        path: profile.archivePath,
        bytes: profile.evidence.archiveBytes,
        sha256: profile.evidence.sha256,
      })),
    ];
    if (optional)
      assets.push(
        ...OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"].map((path, i) => ({
          path,
          optionalGroup: "legacy-astc",
          bytes: i + 1,
          sha256: (i + 10).toString(16).padStart(64, "0"),
        })),
      );
    if (shared) {
      const alias = { ...assets.at(-1), path: "common/sha-license.txt" };
      delete alias.optionalGroup;
      assets.push(alias);
    }
    const manifest = {
      version: 1,
      assets,
      totalBytes: assets.reduce((n, asset) => n + asset.bytes, 0),
    };
    if (packed) {
      const contents = [
        ...new Map(assets.map((asset) => [`${asset.sha256}-${asset.bytes}`, asset])).values(),
      ];
      manifest.transport = {
        version: 1,
        totalBytes: contents.reduce((n, asset) => n + asset.bytes, 0),
        files: contents.map((asset, i) => ({
          path: `payloads/${i}.bin`,
          bytes: asset.bytes,
          sha256: asset.sha256,
          encoding: "identity",
          decodedBytes: asset.bytes,
        })),
        entries: contents.map((asset, i) => ({
          sha256: asset.sha256,
          bytes: asset.bytes,
          file: `payloads/${i}.bin`,
          offset: 0,
        })),
      };
    }
    return manifest;
  }

  test("both bundled profiles retain every common asset and download exactly their own shell", () => {
    for (const packed of [true, false]) {
      for (const profileId of [undefined, ...BUNDLED_SHELL_PROFILE_IDS]) {
        const manifest = fixture({ packed });
        const before = structuredClone(manifest);
        const selected = resolveShellProfile(profileId);
        const result = validateRuntimeProfileSelection(manifest, profileId);
        assert.deepEqual(
          result.selectedInventory.map((asset) => asset.path),
          [...common.map((asset) => asset.path), selected.archivePath],
        );
        assert.equal(result.inventory.length, manifest.assets.length);
        assert.equal(result.transport.files.length, manifest.assets.length);
        assert.equal(result.downloadFiles.length, common.length + 1);
        assert.equal(result.downloadBytes, 32 * common.length + selected.evidence.archiveBytes);
        assert.deepEqual(manifest, before);
      }
    }
  });

  test("default-only inventories remain unchanged and absent profiles fail closed", () => {
    for (const packed of [true, false]) {
      const manifest = fixture({ profileIds: BUNDLED_SHELL_PROFILE_IDS.slice(0, 1), packed });
      const result = validateRuntimeProfileSelection(manifest);
      assert.deepEqual(result.selectedInventory, manifest.assets);
      assert.deepEqual(result.downloadFiles, manifest.transport?.files ?? manifest.assets);
      assert.equal(result.downloadBytes, manifest.totalBytes);
      for (const id of SHELL_PROFILE_IDS.filter((id) => id !== resolveShellProfile().id))
        assert.throws(() => validateRuntimeProfileSelection(manifest, id), /unavailable/);
    }
  });

  test("the runtime carries only the v2 pair and rejects retired profile IDs", () => {
    const legacyIds = ["v1.0.18-ios26", "v1.0.18-ios18"];
    assert.deepEqual(BUNDLED_SHELL_PROFILE_IDS, ["v2.0.0-ios26", "v2.0.0-ios18"]);
    for (const packed of [false, true]) {
      const bundled = fixture({ packed });
      assert.deepEqual(
        bundled.assets
          .filter((asset) => asset.path.startsWith("profile/"))
          .map((asset) => asset.path),
        ["profile/shell.zip", "profile/shell-ios18.zip"],
      );
      for (const profileId of legacyIds)
        assert.throws(
          () => validateRuntimeProfileSelection(bundled, profileId),
          /SHELL_PROFILE_UNKNOWN/,
        );
      const explicit = fixture({ packed, profileIds: SHELL_PROFILE_IDS });
      for (const profileId of SHELL_PROFILE_IDS) {
        const selected = validateRuntimeProfileSelection(explicit, profileId);
        assert.deepEqual(
          selected.selectedInventory.map((asset) => asset.path),
          [...common.map((asset) => asset.path), resolveShellProfile(profileId).archivePath],
        );
        assert.equal(selected.downloadFiles.length, common.length + 1);
      }
    }
  });

  test("runtime without current archives never falls back and rejects retired profiles", () => {
    for (const packed of [false, true]) {
      const manifest = fixture({ packed, profileIds: [] });
      for (const profileId of [undefined, ...BUNDLED_SHELL_PROFILE_IDS])
        assert.throws(() => validateRuntimeProfileSelection(manifest, profileId), /unavailable/);
      for (const profileId of ["v1.0.18-ios26", "v1.0.18-ios18"])
        assert.throws(
          () => validateRuntimeProfileSelection(manifest, profileId),
          /SHELL_PROFILE_UNKNOWN/,
        );
    }
  });

  test("mismatched shell identities at current paths fail integrity instead of being relabeled", () => {
    for (const profileId of [undefined, ...BUNDLED_SHELL_PROFILE_IDS]) {
      for (const current of BUNDLED_SHELL_PROFILE_IDS) {
        const manifest = fixture({ packed: false });
        const record = manifest.assets.find(
          (asset) => asset.path === resolveShellProfile(current).archivePath,
        );
        manifest.totalBytes += 1;
        record.bytes += 1;
        record.sha256 = "f".repeat(64);
        assert.throws(
          () => validateRuntimeProfileSelection(manifest, profileId),
          /shell profile integrity/,
        );
      }
    }
  });

  test("unknown profile IDs reject before browser capability or network work", async () => {
    for (const profileId of [null, "", "ios18", {}, "__proto__"]) {
      assert.throws(
        () => validateRuntimeProfileSelection(fixture(), profileId),
        /SHELL_PROFILE_UNKNOWN/,
      );
      await assert.rejects(prepareSealedRuntime({ profileId }), /SHELL_PROFILE_UNKNOWN/);
    }
  });

  test("selected and omitted shell records must match their closed registry", () => {
    for (const profileId of SHELL_PROFILE_IDS) {
      for (const changedId of SHELL_PROFILE_IDS) {
        for (const field of ["sha256", "bytes"]) {
          const manifest = fixture({ packed: false, profileIds: SHELL_PROFILE_IDS });
          const record = manifest.assets.find(
            (asset) => asset.path === resolveShellProfile(changedId).archivePath,
          );
          if (field === "sha256") record.sha256 = "a".repeat(64);
          else {
            record.bytes++;
            manifest.totalBytes++;
          }
          assert.throws(
            () => validateRuntimeProfileSelection(manifest, profileId),
            /shell profile integrity/,
          );
        }
      }
    }
  });

  test("full transport and inventory validation includes the omitted shell", () => {
    for (const mutate of [
      (manifest) => {
        manifest.transport.entries.at(-1).offset = 1;
      },
      (manifest) => {
        manifest.transport.entries.pop();
      },
      (manifest) => {
        manifest.transport.files.at(-1).decodedBytes++;
      },
      (manifest) => {
        manifest.transport.totalBytes--;
      },
      (manifest) => {
        manifest.totalBytes--;
      },
    ]) {
      const manifest = fixture();
      mutate(manifest);
      assert.throws(() => validateRuntimeProfileSelection(manifest));
    }
  });

  test("an omitted shell cannot share a physical payload or logical content with common assets", () => {
    const manifest = fixture();
    const files = manifest.transport.files;
    const entries = manifest.transport.entries;
    const first = files[0],
      last = files.at(-1);
    entries.at(-1).file = first.path;
    entries.at(-1).offset = first.bytes;
    first.bytes += last.bytes;
    first.decodedBytes += last.decodedBytes;
    files.pop();
    assert.throws(() => validateRuntimeProfileSelection(manifest), /payload is shared/);

    const alias = fixture();
    const shell = alias.assets.at(-1);
    alias.assets.push({ ...shell, path: "common/required.bin" });
    alias.totalBytes += shell.bytes;
    assert.throws(() => validateRuntimeProfileSelection(alias), /payload is shared/);
  });

  test("packed progress reports selected wire bytes, not decoded logical bytes", () => {
    const manifest = fixture();
    const shell = manifest.transport.files.at(-2);
    manifest.transport.totalBytes -= shell.bytes - 100;
    shell.bytes = 100;
    shell.encoding = "gzip";
    const result = validateRuntimeProfileSelection(manifest);
    assert.equal(result.downloadBytes, common.length * 32 + 100);
    assert.ok(result.downloadBytes < manifest.totalBytes);
  });

  test("both bundled shell selections omit optional codec downloads unless explicitly selected", () => {
    for (const packed of [false, true])
      for (const profileId of BUNDLED_SHELL_PROFILE_IDS) {
        const manifest = fixture({ optional: true, packed }),
          baseline = validateRuntimeProfileSelection(fixture({ packed }), profileId),
          before = structuredClone(manifest),
          defaultSelection = validateRuntimeProfileSelection(manifest, profileId),
          enabled = validateRuntimeProfileSelection(manifest, profileId, {
            optionalGroups: ["legacy-astc"],
          });
        assert.deepEqual(defaultSelection.downloadFiles, baseline.downloadFiles);
        assert.equal(defaultSelection.downloadBytes, baseline.downloadBytes);
        assert.deepEqual(defaultSelection.selectedInventory, baseline.selectedInventory);
        assert.equal(
          enabled.selectedInventory.length,
          baseline.selectedInventory.length + OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"].length,
        );
        assert.equal(
          enabled.selectedInventory.filter((asset) => asset.path.startsWith("profile/shell"))
            .length,
          1,
        );
        assert.ok(enabled.downloadBytes > defaultSelection.downloadBytes);
        assert.deepEqual(manifest, before);
      }
  });

  test("optional shared license content stays common without duplicate downloads", () => {
    const manifest = fixture({ optional: true, shared: true }),
      selected = validateRuntimeProfileSelection(manifest),
      enabled = validateRuntimeProfileSelection(manifest, undefined, {
        optionalGroups: ["legacy-astc"],
      });
    assert.ok(selected.selectedInventory.some((asset) => asset.path === "common/sha-license.txt"));
    assert.ok(selected.selectedInventory.every((asset) => !asset.optionalGroup));
    assert.equal(
      enabled.downloadFiles.length - selected.downloadFiles.length,
      OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"].length - 1,
    );
  });

  test("malformed omitted codec closure, mixed packs, and invalid group selection fail closed", () => {
    for (const packed of [false, true]) {
      const incomplete = fixture({ optional: true, packed });
      delete incomplete.assets.at(-1).optionalGroup;
      assert.throws(() => validateRuntimeProfileSelection(incomplete), /optional runtime asset/);
      assert.throws(
        () =>
          validateRuntimeProfileSelection(fixture({ packed }), undefined, {
            optionalGroups: ["legacy-astc"],
          }),
        /unavailable/,
      );
      assert.throws(
        () =>
          validateRuntimeProfileSelection(fixture({ optional: true, packed }), undefined, {
            optionalGroups: ["unknown"],
          }),
        /group selection/,
      );
    }
    const mixed = fixture({ optional: true }),
      files = mixed.transport.files,
      common = files[0],
      optional = files.at(-1);
    mixed.transport.entries.at(-1).file = common.path;
    mixed.transport.entries.at(-1).offset = common.bytes;
    common.bytes += optional.bytes;
    common.decodedBytes += optional.decodedBytes;
    files.pop();
    assert.throws(() => validateRuntimeProfileSelection(mixed), /payload is shared/);
  });

  test("editable bootstrap preloads both trusted bundled profiles without omitting common bytes", () => {
    for (const packed of [false, true]) {
      const manifest = fixture({ packed });
      const result = validateRuntimeProfileSelection(manifest, undefined, {
        preloadProfileIds: BUNDLED_SHELL_PROFILE_IDS,
      });
      assert.deepEqual(result.selectedInventory, manifest.assets);
      assert.equal(result.downloadBytes, manifest.totalBytes);
      assert.throws(
        () =>
          validateRuntimeProfileSelection(manifest, undefined, {
            preloadProfileIds: ["not-a-profile"],
          }),
        /SHELL_PROFILE_UNKNOWN/,
      );
      assert.throws(
        () =>
          validateRuntimeProfileSelection(manifest, undefined, {
            preloadProfileIds: null,
          }),
        /Invalid runtime profile preload/,
      );
    }
  });
}

// runtime-shell-build-inputs
{
  // 固定 v2 外壳下载和打包输入，并以隔离模拟验证漂移在联网前失败；不下载或执行外壳。
  // Pin v2 shell download/packaging inputs and reject drift before network access using isolated mocks, without downloading or executing shells.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { spawnSync } = nodeChild_processModule;
  const { SHELL_BUILD_INPUTS } = packageModule;
  const { BUNDLED_SHELL_PROFILE_IDS, resolveShellProfile } = shellProfilesModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;

  const expected = [
    {
      profileId: "v2.0.0-ios26",
      sourceUrl:
        "https://github.com/jhaizhou-ops/d2r-mobile/releases/download/v2.0.0/D2RMobile-shell-build-input.zip",
      bytes: 4856775,
      sha256: "bc1152694d9a00eef680dd07f8ea31419be5ff4d0437d8298d3772e57cc92c94",
      archivePath: "profile/shell.zip",
      evidencePath: "toolchain/shell-release-evidence/v2.0.0.json",
      metadataPath: "profile/shell.json",
      buildPath: "runtime-build/.build/shell.zip",
    },
    {
      profileId: "v2.0.0-ios18",
      sourceUrl:
        "https://github.com/jhaizhou-ops/d2r-mobile/releases/download/v2.0.0/D2RMobile-shell-ios18-build-input.zip",
      bytes: 4852823,
      sha256: "b2d1e3b48c0b15f773b4ea94290fde08039fd123c21be98c55b00da8df55ada5",
      archivePath: "profile/shell-ios18.zip",
      evidencePath: "toolchain/shell-release-evidence/v2.0.0-ios18.json",
      metadataPath: "profile/shell-ios18.json",
      buildPath: "runtime-build/.build/shell-ios18.zip",
    },
  ];

  test("download and packaging share exactly two independently pinned v2 archives", async () => {
    assert.deepEqual(SHELL_BUILD_INPUTS, expected);
    assert.deepEqual(
      SHELL_BUILD_INPUTS.map((input) => input.profileId),
      BUNDLED_SHELL_PROFILE_IDS,
    );
    assert.ok(Object.isFrozen(SHELL_BUILD_INPUTS));
    for (const input of SHELL_BUILD_INPUTS) {
      const evidence = JSON.parse(
        await readFile(new URL(`../${input.evidencePath}`, import.meta.url), "utf8"),
      );
      assert.ok(Object.isFrozen(input));
      assert.equal(input.sourceUrl, evidence.sourceUrl);
      assert.equal(input.bytes, evidence.archiveBytes);
      assert.equal(input.sha256, evidence.sha256);
      assert.equal(input.archivePath, resolveShellProfile(input.profileId).archivePath);
    }
    for (const id of ["v1.0.18-ios26", "v1.0.18-ios18"])
      assert.throws(() => resolveShellProfile(id), /SHELL_PROFILE_UNKNOWN/);
  });

  test("both provenance layouts include the complete v2 metadata without shipping registry sources", async () => {
    const [packaging, programBuild, downloading] = await Promise.all([
      readFile(new URL("../runtime-build/package.mjs", import.meta.url), "utf8"),
      readFile(
        new URL("../toolchain/program-rewrite-spike/build-wasm.sh", import.meta.url),
        "utf8",
      ),
      readFile(new URL("../runtime-build/build.mjs", import.meta.url), "utf8"),
    ]);
    const profileSources = packaging.match(/const shellProfileSources = \[([\s\S]*?)\];/)[1];
    const programSources = programBuild.match(/for file in shell-profiles\.mjs ([\s\S]*?); do/)[1];
    for (const name of [
      "v2.0.0.json",
      "v2.0.0-derived-capacities.json",
      "v2.0.0-ios18.json",
      "v2.0.0-ios18-derived-capacities.json",
    ]) {
      assert.ok(profileSources.includes(`"toolchain/shell-release-evidence/${name}"`));
      assert.ok(programSources.includes(`toolchain/shell-release-evidence/${name}`));
    }
    assert.ok(programSources.includes("shell-v2-evidence.mjs"));
    assert.doesNotMatch(profileSources, /shell-(?:profiles|v2-evidence)\.mjs/);
    assert.match(packaging, /await copy\(input\.buildPath, input, input\.archivePath\)/);
    assert.match(packaging, /await copy\(input\.evidencePath, undefined, input\.metadataPath\)/);
    assert.match(downloading, /for \(const input of SHELL_BUILD_INPUTS\)/);
    assert.match(downloading, /offset !== input\.bytes/);
    assert.match(
      downloading,
      /createHash\("sha256"\)\.update\(shellBytes\)\.digest\("hex"\) !== input\.sha256/,
    );
  });

  test("each independent shell pin rejects registry drift before network access", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/runtime-lifecycle.mock.mjs", import.meta.url).pathname,
      ],
      {
        env: { ...env, D2R_TEST_SCENARIO: "runtime-shell-build-inputs" },
        encoding: "utf8",
        timeout: 30000,
        maxBuffer: 1024 * 1024,
      },
    );
    assertIsolatedRun(result);
  });
}

// runtime-transport-groups
{
  // 验证可选组按内容归属隔离传输包，保持旧打包格式及字节不变。
  // Verify content-owned optional pack isolation while preserving the legacy packing format and bytes.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { Buffer } = nodeBufferModule;
  const { createHash } = nodeCryptoModule;
  const { gzipSync } = nodeZlibModule;
  const {
    OPTIONAL_RUNTIME_ASSET_GROUPS,
    runtimeContentKey,
    validateRuntimeAssetGroups,
    selectRuntimeAssetGroups,
  } = runtimeAssetGroupsModule;
  const { createRuntimeTransport, validateRuntimeTransport, verifyTransportPayload } =
    transportModule;

  const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
  function fixture({ optional = true, shared = false, large = false } = {}) {
    const assets = [],
      contents = new Map();
    function add(path, data, optionalGroup) {
      const bytes = Buffer.from(data),
        record = { path, sha256: sha(bytes), bytes: bytes.length };
      if (optionalGroup !== undefined) record.optionalGroup = optionalGroup;
      assets.push(record);
      contents.set(runtimeContentKey(record), { ...record, data: bytes });
    }
    add("common/worker.mjs", "common worker");
    add("toolchain/asset-runtime-js/dist/png-codec.wasm", "common codec");
    if (optional)
      OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"].forEach((path, i) =>
        add(
          path,
          large && i === 1 ? Buffer.alloc(300 * 1024, 1) : `optional codec ${i}`,
          "legacy-astc",
        ),
      );
    if (shared) add("common/license.txt", "optional codec 2");
    return { assets, contents };
  }
  function manifest(assets, transport) {
    return {
      version: 1,
      assets,
      totalBytes: assets.reduce((n, asset) => n + asset.bytes, 0),
      transport,
    };
  }

  test("no-group transport remains byte-for-byte identical with no additional schema fields", () => {
    const { assets, contents } = fixture({ optional: false }),
      previous = createRuntimeTransport(contents),
      grouped = createRuntimeTransport(contents, { assets });
    assert.deepEqual(grouped, previous);
    const sorted = [...contents.values()].sort((a, b) => a.sha256.localeCompare(b.sha256)),
      decoded = Buffer.concat(sorted.map((record) => record.data)),
      gzip = gzipSync(decoded, { level: 9 }),
      encoding = gzip.length < decoded.length ? "gzip" : "identity",
      data = encoding === "gzip" ? gzip : decoded,
      path = `payloads/${sha(data)}.${encoding === "gzip" ? "gz" : "bin"}`;
    assert.deepEqual(previous.payloads, new Map([[path, data]]));
    assert.deepEqual(previous.transport.files, [
      { path, bytes: data.length, sha256: sha(data), encoding, decodedBytes: decoded.length },
    ]);
    assert.deepEqual(Object.keys(previous.transport).sort(), [
      "entries",
      "files",
      "totalBytes",
      "version",
    ]);
  });

  test("small and large optional codec payloads never change common transport bytes", () => {
    for (const shared of [false, true])
      for (const large of [false, true]) {
        const { assets, contents } = fixture({ shared, large }),
          common = fixture({ optional: false, shared }),
          baseline = createRuntimeTransport(common.contents),
          result = createRuntimeTransport(contents, { assets }),
          files = validateRuntimeTransport(manifest(assets, result.transport)),
          ownership = validateRuntimeAssetGroups(assets);
        for (const file of files.values())
          verifyTransportPayload(file, result.payloads.get(file.path));
        const commonFiles = new Set(
          result.transport.entries
            .filter((entry) => ownership.get(runtimeContentKey(entry)) === "")
            .map((entry) => entry.file),
        );
        assert.deepEqual(
          new Map([...result.payloads].filter(([path]) => commonFiles.has(path))),
          baseline.payloads,
        );
        for (const file of files.values())
          assert.equal(
            new Set(file.entries.map((entry) => ownership.get(runtimeContentKey(entry)))).size,
            1,
          );
        assert.equal(result.transport.entries.length, contents.size);
      }
  });

  test("group metadata and logical transport source contents must agree exactly", () => {
    const { assets, contents } = fixture();
    assert.throws(
      () => createRuntimeTransport(contents, { assets: assets.slice(1) }),
      /inventory mismatch/,
    );
    const extra = [...assets, { path: "extra.bin", bytes: 3, sha256: "f".repeat(64) }];
    assert.throws(() => createRuntimeTransport(contents, { assets: extra }), /inventory mismatch/);
    assert.throws(
      () => createRuntimeTransport(contents, { assets: assets.slice(0, -1) }),
      /Incomplete/,
    );
  });

  test("full validator rejects a syntactically valid mixed payload and incomplete optional metadata", () => {
    const { assets, contents } = fixture(),
      mixed = createRuntimeTransport(contents);
    assert.throws(
      () => validateRuntimeTransport(manifest(assets, mixed.transport)),
      /payload is shared/,
    );
    const valid = createRuntimeTransport(contents, { assets }),
      changed = structuredClone(assets);
    delete changed.at(-1).optionalGroup;
    assert.throws(
      () => validateRuntimeTransport(manifest(changed, valid.transport)),
      /optional runtime asset/,
    );
  });

  test("shared signing and ASTC bytes cannot drag an unselected optional payload into downloads", () => {
    const { assets, contents } = fixture();
    // 将一个仅可选内容记录与签名许可证共用，不与公共字节共用。
    // Share one optional-only content record with a signing license, not with common bytes.
    const shared = contents.get(runtimeContentKey(assets[4]));
    OPTIONAL_RUNTIME_ASSET_GROUPS["ipa-signing"].forEach((path, i) => {
      const data = i === 2 ? shared.data : Buffer.from(`signing asset ${i}`);
      const record = { path, optionalGroup: "ipa-signing", bytes: data.length, sha256: sha(data) };
      assets.push(record);
      contents.set(runtimeContentKey(record), { ...record, data });
    });
    const first = createRuntimeTransport(contents, { assets });
    assert.deepEqual(createRuntimeTransport(contents, { assets: [...assets].reverse() }), first);
    validateRuntimeTransport(manifest(assets, first.transport));
    for (const optionalGroups of [
      [],
      ["ipa-signing"],
      ["legacy-astc"],
      ["ipa-signing", "legacy-astc"],
    ]) {
      const selectedKeys = new Set(
        selectRuntimeAssetGroups(assets, optionalGroups).map(runtimeContentKey),
      );
      const selectedFiles = new Set(
        first.transport.entries
          .filter((entry) => selectedKeys.has(runtimeContentKey(entry)))
          .map((entry) => entry.file),
      );
      for (const entry of first.transport.entries)
        if (selectedFiles.has(entry.file))
          assert.ok(
            selectedKeys.has(runtimeContentKey(entry)),
            "selected physical payload contains only selected content",
          );
    }
    const ownership = validateRuntimeAssetGroups(assets);
    assert.equal(ownership.get(runtimeContentKey(shared)), "ipa-signing|legacy-astc");
    const common = { ...shared, path: "common/shared-license.txt" };
    delete common.optionalGroup;
    for (const records of [
      [common, ...assets],
      [...assets, common],
    ])
      assert.equal(validateRuntimeAssetGroups(records).get(runtimeContentKey(shared)), "");
  });

  test("two v2-sized synthetic archives each own one transport payload, without legacy archives", () => {
    const assets = [],
      contents = new Map();
    for (const [path, size, fill] of [
      ["common/metadata.json", 32, 1],
      ["profile/shell.zip", 4856775, 2],
      ["profile/shell-ios18.zip", 4852823, 3],
    ]) {
      const data = Buffer.alloc(size, fill),
        record = { path, bytes: data.length, sha256: sha(data) };
      assets.push(record);
      contents.set(runtimeContentKey(record), { ...record, data });
    }
    const { transport, payloads } = createRuntimeTransport(contents, { assets });
    const files = validateRuntimeTransport(manifest(assets, transport));
    for (const file of files.values()) verifyTransportPayload(file, payloads.get(file.path));
    assert.equal(files.size, 3);
    const archiveFiles = new Set();
    for (const asset of assets.filter((asset) => asset.path.startsWith("profile/"))) {
      const entry = transport.entries.find(
        (entry) => runtimeContentKey(entry) === runtimeContentKey(asset),
      );
      assert.equal(entry.offset, 0);
      assert.equal(files.get(entry.file).entries.length, 1);
      assert.equal(files.get(entry.file).decodedBytes, asset.bytes);
      archiveFiles.add(entry.file);
    }
    assert.equal(archiveFiles.size, 2);
    assert.ok(!assets.some((asset) => asset.path.includes("legacy-shell")));
  });
}

// sealed-cross-transport
{
  // 仅替换转换器导入，执行真实消息/失败路径；不加载或执行编译器、WASM 或私有输入。
  // Replace only converter imports to exercise actual message/failure paths; no compiler, WASM or private inputs execute.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFileSync } = nodeFsModule;
  const { createContext, runInContext } = nodeVmModule;
  const { shaderFailureDiagnostic, sanitizeShaderDiagnostic } = shaderDiagnosticsModule;

  const source = readFileSync(
    new URL("../runtime-sealing/cross-worker.mjs", import.meta.url),
    "utf8",
  ).replace(/^import .+;\r?\n/gm, "");
  function worker(error) {
    let listener;
    const replies = [];
    const context = createContext({
      ArrayBuffer,
      Uint8Array,
      shaderFailureDiagnostic,
      sanitizeShaderDiagnostic,
      factory() {
        throw new Error("The real factory must never execute");
      },
      async convertSpirv() {
        if (error) throw error;
        return { synthetic: true };
      },
      self: {
        addEventListener(type, callback) {
          assert.equal(type, "message");
          listener = callback;
        },
        postMessage(value) {
          replies.push(value);
        },
      },
    });
    runInContext(source + "\nsealedFactory = factory;", context);
    return async (data = { id: 14, type: "convert", spirv: new ArrayBuffer(20) }) => {
      listener({ data });
      await runInContext("queue", context);
      return replies.at(-1);
    };
  }

  test("Cross forwards only bounded recognized stderr diagnostics", async () => {
    const request = worker(
      new Error(
        "SPIRV-Cross exited 1: error: unsupported synthetic intrinsic\nPRIVATE-CANARY\n at /PRIVATE-CANARY/frame.mjs\nvoid PRIVATE-CANARY() {}",
      ),
    );
    const reply = await request();
    assert.equal(reply.id, 14);
    assert.equal(reply.ok, false);
    assert.equal(reply.error, "CROSS_CONVERSION_FAILED");
    assert.deepEqual(reply.shaderDiagnostic, {
      substep: "spirv-cross",
      errorType: "Error",
      message: "SPIRV-Cross conversion failed",
      diagnostics: ["error: unsupported synthetic intrinsic"],
    });
    assert.doesNotMatch(JSON.stringify(reply), /PRIVATE-CANARY/);
  });

  test("Cross preserves fixed contract causes but does not treat arbitrary errors as stderr", async () => {
    for (const [message, expected] of [
      ["Invalid SPIR-V header", "Invalid SPIR-V header"],
      ["error: PRIVATE-CANARY is arbitrary exception text", "SPIRV-Cross conversion failed"],
      ["PRIVATE-CANARY", "SPIRV-Cross conversion failed"],
    ]) {
      const reply = await worker(new Error(message))();
      assert.equal(reply.shaderDiagnostic.message, expected);
      assert.equal(reply.shaderDiagnostic.substep, "spirv-cross");
      assert.equal(Object.hasOwn(reply.shaderDiagnostic, "diagnostics"), false);
      assert.doesNotMatch(JSON.stringify(reply), /PRIVATE-CANARY/);
    }
  });

  test("Cross preserves success protocol and describes invalid conversion requests", async () => {
    assert.deepEqual((await worker()()).result, { synthetic: true });
    const reply = await worker()({ id: 15, type: "bad", spirv: new ArrayBuffer(4) });
    assert.equal(reply.shaderDiagnostic.message, "Invalid conversion request");
    assert.equal(reply.id, 15);
  });
}

// sealed-dotnet-transport
{
  // 执行真实密封工作线程消息路径，仅替换已初始化的托管导出；不加载编译器或游戏数据。
  // Exercise the real sealed-worker message path, replacing only initialized managed exports; no compiler or game inputs.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFileSync } = nodeFsModule;
  const { createContext, runInContext } = nodeVmModule;
  const { sanitizeShaderDiagnostic } = shaderDiagnosticsModule;

  const source = readFileSync(
    new URL("../sealed-runtime/dotnet-worker.mjs", import.meta.url),
    "utf8",
  );
  function worker({ resultJson, parse, managedError } = {}) {
    const listeners = new Map(),
      replies = [],
      calls = [];
    const context = createContext({
      ArrayBuffer,
      Uint8Array,
      ...(parse ? { JSON: { parse, stringify: JSON.stringify } } : {}),
      self: {
        addEventListener(type, listener) {
          listeners.set(type, listener);
        },
        postMessage(reply) {
          replies.push(reply);
        },
      },
      managed: {
        ShaderBridge: {
          Translate(json, bytes) {
            calls.push({ job: JSON.parse(json), bytes: [...bytes] });
            if (managedError) throw managedError;
            return resultJson === undefined
              ? JSON.stringify({ sentinel: "managed-export-reached" })
              : resultJson;
          },
        },
      },
    });
    runInContext(source + "\ninitialized = true; exports = managed;", context);
    return {
      calls,
      async request(data) {
        listeners.get("message")({ data });
        await runInContext("queue", context);
        return replies.at(-1);
      },
    };
  }
  function request(stage, flag) {
    return {
      id: 42,
      type: "run",
      job: {
        key: "5221043a88afa381",
        stage,
        debug: true,
        reduced: true,
        attr: {},
        fout: {},
        img: {},
        tex: {},
        ...(flag === undefined ? {} : { allowUnverifiedTessellation: flag }),
      },
      payload: new Uint8Array([1, 2, 3, 4]).buffer,
    };
  }

  test("sealed transport preserves explicit hull/domain opt-in and all job fields", async () => {
    const transport = worker();
    for (const stage of ["cs", "vs", "ps", "hs", "ds"]) {
      const message = request(stage, ["hs", "ds"].includes(stage) ? true : undefined),
        reply = await transport.request(message);
      assert.equal(reply.id, 42);
      assert.equal(reply.ok, true);
      assert.equal(reply.result.sentinel, "managed-export-reached");
      assert.deepEqual(transport.calls.at(-1), { job: message.job, bytes: [1, 2, 3, 4] });
    }
  });

  test("sealed transport rejects absent, false and nonboolean tessellation opt-ins and geometry", async () => {
    const transport = worker();
    for (const stage of ["hs", "ds"])
      for (const flag of [undefined, false, 1, "true", null]) {
        const reply = await transport.request(request(stage, flag));
        assert.equal(reply.id, 42);
        assert.equal(reply.ok, false);
      }
    for (const stage of ["gs", "unknown"])
      assert.equal((await transport.request(request(stage, true))).ok, false);
    assert.equal(transport.calls.length, 0);
  });

  test("tessellation opt-in does not bypass existing payload or metadata caps", async () => {
    const transport = worker();
    for (const change of [
      { payload: new Uint8Array(4) },
      { payload: new ArrayBuffer(1024 * 1024 + 1) },
      { job: { ...request("hs", true).job, extra: "x".repeat(65536) } },
      { job: null },
    ])
      assert.equal((await transport.request({ ...request("hs", true), ...change })).ok, false);
    assert.equal(transport.calls.length, 0);
  });

  // 不允许超限回复先进入 JSON.parse；保留正常回复与净化错误行为。
  // Oversized replies must not reach JSON.parse; normal replies and sanitized errors remain intact.
  test("managed output is bounded before parsing", async () => {
    for (const resultJson of ['"' + "x".repeat(24 * 1024 * 1024) + '"', null, 7]) {
      let parses = 0;
      const transport = worker({
        resultJson,
        parse(value) {
          parses++;
          return JSON.parse(value);
        },
      });
      const reply = await transport.request(request("cs"));
      assert.equal(transport.calls.length, 1);
      assert.equal(parses, 0);
      assert.equal(reply.ok, false);
      assert.equal(reply.error, "Offline .NET operation failed (new)");
      assert.equal(reply.shaderDiagnostic.substep, "translation");
      assert.equal(reply.shaderDiagnostic.message, "Shader result cap");
    }
    const transport = worker({ resultJson: '{"bounded":true}' });
    assert.equal((await transport.request(request("cs"))).result.bounded, true);
  });

  test("managed failures expose reviewed causes and exception types without arbitrary text", async () => {
    for (const [message, expected, type] of [
      [
        "Shader must be 16 bytes to 1 MiB and 8-byte aligned.",
        "Shader must be 16 bytes to 1 MiB and 8-byte aligned.",
        "Error",
      ],
      [
        "System.ArgumentException: Graphics jobs require explicit attr and fout type tables.\n at /PRIVATE-CANARY/frame.cs",
        "Graphics jobs require explicit attr and fout type tables.",
        "ArgumentException",
      ],
      [
        "System.NotImplementedException: PRIVATE-CANARY",
        "Translator does not implement this operation",
        "NotImplementedException",
      ],
      [
        "System.MissingMethodException: PRIVATE-CANARY",
        "Translator dependency is missing",
        "MissingMethodException",
      ],
      [
        "System.InvalidOperationException: Missing embedded shader helper: PRIVATE-CANARY",
        "Missing embedded shader helper",
        "InvalidOperationException",
      ],
      ["PRIVATE-CANARY source bytes and keys", undefined, "Error"],
      ["error: PRIVATE-CANARY is not compiler stderr", undefined, "Error"],
    ]) {
      const transport = worker({ managedError: new Error(message) });
      const reply = await transport.request(request("cs"));
      assert.equal(reply.ok, false);
      assert.equal(reply.error, "Offline .NET operation failed (new)");
      assert.equal(reply.shaderDiagnostic.substep, "translation");
      assert.equal(reply.shaderDiagnostic.errorType, type);
      assert.equal(reply.shaderDiagnostic.message, expected);
      assert.deepEqual(sanitizeShaderDiagnostic(reply.shaderDiagnostic), {
        ...reply.shaderDiagnostic,
      });
      assert.doesNotMatch(JSON.stringify(reply), /PRIVATE-CANARY/);
      assert.equal(Object.hasOwn(reply.shaderDiagnostic, "diagnostics"), false);
    }
  });

  test("standalone translator diagnostic allowlist stays within the shared boundary", () => {
    const context = createContext({ self: { addEventListener() {} } });
    runInContext(source, context);
    const messages = runInContext("[...translatorMessages]", context);
    for (const message of messages)
      assert.equal(sanitizeShaderDiagnostic({ message })?.message, message);
    const errorTypes = runInContext("[...translatorErrorTypes]", context);
    for (const errorType of errorTypes)
      assert.equal(sanitizeShaderDiagnostic({ errorType })?.errorType, errorType);
  });

  test("managed output parse failures do not expose the invalid returned JSON", async () => {
    const transport = worker({ resultJson: "PRIVATE-CANARY" });
    const reply = await transport.request(request("cs"));
    assert.equal(reply.shaderDiagnostic.errorType, "SyntaxError");
    assert.doesNotMatch(JSON.stringify(reply), /PRIVATE-CANARY/);
  });
}

// runtime-lifecycle
{
  // 隔离运行公开运行时生命周期模拟，不将其视为真实浏览器、存储或 WASM 证明。
  // Run isolated public-runtime lifecycle mocks, not actual browser, storage or WASM proof.
  const { default: test } = nodeTestModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;
  const { spawnSync } = nodeChild_processModule;
  const { default: process } = nodeProcessModule;

  test("sealed runtime worker RPC lifecycle (isolated module mocks)", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/runtime-lifecycle.mock.mjs", import.meta.url).pathname,
      ],
      {
        env: { ...env, D2R_TEST_SCENARIO: "runtime-worker-lifecycle" },
        encoding: "utf8",
        timeout: 30000,
        maxBuffer: 1024 * 1024,
      },
    );
    assertIsolatedRun(result);
  });

  test("public runtime cache cancellation boundaries (isolated module mocks)", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/runtime-lifecycle.mock.mjs", import.meta.url).pathname,
      ],
      {
        env: { ...env, D2R_TEST_SCENARIO: "runtime-cache-cancellation" },
        encoding: "utf8",
        timeout: 30000,
        maxBuffer: 1024 * 1024,
      },
    );
    assertIsolatedRun(result);
  });

  test("public runtime stream cancellation and cleanup (isolated module mocks)", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/runtime-lifecycle.mock.mjs", import.meta.url).pathname,
      ],
      {
        env: { ...env, D2R_TEST_SCENARIO: "runtime-stream-cancellation" },
        encoding: "utf8",
        timeout: 30000,
        maxBuffer: 1024 * 1024,
      },
    );
    assertIsolatedRun(result);
  });

  test("optional producer public-bootstrap lifecycle (isolated module mocks)", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/runtime-lifecycle.mock.mjs", import.meta.url).pathname,
      ],
      {
        env: { ...env, D2R_TEST_SCENARIO: "runtime-optional-bootstrap" },
        encoding: "utf8",
        timeout: 30000,
        maxBuffer: 1024 * 1024,
      },
    );
    assertIsolatedRun(result);
  });
}
