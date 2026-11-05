// Feature suite: build-policy. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as serveModule from "../scripts/serve.mjs";
import * as promisesModule from "node:fs/promises";
import * as nodeOsModule from "node:os";
import * as nodePathModule from "node:path";
import * as nodeChild_processModule from "node:child_process";
import * as verifyModule from "../verification/verify.mjs";
import * as frontendNoticesModule from "../scripts/frontend-notices.mjs";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as bunE2eFsModule from "../scripts/bun-e2e-fs.mjs";
import * as bunE2eModule from "../scripts/bun-e2e.mjs";
import * as nodeCryptoModule from "node:crypto";
import * as nodeZlibModule from "node:zlib";
import * as nodeEventsModule from "node:events";
import * as bunE2eNodeCrossModule from "../scripts/bun-e2e-node-cross.mjs";

// build-output
{
  // 验证 build-output 的约定与边界；依据 ../scripts/frontend-notices.mjs、../scripts/serve.mjs 及本文件的测试断言。
  // Verify build-output contracts and boundaries against ../scripts/frontend-notices.mjs、../scripts/serve.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { validateStaticBuild } = frontendNoticesModule;
  const { assetPath } = serveModule;

  const html = `<link href="./app.css" rel="stylesheet"><script src="./app.js" type="module"></script>`;
  const assets = new Map([
    ["app.js", "document.title = 'local';"],
    ["app.css", "body{color:black}"],
  ]);

  test("static build accepts compiled local browser assets", () => {
    assert.doesNotThrow(() => validateStaticBuild(html, assets));
  });

  test("static build rejects missing, remote, runtime and uncompiled assets", () => {
    for (const invalid of [
      html.replace("./app.js", "https://example.com/app.js"),
      html.replace("./app.css", "./missing.css"),
      html.replace("./app.js", "../app.js"),
      '<script src="./app.js" type="module"></script>',
      '<link href="./app.css" rel="stylesheet">',
    ]) {
      assert.throws(() => validateStaticBuild(invalid, assets));
    }
    assert.throws(() =>
      validateStaticBuild(html, new Map([...assets, ["app.js", "Bun.file('secret')"]])),
    );
    assert.throws(() =>
      validateStaticBuild(html, new Map([...assets, ["app.css", "@apply bg-background;"]])),
    );
  });

  test("preview only resolves visible generated files within dist", () => {
    assert.match(assetPath("/"), /[/\\]dist[/\\]index\.html$/);
    assert.match(assetPath("/app.js"), /[/\\]dist[/\\]app\.js$/);
    for (const path of [
      "/../prod.keys",
      "/%2e%2e/prod.keys",
      "/.env",
      "/a\\..\\prod.keys",
      "/%00",
      "/%zz",
    ]) {
      assert.equal(assetPath(path), null);
    }
  });
}

// ci-policy
{
  // 验证 ci-policy 的约定与边界；依据 ../verification/verify.mjs 及本文件的测试断言。
  // Verify ci-policy contracts and boundaries against ../verification/verify.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readdir, readFile, mkdtemp, rm, writeFile, chmod } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { spawnSync } = nodeChild_processModule;
  const { profiles, validateProfile, run } = verifyModule;
  const workflows = new URL("../../.github/workflows/", import.meta.url);

  test("one GitHub workflow builds once and only adds a trusted-branch Pages deployment", async () => {
    assert.deepEqual(
      (await readdir(workflows)).filter((name) => name.endsWith(".yml")),
      ["verify.yml"],
    );
    const text = await readFile(new URL("verify.yml", workflows), "utf8");
    assert.deepEqual(
      [...text.slice(text.indexOf("\njobs:")).matchAll(/^ {2}([a-z-]+):/gm)].map(
        (match) => match[1],
      ),
      ["quick", "deploy-pages"],
    );
    assert.match(text, /runs-on: ubuntu-24\.04/);
    assert.match(text, /run: node web\/verification\/verify\.mjs quick/);
    assert.doesNotMatch(
      text,
      /matrix:|strategy:|inputs:|desktop-build:|windows-|macos-|download-artifact/,
    );
    assert.match(text, /node web\/runtime-build\/build\.mjs/);
    assert.match(text, /node web\/runtime-build\/stage-preview\.mjs/);
    const steps = text.split(/\n {6}- /).slice(1);
    const uploads = steps.filter((step) => /uses: actions\/upload-artifact@/.test(step));
    const artifacts = new Map(
      uploads.map((step) => [step.match(/^ {10}name: (\S+)$/m)?.[1], step]),
    );
    assert.equal(uploads.length, 4);
    assert.deepEqual([...artifacts.keys()].sort(), [
      "air-compiler-diagnostic-source",
      "air-compiler-diagnostic-unvalidated",
      "air-compiler-source",
      "web-preview",
    ]);
    assert.match(text, /failure\(\) && steps\.runtime\.outcome == 'failure'/);
    assert.match(
      artifacts.get("air-compiler-diagnostic-source"),
      /if: \$\{\{ !cancelled\(\) && steps\.air_failure_source\.outcome == 'success' \}\}/,
    );
    assert.match(
      artifacts.get("air-compiler-diagnostic-unvalidated"),
      /if: \$\{\{ !cancelled\(\) && steps\.air_failure_bundle\.outcome == 'success' \}\}/,
    );
    // 没有显式条件时采用 GitHub 默认 success() 防护，而非失败时保留。
    // No explicit condition means GitHub's default success() guard, never failure retention.
    for (const name of ["air-compiler-source", "web-preview"])
      assert.doesNotMatch(artifacts.get(name), /\n {8}if:/);
    assert.doesNotMatch(text, /save-always:|continue-on-error:|screenshot|google-chrome/);
    assert.doesNotMatch(text, /pull_request_target:|self-hosted|secrets\./);
    for (const sha of text.matchAll(/uses: [^@]+@(\S+)/g)) assert.match(sha[1], /^[0-9a-f]{40}$/);
    assert.match(text, /name: web-preview\n\s+path: web\/dist\//);
    assert.match(text, /retention-days: 7/);
  });

  test("Pages reuses verified public output without granting deployment permissions to builds or PRs", async () => {
    const text = await readFile(new URL("verify.yml", workflows), "utf8");
    const [build, deploy] = text.split("\n  deploy-pages:\n");
    assert.ok(deploy);
    assert.match(text, /push:\n\s+branches: \[pr\/web-ipa-packager\]/);
    assert.doesNotMatch(build, /pages: write|id-token: write/);
    assert.match(deploy, /needs: quick/);
    assert.match(deploy, /needs\.quick\.result == 'success'/);
    const upload = build.slice(build.indexOf("      - name: Package the same verified webpage"));
    for (const section of [upload, deploy]) {
      assert.match(section, /github\.repository == 'n3crosis\/d2r-mobile'/);
      assert.match(section, /github\.ref == 'refs\/heads\/pr\/web-ipa-packager'/);
      assert.match(
        section,
        /github\.event_name == 'push' \|\| github\.event_name == 'workflow_dispatch'/,
      );
      assert.doesNotMatch(section, /pull_request|always\(|secrets\./);
    }
    assert.match(upload, /uses: actions\/upload-pages-artifact@[a-f0-9]{40}/);
    assert.match(upload, /path: web\/dist\//);
    assert.match(upload, /retention-days: 1/);
    assert.match(deploy, /permissions:\n\s+pages: write\n\s+id-token: write/);
    assert.match(
      deploy,
      /environment:\n\s+name: github-pages\n\s+url: \$\{\{ steps\.deployment\.outputs\.page_url \}\}/,
    );
    assert.match(deploy, /uses: actions\/configure-pages@[a-f0-9]{40}/);
    assert.match(deploy, /uses: actions\/deploy-pages@[a-f0-9]{40}/);
    assert.doesNotMatch(deploy, /checkout|run:|enablement:|contents: write|download-artifact/);
    assert.ok(build.indexOf("node web/runtime-build/stage-preview.mjs") < build.indexOf(upload));
  });

  // 仅网页代码、验证配置及直接使用的共享来源触发自动构建；文档和无关原生代码不触发。
  // Only web code, verification configuration and consumed shared sources trigger automatic builds.
  test("both workflow triggers include web dependencies but exclude docs and unrelated native code", async () => {
    const text = await readFile(new URL("verify.yml", workflows), "utf8");
    const filters = [...text.matchAll(/paths:\s*\[([\s\S]*?)\]/g)].map((match) =>
      [...match[1].matchAll(/"([^"\n]+)"/g)].map((value) => value[1]),
    );
    assert.equal(filters.length, 2);
    assert.deepEqual(filters[0], filters[1]);
    assert.match(text, /^ {2}workflow_dispatch:$/m);
    // 按声明顺序应用包含和排除规则，与工作流路径筛选一致。
    // Apply positive and negative patterns in declaration order, as workflow filters do.
    const triggers = (patterns, path) =>
      patterns.reduce((included, pattern) => {
        const excluded = pattern.startsWith("!");
        const glob = excluded ? pattern.slice(1) : pattern;
        const matches = glob.endsWith("/**")
          ? path.startsWith(glob.slice(0, -2))
          : glob.startsWith("**/*.")
            ? path.endsWith(glob.slice(4))
            : path === glob;
        return matches ? !excluded : included;
      }, false);
    const inputs = new Set([
      "web/main.tsx",
      "web/tests/build-policy.test.mjs",
      "web/package.json",
      "web/bun.lock",
      "web/.oxlintrc.json",
      "web/scripts/build.mjs",
      "web/runtime-build/build.mjs",
      "web/toolchain/shader-spike/ShaderSpike.csproj",
      ".github/workflows/verify.yml",
      "LICENSE",
      "dm/i18n/scan_literals.py",
      "dm/i18n/literal_whitelist.tsv",
      "dm/host/dm_i18n_tab.h",
      "dm/gfx/nvn/nvn_tex.c",
      "dm/gfx/nvn/nvn_tex.h",
      "dm/gfx/nvn/shader_map.tsv",
      "dm/gfx/nvn/shader_map.tsv.conflicts.tsv",
      "dm/gfx/nvn/shader_facts.tsv",
      "dm/gfx/nvn/bf_want.tsv",
      "dm/gfx/tex/nvn_bcdec.c",
      "dm/gfx/tex/nvn_bcdec.h",
      "dm/gfx/tex/nvn_deswizzle.c",
      "dm/gfx/tex/nvn_deswizzle.h",
      "dm/tools/d2rm_pack.py",
      "dm/tools/dmx.py",
      "dm/tools/guest_dylib.py",
      "dm/tools/patch_nso.py",
      "dm/tools/extract_game.sh",
      "dm/third_party/ryujinx/LICENSE.txt",
      "dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Translator.cs",
      "dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Glsl/HelperFunctions/test.glsl",
      "dm/third_party/ryujinx/src/Spv.Generator/Module.cs",
      "dm/third_party/ryujinx/src/Ryujinx.Common/Pools/ObjectPool.cs",
      "dm/third_party/bcdec/bcdec.h",
      "dm/third_party/bcdec/LICENSE",
    ]);
    for (const name of [
      "asset-generation-spike/source-provenance.json",
      "asset-transforms-js/source-provenance.json",
      "casc-overlay-js/source-provenance.json",
      "art-recipes-js/source-provenance.json",
      "remaining-art-js/source-provenance.json",
      "hud-recipes-js/provenance.json",
      "overlay-recipe-js/source-provenance.json",
      "overlay-recipe-js/contract-source.json",
    ]) {
      const manifest = JSON.parse(
        await readFile(new URL(`../toolchain/${name}`, import.meta.url), "utf8"),
      );
      for (const path of Object.keys(manifest.normalizedSources ?? manifest.sources ?? manifest))
        if (path.startsWith("dm/")) inputs.add(path);
    }
    for (const patterns of filters) {
      for (const input of inputs)
        assert.ok(triggers(patterns, input), `Web input does not trigger CI: ${input}`);
      for (const unrelated of [
        "README.md",
        "docs/WEB_IPA_PACKAGER.md",
        "web/README.md",
        "web/toolchain/shader-spike/README.MD",
        "dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/README.md",
        "dm/host/loader.c",
        "dm/ios/Sources/AppDelegate.m",
        "dm/ios/project.yml",
        "dm/ios/tools/shell_build.sh",
        "dm/ios/tools/unrelated.py",
        "dm/tools/unrelated.py",
        "dm/gfx/tools/unrelated.py",
        "dm/gfx/nvn/nvn_mtl.m",
        "dm/gfx/tex/unrelated.c",
        "dm/third_party/ryujinx/src/Ryujinx.Common/Unrelated.cs",
        "dm/third_party/ryujinx/src/Ryujinx.HLE/Unrelated.cs",
        ".gitignore",
      ])
        assert.equal(
          triggers(patterns, unrelated),
          false,
          `Unrelated change triggers CI: ${unrelated}`,
        );
    }
  });

  test("profile selection rejects arbitrary shell, paths, and prototype names", () => {
    for (const bad of [
      "../quick",
      "quick;echo unsafe",
      "constructor",
      "__proto__",
      "",
      "assets\nshader",
    ]) {
      assert.throws(() => validateProfile(bad), /Unknown verification profile/);
    }
    for (const name of ["quick", "full", ...Object.keys(profiles)])
      assert.equal(validateProfile(name), name);
    assert.throws(() => run("texture", {}), /--provision/);
    assert.throws(() => run("texture", { GITHUB_ACTIONS: "true" }), /local verification only/);
  });

  test("all verification profiles retain fresh processes, environments and feature dispatch", async () => {
    const expectedGroups = {
      quick: "quick",
      shader: "shader-proofs",
      "offline-dotnet": "shader-proofs",
      "spirv-cross": "shader-proofs",
      "shader-pipeline": "shader-proofs",
      extraction: "input-proofs",
      "signed-nca": "input-proofs",
      program: "input-proofs",
      texture: "browser-runtime-proofs",
      "offline-wasm": "browser-runtime-proofs",
      "desktop-build": "browser-runtime-proofs",
      desktop: "browser-runtime-proofs",
      "staged-output": "storage-proofs",
      zip64: "storage-proofs",
    };
    assert.deepEqual(
      Object.keys(expectedGroups).sort(),
      ["quick", ...Object.keys(profiles)].sort(),
    );
    const temp = await mkdtemp(join(tmpdir(), "verification-dispatch-"));
    try {
      const capture = join(temp, "capture.cjs");
      const output = join(temp, "capture.json");
      await writeFile(
        capture,
        `const fs = require("node:fs"); fs.writeFileSync(process.env.CAPTURE_FILE, JSON.stringify({ pid: process.pid, argv: process.argv.slice(2), cwd: process.cwd(), env: process.env }));`,
      );
      const bash = join(temp, "bash");
      await writeFile(
        bash,
        `#!/bin/sh\nexec ${JSON.stringify(process.execPath)} ${JSON.stringify(capture)} "$@"\n`,
      );
      await chmod(bash, 0o755);
      const pids = new Set();
      const directories = new Set();
      for (const [profile, group] of Object.entries(expectedGroups)) {
        const command = verifyModule.recipeCommand(profile);
        assert.equal(
          command[0],
          join(verifyModule.root, "web/verification/recipes", `${group}.sh`),
        );
        assert.deepEqual(command.slice(1), profile === "quick" ? [] : [profile]);
        const recipe = await readFile(command[0], "utf8");
        if (profile !== "quick")
          assert.ok(recipe.includes(`\n${profile})\n# BEGIN PROFILE ${profile}\n`));
        assert.equal(
          run(
            profile,
            {
              ...process.env,
              GITHUB_ACTIONS: "false",
              GITHUB_STEP_SUMMARY: "",
              GITHUB_OUTPUT: "",
              PATH: temp + nodePathModule.delimiter + process.env.PATH,
              RUNNER_TEMP: temp,
              CAPTURE_FILE: output,
              VERIFY_TEST_SENTINEL: "unchanged environment",
            },
            true,
          ),
          0,
        );
        const captured = JSON.parse(await readFile(output, "utf8"));
        assert.deepEqual(captured.argv, command);
        assert.equal(captured.cwd, nodePathModule.resolve(verifyModule.root));
        assert.equal(captured.env.VERIFY_ROOT, verifyModule.root.replaceAll("\\", "/"));
        assert.equal(captured.env.GITHUB_WORKSPACE, verifyModule.root);
        assert.equal(captured.env.VERIFY_TEST_SENTINEL, "unchanged environment");
        assert.ok(captured.env.RUNNER_TEMP.startsWith(join(temp, `d2r-${profile}-`)));
        assert.equal(
          captured.env.GITHUB_STEP_SUMMARY,
          join(captured.env.RUNNER_TEMP, "summary.md"),
        );
        assert.equal(captured.env.GITHUB_OUTPUT, join(captured.env.RUNNER_TEMP, "output"));
        assert.equal(captured.env.GITHUB_PATH, join(captured.env.RUNNER_TEMP, "path"));
        assert.equal(captured.env.GITHUB_ENV, join(captured.env.RUNNER_TEMP, "env"));
        pids.add(captured.pid);
        directories.add(captured.env.RUNNER_TEMP);
      }
      assert.equal(pids.size, Object.keys(expectedGroups).length);
      assert.equal(directories.size, Object.keys(expectedGroups).length);
      assert.throws(() => verifyModule.recipeCommand("full"), /individual profiles/);
      assert.throws(() => verifyModule.recipeCommand("../quick"), /Unknown verification profile/);
    } finally {
      await rm(temp, { recursive: true, force: true });
    }
  });

  test("grouped recipe entrypoints reject invalid profiles before executing any step", () => {
    for (const group of [
      "shader-proofs",
      "input-proofs",
      "browser-runtime-proofs",
      "storage-proofs",
    ]) {
      const result = spawnSync(
        "bash",
        [join(verifyModule.root, "web/verification/recipes", `${group}.sh`), "__proto__"],
        { encoding: "utf8" },
      );
      assert.equal(result.status, 2);
      assert.match(result.stderr, /Unknown verification profile/);
      assert.equal(result.stdout, "");
    }
  });

  test("allowlisted metadata is emitted without executing a proof", async () => {
    const temp = await mkdtemp(join(tmpdir(), "verification-metadata-"));
    try {
      const output = join(temp, "output");
      const result = spawnSync(
        process.execPath,
        [
          new URL("../verification/verify.mjs", import.meta.url).pathname,
          "shader-pipeline",
          "--metadata",
        ],
        {
          env: { ...process.env, GITHUB_OUTPUT: output, RUNNER_TEMP: temp },
          encoding: "utf8",
        },
      );
      assert.equal(result.status, 0, result.stderr);
      const text = await readFile(output, "utf8");
      assert.match(text, /dotnet=true/);
      assert.match(text, /linked-shader\/native/);
      assert.doesNotMatch(text, /\$\{\{/);
    } finally {
      await rm(temp, { recursive: true, force: true });
    }
  });

  test("quick installs frozen dependencies before verifying all asset runtime sources", async () => {
    const recipe = await readFile(
      new URL("../verification/recipes/quick.sh", import.meta.url),
      "utf8",
    );
    const install = recipe.indexOf("\nbun install --frozen-lockfile\n");
    const verify = recipe.indexOf("\nnode toolchain/asset-runtime-js/build.mjs --verify-sources\n");
    const build = recipe.indexOf("\nbun run build\n");
    assert.ok(install >= 0 && verify > install && build > verify);
    const result = spawnSync(
      process.execPath,
      [
        new URL("../toolchain/asset-runtime-js/build.mjs", import.meta.url).pathname,
        "--verify-sources",
      ],
      { encoding: "utf8" },
    );
    assert.equal(result.status, 0, result.stderr);
  });

  test("shared recipe runner preserves failure, skipped steps, and diagnostics", async () => {
    const temp = await mkdtemp(join(tmpdir(), "verification-steps-"));
    try {
      const script = `source "$VERIFY_ROOT/web/verification/steps.sh"
step first fail . 0 <<'STEP'
exit 7
STEP
step second skipped . 0 <<'STEP'
exit 0
STEP
always_step report diagnostics . 0 <<'STEP'
printf '%s %s' "$VERIFY_OUTCOME_first" "$VERIFY_OUTCOME_second" > "$RUNNER_TEMP/result"
STEP
finish
`;
      const result = spawnSync("bash", ["-c", script], {
        env: {
          ...process.env,
          VERIFY_ROOT: new URL("../../", import.meta.url).pathname,
          RUNNER_TEMP: temp,
        },
        encoding: "utf8",
      });
      assert.equal(result.status, 1, result.stderr);
      assert.equal(await readFile(join(temp, "result"), "utf8"), "failure skipped");
    } finally {
      await rm(temp, { recursive: true, force: true });
    }
  });

  test("AIR release cache is exact and the consumer cannot fall back to a compiler build", async () => {
    const text = await readFile(new URL("verify.yml", workflows), "utf8");
    const finished = text
      .split(/\n {6}- /)
      .find((value) => value.includes("\n        id: air_cache\n"));
    assert(
      finished.includes(
        "public-air-release-v1-${{ steps.air_source.outputs.release_manifest_sha256 }}",
      ),
    );
    for (const path of [
      "web/toolchain/metal-air-compiler/dist",
      "web/runtime-build/.build/air-compiler-receipt.sha256",
    ])
      assert(finished.includes(path));
    const assetCache = text
      .split(/\n {6}- /)
      .find((value) => value.includes("path: web/runtime-build/.build/air-compiler-release"));
    assert(
      assetCache.includes(
        "public-air-release-assets-v1-${{ steps.air_source.outputs.release_manifest_sha256 }}",
      ),
    );
    assert.doesNotMatch(
      text,
      /restore-keys:|ccache|EM_COMPILER_WRAPPER|air_objects|build-air-compiler\.mjs --(?:work|jobs)/,
    );
    const build = await readFile(
      new URL("../runtime-build/build-air-compiler.mjs", import.meta.url),
      "utf8",
    );
    assert.match(build, /importPinnedAirCompiler/);
    assert.match(build, /compileCanary: await verifyAirCompilerCanary\(\)/);
    assert.doesNotMatch(build, /spawn|runToolchain|build-host-tools|build-wasm|emsdk.*install/);
    // 现有非 AIR 构建仍需要固定版本的 SDK。
    // Existing non-AIR builds still need the pinned SDK.
    assert(text.includes('source "$RUNNER_TEMP/browser-emsdk/emsdk_env.sh"'));
    assert(text.includes('"$RUNNER_TEMP/browser-emsdk/emsdk" install 4.0.20'));
    assert(text.includes("node web/runtime-build/build.mjs"));
  });
  test("AIR import and canary failures never authenticate the runtime", async () => {
    const text = await readFile(new URL("verify.yml", workflows), "utf8");
    const runtime = text
      .split(/\n {6}- /)
      .find((value) => value.includes("\n        id: runtime\n"));
    const segment = runtime.slice(
      runtime.indexOf("          node web/runtime-build/build-air-compiler.mjs"),
      runtime.indexOf("          rustup toolchain install"),
    );
    assert(segment.includes("air_compiler_verified=true"));
    const temp = await mkdtemp(join(tmpdir(), "air-release-gate-"));
    try {
      for (const status of [0, 2, 7]) {
        const output = join(temp, `output-${status}`);
        const result = spawnSync("bash", ["--noprofile", "--norc", "-eo", "pipefail"], {
          input: `node() { return ${status}; }\n` + segment,
          env: { ...process.env, GITHUB_OUTPUT: output },
          encoding: "utf8",
        });
        assert.equal(result.error, undefined);
        assert.equal(result.signal, null);
        assert.equal(result.status, status, result.stderr);
        if (status === 0)
          assert.equal(await readFile(output, "utf8"), "air_compiler_verified=true\n");
        else await assert.rejects(readFile(output), { code: "ENOENT" });
      }
    } finally {
      await rm(temp, { recursive: true, force: true });
    }
  });
}

// frontend-notices
{
  // 验证实际产物的许可证保留及输入边界；依据构建元数据和已安装包的原文。
  // Verify emitted license preservation and input boundaries using metadata and installed originals.
  const { default: assert } = strictModule;
  const { mkdtemp, readFile, rm } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { resolve } = nodePathModule;
  const { default: test } = nodeTestModule;
  const { writeFrontendNotices } = frontendNoticesModule;

  const metadata = (inputs) => ({ outputs: { "app.js": { inputs } } });

  test("frontend notices retain actual runtime and vendored license bytes, not unused dependencies", async () => {
    const directory = await mkdtemp(resolve(tmpdir(), "frontend-notices-"));
    try {
      await writeFrontendNotices(
        directory,
        metadata({
          "src/index.css": { bytesInOutput: 1 },
          "node_modules/react/index.js": { bytesInOutput: 1 },
          "node_modules/hash-wasm/dist/index.esm.js": { bytesInOutput: 1 },
          "node_modules/@radix-ui/react-compose-refs/dist/index.mjs": { bytesInOutput: 1 },
          "node_modules/react-dom/client.js": { bytesInOutput: 1 },
          "node_modules/scheduler/index.js": { bytesInOutput: 1 },
          "node_modules/@radix-ui/react-checkbox/dist/index.mjs": { bytesInOutput: 1 },
          "node_modules/class-variance-authority/dist/index.mjs": { bytesInOutput: 1 },
          "node_modules/clsx/dist/clsx.mjs": { bytesInOutput: 1 },
          "node_modules/tailwind-merge/dist/bundle-mjs.mjs": { bytesInOutput: 1 },
          "node_modules/lucide-react/dist/cjs/lucide-react.js": { bytesInOutput: 1 },
          "node_modules/typescript/lib/typescript.js": { bytesInOutput: 0 },
        }),
      );
      const notices = await readFile(resolve(directory, "third-party-notices.txt"), "utf8");
      const md5Notice = (
        await readFile(new URL("../node_modules/hash-wasm/src/md5.c", import.meta.url), "utf8")
      ).match(/^\/\*[\s\S]*?\*\//)[0];
      const helperNotice = (
        await readFile(
          new URL("../node_modules/hash-wasm/dist/index.esm.js", import.meta.url),
          "utf8",
        )
      ).match(/\/\*\*+\r?\nCopyright \(c\) Microsoft Corporation\.[\s\S]*?\*\//)[0];
      assert.ok(notices.includes(md5Notice));
      assert.ok(notices.includes(helperNotice));
      for (const file of [
        "tailwindcss/LICENSE",
        "hash-wasm/LICENSE",
        "react/LICENSE",
        "react-dom/LICENSE",
        "scheduler/LICENSE",
        "@radix-ui/react-checkbox/LICENSE",
        "class-variance-authority/LICENSE",
        "clsx/license",
        "tailwind-merge/LICENSE.md",
        "lucide-react/LICENSE",
      ])
        assert.ok(
          notices.includes(
            await readFile(new URL(`../node_modules/${file}`, import.meta.url), "utf8"),
          ),
          file,
        );
      for (const file of ["SHADCN_LICENSE.md"])
        assert.ok(
          notices.includes(await readFile(new URL(`../${file}`, import.meta.url), "utf8")),
          file,
        );
      for (const name of [
        "react@",
        "hash-wasm@4.12.0",
        "react-dom@",
        "scheduler@",
        "@radix-ui/react-checkbox@",
        "class-variance-authority@",
      ])
        assert.ok(notices.includes(name));
      assert.match(notices, /react-compose-refs@1\.1\.2 \(shared upstream/);
      assert.ok(
        notices.includes(
          await readFile(new URL("../node_modules/radix-ui/LICENSE", import.meta.url), "utf8"),
        ),
      );
      assert.doesNotMatch(notices, /typescript@|oxlint@|bun-plugin-tailwind@/);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  test("frontend notices fail closed on missing or malformed bundle metadata", async () => {
    for (const value of [
      undefined,
      {},
      { outputs: {} },
      metadata({}),
      metadata({ x: { bytesInOutput: -1 } }),
      { outputs: { x: {} } },
    ])
      await assert.rejects(writeFrontendNotices(tmpdir(), value), /metadata|inputs|contribution/);
  });
}

// isolated-run-summary
{
  // 包装器允许增减有效用例，但不能把空跑、跳过、失败或中断当作通过。
  // Wrappers permit changing valid case counts, but never accept empty, skipped, failed or interrupted runs.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;
  function result(overrides = {}, process = {}) {
    const totals = { tests: 3, pass: 3, fail: 0, cancelled: 0, skipped: 0, todo: 0, ...overrides };
    return {
      status: 0,
      stdout:
        Object.entries(totals)
          .map(([key, value]) => `# ${key} ${value}`)
          .join("\n") + "\n",
      stderr: "",
      ...process,
    };
  }
  test("isolated wrapper accepts any nonzero all-passing count", () => {
    for (const count of [1, 8, 13, 35, 80, 100])
      assert.doesNotThrow(() => assertIsolatedRun(result({ tests: count, pass: count })));
    assert.doesNotThrow(() =>
      assertIsolatedRun(result({}, { stdout: result().stdout.replaceAll("\n", "\r\n") })),
    );
  });
  test("isolated wrapper rejects incomplete, skipped, failed and empty execution", () => {
    for (const totals of [
      { tests: 0, pass: 0 },
      { pass: 2 },
      { fail: 1 },
      { cancelled: 1 },
      { skipped: 1 },
      { todo: 1 },
    ])
      assert.throws(() => assertIsolatedRun(result(totals)));
    for (const process of [
      { status: 1 },
      { status: null },
      { error: new Error("timeout") },
      { stdout: "" },
      { stdout: result().stdout + "# tests 3\n" },
      { stdout: result().stdout.replace("# pass 3\n", "") },
    ])
      assert.throws(() => assertIsolatedRun(result({}, process)));
  });
}

// bun-e2e-fs
{
  // 仅使用合成临时文件，不运行私有输入打包器。
  // Use synthetic temporary files only, never the private-input runner.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { mkdtemp, rm, readFile, writeFile, rename, mkdir, readdir } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { DirectoryHandle } = bunE2eFsModule;
  const { cleanupBunE2e } = bunE2eModule;
  async function fixture(run) {
    const path = await mkdtemp(join(tmpdir(), "d2r-synthetic-fs-"));
    try {
      await run(new DirectoryHandle(path), path);
    } finally {
      await rm(path, { recursive: true, force: true });
    }
  }
  test("handle identity survives own writes but rejects path replacements", () =>
    fixture(async (root, path) => {
      const file = await root.getFileHandle("a", { create: true });
      assert.equal(await file.isSameEntry(await root.getFileHandle("a")), true);
      const writer = await file.createWritable();
      await writer.write(Uint8Array.of(1, 2, 3));
      await writer.close();
      assert.deepEqual(await readFile(join(path, "a")), Buffer.from([1, 2, 3]));
      assert.equal(await file.isSameEntry(await root.getFileHandle("a")), true);
      await writeFile(join(path, "replacement"), "other");
      await rename(join(path, "replacement"), join(path, "a"));
      assert.equal(await file.isSameEntry(await root.getFileHandle("a")), false);
      assert.equal(await file.isSameEntry({ path: file.path, kind: "file" }), false);
      const dir = await root.getDirectoryHandle("dir", { create: true });
      assert.equal(await dir.isSameEntry(await root.getDirectoryHandle("dir")), true);
      await rename(join(path, "dir"), join(path, "old"));
      await mkdir(join(path, "dir"));
      assert.equal(await dir.isSameEntry(await root.getDirectoryHandle("dir")), false);
    }));
  test("nonrecursive directory removal preserves nonempty directories", () =>
    fixture(async (root, path) => {
      await root.getDirectoryHandle("empty", { create: true });
      await root.removeEntry("empty");
      const full = await root.getDirectoryHandle("full", { create: true });
      await full.getFileHandle("keep", { create: true });
      await assert.rejects(root.removeEntry("full"), { name: "InvalidModificationError" });
      assert.deepEqual(await readdir(join(path, "full")), ["keep"]);
    }));
  test("failed rename leaves temporary file recoverable by abort", () =>
    fixture(async (root, path) => {
      const file = await root.getFileHandle("a", { create: true });
      const writer = await file.createWritable();
      await writer.write(Uint8Array.of(7));
      await rm(join(path, "a"));
      await mkdir(join(path, "a"));
      await assert.rejects(writer.close());
      await writer.abort();
      await writer.abort();
      assert.deepEqual(await readdir(path), ["a"]);
    }));
  test("cleanup drains every release and retains the primary cause", async () => {
    const primary = new Error("synthetic primary"),
      calls = [];
    await assert.rejects(
      cleanupBunE2e(
        [
          () => {
            calls.push(1);
            throw new Error("close");
          },
          () => {
            calls.push(2);
          },
          async () => {
            calls.push(3);
            throw new Error("remove");
          },
          () => {
            calls.push(4);
          },
        ],
        primary,
      ),
      (error) =>
        error.message === "BUN_E2E_CLEANUP_FAILED" &&
        error.cause === primary &&
        error.errors.length === 2,
    );
    assert.deepEqual(calls, [1, 2, 3, 4]);
  });
}

// bun-e2e-lifecycle
{
  // 仅合成输出和启动失败，不打开私有输入或启动转换。
  // Synthetic outputs and startup failures only; no private inputs or conversion.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { mkdtemp, mkdir, writeFile, readFile, rm, symlink, realpath } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { createHash } = nodeCryptoModule;
  const { gzipSync } = nodeZlibModule;
  const { replaceBunGlobal, requireUnusedOutput, writeBunOutput, resolvePreviewFile, run } =
    bunE2eModule;
  async function fixture(operation) {
    const root = await mkdtemp(join(tmpdir(), "d2r-synthetic-run-"));
    try {
      await operation(root);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  }
  test("exclusive output rejects existing files and symlinks, including a precheck race", () =>
    fixture(async (root) => {
      const output = join(root, "output.ipa");
      await requireUnusedOutput(output);
      await writeFile(output, "keep");
      await assert.rejects(writeBunOutput(output, new Blob(["new"])), { code: "EEXIST" });
      await assert.rejects(requireUnusedOutput(output), /BUN_E2E_OUTPUT_EXISTS/);
      assert.equal(await readFile(output, "utf8"), "keep");
      const alias = join(root, "alias.ipa");
      await symlink(output, alias);
      await assert.rejects(writeBunOutput(alias, new Blob(["new"])), { code: "EEXIST" });
      const fresh = join(root, "fresh.ipa");
      await writeBunOutput(fresh, new Blob(["synthetic"]));
      assert.equal(await readFile(fresh, "utf8"), "synthetic");
    }));
  test("global replacement restores original descriptors and absence", () => {
    const target = {};
    Object.defineProperty(target, "value", { configurable: true, get: () => 42 });
    Object.defineProperty(target, "fixed", {
      value: 1,
      writable: true,
      configurable: false,
      enumerable: true,
    });
    const fixed = Object.getOwnPropertyDescriptor(target, "fixed");
    const restoreFixed = replaceBunGlobal(target, "fixed", 2);
    assert.deepEqual(Object.getOwnPropertyDescriptor(target, "fixed"), { ...fixed, value: 2 });
    restoreFixed();
    assert.deepEqual(Object.getOwnPropertyDescriptor(target, "fixed"), fixed);
    const before = Object.getOwnPropertyDescriptor(target, "value");
    const restore = replaceBunGlobal(target, "value", 7);
    assert.equal(target.value, 7);
    restore();
    assert.deepEqual(Object.getOwnPropertyDescriptor(target, "value"), before);
    replaceBunGlobal(target, "absent", 1)();
    assert.equal(Object.hasOwn(target, "absent"), false);
  });
  test("setup failure restores globals, stops server, revokes URL and permits another run", () =>
    fixture(async (root) => {
      const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
      const source = Buffer.from("// synthetic worker, never executed");
      const compressed = gzipSync(source);
      const runtime = join(root, "runtime");
      await mkdir(runtime);
      const manifest = Buffer.from(
        JSON.stringify({
          assets: [
            { path: "sealed-workers/nsp-worker.mjs", bytes: source.length, sha256: hash(source) },
            { path: "sealed-workers/cross-worker.mjs", bytes: source.length, sha256: hash(source) },
          ],
          transport: {
            entries: [{ sha256: hash(source), bytes: source.length, offset: 0, file: "chunk.gz" }],
            files: [{ path: "chunk.gz", sha256: hash(compressed) }],
          },
        }),
      );
      await writeFile(join(runtime, "runtime-manifest.json"), manifest);
      await writeFile(
        join(runtime, "runtime-integrity.mjs"),
        `export const runtimeManifestSha256='${hash(manifest)}';`,
      );
      await writeFile(join(runtime, "chunk.gz"), compressed);
      const fakeNavigator = {};
      Object.defineProperty(fakeNavigator, "storage", { value: "original", configurable: false });
      let stops = 0,
        revokes = 0;
      const restores = [];
      const originals = new Map(
        ["Worker", "location", "isSecureContext"].map((key) => [
          key,
          Object.getOwnPropertyDescriptor(globalThis, key),
        ]),
      );
      try {
        restores.push(replaceBunGlobal(globalThis, "Worker", class {}));
        const worker = globalThis.Worker;
        restores.push(replaceBunGlobal(globalThis, "navigator", fakeNavigator));
        // Bun 自身不可写，但公开方法可替换；Node 下仅创建合成命名空间。
        // Bun itself is readonly, but its public methods are writable; create a synthetic namespace only in Node.
        if (!globalThis.Bun) restores.push(replaceBunGlobal(globalThis, "Bun", {}));
        restores.push(replaceBunGlobal(globalThis.Bun, "file", () => ({ text: async () => "" })));
        restores.push(
          replaceBunGlobal(globalThis.Bun, "serve", () => ({
            url: new URL("http://127.0.0.1:1234"),
            stop() {
              stops++;
            },
          })),
        );
        const revoke = URL.revokeObjectURL;
        restores.push(
          replaceBunGlobal(URL, "revokeObjectURL", (url) => {
            revokes++;
            revoke(url);
          }),
        );
        for (let attempt = 0; attempt < 2; attempt++)
          await assert.rejects(
            run({
              preview: root,
              input: "unused",
              output: join(root, "new.ipa"),
              nsps: ["synthetic.nsp"],
            }),
            TypeError,
          );
        assert.equal(stops, 2);
        assert.equal(revokes, 2);
        assert.equal(globalThis.Worker, worker);
        for (const key of ["location", "isSecureContext"])
          assert.deepEqual(Object.getOwnPropertyDescriptor(globalThis, key), originals.get(key));
        assert.equal(fakeNavigator.storage, "original");
      } finally {
        for (const restore of restores.reverse()) restore();
      }
    }));

  test("preview serves confined real targets and rejects external symlinks", () =>
    fixture(async (root) => {
      const preview = join(root, "preview");
      await mkdir(preview);
      const inside = join(preview, "asset");
      await writeFile(inside, "public synthetic");
      const outside = join(root, "outside");
      await writeFile(outside, "private synthetic");
      await symlink(outside, join(preview, "escape"));
      await symlink(inside, join(preview, "alias"));
      const previewRoot = await realpath(preview);
      assert.equal(await resolvePreviewFile(previewRoot, inside), await realpath(inside));
      assert.equal(
        await resolvePreviewFile(previewRoot, join(preview, "alias")),
        await realpath(inside),
      );
      assert.equal(await resolvePreviewFile(previewRoot, join(preview, "escape")), null);
    }));
}

// bun-e2e-node-worker
{
  // 合成子进程测试，不使用转换器二进制或私有输入。
  // Synthetic child-process tests, without converter binaries or private inputs.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { mkdtemp, writeFile, rm, readFile } = promisesModule;
  const { join } = nodePathModule;
  const { tmpdir } = nodeOsModule;
  const { once } = nodeEventsModule;
  const { NodeCrossWorker } = bunE2eNodeCrossModule;
  const { readNativeOutput, runNativeCommand, stopNativeChildren } = bunE2eNodeCrossModule;
  async function fixture(action) {
    const root = await mkdtemp(join(tmpdir(), "d2r-synthetic-cross-"));
    try {
      await action(root);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  }
  test("native output rejects oversized files before returning text", () =>
    fixture(async (root) => {
      const file = join(root, "text");
      await writeFile(file, "1234");
      assert.equal(await readNativeOutput(file, 4), "1234");
      await assert.rejects(readNativeOutput(file, 3), /OUTPUT_LIMIT/);
    }));
  test("worker receives synthetic response and termination clears retained state", () =>
    fixture(async (root) => {
      const source = join(root, "worker.mjs");
      await writeFile(
        source,
        `self.addEventListener('message',({data})=>self.postMessage({id:data.id,ok:true}));`,
      );
      const worker = new NodeCrossWorker(source);
      try {
        const response = new Promise((resolve, reject) => {
          worker.onmessage = resolve;
          worker.onerror = reject;
        });
        worker.postMessage({ id: 0 });
        assert.equal((await response).data.ok, true);
        const exited = once(worker.child, "exit");
        worker.terminate();
        await exited;
        assert.equal(worker.closed, true);
        assert.equal(worker.buffer, "");
        assert.equal(worker.listeners.size, 0);
      } finally {
        worker.terminate();
      }
    }));
  test("stdin failure terminates even when an observer throws", () =>
    fixture(async (root) => {
      const source = join(root, "worker.mjs");
      await writeFile(source, `self.addEventListener('message',()=>{});`);
      const worker = new NodeCrossWorker(source);
      worker.onerror = () => {
        throw new Error("observer");
      };
      const exited = once(worker.child, "exit");
      worker.child.stdin.emit(
        "error",
        Object.assign(new Error("synthetic pipe"), { code: "EPIPE" }),
      );
      assert.equal(worker.closed, true);
      await exited;
    }));
  test("owned child ignoring SIGTERM is killed and drained before cleanup", () =>
    fixture(async (root) => {
      const marker = join(root, "ready");
      // 使用 Node 本身运行固定合成脚本，不调用 git 或真实编译器。
      // Run a fixed synthetic script with Node itself, never git or a real compiler.
      const result = runNativeCommand("node", [
        "-e",
        `process.on('SIGTERM',()=>{});require('node:fs').writeFileSync(process.argv[1],String(process.pid));setInterval(()=>{},1000);`,
        marker,
      ]).catch((error) => error);
      for (let i = 0; i < 200; i++) {
        if (await readFile(marker, "utf8").catch(() => null)) break;
        await new Promise((resolve) => setTimeout(resolve, 10));
      }
      const pid = Number(await readFile(marker, "utf8"));
      await Promise.all([stopNativeChildren(), stopNativeChildren()]);
      assert.ok((await result) instanceof Error);
      assert.throws(() => process.kill(pid, 0), { code: "ESRCH" });
    }));
}
