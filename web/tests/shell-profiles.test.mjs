// Feature suite: shell-profiles. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as v200JsonModule from "../toolchain/shell-release-evidence/v2.0.0.json" with { type: "json" };
import * as shellContractModule from "../shell-contract.mjs";
import * as nodeChild_processModule from "node:child_process";
import * as shellLoaderModule from "../shell-loader.mjs";
import * as zipWriterModule from "../zip-writer.mjs";
import * as shellProfilesModule from "../shell-profiles.mjs";
import * as d2rRuntimeContentModeModule from "../d2r-runtime-content-mode.mjs";
import * as shellTargetModule from "../shell-target.mjs";
import * as promisesModule from "node:fs/promises";
import * as shellV2EvidenceModule from "../shell-v2-evidence.mjs";
import * as nodeCryptoModule from "node:crypto";
import * as typescriptModule from "typescript";
import * as productionProfileModule from "../production-profile.mjs";

// shell-contract
{
  // 验证 shell-contract 的约定与边界；依据 ../toolchain/shell-release-evidence/v2.0.0.json、../shell-contract.mjs 及本文件的测试断言。
  // Verify shell-contract contracts and boundaries against ../toolchain/shell-release-evidence/v2.0.0.json、../shell-contract.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { default: releaseEvidence } = v200JsonModule;
  const { validateShellContract, validateAssemblyInventory, SHELL_LIMITS } = shellContractModule;
  const base = "Payload/DmIOS.app/";
  const entry = (path, size = 10, mode = 0o100644) => ({ path, size, mode });
  const requiredPaths = ["dm/derived.tsv", "Frameworks/Guest.framework/", "msl/b-test/"];
  function fixture() {
    const targetOS = { platform: "ios", minimumVersion: "16.0" };
    return {
      shell: {
        entries: ["Info.plist", "DmIOS", "dm/shell_info.txt", "dm/shell_manifest.txt"].map((p) =>
          entry(base + p),
        ),
        shellInfoText:
          "PRODUCT=synthetic\nCHAIN=test\nKIND=ios\nSHADER_SRC=0123456789abcdef\nSPIRV_CROSS=synthetic\n",
        shellManifestText: requiredPaths.join("\n"),
        trustedRecipe: {
          schemaVersion: 1,
          product: "synthetic",
          chain: "test",
          kind: "ios",
          shaderDigest: "0123456789abcdef",
          targetOS,
          requiredPaths,
          extraFiles: ["dm/pack_info.txt"],
        },
        observedTargetOS: targetOS,
      },
      generatedEntries: [
        entry("dm/derived.tsv"),
        {
          ...entry("Frameworks/Guest.framework/Guest", 100, 0o100755),
          signature: { kind: "ad-hoc", offset: 80, size: 20 },
        },
        entry("Frameworks/Guest.framework/Info.plist"),
        entry("msl/b-test/synthetic.metal"),
        entry("dm/pack_info.txt"),
      ],
      observedOutputTargetOS: targetOS,
    };
  }
  test("metadata preserves executable Guest and signature without installability claims", () => {
    const f = fixture(),
      r = validateAssemblyInventory(f);
    assert.equal(r.status, "assembly-inventory-valid");
    assert.equal(r.installable, false);
    assert.equal(r.contentVerified, false);
    const g = r.entries.find((e) => e.path.endsWith("/Guest"));
    assert.equal(g.mode, 0o100755);
    assert.deepEqual(g.signature, { kind: "ad-hoc", offset: 80, size: 20 });
    g.signature.offset = 0;
    assert.equal(f.generatedEntries[1].signature.offset, 80);
  });
  test("strict recipe, independent target observations and text", () => {
    for (const mutate of [
      (f) => {
        delete f.shell.trustedRecipe;
      },
      (f) => {
        f.shell.trustedRecipe.schemaVersion = 2;
      },
      (f) => {
        delete f.shell.observedTargetOS;
      },
      (f) => {
        f.shell.observedTargetOS = { platform: "ios", minimumVersion: "17.0" };
      },
      (f) => {
        f.observedOutputTargetOS = { platform: "ios", minimumVersion: "15.0" };
      },
      (f) => {
        f.shell.trustedRecipe.kind = "mac";
      },
      (f) => {
        f.shell.shellInfoText += "VERSION=2\n";
      },
      (f) => {
        f.shell.shellInfoText += "PRODUCT=synthetic\n";
      },
      (f) => {
        f.shell.shellInfoText = f.shell.shellInfoText.replace("test", "other");
      },
      (f) => {
        f.shell.shellInfoText = f.shell.shellInfoText.replace(
          "0123456789abcdef",
          "ffffffffffffffff",
        );
      },
      (f) => {
        f.shell.shellManifestText += "\nunknown/";
      },
    ]) {
      const f = fixture();
      mutate(f);
      assert.throws(() => validateAssemblyInventory(f));
    }
  });
  test("one app and required shell files", () => {
    for (const extra of ["Payload/Other.app/Info.plist", "__MACOSX/file", "outside"]) {
      const f = fixture();
      f.shell.entries.push(entry(extra));
      assert.throws(() => validateShellContract(f.shell));
    }
    const f = fixture();
    f.shell.entries.shift();
    assert.throws(() => validateShellContract(f.shell), /Missing required file/);
  });
  test("pinned production shell accepts Apple icon density and device suffixes", () => {
    const targetOS = {
      platform: "ios",
      minimumVersion: releaseEvidence.machOBuildVersions[0].minimumVersion,
    };
    const shell = {
      entries: releaseEvidence.entries,
      shellInfoText: Object.entries(releaseEvidence.shellInfo)
        .map(([key, value]) => `${key}=${value}`)
        .join("\n"),
      shellManifestText: releaseEvidence.requiredPaths.join("\n"),
      trustedRecipe: {
        schemaVersion: 1,
        product: releaseEvidence.shellInfo.PRODUCT,
        chain: releaseEvidence.shellInfo.CHAIN,
        kind: "ios",
        shaderDigest: releaseEvidence.shellInfo.SHADER_SRC,
        targetOS,
        requiredPaths: releaseEvidence.requiredPaths,
        extraFiles: ["dm/pack_info.txt", "dm/content_mode.json"],
      },
      observedTargetOS: targetOS,
    };
    const result = validateShellContract(shell);
    assert.equal(result.status, "shell-inventory-valid");
    for (const icon of ["AppIcon60x60@2x.png", "AppIcon76x76@2x~ipad.png"])
      assert.ok(result.entries.some((entry) => entry.path === base + icon));
    assert.deepEqual(
      result.entries.map((entry) => entry.path),
      shell.entries.map((entry) => entry.path),
    );
  });
  test("unsafe paths, duplicate entries, case collisions and ancestor conflicts", () => {
    for (const p of [
      "../escape",
      "/absolute",
      "a\\b",
      "a//b",
      "a/./b",
      "a/../b",
      "a\0b",
      "a:b",
      "a./b",
      "a /b",
      "@2x/../escape.png",
      "~ipad/./escape.png",
      "AppIcon@2x~ipad.png/../../escape.png",
      "AppIcon@2x~ipad.png\\escape",
      "AppIcon@2x~ipad.png:stream",
      "AppIcon@2x~ipad.png%2fescape",
      "AppIcon@2x~ipad.png?query",
      "AppIcon@2x~ipad.png#fragment",
      "AppIcon@2x~ipad.png.",
      "AppIcon@2x~ipad.png ",
      "dm/shell_info.txt",
      "DM/extra",
      "Info.plist/child",
    ]) {
      const f = fixture();
      f.shell.entries.push(entry(base + p));
      assert.throws(() => validateShellContract(f.shell), p);
    }
    const f = fixture();
    f.generatedEntries.push(f.generatedEntries[0]);
    assert.throws(() => validateAssemblyInventory(f), /Duplicate/);
  });
  test("reject symlinks, special files, credentials, signing and bulk archives", () => {
    for (const mode of [0o120777, 0o020600, 0o104755]) {
      const f = fixture();
      f.shell.entries.push(entry(base + "bad", 10, mode));
      assert.throws(() => validateShellContract(f.shell));
    }
    for (const p of [
      "embedded.mobileprovision",
      "_CodeSignature/CodeResources",
      "Extensions/Bad",
      "PlugIns/Bad",
      "RomFS/data/file",
      "dm/00-00000000.data",
      "secret.keys",
      "file.nsp",
      "cert.p12",
    ]) {
      const f = fixture();
      f.shell.entries.push(entry(base + p));
      assert.throws(() => validateShellContract(f.shell), /Forbidden/);
    }
  });
  test("only bounded generated overlay delta archives are allowed", () => {
    const allowOverlay = (value) => {
      value.shell.trustedRecipe.requiredPaths = [
        ...value.shell.trustedRecipe.requiredPaths,
        "romfs_overlay/",
      ];
      value.shell.shellManifestText += "\nromfs_overlay/";
    };
    const f = fixture();
    allowOverlay(f);
    f.generatedEntries.push(entry("romfs_overlay/data/00-00000002.data", 1024));
    assert.equal(validateAssemblyInventory(f).status, "assembly-inventory-valid");

    const shellArchive = fixture();
    shellArchive.shell.entries.push(entry(base + "romfs_overlay/data/00-00000002.data"));
    assert.throws(() => validateShellContract(shellArchive.shell), /Forbidden/);

    for (const path of [
      "dm/data/00-00000002.data",
      "romfs_overlay/other/00-00000002.data",
      "romfs_overlay/data/00-00000002.data/child",
    ]) {
      const bad = fixture();
      allowOverlay(bad);
      bad.generatedEntries.push(entry(path, 1024));
      assert.throws(() => validateAssemblyInventory(bad));
    }
    const oversized = fixture();
    allowOverlay(oversized);
    oversized.generatedEntries.push(
      entry("romfs_overlay/data/00-00000002.data", 128 * 1024 * 1024 + 1),
    );
    assert.throws(() => validateAssemblyInventory(oversized), /Forbidden/);
  });
  test("manifest completeness, no empty directories and no shell overwrite", () => {
    for (const remove of [
      "dm/derived.tsv",
      "msl/b-test/synthetic.metal",
      "Frameworks/Guest.framework/Guest",
    ]) {
      const f = fixture();
      f.generatedEntries = f.generatedEntries.filter((e) => e.path !== remove);
      assert.throws(() => validateAssemblyInventory(f));
    }
    const f = fixture();
    f.generatedEntries = f.generatedEntries.filter((e) => !e.path.startsWith("msl/"));
    f.generatedEntries.push(entry("msl/b-test/", 0, 0o40755));
    assert.throws(() => validateAssemblyInventory(f), /empty required directory/);
    const c = fixture();
    c.shell.entries.push(entry(base + "dm/pack_info.txt"));
    assert.throws(() => validateAssemblyInventory(c), /Duplicate/);
    const l = fixture();
    l.shell.entries.push(entry(base + "dm/derived.tsv"));
    assert.throws(() => validateAssemblyInventory(l), /already contains/);
  });
  test("Guest signature range and executable permissions required", () => {
    for (const change of [
      (g) => {
        g.mode = 0o100644;
      },
      (g) => {
        delete g.signature;
      },
      (g) => {
        g.signature.size = 21;
      },
      (g) => {
        g.signature.kind = "developer";
      },
    ]) {
      const f = fixture();
      change(f.generatedEntries[1]);
      assert.throws(() => validateAssemblyInventory(f));
    }
  });
  test("bounded inputs and generated allowlist", () => {
    const f = fixture();
    f.generatedEntries.push(entry("unapproved"));
    assert.throws(() => validateAssemblyInventory(f), /outside trusted/);
    for (const size of [-1, NaN, Infinity, SHELL_LIMITS.bytes + 1]) {
      const f = fixture();
      f.shell.entries[0].size = size;
      assert.throws(() => validateShellContract(f.shell));
    }
    const b = fixture();
    b.shell.shellInfoText = "x".repeat(SHELL_LIMITS.text + 1);
    assert.throws(() => validateShellContract(b.shell), /oversized/);
    const m = fixture();
    m.shell.entries = Array(SHELL_LIMITS.entries + 1).fill(entry(base + "x"));
    assert.throws(() => validateShellContract(m.shell), /limit/);
  });
  test("explicit parent directories and equivalent target patch zero", () => {
    const f = fixture();
    f.shell.entries.unshift(entry("Payload/", 0, 0o40755), entry(base, 0, 0o40755));
    f.observedOutputTargetOS = { platform: "ios", minimumVersion: "16.0.0" };
    assert.equal(validateAssemblyInventory(f).status, "assembly-inventory-valid");
  });

  // 四种生成覆盖路径共享规范 0–255 编号；外壳本身仍不可携带归档。
  // All four generated overlay paths share canonical 0–255 numbering; shell archives remain forbidden.
  for (const root of [
    "romfs_overlay",
    "romfs_overlay_variants/item_notes",
    "romfs_overlay_astc",
    "romfs_overlay_astc_variants/item_notes",
  ]) {
    test(`generated ${root} accepts canonical two/three-digit chunks only`, () => {
      const setup = (name, size = 1024) => {
        const f = fixture();
        f.shell.trustedRecipe.requiredPaths = [...f.shell.trustedRecipe.requiredPaths, root + "/"];
        f.shell.shellManifestText += "\n" + root + "/";
        f.generatedEntries.push(entry(`${root}/data/${name}`, size));
        return f;
      };
      for (const name of [
        "00-00000002.data",
        "99-00000255.data",
        "100-00000002.data",
        "255-00000255.data",
      ])
        assert.equal(validateAssemblyInventory(setup(name)).status, "assembly-inventory-valid");
      assert.equal(
        validateAssemblyInventory(setup("255-00000002.data", 128 * 1024 * 1024)).status,
        "assembly-inventory-valid",
      );
      for (const name of [
        "0-00000002.data",
        "100-0000002.data",
        "000-00000002.data",
        "099-00000002.data",
        "256-00000002.data",
        "100-00000256.data",
        "100-00000002.DATA",
        "100-00000002.data/child",
      ])
        assert.throws(() => validateAssemblyInventory(setup(name)), /Forbidden/);
      for (const size of [0, 128 * 1024 * 1024 + 1])
        assert.throws(
          () => validateAssemblyInventory(setup("100-00000002.data", size)),
          /Forbidden/,
        );
      const shell = fixture();
      shell.shell.entries.push(entry(`${base}${root}/data/100-00000002.data`));
      assert.throws(() => validateShellContract(shell.shell), /Forbidden/);
    });
  }
}

// shell-loader
{
  // 验证 shell-loader 的约定与边界；依据 ../shell-loader.mjs、../shell-contract.mjs 及本文件的测试断言。
  // Verify shell-loader contracts and boundaries against ../shell-loader.mjs、../shell-contract.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { execFileSync } = nodeChild_processModule;
  const { default: test } = nodeTestModule;
  const { createShellLoader, SHELL_OUTPUT_CHUNK_BYTES } = shellLoaderModule;
  const { validateShellContract } = shellContractModule;
  const { writeZip } = zipWriterModule;
  const fixture = JSON.parse(
    execFileSync(
      "python3",
      [
        "-c",
        String.raw`
import hashlib, io, json, zipfile
base = 'Payload/Demo.app/'
required = ['dm/derived.tsv', 'Frameworks/Guest.framework/', 'msl/b-test/']
contents = {
 'Payload/': b'', base: b'',
 base + 'Info.plist': b'synthetic-plist:ios:16.0',
 base + 'Demo': b'synthetic-executable:ios:16.0',
 base + 'dm/shell_info.txt': b'PRODUCT=synthetic\nCHAIN=test\nKIND=ios\nSHADER_SRC=0123456789abcdef\n',
 base + 'dm/shell_manifest.txt': '\n'.join(required).encode(),
 base + 'AppIcon60x60@2x.png': b'synthetic-icon',
 base + 'AppIcon76x76@2x~ipad.png': b'synthetic-ipad-icon',
 base + 'public.bin': bytes(range(256)) * 512,
}
def mode(path): return 0o40755 if path.endswith('/') else (0o100755 if path.endswith('/Demo') else 0o100644)
def make(changed=False):
 b = io.BytesIO()
 with zipfile.ZipFile(b, 'w') as z:
  for path, value in contents.items():
   info = zipfile.ZipInfo(path)
   info.external_attr = mode(path) << 16
   if changed and path.endswith('Info.plist'): value = b'synthetic-plist:ios:17.0'
   z.writestr(info, value)
 return b.getvalue().hex()
files = [{'path': p, 'size': len(v), 'mode': mode(p), 'sha256': hashlib.sha256(v).hexdigest()} for p,v in contents.items()]
print(json.dumps({'zip': make(), 'changed': make(True), 'files': files, 'requiredPaths': required}))
`,
      ],
      { encoding: "utf8", maxBuffer: 2 * 1024 * 1024 },
    ),
  );
  const base = "Payload/Demo.app/";
  function configuration(kind = "zip") {
    const bytes = Uint8Array.from(Buffer.from(fixture[kind], "hex"));
    const source = {
      size: bytes.length,
      async read(offset, length) {
        return bytes.slice(offset, offset + length);
      },
    };
    const release = {
      verified: true,
      releaseId: "synthetic-only",
      files: structuredClone(fixture.files),
      trustedRecipe: {
        schemaVersion: 1,
        product: "synthetic",
        chain: "test",
        kind: "ios",
        shaderDigest: "0123456789abcdef",
        targetOS: { platform: "ios", minimumVersion: "16.0" },
        requiredPaths: [...fixture.requiredPaths],
        extraFiles: [],
      },
    };
    return {
      source,
      releaseId: release.releaseId,
      release,
      resolveTrustedRelease: async () => release,
      async inspectTargetOS(context) {
        assert.equal(context.trustedRecipe, undefined);
        const plist = new TextDecoder().decode(await context.readEntry(base + "Info.plist"));
        const executable = new TextDecoder().decode(await context.readEntry(base + "Demo"));
        // 仅为合成解析器，不是生产 Mach-O/plist 实现。
        // Synthetic parser only, not a production Mach-O/plist implementation.
        assert.equal(plist, "synthetic-plist:ios:16.0");
        assert.equal(executable, "synthetic-executable:ios:16.0");
        return { platform: "ios", minimumVersion: "16.0" };
      },
    };
  }
  async function collect(entry) {
    const chunks = [];
    for await (const chunk of entry.chunks()) chunks.push(chunk.slice());
    return Buffer.concat(chunks);
  }
  test("real ZIP bridges to contract and existing writer with verified bounded bytes", async () => {
    const loaded = await createShellLoader(configuration())();
    assert.equal(validateShellContract(loaded.contract).status, "shell-inventory-valid");
    assert.equal(loaded.entries.length, fixture.files.length - 2);
    assert.ok(loaded.entries.every((e) => !e.path.endsWith("/")));
    assert.equal(loaded.contentVerified, false);
    assert.equal(loaded.bytesVerifiedOnRead, true);
    assert.equal(loaded.installable, false);
    const large = loaded.entries.find((e) => e.path.endsWith("public.bin"));
    let total = 0,
      count = 0;
    for await (const bytes of large.chunks()) {
      assert.ok(bytes.length <= SHELL_OUTPUT_CHUNK_BYTES);
      total += bytes.length;
      count++;
    }
    assert.equal(total, 131072);
    assert.equal(count, 2);
    let written = 0,
      closed = false;
    await writeZip(
      loaded.entries.map((e) => ({ ...e, mode: e.mode & 0o777 })),
      {
        async write(bytes) {
          written += bytes.length;
        },
        async close() {
          closed = true;
        },
        async abort() {
          assert.fail("unexpected abort");
        },
      },
    );
    assert.ok(written > total);
    assert.equal(closed, true);
  });
  test("missing capabilities or unverified releases fail before ZIP I/O", async () => {
    for (const field of ["resolveTrustedRelease", "inspectTargetOS"]) {
      const config = configuration();
      delete config[field];
      assert.throws(() => createShellLoader(config), /CAPABILITY_UNAVAILABLE/);
    }
    for (const mutate of [
      (r) => {
        r.verified = false;
      },
      (r) => {
        r.releaseId = "wrong";
      },
      (r) => {
        r.files[0].sha256 = "invalid";
      },
    ]) {
      const config = configuration();
      mutate(config.release);
      config.source.read = () => assert.fail("must not read before release validation");
      await assert.rejects(createShellLoader(config)(), /RELEASE/);
    }
  });
  test("exact trusted inventory rejects omitted or altered entry metadata", async () => {
    for (const mutate of [
      (r) => {
        r.files.pop();
      },
      (r) => {
        r.files[2].mode = 0o100755;
      },
      (r) => {
        r.files[2].size++;
      },
      (r) => {
        r.files[2].path = base + "other";
      },
    ]) {
      const config = configuration();
      mutate(config.release);
      await assert.rejects(createShellLoader(config)(), /INVENTORY_MISMATCH/);
    }
  });
  test("correct CRC cannot bypass trusted SHA-256 and no unverified bytes yield", async () => {
    await assert.rejects(createShellLoader(configuration("changed"))(), /TARGET_INSPECTION_FAILED/);
    const config = configuration();
    config.release.files.find((e) => e.path.endsWith("public.bin")).sha256 = "0".repeat(64);
    const loaded = await createShellLoader(config)();
    let yielded = false;
    await assert.rejects(async () => {
      for await (const chunk of loaded.entries.find((e) => e.path.endsWith("public.bin")).chunks())
        yielded = chunk.length > 0;
    }, /DIGEST_MISMATCH/);
    assert.equal(yielded, false);
  });
  test("recipe and independent observed target mismatches reject", async () => {
    const config = configuration();
    config.release.trustedRecipe.chain = "other";
    await assert.rejects(createShellLoader(config)(), /mismatch/);
    const wrongTarget = configuration();
    wrongTarget.inspectTargetOS = async () => ({ platform: "ios", minimumVersion: "17.0" });
    await assert.rejects(createShellLoader(wrongTarget)(), /targetOS version mismatch/);
  });
  test("metadata and recipe are detached frozen snapshots", async () => {
    const config = configuration(),
      loaded = await createShellLoader(config)();
    config.release.trustedRecipe.requiredPaths.push("unexpected");
    config.release.files[2].sha256 = "0".repeat(64);
    assert.equal(loaded.contract.trustedRecipe.requiredPaths.length, 3);
    assert.ok(Object.isFrozen(loaded.contract.trustedRecipe.targetOS));
    assert.ok(Object.isFrozen(loaded.entries[0]));
    assert.equal(
      (await collect(loaded.entries.find((e) => e.path.endsWith("Info.plist")))).toString(),
      "synthetic-plist:ios:16.0",
    );
  });
  test("cancellation and callback errors do not echo private failure details", async () => {
    const config = configuration();
    config.resolveTrustedRelease = () => {
      throw new Error("private callback data");
    };
    await assert.rejects(
      createShellLoader(config)(),
      (e) => e.message === "SHELL_RELEASE_UNAVAILABLE",
    );
    const controller = new AbortController();
    controller.abort("private abort reason");
    await assert.rejects(
      createShellLoader(configuration())({ signal: controller.signal }),
      /SHELL_CANCELLED/,
    );
    const live = new AbortController(),
      loaded = await createShellLoader(configuration())({ signal: live.signal });
    live.abort("private abort reason");
    await assert.rejects(collect(loaded.entries[0]), /SHELL_CANCELLED/);
  });
  test("simultaneous output streams cannot accumulate decoded entries", async () => {
    const loaded = await createShellLoader(configuration())();
    const entry = loaded.entries.find((e) => e.path.endsWith("public.bin"));
    const iterator = entry.chunks();
    await iterator.next();
    await assert.rejects(collect(entry), /CONCURRENT_READ_UNSUPPORTED/);
    await iterator.return();
    assert.equal((await collect(entry)).length, 131072);
  });
}

// shell-profiles
{
  // 公开外壳配置及数据派生的合成拒绝检查，不使用游戏或外壳二进制。
  // Synthetic rejection checks for public profiles and data derivation, without game or shell binaries.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const {
    DEFAULT_SHELL_PROFILE_ID,
    SHELL_PROFILE_IDS,
    BUNDLED_SHELL_PROFILE_IDS,
    resolveShellProfile,
  } = shellProfilesModule;
  const { D2R_RUNTIME_CONTENT_PIN, deriveD2rRuntimeContentMode } = d2rRuntimeContentModeModule;

  test("closed shell registry preserves both current targets and independent metadata", () => {
    const current = resolveShellProfile("v2.0.0-ios26"),
      legacy = resolveShellProfile("v2.0.0-ios18");
    assert.equal(current.id, "v2.0.0-ios26");
    assert.equal(current.archivePath, "profile/shell.zip");
    assert.equal(legacy.archivePath, "profile/shell-ios18.zip");
    assert.equal(current.contentPin, D2R_RUNTIME_CONTENT_PIN);
    assert.deepEqual(current.guestTarget, {
      platform: "ios",
      minimumVersion: "26.0.0",
      sdkVersion: "26.0.0",
    });
    assert.deepEqual(legacy.guestTarget, {
      platform: "ios",
      minimumVersion: "18.0.0",
      sdkVersion: "26.0.0",
    });
    for (const id of SHELL_PROFILE_IDS) {
      const p = resolveShellProfile(id),
        e = p.evidence.entries.find((entry) => entry.path === p.capacities.evidence.executablePath);
      assert.equal(p.capacities.shellSha256, p.evidence.sha256);
      assert.equal(p.contentPin.originalSha256, e.sha256);
      assert.equal(p.contentPin.size, e.size);
      assert.equal(p.capacities.evidence.executableSha256, e.sha256);
      assert.deepEqual(p.capacities.counts, {
        EXP: 100,
        VTPART: 11,
        VTROW: 562,
        VTSET: 35,
        TAROW: 429,
      });
    }

    assert.notEqual(current.contentPin.originalSha256, legacy.contentPin.originalSha256);
    assert.notEqual(current.contentPin.fileOffset, legacy.contentPin.fileOffset);
  });

  test("unknown IDs, caller-owned profiles and mutation cannot cross-bind profiles", async () => {
    for (const id of [
      null,
      "ios18",
      "v1.0.18",
      "v1.0.18-ios26",
      "v1.0.18-ios18",
      "__proto__",
      "constructor",
      {},
      { id: "v1.0.18-ios18" },
    ]) {
      assert.throws(() => resolveShellProfile(id), /SHELL_PROFILE_UNKNOWN/);
      await assert.rejects(
        deriveD2rRuntimeContentMode(new Uint8Array(), { contentMode: "warlock", profileId: id }),
        /SHELL_PROFILE_UNKNOWN/,
      );
    }
    const legacy = resolveShellProfile("v2.0.0-ios18");
    assert.throws(() => {
      legacy.contentPin.fileOffset = D2R_RUNTIME_CONTENT_PIN.fileOffset;
    }, TypeError);
    assert.throws(() => {
      legacy.capacities.counts.EXP = 99;
    }, TypeError);
    assert.throws(() => {
      legacy.evidence.entries[0].sha256 = "0".repeat(64);
    }, TypeError);
    assert.throws(() => {
      SHELL_PROFILE_IDS.push("custom");
    }, TypeError);
  });

  test("synthetic wrong and mixed-profile executable identities reject", async () => {
    for (const id of SHELL_PROFILE_IDS) {
      const p = resolveShellProfile(id),
        other = resolveShellProfile(SHELL_PROFILE_IDS.find((candidate) => candidate !== id));
      for (const contentMode of ["warlock", "base-update-only"]) {
        await assert.rejects(
          deriveD2rRuntimeContentMode(new Uint8Array(p.contentPin.size), {
            profileId: id,
            contentMode,
          }),
          /D2R_RUNTIME_CONTENT_BINDING/,
        );
        await assert.rejects(
          deriveD2rRuntimeContentMode(new Uint8Array(other.contentPin.size), {
            profileId: id,
            contentMode,
          }),
          /D2R_RUNTIME_CONTENT_BINDING/,
        );
      }
    }
  });

  test("only the matched v2 pair is registered and retired IDs reject", () => {
    assert.equal(resolveShellProfile().id, DEFAULT_SHELL_PROFILE_ID);
    assert.equal(DEFAULT_SHELL_PROFILE_ID, "v2.0.0-ios26");
    assert.equal(resolveShellProfile().contentPin, D2R_RUNTIME_CONTENT_PIN);
    assert.deepEqual(SHELL_PROFILE_IDS, ["v2.0.0-ios26", "v2.0.0-ios18"]);
    assert.deepEqual(BUNDLED_SHELL_PROFILE_IDS, SHELL_PROFILE_IDS);
    assert.equal(
      new Set(SHELL_PROFILE_IDS.map((id) => resolveShellProfile(id).archivePath)).size,
      2,
    );
    for (const id of ["v1.0.18-ios26", "v1.0.18-ios18"])
      assert.throws(() => resolveShellProfile(id), /SHELL_PROFILE_UNKNOWN/);
  });
}

// shell-target
{
  // 验证 shell-target 的约定与边界；依据 ../shell-target.mjs 及本文件的测试断言。
  // Verify shell-target contracts and boundaries against ../shell-target.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { inspectMachOTarget, createShellTargetInspector } = shellTargetModule;
  function fixture() {
    const b = new Uint8Array(64),
      v = new DataView(b.buffer);
    for (const [offset, value] of [
      [0, 0xfeedfacf],
      [4, 0x0100000c],
      [12, 2],
      [16, 1],
      [20, 32],
      [32, 0x32],
      [36, 32],
      [40, 2],
      [44, 0x1a0000],
      [48, 0x1a0500],
      [52, 1],
    ])
      v.setUint32(offset, value, true);
    return b;
  }
  test("observes target from bounded thin arm64 Mach-O, including sliced buffers", () => {
    const raw = fixture(),
      padded = new Uint8Array(100);
    padded.set(raw, 3);
    assert.deepEqual(inspectMachOTarget(padded.subarray(3, 67)), {
      platform: "ios",
      minimumVersion: "26.0.0",
    });
  });
  test("rejects unsupported and malformed load commands instead of trusting a recipe", () => {
    for (const [offset, value] of [
      [0, 0xcafebabe],
      [4, 7],
      [12, 6],
      [16, 4097],
      [20, 0xffffff],
      [32, 0x25],
      [36, 0],
      [36, 31],
      [40, 7],
      [44, 0],
      [52, 129],
    ]) {
      const b = fixture();
      new DataView(b.buffer).setUint32(offset, value, true);
      assert.throws(() => inspectMachOTarget(b));
    }
    assert.throws(() => inspectMachOTarget(new Uint8Array(31)));
  });
  test("verified reader supplies executable bytes and cancellation blocks reads", async () => {
    const inspect = createShellTargetInspector({ executablePath: "Game" });
    const context = {
      base: "Payload/Game.app/",
      entries: [{ path: "Payload/Game.app/Game", mode: 0o100755 }],
      readEntry: async (path) => {
        assert.equal(path, "Payload/Game.app/Game");
        return fixture();
      },
    };
    assert.equal((await inspect(context)).minimumVersion, "26.0.0");
    await assert.rejects(
      inspect({
        ...context,
        signal: AbortSignal.abort(),
        readEntry: () => assert.fail("read after abort"),
      }),
    );
    for (const executablePath of ["../Game", "a/b", ".", ""])
      assert.throws(() => createShellTargetInspector({ executablePath }));
  });
}

// shell-v2-evidence
{
  // 只验证固定元数据与拒绝边界，不执行外壳、编译器或游戏。
  // Fixed metadata and rejection boundaries only; no shell, compiler or game execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const {
    V2_SHELL_PROFILE_IDS,
    V2_SHELL_CAPABILITIES,
    V2_SHELL_RELEASE_SOURCE,
    resolveV2ShellEvidence,
  } = shellV2EvidenceModule;
  const { DEFAULT_SHELL_PROFILE_ID, SHELL_PROFILE_IDS, resolveShellProfile } = shellProfilesModule;
  const { deriveD2rRuntimeContentMode } = d2rRuntimeContentModeModule;

  test("default v2 profiles bind reviewed recipes without claiming device validation", async () => {
    assert.equal(DEFAULT_SHELL_PROFILE_ID, "v2.0.0-ios26");
    assert.deepEqual(V2_SHELL_PROFILE_IDS, ["v2.0.0-ios26", "v2.0.0-ios18"]);
    assert.deepEqual(SHELL_PROFILE_IDS, [...V2_SHELL_PROFILE_IDS]);
    for (const id of V2_SHELL_PROFILE_IDS) {
      const selected = resolveShellProfile(id);
      const evidence = resolveV2ShellEvidence(id);
      assert.equal(selected.evidence, evidence.evidence);
      assert.equal(selected.contentPin, evidence.contentPin);
      assert.equal(selected.assetRecipe.artRevision, V2_SHELL_RELEASE_SOURCE);
      assert.equal(selected.assetRecipe.overlayRevision, V2_SHELL_RELEASE_SOURCE);
      assert.equal(
        selected.assetRecipe.overlaySourceFingerprint,
        V2_SHELL_CAPABILITIES.overlayNativeSourceFingerprint,
      );
      await assert.rejects(
        deriveD2rRuntimeContentMode(new Uint8Array(), { profileId: id, contentMode: "warlock" }),
        /D2R_RUNTIME_CONTENT_BINDING/,
      );
    }
    assert.equal(V2_SHELL_CAPABILITIES.status, "source-reviewed-not-device-validated");
    assert.equal(V2_SHELL_CAPABILITIES.matchingReleaseAssetRecipeRequired, true);
    assert.equal(V2_SHELL_CAPABILITIES.completeShaderSourceDigestRequired, true);
    assert.equal(V2_SHELL_CAPABILITIES.reflectionAndCompiledLibraryValidationRequired, true);
    assert.equal(V2_SHELL_CAPABILITIES.laterPlayStationGlyphsSupported, false);
    assert.equal(V2_SHELL_CAPABILITIES.laterIosVp8Supported, false);
    assert.equal(V2_SHELL_CAPABILITIES.deviceExecutionVerified, false);
  });

  test("both candidates bind exact release, executable, minimum OS and distinct patch sites", () => {
    assert.equal(V2_SHELL_RELEASE_SOURCE, "b811b90172f4eb40ec1bc89d903308e0c5ab88d0");
    const current = resolveV2ShellEvidence("v2.0.0-ios26");
    const legacy = resolveV2ShellEvidence("v2.0.0-ios18");
    assert.equal(current.contentPin.fileOffset, 0x1a71e8);
    assert.equal(legacy.contentPin.fileOffset, 0x1a3058);
    assert.notEqual(current.contentPin.originalSha256, legacy.contentPin.originalSha256);
    assert.notEqual(current.contentPin.noDlcSha256, legacy.contentPin.noDlcSha256);
    assert.notEqual(current.evidence.sha256, legacy.evidence.sha256);
    for (const p of [current, legacy]) {
      const { evidence, capacities, contentPin } = p;
      assert.equal(evidence.releaseTarget, V2_SHELL_RELEASE_SOURCE);
      assert.equal(capacities.evidence.sourceCommit, V2_SHELL_RELEASE_SOURCE);
      assert.equal(capacities.evidence.reviewStatus, "independently-reviewed-static-control-flow");
      assert.equal(evidence.publisherDigestMatched, true);
      assert.equal(evidence.publisherChecksumAssetMatched, true);
      assert.equal(evidence.publisherBinaryBuildAttestationVerified, false);
      assert.equal(evidence.productionCompatibilityEstablished, false);
      assert.equal(evidence.entries.length, 15);
      const executable = evidence.entries.find(
        (e) => e.path === capacities.evidence.executablePath,
      );
      assert.equal(executable.sha256, contentPin.originalSha256);
      assert.equal(executable.size, contentPin.size);
      assert.equal(capacities.shellSha256, evidence.sha256);
      assert.equal(capacities.evidence.executableSha256, contentPin.originalSha256);
      assert.equal(evidence.machOBuildVersions[0].minimumVersion, p.guestTarget.minimumVersion);
      assert.equal(evidence.machOBuildVersions[0].sdkVersion, "27.1.0");
      assert.equal(p.guestTarget.sdkVersion, "26.0.0");
      assert.equal(evidence.shellInfo.CHAIN, "420aeb7c8bc6");
      assert.equal(evidence.shellInfo.SHADER_SRC, "be59c864f087b377");
      assert.equal(evidence.shellInfo.SPIRV_CROSS, "1.4.363.0");
      assert.equal(evidence.inspection.executed, false);
      assert.equal(evidence.inspection.contentMode.mutationOnlyInMemory, true);
      assert.equal(evidence.inspection.contentMode.derivedSha256, contentPin.noDlcSha256);
      assert.deepEqual(capacities.counts, {
        EXP: 100,
        VTPART: 11,
        VTROW: 562,
        VTSET: 35,
        TAROW: 429,
      });
      assert.deepEqual(capacities.evidence.bindings.map((b) => b.name).sort(), [
        "_k_exp_load",
        "_ta_load",
        "_vt_load",
        "_vt_part_cb",
        "_vt_row_cb",
        "_vt_set_cb",
      ]);
      for (const binding of capacities.evidence.bindings) {
        assert.match(binding.functionSha256, /^[a-f0-9]{64}$/);
        assert.ok(binding.functionBytes > 0);
        for (const group of binding.instructions) {
          assert.ok(group.window.length >= 2);
          assert.match(group.window[0].decodedSubset, /^cmp [wx][0-9]+, #[0-9]+$/);
          for (const instruction of group.window) {
            assert.ok(instruction.fileOffset >= binding.fileOffset);
            assert.ok(instruction.fileOffset < binding.fileOffset + binding.functionBytes);
            assert.match(instruction.bytes, /^[a-f0-9]{8}$/);
          }
        }
      }
    }
    assert.equal(legacy.evidence.inspection.metal4SymbolNames.length, 0);
  });

  test("independent static review binds the same ZIPs, functions, callback tags and patch sites", async () => {
    const reviews = JSON.parse(
      await readFile(
        new URL(
          "../toolchain/shell-release-evidence/v2.0.0-independent-native-review.json",
          import.meta.url,
        ),
        "utf8",
      ),
    );
    assert.equal(reviews.length, V2_SHELL_PROFILE_IDS.length);
    for (const id of V2_SHELL_PROFILE_IDS) {
      const p = resolveV2ShellEvidence(id);
      const review = reviews.find((r) => r.asset.sha256 === p.evidence.sha256);
      assert.ok(review);
      assert.equal(review.executableSha256, p.contentPin.originalSha256);
      assert.equal(review.minimumVersion, p.guestTarget.minimumVersion);
      assert.equal(review.aocDataSymbol.fileOffset, p.contentPin.fileOffset);
      assert.equal(BigInt(review.aocDataSymbol.address), p.contentPin.address);
      assert.equal(review.aocDerivedSha256, p.contentPin.noDlcSha256);
      assert.equal(review.rawInstructionWindowBytesVerified, true);
      assert.deepEqual(review.vaultTagCallbackContextBindings, [
        { tag: "VTPART", callback: "_vt_part_cb", contextFrameOffset: -20 },
        { tag: "VTROW", callback: "_vt_row_cb", contextFrameOffset: -24 },
        { tag: "VTSET", callback: "_vt_set_cb", contextFrameOffset: -28 },
      ]);
      assert.equal(review.functionBindings.length, p.capacities.evidence.bindings.length);
      for (const binding of p.capacities.evidence.bindings) {
        assert.deepEqual(
          review.functionBindings.find((b) => b.name === binding.name),
          {
            name: binding.name,
            fileOffset: binding.fileOffset,
            functionSha256: binding.functionSha256,
          },
        );
      }
    }
  });

  test("candidate metadata is immutable and never accepts caller-supplied identities", () => {
    for (const id of [undefined, null, {}, "__proto__", "constructor", "v2.0.0", "v1.0.18-ios26"])
      assert.throws(() => resolveV2ShellEvidence(id), /SHELL_V2_EVIDENCE_UNKNOWN/);
    const p = resolveV2ShellEvidence("v2.0.0-ios26");
    assert.throws(() => {
      p.contentPin.fileOffset = 0x18ef78;
    }, TypeError);
    assert.throws(() => {
      p.capacities.counts.EXP = 99;
    }, TypeError);
    assert.throws(() => {
      p.evidence.shellInfo.CHAIN = "f2bc055e8f1e";
    }, TypeError);
    assert.throws(() => {
      V2_SHELL_CAPABILITIES.status = "enabled";
    }, TypeError);
  });

  test("CHAIN diagnostics distinguish publisher reproducibility from source-equivalent consumption", async () => {
    const records = JSON.parse(
      await readFile(
        new URL(
          "../toolchain/shell-release-evidence/v2.0.0-source-chain-comparison.json",
          import.meta.url,
        ),
        "utf8",
      ),
    );
    const [old, release, latest] = records;
    assert.deepEqual(old.chainInputSha256, release.chainInputSha256);
    assert.deepEqual(release.chainInputSha256, latest.chainInputSha256);
    assert.equal(release.computedChain, "c675d3fa7c90");
    assert.equal(release.headerChain, "15cf7ca29eaf");
    assert.equal(release.headerMatchesCommittedInputs, false);
    assert.notEqual(
      release.computedChain,
      resolveV2ShellEvidence("v2.0.0-ios26").evidence.shellInfo.CHAIN,
    );
    const loaders = JSON.parse(
      await readFile(
        new URL(
          "../toolchain/shell-release-evidence/v2.0.0-native-loader-comparison.json",
          import.meta.url,
        ),
        "utf8",
      ),
    );
    for (const record of loaders.records.filter((r) => r.path !== "dm/gfx/nvn/nvn_mtl.m"))
      assert.equal(record.byteIdentical, true);
    const metal = loaders.records.find((r) => r.path === "dm/gfx/nvn/nvn_mtl.m");
    assert.equal(metal.shaderLoadBodyByteIdentical, true);
    assert.equal(metal.libraryLoaderBodyByteIdentical, true);
  });

  test("v2 uses the existing full source proof before all three CHAIN labels", async () => {
    const source = await readFile(new URL("../production-shaders.mjs", import.meta.url), "utf8");
    const proof = source.indexOf("await pipeline.verifyCompleteSourceSet()");
    const chain = source.indexOf("const chain = trustedShell.trustedRecipe.chain");
    assert.ok(proof >= 0 && chain > proof);
    assert.match(source.slice(proof, chain), /pending.length !== 6927/);
    assert.match(
      source.slice(proof, chain),
      /proof.sourceDigest !== trustedShell.trustedRecipe.shaderDigest/,
    );
    for (const marker of ["row.replay.chain = chain", "msl/b-${chain}/", "# chain b-${chain}"]) {
      assert.ok(source.indexOf(marker) > chain);
    }
    for (const id of V2_SHELL_PROFILE_IDS) {
      const selected = resolveShellProfile(id);
      assert.equal(selected.evidence.shellInfo.CHAIN, "420aeb7c8bc6");
      assert.equal(selected.evidence.shellInfo.SHADER_SRC, "be59c864f087b377");
      assert.ok(selected.evidence.requiredPaths.includes("msl/b-420aeb7c8bc6/"));
    }
  });
}

// production-profile
{
  // 依据生产配置的固定入口验证配置选择；成功路径只使用合成字节和隔离的适配器。
  // Verify closed production-profile selection; success paths use synthetic bytes and isolated adapters only.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { readFile } = promisesModule;
  const { default: test } = nodeTestModule;
  const { default: ts } = typescriptModule;
  const { loadProductionProfile } = productionProfileModule;
  const { resolveShellProfile, SHELL_PROFILE_IDS } = shellProfilesModule;

  const digest = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const moduleURL = (source) =>
    `data:text/javascript;base64,${Buffer.from(source).toString("base64")}`;
  function rewriteImports(source, resolve) {
    const file = ts.createSourceFile(
      "fixture.mjs",
      source,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.JS,
    );
    assert.equal(file.parseDiagnostics.length, 0);
    // 只替换真实导入的模块字面量，保留注释、字符串与 JSON 导入属性。
    // Replace only real import module literals, preserving comments, strings and JSON import attributes.
    for (const statement of [...file.statements].reverse()) {
      if (!ts.isImportDeclaration(statement)) continue;
      const specifier = statement.moduleSpecifier;
      assert.ok(ts.isStringLiteral(specifier));
      source =
        source.slice(0, specifier.getStart(file)) +
        JSON.stringify(resolve(specifier.text)) +
        source.slice(specifier.end);
    }
    return source;
  }

  test("isolated profile imports accept comments, quotes and newlines without rewriting text", async () => {
    for (const declaration of [
      'import { value } from "./synthetic.mjs";',
      "import{value}from'./synthetic.mjs';",
      "import {\n value,\n} from /* unchanged import */\n './synthetic.mjs';",
    ]) {
      const resolved = [];
      const text = 'from "./text-only.mjs"';
      const source = `${declaration}\nexport default value;\nexport const text = ${JSON.stringify(text)};\n// from "./comment-only.mjs"`;
      const rewritten = rewriteImports(source, (specifier) => {
        resolved.push(specifier);
        assert.equal(specifier, "./synthetic.mjs");
        return moduleURL("export const value = 7;");
      });
      const loaded = await import(moduleURL(rewritten));
      assert.equal(loaded.default, 7);
      assert.equal(loaded.text, text);
      assert.deepEqual(resolved, ["./synthetic.mjs"]);
      assert.ok(rewritten.endsWith('// from "./comment-only.mjs"'));
    }
  });

  const syntheticBytes = new Uint8Array([1, 3, 5, 7]);
  function syntheticProfile() {
    return {
      id: "synthetic-profile",
      releaseId: "synthetic-release",
      archivePath: "profile/synthetic.zip",
      assetRecipe: { artRevision: "v1.0.18", overlayRevision: "v1.0.18" },
      guestTarget: { platform: "ios", minimumVersion: "18.0.0", sdkVersion: "26.0.0" },
      contentPin: { executablePath: "Synthetic", originalSha256: digest(syntheticBytes) },
      capacities: {
        shellSha256: digest(syntheticBytes),
        counts: { EXP: 7 },
        evidence: {
          executablePath: "Payload/Demo.app/Synthetic",
          executableSha256: "a".repeat(64),
        },
      },
      evidence: {
        sha256: digest(syntheticBytes),
        archiveBytes: syntheticBytes.length,
        entries: [{ path: "Payload/Demo.app/Synthetic", sha256: "a".repeat(64) }],
        shellInfo: {
          PRODUCT: "synthetic",
          CHAIN: "test",
          SHADER_SRC: "0123456789abcdef",
          SPIRV_CROSS: "test",
        },
        machOBuildVersions: [{ minimumVersion: "18.0.0" }],
        requiredPaths: ["synthetic-required"],
      },
    };
  }
  async function isolatedLoader(profile) {
    // 仅替换测试副本的依赖；生产入口不开放配方、摘要或适配器注入。
    // Replace dependencies only in the test copy; production exposes no recipe, digest or adapter injection.
    const mocks = {
      "./shell-profiles.mjs": `export const resolveShellProfile = () => (${JSON.stringify(profile)});`,
      "./shader-catalog-adapter.mjs": `export const loadShaderCatalog = async () => ({keys: ["synthetic-key"]});`,
      "./shell-target.mjs": `export const createShellTargetInspector = (options) => options;`,
      "./shell-loader.mjs": `export const createShellLoader = ({source, releaseId, resolveTrustedRelease, inspectTargetOS}) => async ({signal}) => ({source, releaseId, trusted: await resolveTrustedRelease(), inspector: inspectTargetOS, signal});`,
    };
    const sourceURL = new URL("../production-profile.mjs", import.meta.url);
    const source = rewriteImports(await readFile(sourceURL, "utf8"), (specifier) =>
      mocks[specifier] ? moduleURL(mocks[specifier]) : new URL(specifier, sourceURL).href,
    );
    return (await import(moduleURL(source))).loadProductionProfile;
  }

  test("default and explicit profiles read only their own archive, without fallback", async () => {
    for (const profileId of [undefined, ...SHELL_PROFILE_IDS]) {
      const reads = [];
      await assert.rejects(
        loadProductionProfile(
          {
            readPublicAsset(path) {
              reads.push(path);
              throw new Error("SYNTHETIC_MISSING_ARCHIVE");
            },
          },
          { profileId },
        ),
        /SYNTHETIC_MISSING_ARCHIVE/,
      );
      assert.deepEqual(reads, [resolveShellProfile(profileId).archivePath]);
    }
  });

  test("unknown, object and prototype profile identifiers fail before asset reads", async () => {
    for (const profileId of [
      null,
      "",
      "ios18",
      "constructor",
      "__proto__",
      {},
      { id: SHELL_PROFILE_IDS[0] },
    ]) {
      await assert.rejects(
        loadProductionProfile(
          {
            readPublicAsset() {
              assert.fail("unexpected asset read");
            },
          },
          { profileId },
        ),
        /SHELL_PROFILE_UNKNOWN/,
      );
    }
  });

  test("synthetic untrusted bytes cannot pass either built-in archive digest", async () => {
    for (const profileId of SHELL_PROFILE_IDS) {
      const reads = [];
      await assert.rejects(
        loadProductionProfile(
          {
            readPublicAsset(path) {
              reads.push(path);
              return syntheticBytes.slice();
            },
          },
          { profileId },
        ),
        /PUBLIC_SHELL_INTEGRITY/,
      );
      assert.deepEqual(reads, [resolveShellProfile(profileId).archivePath]);
    }
  });

  test("synthetic selection binds recipe, capacities, content pin, target and lifecycle", async () => {
    const selected = syntheticProfile();
    const load = await isolatedLoader(selected);
    const reads = [],
      bytes = syntheticBytes.slice(),
      signal = new AbortController().signal;
    const result = await load(
      {
        readPublicAsset(path) {
          reads.push(path);
          return path === selected.archivePath ? bytes : new TextEncoder().encode("{}");
        },
      },
      { profileId: selected.id, signal },
    );
    assert.equal(result.profileId, selected.id);
    assert.deepEqual(result.assetRecipe, selected.assetRecipe);
    assert.equal(result.contentMode, "optional-warlock");
    assert.deepEqual(result.derivedCounts, selected.capacities.counts);
    assert.deepEqual(result.contentPin, selected.contentPin);
    assert.deepEqual(result.guestTarget, selected.guestTarget);
    assert.equal(result.shell.releaseId, selected.releaseId);
    assert.equal(result.shell.signal, signal);
    assert.deepEqual(result.shell.inspector, {
      executablePath: selected.contentPin.executablePath,
    });
    assert.deepEqual(result.trustedShell.files, selected.evidence.entries);
    assert.equal(result.trustedShell.trustedRecipe.product, selected.evidence.shellInfo.PRODUCT);
    assert.equal(
      result.trustedShell.trustedRecipe.shaderDigest,
      selected.evidence.shellInfo.SHADER_SRC,
    );
    assert.deepEqual(result.trustedShell.trustedRecipe.targetOS, {
      platform: "ios",
      minimumVersion: "18.0.0",
    });
    assert.deepEqual(
      result.trustedShell.trustedRecipe.requiredPaths,
      selected.evidence.requiredPaths,
    );
    assert.deepEqual(result.trustedShell.trustedRecipe.shaderKeys, ["synthetic-key"]);
    assert.equal(reads[0], selected.archivePath);
    assert.ok(!reads.includes("profile/shell.zip"));
    assert.deepEqual(await result.shell.source.read(0, bytes.length), syntheticBytes);
    result.close();
    result.close();
    assert.deepEqual(bytes, new Uint8Array(bytes.length));
    await assert.rejects(result.shell.source.read(0, 1), /PROFILE_CLOSED/);
  });

  test("mixed capacity evidence fails before I/O and matching digest cannot bypass archive size", async () => {
    for (const field of ["shellSha256", "executableSha256", "executablePath"]) {
      const selected = syntheticProfile();
      if (field === "shellSha256") selected.capacities[field] = "b".repeat(64);
      else selected.capacities.evidence[field] = "mismatch";
      const load = await isolatedLoader(selected);
      await assert.rejects(
        load({
          readPublicAsset() {
            assert.fail("unexpected asset read");
          },
        }),
        /SHELL_CAPACITY_BINDING/,
      );
    }
    const selected = syntheticProfile();
    selected.evidence.archiveBytes++;
    const load = await isolatedLoader(selected);
    await assert.rejects(
      load({ readPublicAsset: () => syntheticBytes.slice() }),
      /PUBLIC_SHELL_INTEGRITY/,
    );
  });
}
