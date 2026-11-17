// Feature suite: application-ui. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as nodeChild_processModule from "node:child_process";
import * as nodeZlibModule from "node:zlib";
import * as nodeCryptoModule from "node:crypto";
import * as nodeFsModule from "node:fs";
import * as appCustomizationModule from "../app-customization.mjs";
import * as appCustomizationPlistModule from "../app-customization-plist.mjs";
import * as appCustomizationIconModule from "../app-customization-icon.mjs";
import * as ipaSigningZipModule from "../ipa-signing-zip.mjs";
import * as appCustomizationDefaultsModule from "../app-customization-defaults.mjs";
import * as shellProfilesModule from "../shell-profiles.mjs";
import * as promisesModule from "node:fs/promises";
import * as nodeVmModule from "node:vm";
import * as typescriptModule from "typescript";
import * as i18nModule from "../i18n.mjs";
import * as i18nCatalogModule from "../i18n-catalog.mjs";
import * as ipaSigningContractModule from "../ipa-signing-contract.mjs";
import * as postGenerationGuideModule from "../post-generation-guide.mjs";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as nodeProcessModule from "node:process";
import * as programCompatibilityModule from "../program-compatibility.mjs";
import * as generationProgressModule from "../generation-progress.mjs";
import * as generationReadinessModule from "../generation-readiness.mjs";
import * as packagingDiagnosticsModule from "../packaging-diagnostics.mjs";
import * as nodePathModule from "node:path";
import * as nodeOsModule from "node:os";
import * as nodeUrlModule from "node:url";
import * as tailwindcssModule from "tailwindcss";
import * as frontendNoticesModule from "../scripts/frontend-notices.mjs";
import * as previewManifestModule from "../verification/preview-manifest.mjs";
import * as downloadPreparationModule from "../download-preparation.mjs";
import * as uiOperationGateModule from "../ui-operation-gate.mjs";
import * as signingInputModule from "../signing-input.mjs";

// app-customization
{
  // 仅使用自造 plist、PNG 和惰性 ZIP 字节；不读取游戏、凭据或实际应用。
  // Authored plist, PNG and lazy ZIP fixtures only; no games, credentials or real applications.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const { deflateSync, inflateSync } = nodeZlibModule;
  const { createHash } = nodeCryptoModule;
  const { readFileSync } = nodeFsModule;
  const { normalizeAppCustomization, prepareAppCustomization, customizeUnsignedIpa } =
    appCustomizationModule;
  const { readCustomizationPlist, writeCustomizationPlist } = appCustomizationPlistModule;
  const { prepareCustomizationIcons, CUSTOM_ICON_FILES } = appCustomizationIconModule;
  const { inspectUnsignedSigningArchive, createSignedSigningArchive } = ipaSigningZipModule;
  const root = "Payload/Authored.app/";
  const encode = (text) => new TextEncoder().encode(text);
  const dict = (value) => Object.assign(Object.create(null), value);
  const base = () =>
    dict({
      CFBundleIdentifier: "local.authored.original",
      CFBundlePackageType: "APPL",
      CFBundleExecutable: "Authored",
      CFBundleName: "Original",
      CFBundleDisplayName: "Original",
      CFBundleSpokenName: "Original",
      CFBundleShortVersionString: "1.0.0",
      CFBundleVersion: "1",
      MinimumOSVersion: "18.0",
      CFBundleIcons: dict({
        CFBundlePrimaryIcon: dict({
          CFBundleIconName: "AppIcon",
          CFBundleIconFiles: ["AppIcon60x60"],
        }),
        CFBundleAlternateIcons: dict({ stale: dict({ CFBundleIconName: "Stale" }) }),
      }),
      CFBundleIconName: "AppIcon",
      CFBundleIconsExtra: "not an icon key",
    });
  const entry = (path, bytes, mode = 0o644) => ({
    path: root + path,
    size: bytes.length,
    mode,
    blob: new Blob([bytes]),
  });
  const fixtures = (plist = writeCustomizationPlist(base())) => [
    entry("Info.plist", plist),
    entry("Authored", new Uint8Array([0xcf, 0xfa, 0xed, 0xfe, 0, 0, 0, 0]), 0o755),
    entry("Assets.car", encode("opaque immutable asset catalog")),
    entry("Frameworks/Guest.framework/Info.plist", encode("immutable guest plist")),
    entry("dm/compiled_shader_receipt.json", encode('{"authored":true}')),
    entry("romfs_overlay/inert", encode("immutable resource")),
  ];
  const digest = async (blob) =>
    createHash("sha256")
      .update(new Uint8Array(await blob.arrayBuffer()))
      .digest("hex");
  function python(code, input = "") {
    const run = spawnSync("python3", ["-c", code], {
      input,
      encoding: "utf8",
      maxBuffer: 2 * 1024 * 1024,
    });
    assert.equal(run.status, 0, run.stderr);
    return run.stdout;
  }
  function binaryFixture() {
    return new Uint8Array(
      Buffer.from(
        python(`import plistlib,base64,datetime
v={"CFBundleIdentifier":"local.authored.original","CFBundlePackageType":"APPL","CFBundleExecutable":"Authored","CFBundleDisplayName":"Original","CFBundleShortVersionString":"1.0.0","CFBundleVersion":"1","MinimumOSVersion":"26.0","Unicode":"中文 😀 & < >","Nested":{"__proto__":"ordinary key","items":[True,False,123,-1,1.25,b"\\x00\\xff",datetime.datetime(2020,1,2,3,4,5)]}}
print(base64.b64encode(plistlib.dumps(v,fmt=plistlib.FMT_BINARY)).decode())`).trim(),
        "base64",
      ),
    );
  }
  const crc = (bytes) => {
    let n = 0xffffffff;
    for (const b of bytes) {
      n ^= b;
      for (let i = 0; i < 8; i++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
    }
    return (n ^ 0xffffffff) >>> 0;
  };
  function chunk(name, data) {
    const out = Buffer.alloc(data.length + 12);
    out.writeUInt32BE(data.length);
    out.write(name, 4);
    out.set(data, 8);
    out.writeUInt32BE(crc(out.subarray(4, out.length - 4)), out.length - 4);
    return out;
  }
  function png({
    side = 256,
    channels = 3,
    transparent = false,
    filter = 0,
    width = side,
    interlace = 0,
    tail = false,
    trailingDeflate = false,
    invalidFilter = false,
    longDecoded = false,
    ancillary = [],
  } = {}) {
    const header = Buffer.alloc(13);
    header.writeUInt32BE(width);
    header.writeUInt32BE(side, 4);
    header[8] = 8;
    header[9] = channels === 3 ? 2 : 6;
    header[12] = interlace;
    const stride = side * channels,
      raw = Buffer.alloc(side * (stride + 1) + (longDecoded ? 1 : 0));
    for (let y = 0; y < side; y++) {
      raw[y * (stride + 1)] = invalidFilter ? 5 : filter;
      for (let x = 0; x < stride; x++) {
        const value =
          channels === 4 && x % channels === 3
            ? transparent
              ? 254
              : 255
            : [40, 100, 170][x % channels];
        const a = x >= channels ? value : 0,
          b = y ? value : 0,
          c = x >= channels && y ? value : 0;
        const p = a + b - c,
          da = Math.abs(p - a),
          db = Math.abs(p - b),
          dc = Math.abs(p - c),
          predict = da <= db && da <= dc ? a : db <= dc ? b : c;
        raw[y * (stride + 1) + x + 1] = value - [0, a, b, (a + b) >> 1, predict][filter];
      }
    }
    let z = deflateSync(raw);
    if (trailingDeflate) z = Buffer.concat([z.subarray(0, -4), Buffer.from([0]), z.subarray(-4)]);
    return new Blob([
      Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
      chunk("IHDR", header),
      ...ancillary.map((name) => chunk(name, Buffer.alloc(0))),
      chunk("IDAT", z),
      chunk("IEND", Buffer.alloc(0)),
      ...(tail ? [Buffer.from([0])] : []),
    ]);
  }
  async function pngOutput(blob) {
    const bytes = Buffer.from(await blob.arrayBuffer());
    const side = bytes.readUInt32BE(16);
    let at = 8;
    const parts = [];
    while (at < bytes.length) {
      const size = bytes.readUInt32BE(at);
      assert.equal(crc(bytes.subarray(at + 4, at + 8 + size)), bytes.readUInt32BE(at + 8 + size));
      if (bytes.toString("ascii", at + 4, at + 8) === "IDAT")
        parts.push(bytes.subarray(at + 8, at + 8 + size));
      at += size + 12;
    }
    const raw = inflateSync(Buffer.concat(parts));
    assert.equal(raw.length, side * (side * 3 + 1));
    return { side, raw };
  }
  test("default is byte-identical and blank values retain shell metadata without reading archive", async () => {
    const blob = new Blob(["not read"]);
    const options = await prepareAppCustomization({
      displayName: "",
      bundleIdentifier: undefined,
      version: "",
      buildVersion: "",
    });
    assert.deepEqual(options, {});
    assert.equal((await customizeUnsignedIpa(blob, options)).blob, blob);
    assert.equal(await prepareAppCustomization(options), options);
    await assert.rejects(customizeUnsignedIpa(blob, {}), /APP_CUSTOMIZATION_INVALID/);
  });
  test("field validation is closed, bounded and keeps release/build versions separate", () => {
    assert.deepEqual(
      normalizeAppCustomization({
        displayName: "My & 中文 😀",
        bundleIdentifier: "local.authored.My-App",
        version: "0.1.2",
        buildVersion: "9999.99.99",
      }),
      {
        displayName: "My & 中文 😀",
        bundleIdentifier: "local.authored.My-App",
        version: "0.1.2",
        buildVersion: "9999.99.99",
      },
    );
    for (const value of [null, [], { entitlements: {} }, { appPath: "elsewhere" }])
      assert.throws(() => normalizeAppCustomization(value), /APP_CUSTOMIZATION_INVALID/);
    for (const value of [" ", " Name", "Name ", "x\0", "x\n", "\ud800", "x".repeat(81)])
      assert.throws(() => normalizeAppCustomization({ displayName: value }), /NAME_INVALID/);
    for (const value of [
      "local",
      "local..app",
      ".app",
      "local.app.",
      "local.*",
      "local.app/name",
      "local_app.test",
      "local.😀",
      "x".repeat(256),
    ])
      assert.throws(() => normalizeAppCustomization({ bundleIdentifier: value }), /BUNDLE_INVALID/);
    for (const value of ["1", "1.2", "01.2.3", "1.2.3b1", "1.2.3.4", "1.2.-3", "1.2. 3"])
      assert.throws(() => normalizeAppCustomization({ version: value }), /VERSION_INVALID/);
    for (const value of ["0", "10000", "1.100", "1.01", "1.2.3.4", "1.2b1"])
      assert.throws(() => normalizeAppCustomization({ buildVersion: value }), /BUILD_INVALID/);
  });
  test("binary and XML plists round-trip standard typed data with independent Python parser", () => {
    const source = binaryFixture(),
      plist = readCustomizationPlist(source);
    assert.equal(plist.Unicode, "中文 😀 & < >");
    assert.equal(plist.Nested.__proto__, "ordinary key");
    const xml = writeCustomizationPlist(plist);
    assert.deepEqual(readCustomizationPlist(xml), plist);
    const result = python(
      `import plistlib,sys,base64
v=plistlib.loads(base64.b64decode(sys.stdin.read()))
assert v["Nested"]["items"][:5] == [True,False,123,-1,1.25]
assert v["Nested"]["items"][5] == b"\\x00\\xff"
assert v["Nested"]["items"][6].isoformat() == "2020-01-02T03:04:05"
assert v["Unicode"] == "中文 😀 & < >"
print("ok")`,
      Buffer.from(xml).toString("base64"),
    );
    assert.equal(result.trim(), "ok");
  });
  test("plist parser rejects malicious XML, duplicates, malformed references and budgets", () => {
    const wrap = (body) => encode(`<plist version="1.0">${body}</plist>`);
    for (const source of [
      wrap("<dict><key>x</key><string>1</string><key>x</key><string>2</string></dict>"),
      wrap("<dict><key>x</key><string>&unknown;</string></dict>"),
      wrap("<dict><key>x</key><string>&amp</string></dict>"),
      wrap("<dict><key>x</key><string>&#0;</string></dict>"),
      wrap("<dict><key>x</key><string><![CDATA[x]]></string></dict>"),
      encode(
        '<!DOCTYPE plist [<!ENTITY x SYSTEM "file:///private">]><plist version="1.0"><dict/></plist>',
      ),
      wrap("<dict><key>x</key><real>Infinity</real></dict>"),
      wrap("<dict><key>x</key><array>".repeat(34) + "</array></dict>".repeat(34)),
      new Uint8Array(1024 * 1024 + 1),
    ])
      assert.throws(() => readCustomizationPlist(source), /PLIST_INVALID/);
    for (const mutate of [
      (b) => b.subarray(0, -1),
      (b) => {
        b[b.length - 25] = 3;
        return b;
      },
      (b) => {
        b[b.length - 16] = 255;
        return b;
      },
    ])
      assert.throws(() => readCustomizationPlist(mutate(binaryFixture().slice())), /PLIST_INVALID/);
  });
  test("metadata transforms binary plist and preserves all unrelated payloads and input bytes", async () => {
    const initial = fixtures(binaryFixture()),
      original = await createSignedSigningArchive(initial),
      before = await digest(original);
    const result = await customizeUnsignedIpa(
      original,
      await prepareAppCustomization({
        displayName: "New & 中文",
        bundleIdentifier: "local.authored.changed",
        version: "2.4.6",
        buildVersion: "7.8.9",
      }),
    );
    assert.equal(result.changed, true);
    assert.equal(result.iconMode, "unchanged");
    assert.equal(await digest(original), before);
    const archive = await inspectUnsignedSigningArchive(result.blob),
      info = archive.entries.find((e) => e.path === root + "Info.plist"),
      parsed = readCustomizationPlist(new Uint8Array(await info.blob.arrayBuffer()));
    assert.equal(parsed.CFBundleIdentifier, "local.authored.changed");
    assert.equal(parsed.CFBundleDisplayName, "New & 中文");
    assert.equal(parsed.CFBundleName, "New & 中文");
    assert.equal(parsed.CFBundleShortVersionString, "2.4.6");
    assert.equal(parsed.CFBundleVersion, "7.8.9");
    assert.equal(parsed.MinimumOSVersion, "26.0");
    assert.equal(parsed.Unicode, "中文 😀 & < >");
    for (const e of initial.filter((e) => e.path !== info.path)) {
      const next = archive.entries.find((n) => n.path === e.path);
      assert.equal(await digest(next.blob), await digest(e.blob));
      assert.equal(next.mode, e.mode);
    }
    const onlyVersion = await customizeUnsignedIpa(
      original,
      await prepareAppCustomization({ version: "3.0.0" }),
    );
    const changed = await inspectUnsignedSigningArchive(onlyVersion.blob);
    const v = readCustomizationPlist(
      new Uint8Array(
        await changed.entries
          .find((e) => e.path.endsWith("Authored.app/Info.plist"))
          .blob.arrayBuffer(),
      ),
    );
    assert.equal(v.CFBundleVersion, "1");
  });
  test("icons decode all five PNG filters and opaque RGB/RGBA, then emit valid RGB sizes", async () => {
    for (const channels of [3, 4])
      for (const filter of [0, 1, 2, 3, 4]) {
        const icons = await prepareCustomizationIcons(png({ channels, filter }));
        assert.equal(icons.length, CUSTOM_ICON_FILES.length);
        for (let i = 0; i < icons.length; i++) {
          const { side, raw } = await pngOutput(icons[i].blob);
          assert.equal(side, CUSTOM_ICON_FILES[i][1]);
          assert.deepEqual([...raw.subarray(1, 4)], [40, 100, 170]);
          assert.deepEqual([...raw.subarray(-3)], [40, 100, 170]);
        }
      }
  });
  test("icons reject alpha, APNG, geometry, corruption, trailing streams and decompression excess", async () => {
    for (const options of [
      { channels: 4, transparent: true },
      { side: 255 },
      { side: 1025 },
      { width: 300 },
      { interlace: 1 },
      { invalidFilter: true },
      { tail: true },
      { trailingDeflate: true },
      { longDecoded: true },
      { ancillary: ["acTL"] },
      { ancillary: ["tRNS"] },
    ])
      await assert.rejects(prepareCustomizationIcons(png(options)), /ICON_INVALID/);
    const bytes = new Uint8Array(await png().arrayBuffer());
    bytes[20] ^= 1;
    await assert.rejects(prepareCustomizationIcons(new Blob([bytes])), /ICON_INVALID/);
    await assert.rejects(
      prepareCustomizationIcons(new Blob([new Uint8Array(4 * 1024 * 1024 + 1)])),
      /ICON_INVALID/,
    );
    await assert.rejects(
      prepareCustomizationIcons(new Blob(["<svg/>"], { type: "image/png" })),
      /ICON_INVALID/,
    );
  });
  test("icon references change coherently while Assets.car and old icon files stay immutable", async () => {
    const initial = [...fixtures(), entry("AppIcon60x60@2x.png", encode("original"))];
    const result = await customizeUnsignedIpa(
      await createSignedSigningArchive(initial),
      await prepareAppCustomization({ icon: png() }),
    );
    assert.equal(result.iconMode, "loose-png-only");
    const archive = await inspectUnsignedSigningArchive(result.blob),
      info = archive.entries.find((e) => e.path === root + "Info.plist"),
      p = readCustomizationPlist(new Uint8Array(await info.blob.arrayBuffer()));
    assert.equal(p.CFBundleIconName, undefined);
    assert.equal(p.CFBundleIcons.CFBundleAlternateIcons, undefined);
    assert.equal(p.CFBundleIcons.CFBundlePrimaryIcon.CFBundleIconName, undefined);
    assert.equal(p.CFBundleIconsExtra, "not an icon key");
    for (const key of ["CFBundleIcons", "CFBundleIcons~ipad"])
      for (const name of p[key].CFBundlePrimaryIcon.CFBundleIconFiles)
        assert.ok(archive.entries.some((e) => e.path === root + name));
    for (const e of initial.filter((e) => e.path !== info.path))
      assert.equal(
        await digest(archive.entries.find((n) => n.path === e.path).blob),
        await digest(e.blob),
      );
  });
  test("signed/extension archives, localized overrides and name collisions fail closed", async () => {
    const metadata = await prepareAppCustomization({ displayName: "Changed" });
    for (const path of [
      "embedded.mobileprovision",
      "_CodeSignature/CodeResources",
      "PlugIns/Extra.appex/Info.plist",
    ])
      await assert.rejects(
        customizeUnsignedIpa(
          await createSignedSigningArchive([...fixtures(), entry(path, encode("fixture"))]),
          metadata,
        ),
        /CUSTOMIZATION_FAILED/,
      );
    await assert.rejects(
      customizeUnsignedIpa(
        await createSignedSigningArchive([
          ...fixtures(),
          entry("en.lproj/InfoPlist.strings", encode('"CFBundleDisplayName"="Other";')),
        ]),
        metadata,
      ),
      /LOCALIZED_METADATA/,
    );
    await assert.rejects(
      customizeUnsignedIpa(
        await createSignedSigningArchive([
          ...fixtures(),
          entry(CUSTOM_ICON_FILES[0][0], encode("existing")),
        ]),
        await prepareAppCustomization({ icon: png() }),
      ),
      /ARCHIVE_INVALID/,
    );
  });
  test("cancellation is preserved before prepare and archive output", async () => {
    const controller = new AbortController();
    controller.abort();
    await assert.rejects(prepareAppCustomization({ icon: png() }, { signal: controller.signal }), {
      name: "AbortError",
    });
    await assert.rejects(
      customizeUnsignedIpa(
        new Blob(["test"]),
        await prepareAppCustomization({ displayName: "Cancelled" }),
        {
          signal: controller.signal,
        },
      ),
      { name: "AbortError" },
    );
  });
  test("browser integration customizes only after checked assembly and keeps native signing restrictions", () => {
    const browser = readFileSync(new URL("../browser-packager.mjs", import.meta.url), "utf8");
    assert.ok(
      browser.indexOf("prepareAppCustomization(appCustomization") < browser.indexOf("used = true"),
    );
    assert.ok(
      browser.indexOf("customizeUnsignedIpa(download.blob") > browser.indexOf("runPackagingJob({"),
    );
    assert.ok(browser.includes("committedDownloads.set(error, download)"));
  });
  test("binary reference expansion and serialized text have aggregate allocation budgets", () => {
    const bytes = new Uint8Array(
      Buffer.from(
        python(`import plistlib,base64
s="a"*100000
print(base64.b64encode(plistlib.dumps({"shared":[s]*20},fmt=plistlib.FMT_BINARY)).decode())`).trim(),
        "base64",
      ),
    );
    assert.ok(bytes.length < 110000);
    assert.throws(() => readCustomizationPlist(bytes), /PLIST_INVALID/);
    assert.throws(
      () => writeCustomizationPlist(dict({ repeated: Array(20).fill("a".repeat(100000)) })),
      /PLIST_INVALID/,
    );
  });
  test("archive customization reads large resources only in bounded slices, not whole archive", async () => {
    const initial = fixtures();
    const resource = new Blob(Array(128).fill(new Uint8Array(65536)));
    initial.push({
      path: root + "large-inert-resource",
      size: resource.size,
      mode: 0o644,
      blob: resource,
    });
    const original = await createSignedSigningArchive(initial);
    let largest = 0;
    class TrackedBlob extends Blob {
      arrayBuffer() {
        largest = Math.max(largest, this.size);
        assert.ok(this.size <= 4 * 1024 * 1024, `unexpected whole-file read ${this.size}`);
        return super.arrayBuffer();
      }
      slice(start, end, type) {
        return new TrackedBlob([super.slice(start, end, type)]);
      }
    }
    const result = await customizeUnsignedIpa(
      new TrackedBlob([original]),
      await prepareAppCustomization({ displayName: "Bounded" }),
    );
    assert.ok(result.changed);
    assert.equal(largest, 4 * 1024 * 1024);
  });
  test("signing still checks the final root identifier with no caller-selected native edits", () => {
    const bridge = readFileSync(
      new URL("../toolchain/zsign-wasm/prepare-source.py", import.meta.url),
      "utf8",
    );
    assert.ok(bridge.includes('fopen("/work/Payload/App.app/Info.plist", "rb")'));
    assert.ok(
      bridge.includes('zsa.ValidateProfileForBundle(infoPlist["CFBundleIdentifier"].as_string(),'),
    );
    assert.ok(
      bridge.includes(
        "!bundleVersion.empty() || !displayName.empty() || !entitlementsFile.empty()",
      ),
    );
    const gate = readFileSync(
      new URL("../toolchain/zsign-wasm/profile-compatibility.inc", import.meta.url),
      "utf8",
    );
    assert.ok(gate.includes('entitlements["application-identifier"] = application'));
    assert.ok(gate.includes("D2RCertificateTeam(cert, team)"));
    assert.ok(gate.includes("D2RConcreteEntitlements(entitlements, 0, remaining)"));
  });

  test("a blank request cannot revoke an already committed archive on late cancellation", async () => {
    const options = await prepareAppCustomization({});
    const blob = new Blob(["already committed"]);
    const controller = new AbortController();
    controller.abort();
    assert.equal(
      (await customizeUnsignedIpa(blob, options, { signal: controller.signal })).blob,
      blob,
    );
  });

  test("binary negative zero survives the independent XML plist round-trip", () => {
    const bytes = new Uint8Array(
      Buffer.from(
        python(`import plistlib,base64
print(base64.b64encode(plistlib.dumps({"negativezero": -0.0},fmt=plistlib.FMT_BINARY)).decode())`).trim(),
        "base64",
      ),
    );
    const value = readCustomizationPlist(bytes),
      xml = writeCustomizationPlist(value);
    assert.match(new TextDecoder().decode(xml), /<real>-0<\/real>/);
    assert.deepEqual(readCustomizationPlist(xml), value);
    assert.equal(
      python(
        `import plistlib,base64,sys,math
v=plistlib.loads(base64.b64decode(sys.stdin.read()))
assert math.copysign(1.0,v["negativezero"]) == -1.0
print("ok")`,
        Buffer.from(xml).toString("base64"),
      ).trim(),
      "ok",
    );
  });

  test("binary dates outside supported four-digit XML years reject before serialization", () => {
    const bytes = new Uint8Array(
      Buffer.from(
        python(`import plistlib,base64,datetime,struct
v=plistlib.dumps({"future":datetime.datetime(2001,1,1)},fmt=plistlib.FMT_BINARY)
needle=b"\\x33"+b"\\x00"*8
assert v.count(needle)==1
seconds=(datetime.datetime(9999,12,31)-datetime.datetime(2001,1,1)).total_seconds()+86400
v=v.replace(needle,b"\\x33"+struct.pack(">d",seconds))
print(base64.b64encode(v).decode())`).trim(),
        "base64",
      ),
    );
    assert.throws(() => readCustomizationPlist(bytes), /PLIST_INVALID/);
  });

  test("near-budget malformed entity runs reject before entity substitution", () => {
    const bytes = encode(
      `<plist version="1.0"><dict><key>hostile</key><string>${"&".repeat(1024 * 1024 - 200)}</string></dict></plist>`,
    );
    assert.ok(bytes.length < 1024 * 1024);
    assert.throws(() => readCustomizationPlist(bytes), /PLIST_INVALID/);
    const source = readFileSync(new URL("../app-customization-plist.mjs", import.meta.url), "utf8");
    const start = source.indexOf("const unescape =");
    assert.ok(source.indexOf(".test(value)", start) < source.indexOf("value.replace", start));
  });

  test("binary string and key carriage returns survive conforming XML decoding", () => {
    const bytes = new Uint8Array(
      Buffer.from(
        python(`import plistlib,base64
print(base64.b64encode(plistlib.dumps({"line\\rkey": "a\\rb\\r\\nc"},fmt=plistlib.FMT_BINARY)).decode())`).trim(),
        "base64",
      ),
    );
    const value = readCustomizationPlist(bytes),
      xml = writeCustomizationPlist(value);
    assert.deepEqual(readCustomizationPlist(xml), value);
    assert.equal(value["line\rkey"], "a\rb\r\nc");
    assert.equal(
      python(
        `import plistlib,base64,sys
v=plistlib.loads(base64.b64decode(sys.stdin.read()))
assert v["line\\rkey"] == "a\\rb\\r\\nc"
print("ok")`,
        Buffer.from(xml).toString("base64"),
      ).trim(),
      "ok",
    );
  });

  test("XML literal CRLF and CR normalize but numeric carriage returns stay intact", () => {
    const bytes = encode(
      '<plist version="1.0"><dict><key>line\r\nkey</key><string>a\rb\r\nc&#13;d&#xD;e</string></dict></plist>',
    );
    const value = readCustomizationPlist(bytes);
    assert.equal(value["line\nkey"], "a\nb\nc\rd\re");
    assert.equal(
      python(
        `import plistlib,base64,sys
v=plistlib.loads(base64.b64decode(sys.stdin.read()))
assert v["line\\nkey"] == "a\\nb\\nc\\rd\\re"
print("ok")`,
        Buffer.from(bytes).toString("base64"),
      ).trim(),
      "ok",
    );
  });
}

// app-customization-defaults
{
  // 固定公开元数据和惰性表单状态；不执行外壳、编译器或游戏。
  // Pinned public metadata and inert form state; no shell, compiler or game execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const {
    resolveAppCustomizationDefaults,
    updateAppCustomizationDraft,
    normalizeAppCustomizationDraft,
  } = appCustomizationDefaultsModule;
  const { BUNDLED_SHELL_PROFILE_IDS, resolveShellProfile } = shellProfilesModule;
  const { normalizeAppCustomization, prepareAppCustomization, customizeUnsignedIpa } =
    appCustomizationModule;

  test("visible defaults come from each selected bundled shell's inspected plist, not its release tag", () => {
    for (const id of BUNDLED_SHELL_PROFILE_IDS) {
      const defaults = resolveAppCustomizationDefaults(id);
      const { evidence } = resolveShellProfile(id);
      assert.deepEqual(defaults, {
        displayName: evidence.inspection.plist.CFBundleDisplayName,
        bundleIdentifier: evidence.inspection.plist.CFBundleIdentifier,
        version: evidence.inspection.plist.CFBundleShortVersionString,
        buildVersion: evidence.inspection.plist.CFBundleVersion,
      });
      assert.deepEqual(defaults, {
        displayName: "D2R Mobile",
        bundleIdentifier: "org.d2rmobile.app",
        version: "1.0",
        buildVersion: "1",
      });
      assert.notEqual(defaults.displayName, evidence.inspection.plist.CFBundleName);
      assert.notEqual(defaults.version, evidence.release.replace(/^v/, ""));
      assert.ok(Object.isFrozen(defaults));
      assert.equal(Object.hasOwn(defaults, "icon"), false);
    }
    assert.throws(() => resolveAppCustomizationDefaults("unregistered"), /SHELL_PROFILE_UNKNOWN/);
    assert.throws(() => resolveAppCustomizationDefaults("v1.0.18-ios26"), /SHELL_PROFILE_UNKNOWN/);
  });

  test("untouched or restored defaults retain the exact original IPA with no customization transaction", async () => {
    for (const id of BUNDLED_SHELL_PROFILE_IDS) {
      const defaults = resolveAppCustomizationDefaults(id);
      for (const draft of [{}, { ...defaults }, { version: "1.0" }]) {
        const options = normalizeAppCustomizationDraft(draft, defaults);
        assert.deepEqual(options, {});
        const prepared = await prepareAppCustomization(options);
        const original = new Blob([
          "inert bytes; no archive parsing is needed for an unchanged IPA",
        ]);
        const result = await customizeUnsignedIpa(original, prepared);
        assert.equal(result.blob, original);
        assert.equal(result.changed, false);
      }
    }
    assert.throws(
      () => normalizeAppCustomization({ version: "1.0" }),
      /APP_CUSTOMIZATION_VERSION_INVALID/,
      "the custom-version validator remains unchanged",
    );
  });

  test("editing a field sends only that override and never rewrites other shell metadata", () => {
    const defaults = resolveAppCustomizationDefaults(BUNDLED_SHELL_PROFILE_IDS[0]);
    const draft = updateAppCustomizationDraft({}, defaults, "displayName", "My game");
    assert.deepEqual(draft, { displayName: "My game" });
    assert.deepEqual(normalizeAppCustomizationDraft(draft, defaults), draft);
    const restored = updateAppCustomizationDraft(
      draft,
      defaults,
      "displayName",
      defaults.displayName,
    );
    assert.deepEqual(restored, {});
    assert.deepEqual(draft, { displayName: "My game" }, "updates do not mutate the previous draft");
    assert.throws(
      () => normalizeAppCustomizationDraft({ version: "1.2" }, defaults),
      /APP_CUSTOMIZATION_VERSION_INVALID/,
    );
    assert.deepEqual(normalizeAppCustomizationDraft({ version: "1.2.3" }, defaults), {
      version: "1.2.3",
    });
    for (const draft of [null, [], { path: "not-an-identity-field" }])
      assert.throws(
        () => normalizeAppCustomizationDraft(draft, defaults),
        /APP_CUSTOMIZATION_INVALID/,
      );
  });

  test("profile changes refresh only untouched fields and preserve deliberate edits and custom icon", () => {
    const first = resolveAppCustomizationDefaults(BUNDLED_SHELL_PROFILE_IDS[0]);
    // 不同的自编默认值验证此约定，即使目前两个模板具有相同身份。
    // Distinct authored defaults prove this contract even though today's two shells share identity.
    const next = { ...first, displayName: "Another shell", version: "3.0" };
    const icon = new Blob(["inert icon selection"], { type: "image/png" });
    let draft = updateAppCustomizationDraft({ icon }, first, "buildVersion", "9");
    const displayed = (defaults) => ({ ...defaults, ...draft });
    assert.equal(displayed(first).displayName, first.displayName);
    assert.equal(displayed(next).displayName, next.displayName);
    assert.equal(displayed(next).version, next.version);
    assert.equal(displayed(next).buildVersion, "9");
    assert.equal(displayed(next).icon, icon);
    draft = updateAppCustomizationDraft(draft, first, "displayName", next.displayName);
    assert.equal(displayed(next).displayName, next.displayName);
    assert.equal(
      displayed(first).displayName,
      next.displayName,
      "a deliberate edit survives a round trip",
    );
    assert.deepEqual(normalizeAppCustomizationDraft(draft, next), { buildVersion: "9", icon });
    assert.equal(
      draft.displayName,
      next.displayName,
      "normalizing does not discard an explicit edit",
    );
    draft = {};
    assert.deepEqual(
      displayed(next),
      next,
      "reset resolves the current shell, not the initial shell",
    );
    assert.deepEqual(normalizeAppCustomizationDraft(draft, next), {});
  });

  test("a blank field remains editable and restores its selected-shell value on blur", () => {
    const defaults = resolveAppCustomizationDefaults(BUNDLED_SHELL_PROFILE_IDS[0]);
    const blank = updateAppCustomizationDraft({}, defaults, "version", "");
    assert.deepEqual(blank, { version: "" });
    assert.deepEqual(normalizeAppCustomizationDraft(blank, defaults), {});
    assert.deepEqual(updateAppCustomizationDraft(blank, defaults, "version", defaults.version), {});
  });
}

// app-customization-field-handlers
{
  // 只执行从表单源码读取的隔离回调；不启动浏览器或读取用户文件。
  // Execute only isolated callbacks read from the form source; no browser or user files.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: vm } = nodeVmModule;
  const { default: ts } = typescriptModule;
  const { resolveAppCustomizationDefaults, updateAppCustomizationDraft } =
    appCustomizationDefaultsModule;
  const source = await readFile(
    new URL("../src/components/AppCustomizationFields.tsx", import.meta.url),
    "utf8",
  );
  const parsed = ts.createSourceFile(
    "Fields.tsx",
    source,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TSX,
  );
  function callback(attribute, contains) {
    const found = [];
    function visit(node) {
      if (ts.isJsxAttribute(node) && node.name.getText(parsed) === attribute) {
        const expression = node.initializer?.expression;
        if (expression?.getText(parsed).includes(contains)) found.push(expression.getText(parsed));
      }
      ts.forEachChild(node, visit);
    }
    visit(parsed);
    assert.equal(found.length, 1, `${attribute}: ${contains}`);
    return found[0];
  }
  const defaults = resolveAppCustomizationDefaults("v2.0.0-ios26");
  function context(overrides = {}) {
    const changes = [];
    return {
      changes,
      scope: {
        defaults,
        value: {},
        name: "version",
        disabled: false,
        iconInput: { current: { value: "selected.png" } },
        updateAppCustomizationDraft,
        onChange: (value) => changes.push(value),
        ...overrides,
      },
    };
  }

  test("actual text callback stores only the edit and restores exact shell values without an override", () => {
    const { changes, scope } = context();
    const change = vm.runInNewContext(callback("onChange", "updateAppCustomizationDraft"), scope);
    change({ target: { value: "2.3.4" } });
    change({ target: { value: defaults.version } });
    assert.deepEqual(changes, [{ version: "2.3.4" }, {}]);
  });

  test("actual blur callback restores blanks, preserves deliberate edits and ignores disabled controls", () => {
    const blur = callback("onBlur", "updateAppCustomizationDraft");
    for (const [value, disabled, count] of [
      [{ version: "" }, false, 1],
      [{ version: "2.3.4" }, false, 0],
      [{ version: "" }, true, 0],
    ]) {
      const { changes, scope } = context({ value, disabled });
      vm.runInNewContext(blur, scope)();
      assert.equal(changes.length, count);
      if (count) assert.deepEqual(changes[0], {});
    }
  });

  test("actual reset clears text edits and file selection, is repeatable, and respects disabled state", () => {
    const reset = callback("onClick", "onChange({})");
    const { changes, scope } = context({ value: { displayName: "Changed", icon: {} } });
    const invoke = vm.runInNewContext(reset, scope);
    invoke();
    invoke();
    assert.deepEqual(changes.map(Object.keys), [[], []]);
    assert.equal(scope.iconInput.current.value, "");
    const blocked = context({ disabled: true });
    vm.runInNewContext(reset, blocked.scope)();
    assert.equal(blocked.changes.length, 0);
    assert.equal(blocked.scope.iconInput.current.value, "selected.png");
  });

  test("actual icon removal retains deliberate text edits and returns to the shell icon", () => {
    const { changes, scope } = context({ value: { displayName: "Changed", icon: {} } });
    vm.runInNewContext(callback("onClick", "icon: undefined"), scope)();
    assert.equal(changes[0].displayName, "Changed");
    assert.equal(changes[0].icon, undefined);
    assert.equal(scope.iconInput.current.value, "");
    assert.doesNotMatch(source, /<img\b|createObjectURL|FileReader|fetch\(/);
  });
}

// language-switch
{
  // 仅测试惰性偏好存储及源码接线，不使用浏览器、运行时或私有文件。
  // Inert preference-store and source-wiring tests only; no browser, runtime, or private files.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: ts } = typescriptModule;
  const { createLanguagePreference, browserLanguage, setBrowserLanguage, subscribeLanguage, t } =
    i18nModule;
  const { catalog } = i18nCatalogModule;

  function environment(language = "en", saved) {
    const values = new Map(saved ? [["d2r-ui-language", saved]] : []);
    const writes = [];
    return {
      navigator: { language },
      localStorage: {
        getItem: (key) => values.get(key) ?? null,
        setItem: (key, value) => {
          writes.push([key, value]);
          values.set(key, value);
        },
      },
      values,
      writes,
    };
  }

  test("browser language remains automatic until a supported explicit preference exists", () => {
    for (const [language, expected] of [
      ["zh-CN", "zh-CN"],
      ["zh-TW", "zh-CN"],
      ["ZH-HK", "zh-CN"],
      ["en-GB", "en"],
      ["ja", "en"],
      [undefined, "en"],
    ]) {
      const env = environment(language);
      assert.equal(createLanguagePreference(env).getLanguage(), expected);
      assert.deepEqual(env.writes, []);
    }
    assert.equal(createLanguagePreference(environment("zh-CN", "en")).getLanguage(), "en");
    assert.equal(createLanguagePreference(environment("en", "zh-CN")).getLanguage(), "zh-CN");
    assert.equal(createLanguagePreference(environment("zh-CN", "invalid")).getLanguage(), "zh-CN");
  });

  test("explicit choices notify mounted subscribers, persist only the preference, and survive a new store", () => {
    const env = environment("en");
    const preference = createLanguagePreference(env);
    const seen = [];
    const unsubscribe = preference.subscribe(() => seen.push(preference.getLanguage()));
    assert.equal(preference.setLanguage("zh-CN"), true);
    assert.equal(preference.setLanguage("zh-CN"), true);
    assert.deepEqual(seen, ["zh-CN"]);
    assert.deepEqual(env.writes, [
      ["d2r-ui-language", "zh-CN"],
      ["d2r-ui-language", "zh-CN"],
    ]);
    assert.equal(createLanguagePreference(env).getLanguage(), "zh-CN");
    unsubscribe();
    preference.setLanguage("en");
    assert.deepEqual(seen, ["zh-CN"]);
    assert.equal(preference.setLanguage("arbitrary"), false);
    assert.equal(preference.getLanguage(), "en");
    assert.equal(env.writes.length, 3);
  });

  test("choosing the current browser default still persists an explicit preference", () => {
    const env = environment("en");
    const preference = createLanguagePreference(env);
    assert.equal(preference.getLanguage(), "en");
    env.navigator.language = "zh-CN";
    assert.equal(preference.getLanguage(), "zh-CN");
    preference.setLanguage("zh-CN");
    assert.deepEqual(env.writes, [["d2r-ui-language", "zh-CN"]]);
    env.navigator.language = "en";
    assert.equal(preference.getLanguage(), "zh-CN");
    assert.equal(createLanguagePreference(env).getLanguage(), "zh-CN");
  });

  test("denied storage getters, reads, and writes retain a usable in-memory language switch", () => {
    for (const denied of [
      {
        get localStorage() {
          throw new Error("denied getter");
        },
      },
      {
        localStorage: {
          getItem() {
            throw new Error("denied read");
          },
          setItem() {
            throw new Error("denied write");
          },
        },
      },
      {},
    ]) {
      const preference = createLanguagePreference(
        Object.create(denied, { navigator: { value: { language: "zh-CN" } } }),
      );
      let changes = 0;
      preference.subscribe(() => changes++);
      assert.equal(preference.getLanguage(), "zh-CN");
      assert.equal(preference.setLanguage("en"), true);
      assert.equal(preference.getLanguage(), "en");
      assert.equal(changes, 1);
    }
  });

  test("the shared translation bridge switches future text while earlier messages retain their original text", () => {
    const original = browserLanguage();
    const seen = [];
    const unsubscribe = subscribeLanguage(() => seen.push(t("dmWebChoose")));
    try {
      setBrowserLanguage("en");
      const earlierMessage = t("dmWebChoose");
      setBrowserLanguage("zh-CN");
      assert.equal(t("dmWebChoose"), catalog.dmWebChoose[1]);
      assert.equal(earlierMessage, catalog.dmWebChoose[0]);
      assert.equal(seen.at(-1), catalog.dmWebChoose[1]);
    } finally {
      unsubscribe();
      setBrowserLanguage(original);
    }
  });

  test("top selector rerenders the mounted App and html language without remounting or restarting bootstrap", async () => {
    const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const parsed = ts.createSourceFile(
      "App.tsx",
      app,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.TSX,
    );
    const nodes = [];
    function visit(node) {
      nodes.push(node);
      ts.forEachChild(node, visit);
    }
    visit(parsed);
    const selector = nodes.find(
      (node) =>
        ts.isJsxOpeningElement(node) &&
        node.tagName.getText(parsed) === "NativeSelect" &&
        node.attributes.properties.some(
          (entry) =>
            entry.name.getText(parsed) === "id" && entry.initializer?.text === "ui-language",
        ),
    );
    assert.ok(selector);
    assert.equal(
      selector.attributes.properties.some((entry) => entry.name.getText(parsed) === "disabled"),
      false,
    );
    const change = selector.attributes.properties.find(
      (entry) => entry.name.getText(parsed) === "onChange",
    ).initializer.expression;
    assert.equal(change.getText(parsed), "(event) => setBrowserLanguage(event.target.value)");
    assert.match(
      app,
      /useSyncExternalStore\(subscribeLanguage, browserLanguage, browserLanguage\)/,
    );
    assert.ok(selector.pos < app.indexOf("<InstallationGuide"));
    assert.match(app, /catalog\.dmWebLanguageLabel\[0\]/);
    assert.match(app, /catalog\.dmWebLanguageLabel\[1\]/);
    assert.deepEqual(catalog.dmWebLanguageLabel, ["Language", "语言"]);
    assert.match(app, /catalog\.dmWebLanguageEnglish\[0\]/);
    assert.deepEqual(catalog.dmWebLanguageEnglish, ["English", "英语"]);
    assert.match(app, /catalog\.dmWebLanguageChinese\[1\]/);
    assert.deepEqual(catalog.dmWebLanguageChinese, ["Simplified Chinese", "简体中文"]);
    const effects = nodes.filter(
      (node) => ts.isCallExpression(node) && node.expression.getText(parsed) === "useEffect",
    );
    const bootstrap = effects.find((node) =>
      node.getText(parsed).includes("prepareBrowserPackager"),
    );
    assert.equal(bootstrap.arguments[1].getText(parsed), "[appendActivity]");
    const documentEffect = effects.find((node) =>
      node.getText(parsed).includes("document.documentElement.lang"),
    );
    assert.equal(documentEffect.arguments[1].getText(parsed), "[language]");
    assert.match(documentEffect.getText(parsed), /document.documentElement.lang = language/);
    assert.doesNotMatch(app, /key=\{language\}/);
    assert.doesNotMatch(app, /dmWebLanguageHistoryHelp|ui-language-help/);
    assert.equal(Object.hasOwn(catalog, "dmWebLanguageHistoryHelp"), false);
  });
}

// installation-guide
{
  // 只检查静态指南及声明，不执行浏览器、编译器、游戏或设备。
  // Static guide and notice checks only; no browser, compiler, game or device execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { catalog } = i18nCatalogModule;
  const { IPA_SIGNING_LIMITS } = ipaSigningContractModule;

  const guide = await readFile(
    new URL("../src/components/InstallationGuide.tsx", import.meta.url),
    "utf8",
  );
  const faq = await readFile(
    new URL("../src/components/PreparationFaq.tsx", import.meta.url),
    "utf8",
  );

  test("preparation is visible plain numbered text, not another component or disclosure", () => {
    const imports = [...guide.matchAll(/from "([^"]+)"/g)].map((match) => match[1]);
    assert.deepEqual(imports, ["../../i18n.mjs"]);
    assert.match(guide, /<section aria-labelledby="preparation-heading"/);
    assert.match(guide, /<ol className="list-decimal/);
    for (const key of ["dmWebGuidePrepare", "dmWebGuideOptionalSigning", "dmWebGuideExpect"])
      assert.ok(guide.includes(key));
    assert.doesNotMatch(
      guide,
      /Accordion|<details|hidden|Stepper|Questionnaire|useState|useEffect|setTimeout|fetch\(|localStorage|onDrop|type="file"|completed=/,
    );
  });

  test("generation reminder stays visible beside Generate and sets expectations without a countdown", async () => {
    const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    assert.match(
      app,
      /<p className="text-xs leading-relaxed text-muted-foreground">\s*\{t\("dmWebGenerationExpectation"\)\}\s*<\/p>/,
    );
    const reminder = app.indexOf('t("dmWebGenerationExpectation")');
    assert.ok(reminder > app.indexOf('t("dmWebGenerate")'));
    assert.ok(reminder < app.indexOf("{generationReadinessMessage && ("));
    assert.equal(
      catalog.dmWebGenerationExpectation[0],
      "Keep this tab in the foreground while generating. The first build may take 30 minutes or longer, depending on your computer.",
    );
    assert.match(
      catalog.dmWebGenerationExpectation[1],
      /生成期间.*标签页.*前台.*首次生成可能需要 30 分钟或更长时间.*取决于你的电脑/,
    );
  });

  test("FAQ remains a labeled single-open collapsible section", () => {
    assert.match(faq, /<Accordion type="single" collapsible>/);
    assert.match(faq, /aria-labelledby="faq-heading"/);
  });

  test("all visible copy has English/Chinese translations and preparation avoids placeholder filenames", () => {
    for (const key of [...(guide + faq).matchAll(/"(dmWeb\w+)"/g)].map((match) => match[1])) {
      assert.equal(catalog[key]?.length, 2, key);
      assert.ok(
        catalog[key].every((text) => text.length > 0),
        key,
      );
      if (guide.includes(`"${key}"`))
        assert.doesNotMatch(catalog[key].join(" "), /UUID|<UUID>|Select a step|展开/);
    }
    assert.match(
      catalog.dmWebBrowserOnly[0],
      /entirely in your browser.*No Xcode, Python, \.NET installation, or command-line tools required/,
    );
    assert.match(
      catalog.dmWebGuideTargets[0],
      /iOS 26\+.*iOS 18\+.*BC texture support.*experimental.*unverified/,
    );
  });

  test("input preparation preserves the one-folder, optional-key/DLC and first-install rules", () => {
    assert.match(
      catalog.dmWebGuidePackages[0],
      /prod.keys at the root of one folder.*title.keys.*DLC is optional/,
    );
    assert.match(
      catalog.dmWebGuideExtracted[0],
      /merged ExeFS\/RomFS.*without keys.*not expansion.*automatically/,
    );
    assert.match(catalog.dmWebGuideFolderAccess[0], /read\/write/);
    assert.match(
      catalog.dmWebGuideExport[0],
      /first install.*off.*already on the device.*cannot check/,
    );
    assert.match(
      catalog.dmWebGuideOutputs[0],
      /separately inside the selected input folder.*not inside the IPA.*actual output names/,
    );
  });

  test("local signing preparation distinguishes certificate, private key, profile and password", () => {
    assert.match(catalog.dmWebGuideSigningIntro[0], /optional.*unsigned IPA output is the default/);
    assert.match(
      catalog.dmWebGuideIdentity[0],
      /certificate with its private key.*not a \.cer file alone.*Keychain Access.*password-protected P12/,
    );
    assert.match(
      catalog.dmWebGuideNewCertificate[0],
      /Apple Developer Program.*Account Holder\/Admin.*private key and CSR locally.*only the CSR.*Apple Distribution.*same key as P12/,
    );
    assert.match(
      catalog.dmWebGuideProfile[0],
      /device’s UDID.*App ID.*Ad Hoc.*same certificate.*device.*Bundle ID.*profile/,
    );
    assert.match(
      catalog.dmWebGuideSigningInputs[0],
      /P12 export password, not your Apple Account password.*passwordless.*cannot verify device eligibility/,
    );
    assert.match(catalog.dmWebGuidePrivateKey[0], /Keep your private key.*local.*Do not upload/);
    assert.match(
      catalog.dmWebFaqPrivacyAnswer[0],
      /not uploaded or included in the IPA.*profile is embedded/,
    );
    assert.match(
      catalog.dmWebFaqSigningAnswer[0],
      /local signing.*zsign.*still needs installation.*does not guarantee/,
    );
    for (const key of ["dmWebSigningIdentity", "dmWebSigningProfile", "dmWebGuideSigningInputs"])
      assert.doesNotMatch(catalog[key].join(" "), /4 MiB|4mb|4 MB/i);
    assert.equal(IPA_SIGNING_LIMITS.identityBytes, 4 * 1024 * 1024);
    assert.equal(IPA_SIGNING_LIMITS.profileBytes, 4 * 1024 * 1024);
    for (const url of [
      "https://developer.apple.com/help/account/certificates/create-a-certificate-signing-request",
      "https://support.apple.com/guide/keychain-access/import-and-export-keychain-items-kyca35961/mac",
      "https://docs.openssl.org/3.0/man1/openssl-pkcs12/",
      "https://developer.apple.com/help/account/provisioning-profiles/create-an-ad-hoc-provisioning-profile/",
    ])
      assert.ok(guide.includes(url));
    assert.doesNotMatch(
      guide + faq + JSON.stringify(catalog),
      /Sideloadly|sideloadly\.io|daisign\.lol/,
    );
  });

  test("postgeneration copy accepts actual names and does not imply signing installs the app", () => {
    for (const [key, count] of [
      ["dmWebResultInstall", 1],
      ["dmWebResultLaunch", 1],
      ["dmWebResultCopy", 2],
    ])
      for (const value of catalog[key]) assert.equal((value.match(/%s/g) ?? []).length, count, key);
    assert.match(catalog.dmWebResultInstall[0], /Signing and installation are separate/);
    assert.match(catalog.dmWebResultUnsigned[0], /unsigned.*before installing/);
    assert.match(catalog.dmWebResultSkipped[0], /matching resources already on your device/);
    assert.match(catalog.dmWebResultIncomplete[0], /incomplete.*Do not copy/);
    assert.match(catalog.dmWebResultLegacyIncomplete[0], /Do not copy/);
    assert.match(
      catalog.dmWebGuideSigned[0],
      /profile must cover your device.*Apple Configurator.*does not install/,
    );
    assert.match(
      catalog.dmWebGuideTransfer[0],
      /Finder.*whole resource folder.*Apple Devices.*not signing\/installing an unsigned IPA/,
    );
    assert.match(
      catalog.dmWebGuideZip[0],
      /zip the complete folder.*extra ZIP wrapper.*space for both/,
    );
    assert.match(
      catalog.dmWebGuideRetry[0],
      /copying to finish.*Retry.*does not need NSP\/XCI or key files/,
    );
    assert.match(
      catalog.dmWebGuideRefresh[0],
      /Do not delete the app.*saves and resources can be removed/,
    );
    assert.doesNotMatch(catalog.dmWebGuideRefresh[0], /7 days/);
  });
}

// post-generation-guide
{
  // 仅测试已提交输出模型及源码接线，不生成产物，不使用浏览器或私有数据。
  // Committed-output models and source wiring only; no generation, browser or private data.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { createPostGenerationGuide } = postGenerationGuideModule;

  const defaults = {
    displayName: "D2R Mobile",
    bundleIdentifier: "org.d2rmobile.app",
    version: "1.0",
    buildVersion: "1",
  };
  const resources = { status: "complete", directoryName: "data" };
  const legacyPack = { status: "complete", directoryName: "Legacy Device Pack-3" };

  test("instructions preserve the actual produced folder names and custom app name", () => {
    const result = createPostGenerationGuide({ resources, legacyPack }, "My D2R");
    assert.deepEqual(result, {
      appName: "My D2R",
      resourcesDirectory: resources.directoryName,
      resourcesStatus: "complete",
      legacyDirectory: legacyPack.directoryName,
      legacyIncomplete: false,
    });
    assert.equal(Object.isFrozen(result), true);
  });

  test("legacy instructions require a completed real export, not a selected option", () => {
    const result = createPostGenerationGuide({ resources }, defaults.displayName);
    assert.equal(result.legacyDirectory, null);
    assert.equal(result.legacyIncomplete, false);
    for (const outcome of [
      undefined,
      { status: "pending", directoryName: "partial" },
      { status: "incomplete", directoryName: "partial" },
    ]) {
      const result = createPostGenerationGuide(
        { resources, legacyPack: outcome },
        defaults.displayName,
      );
      assert.equal(result.legacyDirectory, null);
      assert.equal(result.legacyIncomplete, outcome?.status === "incomplete");
    }
  });

  test("skipped, missing and incomplete resources never produce a copy-success instruction", () => {
    for (const outcome of [
      undefined,
      { status: "incomplete", directoryName: "partial" },
      { status: "complete", directoryName: "" },
      { status: "complete", directoryName: "../partial" },
    ]) {
      const result = createPostGenerationGuide({ resources: outcome }, defaults.displayName);
      assert.equal(result.resourcesDirectory, null);
      assert.equal(result.resourcesStatus, "incomplete");
    }
    const skipped = createPostGenerationGuide(
      { resources: { status: "skipped" }, legacyPack },
      defaults.displayName,
    );
    assert.equal(skipped.resourcesStatus, "skipped");
    assert.equal(skipped.resourcesDirectory, null);
    assert.equal(skipped.legacyDirectory, legacyPack.directoryName);
    assert.equal(createPostGenerationGuide({ resources }, ""), null);
  });

  test("UI snapshots successful identity, uses originals for recovery, and reads actual download filename", async () => {
    const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const guide = await readFile(
      new URL("../src/components/PostGenerationGuide.tsx", import.meta.url),
      "utf8",
    );
    assert.match(
      app,
      /createPostGenerationGuide\(\s*result,\s*preparedCustomization.displayName \?\? appCustomizationDefaults.displayName/,
    );
    assert.match(app, /createPostGenerationGuide\(report, appCustomizationDefaults.displayName\)/);
    assert.match(app, /download && completedOutput && !busy/);
    assert.match(app, /downloadFileName=\{download.fileName\}/);
    assert.match(app, /signed=\{signedDownload\}/);
    assert.match(guide, /output.resourcesDirectory &&/);
    assert.match(guide, /output.legacyDirectory &&/);
    assert.match(guide, /t\("dmWebResultCopy", output.resourcesDirectory, output.appName\)/);
    assert.match(guide, /t\("dmWebResultCopy", output.legacyDirectory, output.appName\)/);
    assert.doesNotMatch(guide, /legacyAstc|<UUID>|D2RMobile-unsigned\.ipa|org\.d2rmobile\.app/);
  });
}

// live-option-ui
{
  // 真实回调的隔离测试；不渲染页面、不运行编译器或访问私有文件。
  // Isolated real callbacks, not rendered UI, compiler execution or private file access.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { default: vm } = nodeVmModule;
  const { readFile } = promisesModule;
  const { default: ts } = typescriptModule;
  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  const parsed = ts.createSourceFile(
    "App.tsx",
    app,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TSX,
  );
  const nodes = [];
  function visit(node) {
    if (ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node)) nodes.push(node);
    ts.forEachChild(node, visit);
  }
  visit(parsed);
  function callback(anchor) {
    const node = nodes.find((node) => node.getText(parsed).includes(anchor));
    assert.ok(node, anchor);
    const attribute = node.attributes.properties.find(
      (property) =>
        ts.isJsxAttribute(property) &&
        ["onChange", "onValueChange", "onCheckedChange"].includes(property.name.getText(parsed)),
    );
    assert.ok(attribute, anchor);
    const expression = attribute.initializer.expression;
    // 仅保留参数名称及函数体，类型只解析，不编译或执行。
    // Parameter names + body only: types are parsed, never compiled or executed.
    return {
      code: `(${expression.parameters.map((parameter) => parameter.name.getText(parsed)).join(",")}) => ${expression.body.getText(parsed)}`,
      native: attribute.name.getText(parsed) === "onChange",
    };
  }
  const cases = [
    ['id="ios-target"', "setTarget", "ios18", "value"],
    ["checked={legacyAstc}", "setLegacyAstc", true, "checked"],
    ['id="astc-preset"', "setAstcPreset", "fast", "value"],
    ["checked={signing}", "setSigning", true, "checked"],
    ["checked={ignoreCache}", "setIgnoreCache", true, "checked"],
  ];
  function context(overrides = {}) {
    const writes = [];
    return {
      writes,
      value: {
        target: "ios26",
        legacyAstc: false,
        astcPreset: "thorough",
        signing: false,
        ignoreCache: false,
        busy: false,
        generating: false,
        finished: false,
        download: null,
        folder: { name: "selected" },
        hasSelectedFolder: true,
        operationGate: { current: { busy: false } },
        clearSigningInputs: () => writes.push(["clearSigningInputs"]),
        ...Object.fromEntries(
          cases.map(([, setter]) => [setter, (value) => writes.push([setter, value])]),
        ),
        ...overrides,
      },
    };
  }
  function invoke(anchor, value, field, globals) {
    const handler = callback(anchor);
    vm.runInNewContext(
      handler.code,
      globals,
    )(handler.native ? { target: { [field]: value } } : value);
  }
  test("all configuration choices remain editable after a successful folder selection", () => {
    for (const [anchor, setter, value, field] of cases) {
      const state = context();
      invoke(anchor, value, field, state.value);
      assert.deepEqual(state.writes, [[setter, value]]);
    }
  });
  test("operation ownership and terminal output protect every option from stale events", () => {
    for (const override of [
      { busy: true },
      { generating: true },
      { finished: true },
      { download: {} },
      { operationGate: { current: { busy: true } } },
    ])
      for (const [anchor, , value, field] of cases) {
        const state = context(override);
        invoke(anchor, value, field, state.value);
        assert.deepEqual(state.writes, [], anchor);
      }
  });
  test("disabling local signing clears transient credentials without discarding output", () => {
    const state = context({ signing: true });
    invoke("checked={signing}", false, "checked", state.value);
    assert.deepEqual(state.writes, [["clearSigningInputs"], ["setSigning", false]]);
  });
  test("option changes do not restart bootstrap, consume a job or dispose retained output", () => {
    const setup = app.slice(
      app.indexOf("const preparedDownload ="),
      app.indexOf("  async function choose()"),
    );
    assert.match(setup, /editableOptions: true/);
    assert.doesNotMatch(app, /window\.location\.assign|setHasSelectedFolder/);
    for (const [anchor] of cases)
      assert.doesNotMatch(
        callback(anchor).code,
        /prepareBrowserPackager|\.close\(|\.dispose\(|setFolder|setDownload|setSigningRetryAvailable/,
      );
  });

  test("the removed content-hash option has no UI, URL state or generation override", () => {
    assert.doesNotMatch(
      app,
      /verify-content-hashes|setSkipContentHashes|dmWebVerifyHashOption|dmWebVerifyHashHelp|skipContentHashes/,
    );
    assert.match(app, /appendActivity\(t\("dmWebSkipHashActive"\), true\)/);
  });
}

// live-packager-options
{
  // 隔离模块模拟入口；不执行浏览器、WASM 或真实输入。
  // Isolated module mocks only; no browser, WASM or real inputs.
  const { default: test } = nodeTestModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;
  const { default: process } = nodeProcessModule;
  const { spawnSync } = nodeChild_processModule;
  test("live packager options use preloaded capabilities (isolated mocks)", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    assertIsolatedRun(
      spawnSync(
        process.execPath,
        [
          "--experimental-test-module-mocks",
          "--test",
          "--test-reporter=tap",
          new URL("./fixtures/live-packager-options.mock.mjs", import.meta.url).pathname,
        ],
        { env, encoding: "utf8", timeout: 30000, maxBuffer: 1024 * 1024 },
      ),
    );
  });
}

// resource-export-options-ui
{
  // 源码约束测试，不宣称浏览器交互或设备验证。
  // Source-contract tests do not claim browser interaction or device validation.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { catalog } = i18nCatalogModule;
  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  test("resource skip is opt-in, resets with selection, and has a distinct outcome", () => {
    assert.match(app, /\[skipResources, setSkipResources\] = useState\(false\)/);
    assert.match(app, /exportResources: !skipResources/);
    assert.match(app, /if \(!current\(\)\) return;\s+setSkipResources\(false\)/);
    assert.match(app, /result.resources.status === "skipped"/);
    assert.match(app, /report\?\.resources\?\.status === "skipped"/);
    assert.match(app, /skipResources &&/);
    assert.match(app, /legacyAstc \? ` \$\{t\("dmWebSkipKeepsLegacy"\)\}` : ""/);
    assert.match(catalog.dmWebSkipResourcesHelp[0], /same game, update and DLC/);
    assert.match(catalog.dmWebResourcesSkipped[0], /skipped; device files were not checked/);
  });
  test("storage estimate is nonblocking and excludes invented requirements", () => {
    assert.match(
      app,
      /setReady\(true\);[\s\S]*?void readOpfsStorageAdvisory\(\{ signal: controller.signal \}\)/,
    );
    assert.match(
      app,
      /controller.signal.aborted \|\| !mounted.current \|\| estimate.status === "cancelled"/,
    );
    assert.match(catalog.dmWebStorageEstimate[0], /Not destination disk space or a reservation/);
    assert.match(catalog.dmWebStorageUnknown[0], /you can continue/);
  });
}

// packager-features-ui
{
  // 惰性界面契约与隔离回调测试，不加载浏览器、编译器、真实游戏或签名凭据。
  // Inert UI contracts and isolated callbacks; no browser, compiler, real game or signing credentials.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: vm } = nodeVmModule;
  const { default: ts } = typescriptModule;
  const { PROGRAM_COMPATIBILITY } = programCompatibilityModule;
  const { catalog } = i18nCatalogModule;
  const { createPostGenerationGuide } = postGenerationGuideModule;
  const { createGenerationProgress } = generationProgressModule;
  const { getGenerationReadiness } = generationReadinessModule;
  const { appCustomizationFailureMessage, gameFolderFailureMessage, packagingFailureSummary } =
    packagingDiagnosticsModule;
  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  const customization = await readFile(
    new URL("../src/components/AppCustomizationFields.tsx", import.meta.url),
    "utf8",
  );
  const parsed = ts.createSourceFile(
    "App.tsx",
    app,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TSX,
  );
  function handlers(tag, id) {
    const matches = [];
    function visit(node) {
      if (
        (ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node)) &&
        node.tagName.getText(parsed) === tag
      ) {
        const attributes = node.attributes.properties.filter(ts.isJsxAttribute);
        const identifier = attributes.find((attribute) => attribute.name.getText(parsed) === "id");
        if (!id || identifier?.initializer?.text === id) {
          const change = attributes.find(
            (attribute) => attribute.name.getText(parsed) === "onChange",
          );
          if (change?.initializer && ts.isJsxExpression(change.initializer))
            matches.push(change.initializer.expression.getText(parsed));
        }
      }
      ts.forEachChild(node, visit);
    }
    visit(parsed);
    assert.equal(matches.length, tag === "AppCustomizationFields" ? 2 : 1);
    return matches;
  }
  function inputContext(overrides = {}) {
    const writes = [];
    const context = {
      ready: true,
      busy: false,
      finished: false,
      download: null,
      inputMode: "packages",
      operationGate: { current: { busy: false } },
      t: (key) => key,
      ...Object.fromEntries(
        [
          "setInputMode",
          "setFolder",
          "setNspNames",
          "setSkipResources",
          "setStatus",
          "setAppCustomization",
          "setCustomizationError",
        ].map((name) => [name, (value) => writes.push([name, value])]),
      ),
      ...overrides,
    };
    return { writes, context };
  }
  test("customization callbacks ignore edits while unavailable or owned by an operation", () => {
    for (const overrides of [
      { ready: false },
      { busy: true },
      { finished: true },
      { download: {} },
      { operationGate: { current: { busy: true } } },
    ]) {
      const { context, writes } = inputContext(overrides);
      for (const handler of handlers("AppCustomizationFields"))
        vm.runInNewContext(handler, context)({ displayName: "changed" });
      assert.equal(writes.length, 0);
    }
  });
  test("folder auto-detection includes all packages and preserves the detected generation route", () => {
    assert.match(app, /useState<InputMode \| null>\(null\)/);
    assert.match(app, /discoverGameFolder\(selected, \{\s*signal:/);
    assert.match(app, /setInputMode\(files.inputMode\)/);
    assert.match(
      app,
      /jobOptions.inputMode === "packages" \? \{ selectedNspNames: nspNames \} : \{\}/,
    );
    assert.match(app, /packageCount: nspNames.length/);
    assert.match(app, /disabled=\{!generationReadiness.canGenerate\}/);
    assert.match(app, /dmWebIncludedPackages", nspNames.length/);
    assert.match(
      catalog.dmWebAutoInstructions[0],
      /NSP\/XCI.*prod.keys.*already-merged ExeFS\/RomFS/,
    );
    assert.match(catalog.dmWebExtractedInstructions[0], /Merged ExeFS and RomFS.*No keys needed/);
    assert.match(
      catalog.dmWebExtractedInstructions[0],
      /Base\/update only.*expansion input is unsupported.*not authenticated/,
    );
    assert.match(
      app,
      /if \(jobOptions.inputMode === "packages"\)\s*appendActivity\(\s*t\("dmWebSkipHashActive"/,
    );
  });
  test("compatibility display derives from the actual selected profile registry", () => {
    assert.match(
      app,
      /PROGRAM_COMPATIBILITY.releases[\s\S]*?profile.id === profileId[\s\S]*?programReleaseIds.includes\(release.id\)/,
    );
    assert.match(app, /dmWebSupportedVersions", supportedVersions/);
    assert.ok(PROGRAM_COMPATIBILITY.releases.length > 0);
    assert.equal(PROGRAM_COMPATIBILITY.arbitraryGameVersions, false);
    assert.equal(PROGRAM_COMPATIBILITY.arbitraryShellImports, false);
    assert.equal(PROGRAM_COMPATIBILITY.browserNativeSourceRebuild, false);
    assert.match(catalog.dmWebSupportedVersions[0], /only.*Other versions are not supported/);
  });
  test("advanced customization shows selected-shell defaults without persisting or submitting unchanged values", () => {
    const advanced = app.slice(
      app.indexOf('<AccordionItem value="advanced">'),
      app.indexOf("</AccordionContent>", app.indexOf('<AccordionItem value="advanced">')),
    );
    assert.match(advanced, /<AppCustomizationFields/);
    assert.match(advanced, /<AccordionContent forceMount/);
    assert.doesNotMatch(customization, /<AccordionItem|<AccordionContent/);
    assert.match(app, /useState<string\[\]>\(\[\]\)/);
    assert.match(app, /useState<AppCustomizationDraft>\(\{\}\)/);
    assert.match(app, /resolveAppCustomizationDefaults\(profileId\)/);
    assert.match(
      app,
      /normalizeAppCustomizationDraft\(appCustomization, appCustomizationDefaults\)/,
    );
    assert.match(customization, /value=\{value\[name\] \?\? defaults\[name\]\}/);
    assert.match(
      customization,
      /updateAppCustomizationDraft\(value, defaults, name, event.target.value\)/,
    );
    assert.match(customization, /dmWebCustomDefaultIcon/);
    assert.match(customization, /dmWebCustomReset/);
    assert.match(
      app,
      /Object.keys\(preparedCustomization\).length\s*\? \{ appCustomization: preparedCustomization \}\s*: \{\}/,
    );
    assert.match(customization, /type="file"\s*accept="image\/png,.png"/);
    assert.match(customization, /autoComplete="off"/);
    assert.doesNotMatch(customization, /fetch\(|URL.createObjectURL|localStorage|sessionStorage/);
    assert.match(catalog.dmWebCustomBundleHelp[0], /signing profile must match the new bundle ID/);
    assert.match(catalog.dmWebCustomIconHelp[0], /appearance is unverified/);
  });
  test("icon preflight precedes runtime consumption and invalid input remains correctable", () => {
    const generation = app.slice(
      app.indexOf("  async function generate()"),
      app.indexOf("  return (\n    <main"),
    );
    const preflight = generation.slice(0, generation.indexOf('    setResourceOutcome("")'));
    assert.match(
      preflight,
      /await prepareAppCustomization\(customizationValidation.options, \{\s*signal: controller.signal/,
    );
    assert.match(preflight, /controller.signal.throwIfAborted\(\)/);
    assert.match(preflight, /if \(!current\(\)\) \{\s*operation.finish\(\);\s*return;/);
    assert.match(
      preflight,
      /if \(controller.signal.aborted\)[\s\S]*?setFinished\(true\)[\s\S]*?await jobPackager.close\(\)/,
    );
    assert.match(
      preflight,
      /else \{\s*setCustomizationError\(appCustomizationFailureMessage\(error\)\);\s*setStatus\(t\("dmWebCustomCorrect"\)\)/,
    );
    assert.match(preflight, /if \(jobOptions.signing\) clearSigningInputs\(\)/);
    assert.match(preflight, /operation.finish\(\);\s*return;\s*\}/);
    assert.doesNotMatch(preflight, /jobPackager.generate\(|downloadPreparation.current.retain/);
  });
  test("version errors are allowlisted without raw hashes, file paths or messages", () => {
    for (const detailCode of [
      "PROGRAM_UNSUPPORTED_VERSION",
      "PROGRAM_MODULE_RELEASE_MISMATCH",
      "PROGRAM_SHELL_RELEASE_MISMATCH",
      "PROGRAM_INVALID_MODULE_SET",
      "PROGRAM_INPUT_DIGEST_MISMATCH",
      "PROGRAM_BUILD_ID_MISMATCH",
    ]) {
      const message = packagingFailureSummary({
        stage: "program",
        detailCode,
        message: "PRIVATE-HASH-PATH",
      });
      assert.match(message, new RegExp(`\\[${detailCode}\\]`));
      assert.doesNotMatch(message, /PRIVATE|undefined/);
      const secretMessage = packagingFailureSummary({ stage: "secrets", detailCode });
      if (["PROGRAM_UNSUPPORTED_VERSION", "PROGRAM_MODULE_RELEASE_MISMATCH"].includes(detailCode))
        assert.match(secretMessage, new RegExp(`\\[${detailCode}\\]`));
      else assert.doesNotMatch(secretMessage, new RegExp(detailCode));
      for (const stage of ["assets", "shaders", "discovery", "customization"])
        assert.doesNotMatch(packagingFailureSummary({ stage, detailCode }), new RegExp(detailCode));
    }
    assert.doesNotMatch(
      packagingFailureSummary({ stage: "assets", detailCode: "PROGRAM_SHELL_RELEASE_MISMATCH" }),
      /PROGRAM_SHELL_RELEASE_MISMATCH/,
    );
    assert.doesNotMatch(
      packagingFailureSummary({ stage: "program", detailCode: "PRIVATE-HASH-PATH" }),
      /PRIVATE/,
    );
  });
  test("discovery and customization diagnostics accept fixed codes only", () => {
    for (const code of [
      "MISSING_INPUT",
      "EXTRACTED_MISSING_INPUT",
      "EXTRACTED_DLC_UNSUPPORTED",
      "EXTRACTED_NAME",
      "EXTRACTED_LIMIT",
    ])
      assert.doesNotMatch(gameFolderFailureMessage(code), /undefined/);
    assert.equal(gameFolderFailureMessage("PRIVATE-PATH"), gameFolderFailureMessage(undefined));
    assert.match(
      appCustomizationFailureMessage(new Error("APP_CUSTOMIZATION_ICON_INVALID")),
      /opaque square PNG/,
    );
    assert.match(
      appCustomizationFailureMessage(new Error("APP_CUSTOMIZATION_BUNDLE_INVALID")),
      /bundle ID/,
    );
    assert.doesNotMatch(appCustomizationFailureMessage(new Error("PRIVATE-NAME")), /PRIVATE/);
    assert.doesNotMatch(
      packagingFailureSummary({ stage: "assets", detailCode: "APP_CUSTOMIZATION_ICON_INVALID" }),
      /APP_CUSTOMIZATION_ICON_INVALID/,
    );
  });
  test("feature copy is complete in both browser languages without implementation marketing", () => {
    for (const prefix of [
      "dmWebCustom",
      "dmWebExtracted",
      "dmWebProgram",
      "dmWebInput",
      "dmWebSupportedVersions",
    ]) {
      const entries = Object.entries(catalog).filter(([key]) => key.startsWith(prefix));
      assert.ok(entries.length > 0);
      for (const [key, translations] of entries) {
        assert.equal(translations.length, 2, key);
        assert.ok(
          translations.every((value) => typeof value === "string" && value.length > 0),
          key,
        );
        assert.doesNotMatch(translations[0], /React|shadcn|Tailwind|WebAssembly|powered by/);
      }
    }
  });

  // 只执行实际处理器的预检片段；打包器关闭为内存桩，不执行转换。
  // Execute only the actual handler's preflight slice; packager closure is an in-memory stub, never conversion.
  async function runPreflight(options, makeIcon) {
    const { prepareAppCustomization } = await import("../app-customization.mjs");
    const { createUiOperationGate } = await import("../ui-operation-gate.mjs");
    const generation = app.slice(app.indexOf("  async function generate()"));
    const source = generation.slice(
      generation.indexOf("{") + 1,
      generation.indexOf('    setResourceOutcome("")'),
    );
    const writes = [],
      progress = [],
      generationTracker = { current: null },
      cancellation = { current: null },
      mounted = { current: true };
    let closes = 0;
    const state = { ready: true, finished: false, busy: false, generating: false };
    const context = {
      ...state,
      generationReadiness: getGenerationReadiness({
        ...state,
        hasFolder: true,
        hasDownload: false,
        inputMode: "extracted",
        packageCount: 0,
        customizationError: "",
        signing: false,
        signingReady: false,
        identitySelected: false,
        profileSelected: false,
      }),
      folder: {},
      inputMode: "extracted",
      nspNames: [],
      profileId: "v2.0.0-ios26",
      legacyAstc: false,
      signing: false,
      astcPreset: "thorough",
      ignoreCache: false,
      skipResources: false,
      generationTracker,
      createGenerationProgress: (jobOptions) => {
        assert.equal(Object.isFrozen(jobOptions), true);
        return createGenerationProgress(jobOptions);
      },
      publishProgress: (tracker) => {
        if (mounted.current && generationTracker.current === tracker)
          progress.push(tracker.snapshot());
      },
      customizationError: "",
      customizationValidation: {
        options: makeIcon ? { icon: makeIcon(cancellation, mounted) } : options,
        error: "",
      },
      operationGate: { current: createUiOperationGate() },
      mounted,
      cancellation,
      packager: {
        current: {
          async close() {
            closes++;
          },
        },
      },
      prepareAppCustomization,
      appCustomizationFailureMessage,
      t: (key) => key,
      appendActivity: (value) => writes.push(["activity", value]),
      ...Object.fromEntries(
        [
          "Ready",
          "Finished",
          "Busy",
          "Generating",
          "Cancelling",
          "Status",
          "CustomizationError",
        ].map((key) => [
          `set${key}`,
          (value) => {
            state[key[0].toLowerCase() + key.slice(1)] = value;
            writes.push([key, value]);
          },
        ]),
      ),
    };
    const prepared = await vm.runInNewContext(
      `(async () => { ${source}\n return preparedCustomization; })()`,
      context,
    );
    return { prepared, state, writes, progress, closes, gate: context.operationGate.current };
  }

  test("actual icon preflight failure allows correction without closing the prepared packager", async () => {
    const result = await runPreflight({ icon: new Blob(["invalid PNG"]) });
    assert.equal(result.prepared, undefined);
    assert.equal(result.closes, 0);
    assert.equal(result.state.ready, true);
    assert.equal(result.state.finished, false);
    assert.equal(result.state.busy, false);
    assert.equal(result.state.generating, false);
    assert.equal(result.gate.busy, false);
    assert.match(result.state.customizationError, /opaque square PNG/);
    assert.equal(result.progress.at(-1).status, "failed");
    assert.equal(result.progress.at(-1).percent, 0);
  });

  test("actual icon preflight cancellation closes runtime once and releases the UI lease", async () => {
    const result = await runPreflight(
      undefined,
      (cancellation) =>
        new (class extends Blob {
          async arrayBuffer() {
            cancellation.current.abort();
            return super.arrayBuffer();
          }
        })(["cancelled PNG"]),
    );
    assert.equal(result.prepared, undefined);
    assert.equal(result.closes, 1);
    assert.equal(result.state.ready, false);
    assert.equal(result.state.finished, true);
    assert.equal(result.state.busy, false);
    assert.equal(result.state.generating, false);
    assert.equal(result.gate.busy, false);
    assert.equal(result.state.status, "dmWebCancelledReload");
    assert.equal(result.progress.at(-1).status, "cancelled");
    assert.equal(result.progress.at(-1).percent, 0);
  });

  // 恢复分支直接来自真实界面，覆盖自定义之前发生的提交后清理失败。
  // Use the real UI recovery branch, including post-commit cleanup failures before customization.
  test("every requested-customization recovery stays manual and labels the original IPA", () => {
    assert.match(app, /originalIdentityDownload\s*\? "dmWebCustomOriginalDownload"/);
    assert.match(
      catalog.dmWebCustomOriginalRetained[0],
      /not applied.*original app name, bundle ID, version, and icon.*not automatically signed/,
    );
    const recovery = app.slice(
      app.indexOf("      if (recovered) {"),
      app.indexOf("      appendActivity(packagingFailureSummary"),
    );
    for (const stage of ["cleanup", "customization"]) {
      for (const [requested, signing] of [
        [false, false],
        [false, true],
        [true, false],
        [true, true],
      ]) {
        const calls = [],
          retained = { blob: new Blob(["original"]), fileName: "original.ipa" };
        const context = {
          recovered: retained,
          report: { stage },
          jobOptions: { signing },
          preparedCustomization: requested ? { displayName: "Custom" } : {},
          downloadPreparation: { current: { retain: (value) => assert.equal(value, retained) } },
          setOriginalIdentityDownload: (value) => calls.push(["original", value]),
          createPostGenerationGuide,
          appCustomizationDefaults: { displayName: "Original app" },
          setCompletedOutput: (value) => calls.push(["completedOutput", value]),
          t: (key) => key,
          committedCleanupWarning: () => "cleanup warning",
          appendActivity: (value) => calls.push(["warning", value]),
          setStatus: () => {},
          prepareDownload: (automatic) => calls.push(["automatic", automatic]),
        };
        vm.runInNewContext(`(() => { ${recovery} })()`, context);
        assert.deepEqual(
          calls.find(([kind]) => kind === "original"),
          ["original", requested],
        );
        assert.deepEqual(
          calls.find(([kind]) => kind === "automatic"),
          ["automatic", !requested && !signing],
        );
        const output = calls.find(([kind]) => kind === "completedOutput")[1];
        assert.equal(output.appName, "Original app");
        assert.equal(output.resourcesStatus, "incomplete");
        assert.equal(output.resourcesDirectory, null);
        const warning = calls.find(([kind]) => kind === "warning")[1];
        if (requested) assert.match(warning, /dmWebCustomOriginalRetained/);
        if (stage === "cleanup" || !requested) assert.match(warning, /cleanup warning/);
      }
    }
  });

  // 下载链接准备失败后的重试也不能自动下载未应用自定义的原始 IPA。
  // Retrying failed link preparation must not auto-download the original uncustomized IPA either.
  test("download preparation retries preserve manual original-identity recovery", () => {
    const prepare = app.slice(
      app.indexOf("  function prepareDownload("),
      app.indexOf("  function clearSigningInputs()"),
    );
    for (const originalIdentityDownload of [false, true]) {
      const calls = [];
      const tracker = createGenerationProgress();
      tracker.fail();
      vm.runInNewContext(`${prepare}\nprepareDownload();`, {
        originalIdentityDownload,
        generationTracker: { current: tracker },
        publishProgress: (current) => {
          assert.equal(current, tracker);
          calls.push(["progress", current.snapshot()]);
        },
        downloadPreparation: {
          current: {
            prepare: () => ({ status: "ready", url: "blob:local", fileName: "original.ipa" }),
          },
        },
        setDownload: (value) => calls.push(["prepared", value.fileName]),
        setDownloadPreparationFailed: () => {},
        attemptAutomaticDownload: () => calls.push(["automatic"]),
      });
      assert.equal(calls.filter(([kind]) => kind === "prepared").length, 1);
      assert.equal(calls.filter(([kind]) => kind === "progress").length, 1);
      assert.equal(calls.find(([kind]) => kind === "progress")[1].status, "failed");
      assert.ok(tracker.snapshot().percent < 100);
      assert.equal(
        calls.filter(([kind]) => kind === "automatic").length,
        originalIdentityDownload ? 0 : 1,
      );
    }
  });
}

// primary-controls-ui
{
  // 仅测试静态树及隔离回调，不使用浏览器、运行时或用户文件。
  // Static tree and isolated callbacks only. No browser, runtime or user-provided files.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: ts } = typescriptModule;

  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  const fields = await readFile(
    new URL("../src/components/AppCustomizationFields.tsx", import.meta.url),
    "utf8",
  );
  const parse = (source) =>
    ts.createSourceFile("UI.tsx", source, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
  const appTree = parse(app);
  const fieldTree = parse(fields);
  function matching(tree, predicate) {
    const found = [];
    function visit(node) {
      if (predicate(node)) found.push(node);
      ts.forEachChild(node, visit);
    }
    visit(tree);
    return found;
  }
  const openings = matching(
    appTree,
    (node) => ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node),
  );
  const attr = (node, key) =>
    node.attributes.properties.find(
      (entry) => ts.isJsxAttribute(entry) && entry.name.getText() === key,
    )?.initializer;
  function ancestor(node, tag) {
    for (let parent = node.parent; parent; parent = parent.parent)
      if (ts.isJsxElement(parent) && parent.openingElement.tagName.getText() === tag) return parent;
  }

  test("editable identity and signing controls are in the main view with no disclosure ancestor", () => {
    const customization = openings.filter(
      (node) => node.tagName.getText() === "AppCustomizationFields",
    );
    assert.equal(customization.length, 2);
    const identity = customization.find((node) => attr(node, "section")?.text === "identity");
    const advanced = customization.find((node) => attr(node, "section")?.text === "advanced");
    assert.ok(identity);
    assert.ok(advanced);
    assert.equal(ancestor(identity, "Accordion"), undefined);
    assert.equal(
      attr(ancestor(advanced, "AccordionItem").openingElement, "value").text,
      "advanced",
    );
    assert.ok(
      ancestor(advanced, "AccordionContent").openingElement.attributes.properties.some(
        (entry) => ts.isJsxAttribute(entry) && entry.name.getText() === "forceMount",
      ),
    );
    assert.doesNotMatch(app, /identityPreview|<dl\b|<AccordionItem value="signing"/);
    for (const id of [
      "signing-enabled",
      "signing-identity",
      "signing-password",
      "signing-profile",
    ]) {
      const controls = openings.filter((node) => attr(node, "id")?.text === id);
      assert.equal(controls.length, 1, `${id} has one mounted control`);
      assert.equal(ancestor(controls[0], "Accordion"), undefined, id);
      for (let parent = controls[0].parent; parent; parent = parent.parent)
        assert.ok(!ts.isJsxExpression(parent), `${id} is not conditionally mounted`);
    }
  });

  test("identity and advanced fields form disjoint groups with a single visible error and icon input", () => {
    const definition = matching(
      fieldTree,
      (node) => ts.isVariableDeclaration(node) && node.name.getText() === "fields",
    )[0];
    const descriptors = matching(definition, ts.isObjectLiteralExpression).map((node) =>
      Object.fromEntries(
        node.properties.map((property) => [property.name.getText(), property.initializer.text]),
      ),
    );
    assert.deepEqual(
      descriptors.filter((field) => field.section === "identity").map((field) => field.name),
      ["displayName", "bundleIdentifier"],
    );
    assert.deepEqual(
      descriptors.filter((field) => field.section === "advanced").map((field) => field.name),
      ["version", "buildVersion"],
    );
    assert.equal(new Set(descriptors.map((field) => field.name)).size, 4);
    assert.match(fields, /\.filter\(\(field\) => field.section === section\)/);
    assert.match(fields, /value=\{value\[name\] \?\? defaults\[name\]\}/);
    assert.match(fields, /section === "identity" && error &&/);
    const icon = matching(
      fieldTree,
      (node) => ts.isJsxSelfClosingElement(node) && attr(node, "id")?.text === "custom-icon",
    );
    assert.equal(icon.length, 1);
    let condition;
    for (let parent = icon[0].parent; parent; parent = parent.parent)
      if (ts.isJsxExpression(parent)) {
        condition = parent.expression;
        break;
      }
    assert.match(condition.getText(), /^section === "advanced" &&/);
  });
}

// shadcn-form
{
  // 仅检查静态 JSX/CSS，不渲染应用，不执行浏览器或打包运行时。
  // Static JSX/CSS checks only: no application rendering, browser or packaging runtime.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile, readdir, mkdtemp, writeFile, rm } = promisesModule;
  const { dirname, resolve } = nodePathModule;
  const { tmpdir } = nodeOsModule;
  const { pathToFileURL } = nodeUrlModule;
  const { default: ts } = typescriptModule;
  const { compile } = tailwindcssModule;
  const { FRONTEND_NOTICES_FILE, writeFrontendNotices } = frontendNoticesModule;
  const { createPreviewManifest } = previewManifestModule;

  const sourcePaths = ["../src/App.tsx", "../src/components/AppCustomizationFields.tsx"];
  const sources = await Promise.all(
    sourcePaths.map((path) => readFile(new URL(path, import.meta.url), "utf8")),
  );
  const documents = sources.map((source, index) =>
    ts.createSourceFile(
      sourcePaths[index],
      source,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.TSX,
    ),
  );
  function elements(document) {
    const matches = [];
    function visit(node) {
      if (ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node)) matches.push(node);
      ts.forEachChild(node, visit);
    }
    visit(document);
    return matches;
  }
  function attribute(node, name) {
    return node.attributes.properties.find(
      (entry) => ts.isJsxAttribute(entry) && entry.name.getText() === name,
    );
  }
  function enclosing(node, name) {
    for (let parent = node.parent; parent; parent = parent.parent) {
      if (ts.isJsxElement(parent) && parent.openingElement.tagName.getText() === name)
        return parent.openingElement;
    }
  }

  test("form uses shadcn controls with explicit matching labels and compact disclosure triggers", () => {
    for (const document of documents) {
      const nodes = elements(document);
      const controls = nodes.filter((node) =>
        ["Input", "Checkbox", "NativeSelect"].includes(node.tagName.getText()),
      );
      const labels = nodes.filter((node) => node.tagName.getText() === "FieldLabel");
      assert.ok(controls.length > 0);
      for (const control of controls) {
        const id = attribute(control, "id")?.initializer?.getText();
        assert.ok(id, control.tagName.getText());
        assert.ok(
          labels.some((label) => attribute(label, "htmlFor")?.initializer?.getText() === id),
          id,
        );
      }
      assert.equal(
        nodes.filter((node) =>
          ["input", "select", "fieldset", "details", "summary"].includes(node.tagName.getText()),
        ).length,
        0,
      );
      for (const trigger of nodes.filter((node) => node.tagName.getText() === "AccordionTrigger")) {
        assert.match(attribute(trigger, "className").initializer.text, /min-h-11/);
      }
    }
  });

  test("signing files are always mounted and Advanced retains the icon input when closed", () => {
    let count = 0;
    for (const document of documents) {
      for (const input of elements(document).filter(
        (node) =>
          node.tagName.getText() === "Input" &&
          attribute(node, "type")?.initializer?.text === "file",
      )) {
        count++;
        const accordionContent = enclosing(input, "AccordionContent");
        if (accordionContent) assert.ok(attribute(accordionContent, "forceMount"));
        else if (document !== documents[0]) {
          const appElements = elements(documents[0]);
          const customization = appElements.find(
            (node) =>
              node.tagName.getText() === "AppCustomizationFields" &&
              attribute(node, "section")?.initializer?.text === "advanced",
          );
          assert.ok(attribute(enclosing(customization, "AccordionContent"), "forceMount"));
        }
        assert.ok(attribute(input, "ref"));
        assert.ok(attribute(enclosing(input, "FieldSet"), "disabled"));
      }
    }
    assert.equal(count, 3);
    assert.match(sources[0], /\[&_\[data-slot=accordion-content\]\[data-state=closed\]\]:hidden/);
    assert.match(sources[0], /onValueChange=\{setOpenOptions\}/);
    assert.doesNotMatch(sources[0], /<AccordionItem value="signing"|\{signing && \(/);
    assert.match(sources[0], /useState<string\[\]>\(\[\]\)/);
  });

  test("existing Tailwind pipeline emits file, focus, closed-content and numbered-step utilities", async () => {
    const uiDirectory = new URL("../src/components/ui/", import.meta.url);
    const componentSources = await Promise.all(
      (await readdir(uiDirectory))
        .filter((name) => name.endsWith(".tsx"))
        .map((name) => readFile(new URL(name, uiDirectory), "utf8")),
    );
    const candidates = new Set();
    for (const source of [...sources, ...componentSources]) {
      const document = ts.createSourceFile(
        "component.tsx",
        source,
        ts.ScriptTarget.Latest,
        true,
        ts.ScriptKind.TSX,
      );
      function visit(node) {
        if (ts.isStringLiteral(node))
          for (const candidate of node.text.split(/\s+/)) candidates.add(candidate);
        ts.forEachChild(node, visit);
      }
      visit(document);
    }
    const tailwindRoot = new URL("../node_modules/tailwindcss/", import.meta.url).pathname;
    const css = await readFile(new URL("../src/index.css", import.meta.url), "utf8");
    const compiler = await compile(css, {
      async loadStylesheet(id, base) {
        const path = id === "tailwindcss" ? resolve(tailwindRoot, "index.css") : resolve(base, id);
        assert.ok(path.startsWith(tailwindRoot));
        return { path, base: dirname(path), content: await readFile(path, "utf8") };
      },
    });
    const output = compiler.build([...candidates]);
    assert.match(output, /::file-selector-button/);
    assert.match(
      output,
      /\[data-slot=accordion-content\]\[data-state=closed\]\s*\{\s*display: none;/,
    );
    assert.match(output, /\.min-h-11\s*\{\s*min-height:/);
    assert.match(output, /focus-visible/);
    assert.match(output, /\.sm\\:grid-cols-2/);
    assert.doesNotMatch(output, /@(?:apply|theme|source)\b|@import\s/);
    const input = componentSources.find((source) => source.includes('data-slot="input"'));
    assert.match(input, /border-input/);
    assert.match(input, /disabled:opacity-50/);
    assert.match(input, /focus-visible:ring/);
    assert.match(input, /type === "file"/);
    assert.match(input, /file:bg-secondary/);
    assert.match(input, /file:px-3/);
    assert.match(input, /hover:file:bg-accent/);
    const accordion = componentSources.find((source) =>
      source.includes('data-slot="accordion-content"'),
    );
    assert.match(accordion, /cn\("px-1 pt-0 pb-4", className\)/);
    assert.match(sources[0], /native-select-wrapper\]\]:max-w-xs/);
    assert.match(sources[0], /sm:grid-cols-\[11rem_minmax\(0,1fr\)\]/);
    assert.match(css, /--primary: oklch\(0\.511 0\.096 186\.391\)/);
  });

  test("generated frontend notices preserve full upstream texts and are included in artifact inventory", async () => {
    const directory = await mkdtemp(resolve(tmpdir(), "frontend-notices-"));
    try {
      await writeFrontendNotices(directory, {
        outputs: {
          "test.js": {
            inputs: { "node_modules/lucide-react/dist/cjs/lucide-react.js": { bytesInOutput: 1 } },
          },
        },
      });
      const notices = await readFile(resolve(directory, FRONTEND_NOTICES_FILE), "utf8");
      for (const path of ["../SHADCN_LICENSE.md", "../node_modules/lucide-react/LICENSE"]) {
        assert.ok(notices.includes(await readFile(new URL(path, import.meta.url), "utf8")));
      }
      await writeFile(
        resolve(directory, "index.html"),
        '<link href="./app.css"><script src="./app.js"></script>',
      );
      await writeFile(resolve(directory, "app.js"), "/* inert fixture */");
      await writeFile(resolve(directory, "app.css"), "body{color:black}");
      const manifest = await createPreviewManifest(pathToFileURL(`${directory}/`));
      const record = manifest.files.find((file) => file.path === FRONTEND_NOTICES_FILE);
      assert.equal(record.bytes, Buffer.byteLength(notices));
      assert.match(record.sha256, /^[0-9a-f]{64}$/);
      await writeFile(resolve(directory, "other-notices.txt"), "unapproved extra file");
      await assert.rejects(createPreviewManifest(pathToFileURL(`${directory}/`)), /allowlist/);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });
}

// older-gpu-ui
{
  // 源码约束测试不代替浏览器交互或设备验证。
  // Source-contract tests do not replace browser interaction or device validation.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { catalog } = i18nCatalogModule;

  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  test("older-GPU choice stays in page and reaches the next generation", () => {
    assert.match(app, /get\("legacyAstc"\) === "1"/);
    assert.match(app, /prepareBrowserPackager\(\{[\s\S]*?editableOptions: true,/);
    assert.match(
      app,
      /jobPackager.generate\(\{\s+profileId: jobOptions\.profileId,\s+legacyAstc: jobOptions\.legacyAstc,/,
    );
    assert.match(app, /setLegacyAstc\(/);
    assert.doesNotMatch(app, /window\.location\.assign|hasSelectedFolder/);
    assert.match(app, /window\.history\.replaceState/);
  });
  test("pack progress and complete-on-later-failure outcome remain separate from IPA", () => {
    assert.match(app, /event\.stage === "legacy-pack"/);
    assert.match(app, /result\.legacyPack\?\.status === "complete"/);
    assert.match(app, /report\?\.legacyPack\?\.status === "complete"/);
    assert.match(app, /legacyAstc && legacyStatus/);
    assert.match(app, /setDownload\(\{ url: prepared\.url, fileName: prepared\.fileName \}\)/);
    for (const key of [
      "dmWebLegacyOption",
      "dmWebLegacyHelp",
      "dmWebLegacyPreparing",
      "dmWebLegacySaving",
      "dmWebLegacyComplete",
      "dmWebLegacyIncomplete",
    ]) {
      assert.equal(catalog[key].length, 2);
      assert.ok(catalog[key].every((value) => value.length > 0));
    }
    assert.ok(catalog.dmWebLegacyHelp.every((value) => value.includes("Legacy Device Pack")));
    assert.match(catalog.dmWebLegacyHelp[0], /not verified/);
    assert.match(catalog.dmWebLegacyComplete[0], /does not mean the IPA is complete/);
  });

  test("conversion shows bounded work counters, not a completion percentage", () => {
    const section = app.slice(
      app.indexOf('event.diagnostic?.kind === "legacy-astc"'),
      app.indexOf('event.stage === "legacy-pack"'),
    );
    assert.match(section, /Number\.isSafeInteger\(blocks\)/);
    assert.match(section, /Number\.isSafeInteger\(bytes\)/);
    assert.match(section, /dmWebLegacyConverting/);
    assert.doesNotMatch(section, /percent|totalBytes/);
    assert.match(catalog.dmWebLegacyConverting[0], /multiple passes/);
  });
}

// signing-completion-ui
{
  // 执行真实界面处理器的隔离片段；签名、打包及 URL 均为合成替身，不运行编译器。
  // Execute isolated real UI handlers with synthetic signing, packaging and URLs; no compiler execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: vm } = nodeVmModule;
  const { default: ts } = typescriptModule;
  const { createGenerationProgress } = generationProgressModule;
  const { createDownloadPreparation } = downloadPreparationModule;
  const { createUiOperationGate } = uiOperationGateModule;
  const { signWithLocalFiles } = signingInputModule;
  const { createPostGenerationGuide } = postGenerationGuideModule;
  const { catalog } = i18nCatalogModule;

  const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  const parsed = ts.createSourceFile(
    "App.tsx",
    source,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TSX,
  );
  const functions = new Map(),
    callbacks = new Map();
  function visit(node) {
    if (ts.isFunctionDeclaration(node) && node.name)
      functions.set(node.name.text, node.getText(parsed));
    if (
      ts.isVariableDeclaration(node) &&
      ["publishProgress", "flushActivity"].includes(node.name.getText(parsed))
    )
      callbacks.set(node.name.getText(parsed), node.initializer.arguments[0].getText(parsed));
    ts.forEachChild(node, visit);
  }
  visit(parsed);
  const transpile = (text) =>
    ts.transpileModule(text, { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText;
  const deferred = () => {
    let resolve, reject;
    const promise = new Promise((yes, no) => {
      resolve = yes;
      reject = no;
    });
    return { promise, resolve, reject };
  };
  const signedOutput = () => ({
    blob: new Blob(["synthetic signed IPA"]),
    fileName: "D2RMobile-signed.ipa",
  });
  function harness({ signing = true, urlFails = false } = {}) {
    const unsigned = { blob: new Blob(["synthetic unsigned IPA"]), fileName: "unsigned.ipa" };
    const result = {
      download: unsigned,
      resources: { status: "complete", directoryName: "resources" },
    };
    const writes = [],
      automatic = [],
      urls = [],
      attempts = [];
    let nextAttempt = deferred(),
      started = deferred(),
      packageObserver;
    const context = {
      Blob,
      Error,
      AbortController,
      DOMException,
      ready: true,
      busy: false,
      generating: false,
      finished: false,
      signing,
      signingReady: true,
      signingRetryAvailable: false,
      status: "folder checked",
      signingStatus: "",
      download: null,
      downloadPreparationFailed: false,
      customizationValidation: { options: {}, error: "" },
      generationReadiness: { canGenerate: true },
      profileId: "v2.0.0-ios26",
      legacyAstc: false,
      astcPreset: "thorough",
      ignoreCache: false,
      skipResources: false,
      inputMode: "packages",
      folder: { name: "inert folder" },
      nspNames: ["inert.nsp"],
      originalIdentityDownload: false,
      appCustomizationDefaults: { displayName: "App" },
      mounted: { current: true },
      cancellation: { current: null },
      operationGate: { current: createUiOperationGate() },
      generationTracker: { current: null },
      unsignedForSigning: { current: null },
      pendingGenerationStatus: { current: null },
      pendingOverallProgress: { current: null },
      activityTimer: { current: null },
      renderedActivityRevision: { current: 0 },
      activityRevision: { current: 0 },
      identityInput: { current: { files: [new Blob(["inert certificate"])], value: "" } },
      profileInput: { current: { files: [new Blob(["inert profile"])], value: "" } },
      passwordInput: { current: { value: "" } },
      createGenerationProgress,
      createPostGenerationGuide,
      signWithLocalFiles,
      prepareAppCustomization: async () => ({}),
      t: (key) => key,
      packagingActivity: () => "inert earlier packaging status",
      shaderProgressLabel: () => null,
      resourceProgressLabel: () => null,
      takeCommittedDownload: () => null,
      packagingFailureSummary: () => "unexpected packaging failure",
      packagingFailureMessage: () => "unexpected packaging failure",
      shaderFailureDetails: () => [],
      attemptAutomaticDownload: (value) => automatic.push(value),
      appendActivity: (text, force) => {
        if (text) writes.push(["activity", text]);
        if (force) context.flushActivity();
      },
      downloadPreparation: {
        current: createDownloadPreparation({
          createObjectURL: (blob) => {
            if (urlFails) throw new Error("synthetic URL failure");
            urls.push(blob);
            return `blob:synthetic-${urls.length}`;
          },
          revokeObjectURL: () => {},
        }),
      },
      packager: {
        current: {
          async generate({ onProgress }) {
            packageObserver = onProgress;
            onProgress({ stage: "output", state: "running" });
            return result;
          },
          async prepareSigning() {
            const pending = nextAttempt;
            const entered = started;
            return {
              sign: (options) => {
                attempts.push(options);
                entered.resolve();
                return pending.promise;
              },
              close: () => {},
            };
          },
          close: async () => {},
        },
      },
    };
    for (const name of [
      "Busy",
      "Generating",
      "Cancelling",
      "Ready",
      "Finished",
      "Status",
      "SigningStatus",
      "SigningRetryAvailable",
      "SigningFiles",
      "CustomizationError",
      "ResourceOutcome",
      "LegacyStatus",
      "Download",
      "DownloadPreparationFailed",
      "CompletedOutput",
      "OverallProgress",
    ])
      context[`set${name}`] = (value) => {
        const field = name[0].toLowerCase() + name.slice(1);
        context[field] = typeof value === "function" ? value(context[field]) : value;
        writes.push([field, context[field]]);
      };
    const scope = vm.createContext(context);
    for (const [name, callback] of callbacks)
      context[name] = vm.runInContext(transpile(`(${callback})`), scope);
    for (const name of [
      "prepareDownload",
      "clearSigningInputs",
      "runSigning",
      "retrySigning",
      "generate",
    ])
      vm.runInContext(transpile(functions.get(name)), scope);
    return {
      context,
      writes,
      automatic,
      urls,
      attempts,
      unsigned,
      start: () => context.generate(),
      started: () => started.promise,
      resolve: () => nextAttempt.resolve(signedOutput()),
      reject: () => nextAttempt.reject(new Error("SIGNING_PROFILE_MISMATCH")),
      allowUrls: () => {
        urlFails = false;
      },
      next: () => {
        nextAttempt = deferred();
        started = deferred();
      },
      latePackage: () => packageObserver({ stage: "output", state: "running" }),
    };
  }

  test("requested signing publishes neither unsigned URL nor ready text while step 3/4 is pending", async () => {
    const h = harness();
    const generation = h.start();
    await h.started();
    h.attempts[0].onProgress({ stage: "package", completed: 1, total: 1 });
    h.context.flushActivity();
    assert.equal(h.context.overallProgress.percent, 80);
    assert.equal(h.context.overallProgress.detail.step, 3);
    assert.equal(h.context.status, "dmWebSigningWorking");
    assert.equal(h.context.download, null);
    assert.equal(h.urls.length, 0);
    assert.equal(h.automatic.length, 0);
    assert.equal(h.context.unsignedForSigning.current, h.unsigned);
    assert.equal(
      h.writes.some(
        ([key, text]) =>
          ["status", "activity"].includes(key) &&
          /UnsignedReady|DownloadReady|SigningComplete/.test(text),
      ),
      false,
    );
    h.resolve();
    await generation;
    assert.equal(h.context.download.fileName, "D2RMobile-signed.ipa");
    assert.equal(h.context.status, "dmWebSigningComplete");
    assert.equal(h.context.overallProgress.percent, 100);
    assert.equal(h.automatic.length, 1);
    assert.equal(h.automatic[0].fileName, "D2RMobile-signed.ipa");
    const completed = h.context.overallProgress,
      status = h.context.status;
    h.latePackage();
    h.attempts[0].onProgress({ stage: "package", completed: 1, total: 1 });
    h.context.flushActivity();
    assert.equal(h.context.overallProgress, completed);
    assert.equal(h.context.status, status);
  });

  test("signing failure keeps a manual unsigned fallback and retry never republishes it while running", async () => {
    const h = harness();
    const generation = h.start();
    await h.started();
    h.reject();
    await generation;
    assert.equal(h.context.status, "dmWebSigningProfileMismatch");
    assert.equal(h.context.signingRetryAvailable, true);
    assert.equal(h.context.download.fileName, "unsigned.ipa");
    assert.equal(h.context.overallProgress.status, "signing-failed");
    assert.ok(h.context.overallProgress.percent < 100);
    assert.equal(h.automatic.length, 0);
    h.context.prepareDownload();
    assert.equal(h.automatic.length, 0, "retrying fallback URL preparation does not auto-download");
    h.next();
    const retry = h.context.retrySigning();
    await h.started();
    assert.equal(h.context.download, null);
    assert.equal(h.context.status, "dmWebSigningWorking");
    assert.equal(h.attempts[1].ipa, h.unsigned.blob);
    h.attempts[0].onProgress({ stage: "verify", completed: 1, total: 1 });
    assert.equal(h.context.overallProgress.detail, null);
    h.resolve();
    await retry;
    assert.equal(h.context.overallProgress.percent, 100);
    assert.equal(h.context.download.fileName, "D2RMobile-signed.ipa");
    assert.equal(h.context.signingRetryAvailable, false);
    assert.equal(h.automatic.length, 1);
  });

  test("a late signed result after cancellation remains an error with manual unsigned recovery", async () => {
    const h = harness();
    const generation = h.start();
    await h.started();
    h.context.cancellation.current.abort();
    h.resolve();
    await generation;
    assert.equal(h.context.status, "dmWebSigningCancelled");
    assert.equal(h.context.overallProgress.status, "signing-cancelled");
    assert.ok(h.context.overallProgress.percent < 100);
    assert.equal(h.context.download.fileName, "unsigned.ipa");
    assert.equal(h.context.signingRetryAvailable, true);
    assert.equal(h.automatic.length, 0);
  });

  test("signed bytes are not called ready or 100 percent until a download URL is prepared", async () => {
    const h = harness({ urlFails: true });
    const generation = h.start();
    await h.started();
    h.resolve();
    await generation;
    assert.equal(h.context.overallProgress.status, "download-pending");
    assert.equal(h.context.overallProgress.percent, 90);
    assert.equal(h.context.download, null);
    assert.equal(h.context.status, "dmWebDownloadPreparationFailed");
    assert.equal(h.context.signingStatus, "");
    assert.equal(h.context.signingRetryAvailable, false);
    h.allowUrls();
    h.context.prepareDownload();
    assert.equal(h.context.overallProgress.percent, 100);
    assert.equal(h.context.status, "dmWebSigningComplete");
    assert.equal(h.context.download.fileName, "D2RMobile-signed.ipa");
  });

  test("default unsigned generation still completes and downloads without entering signing", async () => {
    const h = harness({ signing: false });
    await h.start();
    assert.equal(h.attempts.length, 0);
    assert.equal(h.context.overallProgress.percent, 100);
    assert.equal(h.context.download.fileName, "unsigned.ipa");
    assert.equal(h.automatic.length, 1);
  });

  test("manual unsigned recovery is clearly labeled and never shows signed-job installation steps", () => {
    assert.match(source, /download && !busy &&/);
    assert.match(
      source,
      /signedDownload[\s\S]*?"dmWebSignedDownload"[\s\S]*?signing[\s\S]*?"dmWebUnsignedFallbackDownload"/,
    );
    assert.match(
      source,
      /signedDownload[\s\S]*?"dmWebSignedHelp"[\s\S]*?signing[\s\S]*?"dmWebUnsignedFallbackHelp"/,
    );
    assert.match(
      source,
      /download && completedOutput && !busy && \(!signing \|\| signedDownload\)/,
    );
    assert.match(catalog.dmWebUnsignedFallbackDownload[0], /unsigned IPA instead/);
    assert.match(catalog.dmWebUnsignedFallbackDownload[1], /未签名/);
    assert.match(
      catalog.dmWebUnsignedFallbackHelp[0],
      /not finish.*Retry signing.*not ready to install/,
    );
    assert.match(catalog.dmWebUnsignedFallbackHelp[1], /尚未完成.*重试签名.*尚不能直接安装/);
  });
}

// signing-input-ui
{
  // 合成本地凭据与界面契约测试，不构成原生签名、浏览器或设备验证。
  // Synthetic local-credential and UI contract tests, not native signing, browser or device proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: vm } = nodeVmModule;
  const { default: ts } = typescriptModule;
  const { signWithLocalFiles } = signingInputModule;
  const { IPA_SIGNING_LIMITS } = ipaSigningContractModule;
  const inputs = () => ({
    unsigned: { blob: new Blob(["unsigned IPA"]), fileName: "unsigned.ipa" },
    identityFile: new Blob(["p12"]),
    profileFile: new Blob(["profile"]),
    password: "local secret",
  });
  test("local signing reads bounded credentials, wipes buffers and leaves unsigned fallback intact", async () => {
    const options = inputs(),
      expected = await options.unsigned.blob.text();
    let credentials;
    const result = { blob: new Blob(["verified signed"]), fileName: "D2RMobile-signed.ipa" };
    assert.equal(
      await signWithLocalFiles({
        ...options,
        async sign(value) {
          assert.equal(value.ipa, options.unsigned.blob);
          credentials = value.credentials;
          assert.equal(credentials.password, options.password);
          assert.equal(new TextDecoder().decode(credentials.identity), "p12");
          return result;
        },
      }),
      result,
    );
    assert.ok(new Uint8Array(credentials.identity).every((byte) => byte === 0));
    assert.ok(new Uint8Array(credentials.profile).every((byte) => byte === 0));
    assert.equal(await options.unsigned.blob.text(), expected);
  });
  test("signing rejection and cancellation preserve original unsigned bytes and wipe owned reads", async () => {
    for (const cancel of [false, true]) {
      const options = inputs(),
        controller = new AbortController();
      let buffers;
      await assert.rejects(
        signWithLocalFiles({
          ...options,
          signal: controller.signal,
          async sign({ credentials }) {
            buffers = [credentials.identity, credentials.profile];
            if (cancel) {
              controller.abort();
              controller.signal.throwIfAborted();
            }
            throw new Error("native failure");
          },
        }),
      );
      assert.ok(buffers.every((buffer) => new Uint8Array(buffer).every((byte) => byte === 0)));
      assert.equal(await options.unsigned.blob.text(), "unsigned IPA");
    }
  });
  test("invalid credential sizes fail before reading and late cancellation wipes first read", async () => {
    let calls = 0;
    const options = inputs();
    await assert.rejects(
      signWithLocalFiles({
        ...options,
        identityFile: new Blob([new Uint8Array(IPA_SIGNING_LIMITS.identityBytes + 1)]),
        sign() {
          calls++;
        },
      }),
      /SIGNING_INPUT_INVALID/,
    );
    assert.equal(calls, 0);
    const controller = new AbortController(),
      bytes = new Uint8Array([7, 8]).buffer;
    class LateFile extends Blob {
      async arrayBuffer() {
        controller.abort();
        return bytes;
      }
    }
    await assert.rejects(
      signWithLocalFiles({
        ...options,
        identityFile: new LateFile(["id"]),
        signal: controller.signal,
        sign() {
          calls++;
        },
      }),
      { name: "AbortError" },
    );
    assert.deepEqual(new Uint8Array(bytes), new Uint8Array(2));
    assert.equal(calls, 0);
  });
  test("UI preloads signing before credentials and keeps signing errors outside packaging failure", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    assert.match(source, /get\("signing"\) === "1"/);
    assert.match(source, /prepareBrowserPackager\(\{[\s\S]*?editableOptions: true,\s*signal:/);
    assert.match(source, /setSigningReady\(value.signingReady\)/);
    assert.ok(
      source.indexOf("downloadPreparation.current.retain(result.download)") <
        source.indexOf("await runSigning(jobPackager, result.download"),
    );
    const signing = source.slice(
      source.indexOf("await signWithLocalFiles"),
      source.indexOf("  async function retrySigning()"),
    );
    assert.match(signing, /dmWebSigningCancelled/);
    assert.match(signing, /dmWebSigningFailed/);
    assert.match(signing, /prepareDownload\(false\)/);
    assert.match(signing, /setStatus\(signingMessage\)/);
    assert.doesNotMatch(signing, /setStatus\(t\("dmWebUnsignedReady"\)\)/);
    assert.match(source, /const signedDownload = download\?\.fileName === "D2RMobile-signed.ipa"/);
    assert.match(signing, /clearSigningInputs\(\)/);
    assert.match(signing, /setSigningStatus\(signingMessage\)/);
    assert.equal((signing.match(/appendActivity\(signingMessage, true\)/g) ?? []).length, 1);
    assert.doesNotMatch(signing, /appendActivity\(signingError/);
    assert.doesNotMatch(signing, /packagingFailure|setResourceOutcome|setLegacyStatus/);
    assert.doesNotMatch(source, /localStorage|sessionStorage/);
    assert.match(source, /autoComplete="off"/);
  });

  // 检查真实字段组表达式；仅显式签名重试允许在终止后重新选择凭据。
  // Check the real fieldset expression; only an explicit signing retry permits credentials after termination.
  test("signing inputs lock after output or termination without blocking an idle signing retry", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const parsed = ts.createSourceFile(
      "App.tsx",
      source,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.TSX,
    );
    const inputs = [];
    function visit(node) {
      if (
        ts.isJsxSelfClosingElement(node) &&
        node.tagName.getText(parsed) === "Input" &&
        node.attributes.properties.some(
          (attribute) =>
            ts.isJsxAttribute(attribute) &&
            attribute.name.getText(parsed) === "id" &&
            ["signing-identity", "signing-password", "signing-profile"].includes(
              attribute.initializer?.text,
            ),
        )
      )
        inputs.push(node);
      ts.forEachChild(node, visit);
    }
    visit(parsed);
    assert.equal(inputs.length, 3);
    for (const input of inputs) {
      let parent = input.parent;
      while (
        parent &&
        !(ts.isJsxElement(parent) && parent.openingElement.tagName.getText(parsed) === "FieldSet")
      )
        parent = parent.parent;
      assert.ok(parent, "each credential input inherits a disabled fieldset");
      const disabled = parent.openingElement.attributes.properties
        .find(
          (attribute) =>
            ts.isJsxAttribute(attribute) && attribute.name.getText(parsed) === "disabled",
        )
        .initializer.expression.getText(parsed);
      const state = {
        signing: true,
        ready: true,
        signingReady: true,
        busy: false,
        generating: false,
        finished: false,
        download: null,
        signingRetryAvailable: false,
      };
      const locked = (overrides) => vm.runInNewContext(disabled, { ...state, ...overrides });
      assert.equal(locked({}), false, "ready pre-generation credentials are editable");
      for (const override of [
        { signing: false },
        { ready: false },
        { signingReady: false },
        { busy: true },
        { generating: true },
        { finished: true },
        { download: { fileName: "unsigned.ipa" } },
        { download: { fileName: "D2RMobile-signed.ipa" } },
      ])
        assert.equal(locked(override), true, JSON.stringify(override));
      const retry = {
        ready: false,
        finished: true,
        download: { fileName: "unsigned.ipa" },
        signingRetryAvailable: true,
      };
      assert.equal(locked(retry), false, "failed signing permits fresh credentials for retry");
      for (const override of [
        { signing: false },
        { busy: true },
        { generating: true },
        { signingReady: false },
      ])
        assert.equal(
          locked({ ...retry, ...override }),
          true,
          "active or unavailable retry stays locked",
        );
      assert.equal(
        locked({ ...retry, signingRetryAvailable: false }),
        true,
        "successful retry locks credentials",
      );
    }
  });

  test("terminal UI cleanup closes the captured packager without losing completed downloads", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    assert.match(source, /const jobPackager = packager.current/);
    assert.match(source, /await jobPackager.generate\(/);
    const cleanup = source.slice(
      source.indexOf("} finally {\n      acceptingProgress = false;"),
      source.indexOf("  return (\n    <main"),
    );
    assert.match(cleanup, /await jobPackager.close\(\)/);
    assert.match(cleanup, /catch \{[\s\S]*?dmWebCleanupWarning/);
    assert.match(cleanup, /setStatus\(\(previous: string\) =>/);
    assert.doesNotMatch(
      cleanup,
      /downloadPreparation.current.dispose|setDownload\(null\)|setResourceOutcome\(|setLegacyStatus\(/,
    );
    assert.ok(cleanup.indexOf("await jobPackager.close()") < cleanup.indexOf("operation.finish()"));
    assert.match(
      source,
      /dmWebSigningCancelled"[\s\S]*?dmWebSigningFailed"[\s\S]*?setStatus\(signingMessage\)/,
    );
  });

  // 只映射封闭错误码；不展示证书名称或原始错误消息。
  // Map only the closed error code; never display certificate names or raw error messages.
  test("signing mismatch has a fixed actionable label without stronger verification claims", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    assert.match(
      source,
      /signingError instanceof Error &&\s*signingError.message === "SIGNING_PROFILE_MISMATCH"/,
    );
    assert.match(
      source,
      /controller.signal.aborted[\s\S]*?dmWebSigningCancelled[\s\S]*?SIGNING_PROFILE_MISMATCH[\s\S]*?dmWebSigningProfileMismatch[\s\S]*?dmWebSigningFailed/,
    );
    assert.doesNotMatch(source, /setSigningStatus\(signingError|t\(signingError/);
    const table = await readFile(new URL("../i18n/ui_text.tsv", import.meta.url), "utf8");
    const nativeTable = await readFile(
      new URL("../../dm/i18n/ui_text.tsv", import.meta.url),
      "utf8",
    );
    assert.doesNotMatch(nativeTable, /^dmWeb/m);
    const rows = table
      .trimEnd()
      .split("\n")
      .slice(1)
      .map((line) => line.split("\t"));
    assert.deepEqual(
      rows.map(([key]) => key),
      Object.keys(i18nCatalogModule.catalog),
    );
    for (const [key, zh, en] of rows)
      assert.deepEqual(i18nCatalogModule.catalog[key], [
        en.replaceAll("\\n", "\n"),
        zh.replaceAll("\\n", "\n"),
      ]);
    const row = (key) =>
      table
        .split("\n")
        .find((line) => line.startsWith(key + "\t"))
        .split("\t");
    assert.match(
      row("dmWebSigningProfileMismatch")[2],
      /mismatched or expired.*Unsigned IPA retained/,
    );
    assert.match(row("dmWebSigningComplete")[2], /created and ready to download.*not guaranteed/);
    assert.doesNotMatch(row("dmWebSigningComplete")[2], /verified/);
    assert.match(row("dmWebSigningWorking")[2], /checking the archive locally/);
  });

  // 重试只复用已提交的 Blob，输入与一次性线程每次重新拥有。
  // Retry reuses only the committed Blob; inputs and the one-shot worker have fresh ownership each time.
  test("UI signing retry initializes before reads and retains no conversion lifecycle", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const attempt = source.slice(
      source.indexOf("  async function runSigning("),
      source.indexOf("  async function retrySigning()"),
    );
    assert.ok(
      attempt.indexOf("await jobPackager.prepareSigning") <
        attempt.indexOf("await signWithLocalFiles"),
    );
    assert.match(attempt, /sign: attempt.sign/);
    assert.match(
      attempt,
      /finally \{\s*acceptingSigningProgress = false;\s*attempt\?\.close\(\);\s*clearSigningInputs\(\)/,
    );
    assert.match(attempt, /unsignedForSigning.current = null/);
    const retry = source.slice(
      source.indexOf("  async function retrySigning()"),
      source.indexOf("  async function generate()"),
    );
    assert.match(retry, /const unsigned = unsignedForSigning.current/);
    assert.match(retry, /operationGate.current.begin\(\)/);
    assert.match(retry, /await runSigning\(jobPackager, unsigned,/);
    assert.match(retry, /if \(!keepSigningFactory \|\| !current\(\)\) await jobPackager.close\(\)/);
    assert.doesNotMatch(retry, /\.generate\(|setResourceOutcome|setLegacyStatus|fetch\(/);
    assert.match(retry, /setCancelling\(false\)/);
    assert.match(source, /onClick=\{retrySigning\} disabled=\{busy \|\| !credentialsSelected\}/);
  });

  test("sequential signing attempts keep the same unsigned Blob and wipe fresh credentials", async () => {
    const unsigned = inputs().unsigned;
    const consumed = [];
    for (let i = 0; i < 2; i++) {
      const operation = signWithLocalFiles({
        ...inputs(),
        unsigned,
        async sign({ ipa, credentials }) {
          assert.equal(ipa, unsigned.blob);
          consumed.push(credentials);
          if (i === 0) throw new Error("SIGNING_PROFILE_MISMATCH");
          return { blob: new Blob(["signed"]), fileName: "D2RMobile-signed.ipa" };
        },
      });
      if (i === 0) await assert.rejects(operation, /SIGNING_PROFILE_MISMATCH/);
      else assert.equal((await operation).fileName, "D2RMobile-signed.ipa");
    }
    assert.notEqual(consumed[0].identity, consumed[1].identity);
    for (const credentials of consumed) {
      assert.ok(new Uint8Array(credentials.identity).every((byte) => byte === 0));
      assert.ok(new Uint8Array(credentials.profile).every((byte) => byte === 0));
    }
    assert.equal(await unsigned.blob.text(), "unsigned IPA");
  });
}

// simple-packager-flow
{
  // 惰性界面与文件夹状态测试，只运行真实处理器的隔离片段和内存桩。
  // Inert UI/folder state tests run isolated real-handler snippets and in-memory stubs only.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: vm } = nodeVmModule;
  const { default: ts } = typescriptModule;
  const { createUiOperationGate } = uiOperationGateModule;
  const { catalog } = i18nCatalogModule;
  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  const parsed = ts.createSourceFile(
    "App.tsx",
    app,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TSX,
  );
  const details = [];
  const selects = [];
  function visit(node) {
    if (
      ts.isJsxElement(node) &&
      ["details", "AccordionItem"].includes(node.openingElement.tagName.getText(parsed))
    )
      details.push(node);
    if (
      ts.isJsxOpeningElement(node) &&
      ["select", "NativeSelect"].includes(node.tagName.getText(parsed))
    )
      selects.push(node);
    ts.forEachChild(node, visit);
  }
  visit(parsed);
  function attribute(node, name) {
    return node.attributes.properties.find(
      (entry) => ts.isJsxAttribute(entry) && entry.name.getText(parsed) === name,
    );
  }
  test("language remains visible while generation selectors stay in Advanced", () => {
    const advanced = details.find((node) =>
      node.getText(parsed).includes('t("dmWebAdvancedOptions")'),
    );
    assert.ok(advanced);
    assert.equal(attribute(advanced.openingElement, "open"), undefined);
    for (const select of selects) {
      if (attribute(select, "id")?.initializer?.text === "ui-language") {
        assert.ok(select.pos < advanced.pos);
      } else assert.ok(select.pos > advanced.pos && select.end < advanced.end);
    }
    for (const setting of ["checked={legacyAstc}", "checked={skipResources}"])
      assert.ok(advanced.getText(parsed).includes(setting));
    assert.ok(app.indexOf('{t("dmWebChoose")}') < advanced.pos);
    assert.ok(app.indexOf('t("dmWebGenerate")') < advanced.pos);
    assert.ok(app.indexOf("{status}") < advanced.pos);
    assert.ok(app.indexOf("originalIdentityDownload\n") < advanced.pos);
    assert.ok(!details.some((node) => node.getText(parsed).includes('t("dmWebSigningOptions")')));
    for (const id of ["signing-enabled", "signing-identity", "signing-password", "signing-profile"])
      assert.ok(app.indexOf(`id="${id}"`) < advanced.pos);
    assert.match(app, /useState<string\[\]>\(\[\]\)/);
    assert.match(app, /get\("target"\) === "ios18" \? "ios18" : "ios26"/);
    assert.match(app, /get\("legacyAstc"\) === "1"/);
    assert.doesNotMatch(app, /userAgent|userAgentData|deviceMemory/);
  });
  test("brief compatibility and detection copy does not claim authenticated extracted content", () => {
    for (const key of [
      "dmWebAutoInstructions",
      "dmWebOutputCompatibility",
      "dmWebBcTextures",
      "dmWebLegacyTextures",
      "dmWebOptionsBeforeFolder",
      "dmWebAdvancedOptions",
      "dmWebSigningOptions",
      "dmWebIncludedPackages",
      "dmWebPackagesReady",
      "dmWebDetectingInput",
    ])
      assert.ok(
        catalog[key]?.every((value) => value.length > 0),
        key,
      );
    assert.match(catalog.dmWebBcTextures[0], /required/);
    assert.match(catalog.dmWebLegacyTextures[0], /unverified/);
    assert.match(catalog.dmWebOptionsBeforeFolder[0], /before generating/);
    assert.match(catalog.dmWebExtractedInstructions[0], /not authenticated/);
    assert.match(catalog.dmWebIncludedPackages[0], /All/);
  });
  // 只去掉处理器中的三处类型注解，不构建应用、不加载真实打包器。
  // Remove only the handler's three type annotations; do not build the app or load the real packager.
  const chooseSource = app
    .slice(app.indexOf("  async function choose()"), app.indexOf("  function prepareDownload("))
    .replace("let progress: FolderProgress", "let progress")
    .replace("(event: FolderProgress)", "(event)")
    .replace("(error as { code?: string })", "error");
  function harness({ pick, discover, previous = {} }) {
    const writes = [],
      calls = [];
    const state = {
      ready: true,
      busy: false,
      status: "ready",
      folder: null,
      inputMode: null,
      nspNames: [],
      skipResources: false,
      ...previous,
    };
    const gate = createUiOperationGate();
    const context = {
      ...state,
      DOMException,
      mounted: { current: true },
      operationGate: { current: gate },
      cancellation: { current: null },
      packager: { current: { close: async () => calls.push("close") } },
      browser: {
        showDirectoryPicker: async () => {
          calls.push("pick");
          return pick();
        },
      },
      discoverGameFolder: async (folder, options) => {
        calls.push("discover");
        assert.equal(Object.hasOwn(options, "inputMode"), false);
        return discover(folder, options);
      },
      t: (key, ...args) => [key, ...args].join(" | "),
      folderSummary: (event) => event.inputMode ?? "auto",
      gameFolderFailureMessage: (code) => code,
      appendActivity: (value) => writes.push(["activity", value]),
    };
    for (const name of [
      "Busy",
      "Cancelling",
      "Status",
      "SkipResources",
      "Folder",
      "NspNames",
      "InputMode",
      "Finished",
    ])
      context[`set${name}`] = (value) => {
        const key = name[0].toLowerCase() + name.slice(1);
        state[key] = value;
        writes.push([key, value]);
      };
    const choose = vm.runInNewContext(`${chooseSource}\nchoose;`, context);
    return { state, calls, writes, gate, context, choose };
  }
  const deferred = () => {
    let resolve;
    const promise = new Promise((done) => {
      resolve = done;
    });
    return { promise, resolve };
  };
  test("automatic discovery includes every package, including DLC, with no filename heuristics", async () => {
    const folder = { name: "game" };
    const names = ["base.xci", "update.nsp", "DLC.nsp"];
    const h = harness({
      pick: () => folder,
      discover: async (_folder, { onProgress }) => {
        onProgress({
          inputMode: "packages",
          nspCount: names.length,
          prodKeysFound: true,
          titleKeysFound: true,
        });
        return { inputMode: "packages", nsps: names.map((name) => ({ name })) };
      },
      previous: { skipResources: true },
    });
    await h.choose();
    assert.equal(h.state.folder, folder);
    assert.equal(h.state.inputMode, "packages");
    assert.deepEqual([...h.state.nspNames], names);
    assert.equal(h.state.skipResources, false);
    assert.equal(h.state.busy, false);
    assert.equal(h.gate.busy, false);
    assert.match(h.state.status, /dmWebPackagesReady/);
  });
  test("extracted detection sets provenance and clears package state from an earlier folder", async () => {
    const folder = { name: "extracted" };
    const h = harness({
      pick: () => folder,
      discover: async () => ({ inputMode: "extracted", nsps: [] }),
      previous: {
        folder: { name: "old" },
        nspNames: ["old.nsp"],
        inputMode: "packages",
        skipResources: true,
      },
    });
    await h.choose();
    assert.equal(h.state.folder, folder);
    assert.equal(h.state.inputMode, "extracted");
    assert.equal(h.state.nspNames.length, 0);
    assert.equal(h.state.skipResources, false);
    assert.ok(
      h.writes.some(([key, value]) => key === "activity" && value === "dmWebExtractedChecked"),
    );
  });
  test("failed package discovery cannot retain a prior extracted selection or retry a weaker route", async () => {
    const h = harness({
      pick: () => ({ name: "packages" }),
      discover: async () => {
        throw Object.assign(new Error("missing keys"), { code: "MISSING_INPUT" });
      },
      previous: {
        folder: { name: "old" },
        inputMode: "extracted",
        nspNames: ["old.nsp"],
        skipResources: true,
      },
    });
    await h.choose();
    assert.equal(h.state.folder, null);
    assert.equal(h.state.inputMode, null);
    assert.equal(h.state.nspNames.length, 0);
    assert.equal(h.calls.filter((call) => call === "discover").length, 1);
    assert.match(h.state.status, /MISSING_INPUT/);
  });
  test("failed or dismissed first selection does not permanently lock configuration", async () => {
    for (const failure of ["picker", "discovery"]) {
      const h = harness({
        pick: () => {
          if (failure === "picker") throw new DOMException("cancel", "AbortError");
          return { name: "invalid" };
        },
        discover: () => {
          throw Object.assign(new Error("missing keys"), { code: "MISSING_INPUT" });
        },
      });
      await h.choose();
      assert.equal(h.state.busy, false, failure);
      assert.equal(h.gate.busy, false, failure);
    }
  });
  test("picker dismissal preserves an earlier selection and never starts discovery", async () => {
    const old = { name: "old" };
    const h = harness({
      pick: () => {
        throw new DOMException("cancel", "AbortError");
      },
      discover: () => assert.fail("unexpected discovery"),
      previous: {
        folder: old,
        inputMode: "packages",
        nspNames: ["old.nsp"],
        skipResources: true,
        status: "previous",
      },
    });
    await h.choose();
    assert.equal(h.state.folder, old);
    assert.equal(h.state.skipResources, true);
    assert.equal(h.state.status, "previous");
    assert.deepEqual(h.calls, ["pick"]);
    assert.equal(h.gate.busy, false);
  });
  test("duplicate clicks cannot start a second scan before rendered busy state catches up", async () => {
    const pending = deferred();
    const h = harness({
      pick: () => pending.promise,
      discover: async () => ({ inputMode: "packages", nsps: [{ name: "game.nsp" }] }),
    });
    const first = h.choose();
    await h.choose();
    assert.equal(h.calls.filter((call) => call === "pick").length, 1);
    pending.resolve({ name: "game" });
    await first;
    assert.equal(h.calls.filter((call) => call === "discover").length, 1);
    assert.equal(h.gate.busy, false);
  });
  test("late cancelled discovery cannot repopulate mode or packages and closes the owned runtime", async () => {
    const pending = deferred(),
      started = deferred();
    const h = harness({
      pick: () => ({ name: "game" }),
      discover: () => {
        started.resolve();
        return pending.promise;
      },
    });
    const task = h.choose();
    await started.promise;
    h.context.cancellation.current.abort();
    pending.resolve({ inputMode: "packages", nsps: [{ name: "late.nsp" }] });
    await task;
    assert.equal(h.state.folder, null);
    assert.equal(h.state.inputMode, null);
    assert.equal(h.state.nspNames.length, 0);
    assert.equal(h.state.finished, true);
    assert.equal(h.calls.filter((call) => call === "close").length, 1);
    assert.equal(h.gate.busy, false);
  });
  test("late unmounted discovery cannot publish state or release a replacement operation", async () => {
    const pending = deferred(),
      started = deferred();
    const h = harness({
      pick: () => ({ name: "game" }),
      discover: () => {
        started.resolve();
        return pending.promise;
      },
    });
    const task = h.choose();
    await started.promise;
    h.context.mounted.current = false;
    h.gate.cancel();
    const replacement = h.gate.begin();
    const count = h.writes.length;
    pending.resolve({ inputMode: "extracted", nsps: [] });
    await task;
    assert.equal(h.writes.length, count);
    assert.equal(replacement.isCurrent(), true);
    replacement.finish();
  });

  // 固定诊断按优先级映射；不把原始证书、描述文件或错误内容带进界面。
  // Map fixed diagnostics in precedence order without exposing certificate, profile or raw error content.
  test("signer length limit stays actionable, sanitized, and subordinate to cancellation", () => {
    let expression;
    function findDiagnostic(node) {
      if (ts.isVariableDeclaration(node) && node.name.getText(parsed) === "signingMessage") {
        expression = node.initializer?.getText(parsed);
      }
      ts.forEachChild(node, findDiagnostic);
    }
    findDiagnostic(parsed);
    assert.ok(expression);
    for (const [code, expected] of [
      ["SIGNING_APPLICATION_ID_TOO_LONG", "dmWebSigningApplicationIdTooLong"],
      ["SIGNING_PROFILE_MISMATCH", "dmWebSigningProfileMismatch"],
      ["PRIVATE-PROFILE-NAME", "dmWebSigningFailed"],
    ]) {
      for (const aborted of [false, true]) {
        const result = vm.runInNewContext(expression, {
          Error,
          signingError: new Error(code),
          controller: { signal: { aborted } },
          t: (key) => key,
        });
        assert.equal(result, aborted ? "dmWebSigningCancelled" : expected);
      }
    }
    assert.match(
      catalog.dmWebSigningApplicationIdTooLong[0],
      /255-character limit.*Unsigned IPA retained.*another signer.*shorter bundle ID/,
    );
  });
}

// unsupported-browser-ui
{
  // 不支持文件夹访问时的界面契约；只检查源码和能力表达式，不加载运行时或私有输入。
  // UI contract without folder access; inspect source and capability expressions, never runtimes or private inputs.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { runInNewContext } = nodeVmModule;
  const { default: ts } = typescriptModule;
  const { catalog } = i18nCatalogModule;

  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  const faq = await readFile(
    new URL("../src/components/PreparationFaq.tsx", import.meta.url),
    "utf8",
  );
  const tree = ts.createSourceFile("App.tsx", app, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
  const nodes = [];
  function visit(node) {
    nodes.push(node);
    ts.forEachChild(node, visit);
  }
  visit(tree);
  const openings = nodes.filter(
    (node) => ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node),
  );
  const attr = (node, key) =>
    node.attributes.properties.find(
      (entry) => ts.isJsxAttribute(entry) && entry.name.getText(tree) === key,
    )?.initializer;

  test("folder access is detected by callable API, not browser name or OPFS availability", () => {
    const declaration = nodes.find(
      (node) =>
        ts.isVariableDeclaration(node) && node.name.getText(tree) === "supportsDirectoryPicker",
    );
    assert.ok(declaration);
    const expression = declaration.initializer.getText(tree);
    for (const [browser, expected] of [
      [{}, false],
      [{ navigator: { storage: { getDirectory() {} } } }, false],
      [{ showDirectoryPicker: true }, false],
      [{ showDirectoryPicker() {} }, true],
    ]) {
      assert.equal(runInNewContext(expression, { browser }), expected);
    }
    assert.doesNotMatch(expression, /userAgent|platform|storage/);
  });

  test("unsupported folder access has an early shared-table notice before the long guide", () => {
    const notice = openings.find((node) => attr(node, "role")?.text === "alert");
    assert.ok(notice);
    assert.ok(notice.pos < app.indexOf("<InstallationGuide"));
    const paragraph = notice.parent;
    assert.match(paragraph.getText(tree), /t\("dmWebFolderAccessUnsupported"\)/);
    assert.match(paragraph.parent.parent.getText(tree), /!supportsDirectoryPicker &&/);
    let inHeader = false;
    for (let parent = paragraph.parent; parent; parent = parent.parent) {
      if (ts.isJsxElement(parent) && parent.openingElement.tagName.getText(tree) === "CardHeader")
        inHeader = true;
    }
    assert.ok(inHeader);
    assert.equal(catalog.dmWebFolderAccessUnsupported.length, 2);
    assert.match(catalog.dmWebFolderAccessUnsupported[0], /desktop Chrome or Edge/);
    assert.match(catalog.dmWebFolderAccessUnsupported[1], /桌面版 Chrome 或 Edge/);
  });

  test("unsupported bootstrap stops before downloading runtimes and keeps packaging disabled", () => {
    const effect = nodes.find(
      (node) =>
        ts.isCallExpression(node) &&
        node.expression.getText(tree) === "useEffect" &&
        node.getText(tree).includes("prepareBrowserPackager"),
    );
    const body = effect.getText(tree);
    assert.match(
      body,
      /if \(!supportsDirectoryPicker\) \{\s*setStatus\(t\("dmWebFolderAccessUnsupported"\)\);\s*return;/,
    );
    assert.ok(
      body.indexOf("if (!supportsDirectoryPicker)") < body.indexOf("void prepareBrowserPackager"),
    );
    assert.match(app, /disabled=\{!ready \|\| busy\}/);
    assert.match(app, /disabled=\{!generationReadiness\.canGenerate\}/);
  });

  test("language and help remain outside disabled fieldsets without a page-wide interaction block", () => {
    const language = openings.find((node) => attr(node, "id")?.text === "ui-language");
    assert.ok(language);
    assert.equal(attr(language, "disabled"), undefined);
    for (const control of [
      language,
      ...openings.filter((node) =>
        ["InstallationGuide", "PreparationFaq", "AccordionTrigger"].includes(
          node.tagName.getText(tree),
        ),
      ),
    ]) {
      for (let parent = control.parent; parent; parent = parent.parent) {
        if (ts.isJsxElement(parent)) {
          const ancestor = parent.openingElement;
          assert.equal(attr(ancestor, "inert"), undefined);
          if (["FieldSet", "fieldset"].includes(ancestor.tagName.getText(tree)))
            assert.equal(attr(ancestor, "disabled"), undefined);
        }
      }
    }
    assert.doesNotMatch(faq, /disabled=|inert=/);
    assert.doesNotMatch(app, /inert=|pointer-events-none/);
  });
}

// generation-progress
{
  // 合成阶段与 Blob 测试，不运行编译器、WASM、浏览器、设备或私有输入。
  // Synthetic stage/Blob tests only: no compiler, WASM, browser, device or private input execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { createGenerationProgress } = generationProgressModule;
  const { createDownloadPreparation } = downloadPreparationModule;
  const { createUiOperationGate } = uiOperationGateModule;

  const output = () => ({ blob: new Blob([new Uint8Array(22)]), fileName: "synthetic.ipa" });
  const result = () => ({
    download: output(),
    resources: { status: "complete" },
    legacyPack: { status: "complete" },
  });
  const stages = [
    "extraction",
    "program",
    "assets",
    "shaders",
    "assembly",
    "resources",
    "legacy-pack",
    "commit",
    "output",
  ];

  test("the equal-stage plan freezes optional exports and signing at job start", () => {
    for (const legacyAstc of [false, true])
      for (const signing of [false, true])
        for (const exportResources of [false, true]) {
          const options = { legacyAstc, signing, exportResources };
          const tracker = createGenerationProgress(options);
          options.legacyAstc = !legacyAstc;
          options.signing = !signing;
          options.exportResources = !exportResources;
          assert.equal(tracker.plan.includes("legacy-pack"), legacyAstc);
          assert.equal(tracker.plan.includes("signing"), signing);
          assert.equal(tracker.plan.includes("resources"), exportResources);
          assert.equal(
            tracker.snapshot().totalStages,
            8 + Number(legacyAstc) + Number(signing) + Number(exportResources),
          );
          assert.ok(Object.isFrozen(tracker.plan));
          assert.ok(Object.isFrozen(tracker.included));
          assert.ok(tracker.plan.every((stage) => !["install", "copy-resources"].includes(stage)));
        }
  });

  test("stage events are monotonic and cannot claim download readiness", () => {
    const tracker = createGenerationProgress({ legacyAstc: true });
    tracker.prepared();
    let previous = tracker.snapshot().percent;
    for (const stage of stages) {
      tracker.observe({ stage, state: "running", label: "PRIVATE-FILENAME" });
      tracker.observe({ stage, state: "complete" });
      const next = tracker.snapshot();
      assert.ok(next.percent >= previous && next.percent < 100);
      previous = next.percent;
      tracker.observe({ stage, state: "running" });
      assert.equal(tracker.snapshot().percent, previous);
    }
    tracker.downloadPrepared(true);
    assert.ok(tracker.snapshot().percent < 100);
    assert.equal(tracker.generated(result()), true);
    assert.ok(tracker.snapshot().percent < 100);
    tracker.downloadPrepared(true);
    assert.equal(tracker.snapshot().status, "complete");
    assert.equal(tracker.snapshot().percent, 100);
    tracker.observe({ stage: "assets", state: "failed" });
    assert.equal(tracker.snapshot().status, "complete");
  });

  test("measured counters remain stage-local and omit private labels, paths and arbitrary fields", () => {
    const tracker = createGenerationProgress({ legacyAstc: true });
    for (const event of [
      {
        stage: "shaders",
        diagnostic: {
          kind: "shader-records",
          completedRecords: 4,
          totalRecords: 4,
          secret: "PRIVATE-KEY",
        },
      },
      {
        stage: "resources",
        resourceProgress: {
          filesWritten: 2,
          totalFiles: 2,
          bytesWritten: 32n,
          totalBytes: 32n,
          directoryName: "PRIVATE-PATH",
        },
      },
      {
        stage: "assets",
        diagnostic: {
          kind: "legacy-astc",
          completedBlocks: 500,
          processedBytes: 4096,
          private: "PRIVATE-KEY",
        },
      },
      { stage: "legacy-pack", bytesWritten: 32, totalBytes: 32, directoryName: "PRIVATE-PATH" },
      {
        stage: "extraction",
        diagnostic: { stage: "romfs-merged", completed: 100, total: 100, bytes: 32 },
      },
    ]) {
      tracker.observe({ ...event, state: "running", label: "PRIVATE-FILENAME" });
      const view = tracker.snapshot();
      assert.ok(view.detail);
      assert.equal(view.completedStages, 0);
      assert.equal(view.percent, 0);
      assert.doesNotMatch(JSON.stringify(view), /PRIVATE/);
      assert.ok(Object.isFrozen(view.detail));
    }
    tracker.observe({ stage: "program", state: "running" });
    assert.equal(tracker.snapshot().detail, null);
    for (const number of [NaN, Infinity, -1, 0.5, Number.MAX_SAFE_INTEGER + 1]) {
      tracker.observe({
        stage: "legacy-pack",
        state: "running",
        bytesWritten: number,
        totalBytes: 32,
      });
      assert.equal(tracker.snapshot().detail, null);
    }
  });

  test("full record or byte counts do not finish stages before terminal markers", () => {
    const tracker = createGenerationProgress();
    tracker.observe({
      stage: "resources",
      state: "running",
      resourceProgress: { filesWritten: 1, totalFiles: 1, bytesWritten: 10, totalBytes: 10 },
    });
    assert.equal(tracker.snapshot().completedStages, 0);
    tracker.observe({ stage: "resources", state: "complete" });
    assert.equal(tracker.snapshot().completedStages, 1);
    tracker.observe({ stage: "resources", state: "complete" });
    assert.equal(tracker.snapshot().completedStages, 1);
  });

  test("missing requested exports or a missing Blob cannot unlock completion", () => {
    for (const invalid of [
      { ...result(), download: undefined },
      { ...result(), download: { blob: new Blob([]) } },
      { ...result(), resources: { status: "skipped" } },
      { ...result(), legacyPack: { status: "incomplete" } },
    ]) {
      const tracker = createGenerationProgress({ legacyAstc: true });
      assert.equal(tracker.generated(invalid), false);
      tracker.downloadPrepared(true);
      assert.ok(tracker.snapshot().percent < 100);
    }
    const skipped = createGenerationProgress({ exportResources: false });
    assert.equal(skipped.generated({ download: output(), resources: { status: "skipped" } }), true);
  });

  test("download URL failure stays below 100 and retries the retained output only", () => {
    const tracker = createGenerationProgress();
    let unavailable = true;
    const downloads = createDownloadPreparation({
      createObjectURL: () => {
        if (unavailable) throw new Error("Synthetic URL failure");
        return "blob:synthetic";
      },
    });
    const generated = result();
    tracker.generated(generated);
    downloads.retain(generated.download);
    tracker.downloadPrepared(downloads.prepare().status === "ready");
    assert.equal(tracker.snapshot().status, "download-pending");
    assert.ok(tracker.snapshot().percent < 100);
    unavailable = false;
    tracker.downloadPrepared(downloads.prepare().status === "ready");
    assert.equal(tracker.snapshot().percent, 100);
  });

  test("signing fallback and cancellation keep the requested signed output incomplete until retry succeeds", () => {
    for (const cancelled of [false, true]) {
      const tracker = createGenerationProgress({ signing: true });
      tracker.generated(result());
      const total = tracker.snapshot().totalStages;
      tracker.downloadPrepared(true); // 未签名链接虽可用，但不是请求的签名输出。 | Unsigned link is usable, not the requested signed output.
      assert.ok(tracker.snapshot().percent < 100);
      tracker.startSigning();
      for (const stage of ["inspect", "sign", "package", "verify"])
        tracker.observeSigning({ stage, completed: 1, total: 1 });
      tracker.signingFailed(cancelled);
      const stopped = tracker.snapshot();
      tracker.downloadPrepared(true);
      tracker.observeSigning({ stage: "verify", completed: 1, total: 1 });
      assert.deepEqual(tracker.snapshot(), stopped);
      assert.equal(stopped.status, cancelled ? "signing-cancelled" : "signing-failed");
      assert.ok(stopped.percent < 100);
      assert.equal(tracker.startSigning(), true);
      assert.equal(tracker.snapshot().totalStages, total);
      assert.equal(tracker.snapshot().percent, stopped.percent);
      assert.equal(tracker.signed(output()), true);
      tracker.downloadPrepared(true);
      assert.equal(tracker.snapshot().percent, 100);
    }
  });

  test("cancellation and recovered-original failures reject late stage completion", () => {
    for (const cancelled of [true, false]) {
      const tracker = createGenerationProgress();
      tracker.prepared();
      tracker.cancel();
      const stopping = tracker.snapshot();
      tracker.observe({ stage: "shaders", state: "complete" });
      assert.deepEqual(tracker.snapshot(), stopping);
      tracker.fail(cancelled);
      tracker.generated(result()); // 另行恢复的原始文件不代表所请求的生成成功。 | A separately recovered original is not a successful requested generation.
      tracker.downloadPrepared(true);
      assert.equal(tracker.snapshot().status, cancelled ? "cancelled" : "failed");
      assert.ok(tracker.snapshot().percent < 100);
    }
  });

  test("old signing observers cannot affect a newer operation or a replacement job", () => {
    const gate = createUiOperationGate();
    const tracker = createGenerationProgress({ signing: true });
    tracker.generated(result());
    const old = gate.begin();
    tracker.startSigning();
    const late = () => {
      if (old.isCurrent()) tracker.observeSigning({ stage: "verify", completed: 1, total: 1 });
    };
    tracker.signingFailed();
    old.finish();
    const retry = gate.begin();
    tracker.startSigning();
    late();
    assert.equal(tracker.snapshot().detail, null);
    const replacement = createGenerationProgress();
    tracker.signed(output());
    assert.equal(replacement.snapshot().percent, 0);
    retry.finish();
  });

  test("the UI uses actual Radix values, fixed copy and guarded/coalesced job updates", async () => {
    const [app, component, primitive] = await Promise.all(
      [
        "../src/App.tsx",
        "../src/components/GenerationProgress.tsx",
        "../src/components/ui/progress.tsx",
      ].map((name) => readFile(new URL(name, import.meta.url), "utf8")),
    );
    assert.match(primitive, /<ProgressPrimitive.Root[\s\S]*?value=\{value\}/);
    assert.match(component, /<Progress[\s\S]*?value=\{progress.percent\}/);
    assert.match(component, /aria-valuetext=/);
    assert.match(component, /dmWebOverallEstimate/);
    assert.match(component, /dmWebOverallUnmeasured/);
    assert.match(component, /role="status"/);
    assert.match(component, /motion-reduce:transition-none/);
    assert.match(app, /const jobOptions = Object.freeze\(/);
    assert.match(app, /generationTracker.current === tracker/);
    assert.match(app, /generationTracker.current === pendingProgress.tracker/);
    assert.match(
      app,
      /if \(!acceptingProgress \|\| controller.signal.aborted \|\| !current\(\)\) return;\s+if \(event.phase === "cache"\) \{[\s\S]*?return;\s+\}\s+tracker.observe\(event\)/,
    );
    assert.match(
      app,
      /if \(!acceptingSigningProgress \|\| !current\(\) \|\| controller.signal.aborted\) return;\s+tracker\?\.observeSigning\(event\)/,
    );
    assert.doesNotMatch(component, /setInterval|setTimeout|Date.now/);
    assert.equal((app.match(/<GenerationProgress\b/g) ?? []).length, 1);
  });

  test("official Progress renders correct determinate, complete and indeterminate accessibility states", async () => {
    const { default: ts } = await import("typescript");
    const { default: vm } = await import("node:vm");
    const { createRequire } = await import("node:module");
    const { createElement } = await import("react");
    const { renderToStaticMarkup } = await import("react-dom/server");
    const require = createRequire(import.meta.url);
    const source = await readFile(
      new URL("../src/components/ui/progress.tsx", import.meta.url),
      "utf8",
    );
    const compiled = ts.transpileModule(source, {
      compilerOptions: { module: ts.ModuleKind.CommonJS, jsx: ts.JsxEmit.React },
    }).outputText;
    const exports = {};
    vm.runInNewContext(compiled, {
      exports,
      require: (name) =>
        name === "@/lib/utils"
          ? { cn: (...values) => values.filter(Boolean).join(" ") }
          : require(name),
    });
    for (const [value, state] of [
      [37, "loading"],
      [100, "complete"],
      [null, "indeterminate"],
    ]) {
      const markup = renderToStaticMarkup(
        createElement(exports.Progress, { value, "aria-label": "Synthetic progress" }),
      );
      assert.match(markup, /role="progressbar"/);
      assert.match(markup, new RegExp(`data-state="${state}"`));
      if (value === null) assert.doesNotMatch(markup, /aria-valuenow/);
      else assert.match(markup, new RegExp(`aria-valuenow="${value}"`));
    }
  });

  // 在服务端渲染状态容器以确认签名消息只出现一次；不启动浏览器或运行时。
  // Render the status container on the server to prove signing messages appear once; no browser or runtime startup.
  test("job status and signing completion stay inside one progress box without duplicate outside lines", async () => {
    const { default: ts } = await import("typescript");
    const { default: vm } = await import("node:vm");
    const { createRequire } = await import("node:module");
    const { createElement } = await import("react");
    const { renderToStaticMarkup } = await import("react-dom/server");
    const require = createRequire(import.meta.url);
    const component = await readFile(
      new URL("../src/components/GenerationProgress.tsx", import.meta.url),
      "utf8",
    );
    const compiled = ts.transpileModule(component, {
      compilerOptions: {
        module: ts.ModuleKind.CommonJS,
        jsx: ts.JsxEmit.ReactJSX,
        target: ts.ScriptTarget.ES2022,
      },
    }).outputText;
    const exports = {};
    vm.runInNewContext(compiled, {
      exports,
      require: (name) => {
        if (name === "@/components/ui/progress")
          return { Progress: () => createElement("div", { role: "progressbar" }) };
        if (name === "../../i18n.mjs") return { t: (key) => key };
        if (name === "../../packaging-diagnostics.mjs") return { compilerWorkerLabel: () => "" };
        return require(name);
      },
    });
    const tracker = createGenerationProgress({ signing: true });
    tracker.generated(result());
    tracker.startSigning();
    tracker.signed(output());
    tracker.downloadPrepared(true);
    const markup = renderToStaticMarkup(
      createElement(exports.GenerationProgress, {
        progress: tracker.snapshot(),
        statusId: "packager-status",
        statusMessages: ["signed completion detail", "signed completion detail", "resource result"],
      }),
    );
    assert.equal((markup.match(/signed completion detail/g) ?? []).length, 1);
    assert.match(
      markup,
      /<section[\s\S]*id="packager-status"[^>]*role="status"[^>]*aria-live="polite"[\s\S]*signed completion detail[\s\S]*<\/section>/,
    );
    assert.equal(
      renderToStaticMarkup(
        createElement(exports.GenerationProgress, { progress: null, statusMessages: ["startup"] }),
      ),
      "",
    );
    const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    assert.match(app, /statusMessages=\{\[status, signingStatus, resourceOutcome\]\}/);
    assert.match(app, /!overallProgress && \(\s*<p\s+id="packager-status"/);
    assert.match(app, /!overallProgress && signingStatus &&/);
    assert.match(app, /!overallProgress && resourceOutcome &&/);
  });

  test("actual App progress publication rejects old jobs and clears queued snapshots at terminal updates", async () => {
    const { default: ts } = await import("typescript");
    const { default: vm } = await import("node:vm");
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const parsed = ts.createSourceFile(
      "App.tsx",
      source,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.TSX,
    );
    const callbacks = new Map();
    function visit(node) {
      if (
        ts.isVariableDeclaration(node) &&
        ["publishProgress", "flushActivity"].includes(node.name.getText(parsed))
      )
        callbacks.set(node.name.getText(parsed), node.initializer.arguments[0].getText(parsed));
      ts.forEachChild(node, visit);
    }
    visit(parsed);
    const current = createGenerationProgress(),
      old = createGenerationProgress();
    const published = [];
    const scope = {
      mounted: { current: true },
      generationTracker: { current },
      pendingOverallProgress: { current: { tracker: old, snapshot: old.snapshot() } },
      setOverallProgress: (snapshot) => published.push(snapshot),
      activityTimer: { current: null },
      renderedActivityRevision: { current: 0 },
      activityRevision: { current: 0 },
      pendingGenerationStatus: { current: null },
    };
    const invoke = (name) =>
      vm.runInNewContext(
        ts.transpileModule(`(${callbacks.get(name)})`, {
          compilerOptions: { target: ts.ScriptTarget.ES2022 },
        }).outputText,
        scope,
      );
    const publish = invoke("publishProgress"),
      flush = invoke("flushActivity");
    publish(old);
    assert.equal(published.length, 0);
    flush();
    assert.equal(published.length, 0);
    scope.pendingOverallProgress.current = { tracker: current, snapshot: current.snapshot() };
    current.fail();
    publish(current);
    flush();
    assert.equal(published.length, 1);
    assert.equal(published[0].status, "failed");
    scope.mounted.current = false;
    publish(current);
    assert.equal(published.length, 1);
  });

  test("bootstrap uses shadcn for bounded bytes and truly indeterminate initialization", async () => {
    const { default: ts } = await import("typescript");
    const { default: vm } = await import("node:vm");
    const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const parsed = ts.createSourceFile(
      "App.tsx",
      app,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.TSX,
    );
    const bars = [];
    function visit(node) {
      if (ts.isJsxSelfClosingElement(node) && node.tagName.getText(parsed) === "Progress")
        bars.push(node);
      ts.forEachChild(node, visit);
    }
    visit(parsed);
    assert.equal(bars.length, 2);
    assert.doesNotMatch(app, /<progress\b/);
    const values = bars.map((bar) =>
      bar.attributes.properties
        .find((attribute) => attribute.name.getText(parsed) === "value")
        .initializer.expression.getText(parsed),
    );
    for (const [loadedBytes, totalBytes, expected] of [
      [0, 200, 0],
      [50, 200, 25],
      [200, 200, 100],
      [250, 200, 100],
      [-5, 200, 0],
    ])
      assert.equal(
        vm.runInNewContext(values[0], { loading: { loadedBytes, totalBytes } }),
        expected,
      );
    assert.equal(vm.runInNewContext(values[1]), null);
    assert.match(app, /Number.isFinite\(loading.loadedBytes\)/);
    assert.match(app, /Number.isFinite\(loading.totalBytes\)/);
    assert.match(app, /loading.totalBytes > 0/);
    assert.match(bars[0].getText(parsed), /aria-describedby="runtime-loading-details"/);
    assert.match(
      app,
      /<p id="runtime-loading-details">[\s\S]*?"dmWebFilesReady",\s*megabytes\(loading.loadedBytes\),\s*megabytes\(loading.totalBytes\),\s*loading.completedFiles,\s*loading.totalFiles/,
    );
    assert.match(bars[1].getText(parsed), /motion-safe:animate-pulse/);
    assert.match(bars[1].getText(parsed), /aria-label=\{loadingLabel\(loading.phase\)\}/);
  });
}

// generation-readiness
{
  // 就绪条件与真实界面表达式的隔离回归；不读取游戏、凭据，也不运行打包器。
  // Isolated readiness and real UI-expression regressions; no game or credential reads and no packager execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: vm } = nodeVmModule;
  const { default: ts } = typescriptModule;
  const { getGenerationReadiness } = generationReadinessModule;
  const { normalizeAppCustomizationDraft, resolveAppCustomizationDefaults } =
    appCustomizationDefaultsModule;
  const { BUNDLED_SHELL_PROFILE_IDS } = shellProfilesModule;
  const { appCustomizationFailureMessage } = packagingDiagnosticsModule;
  const { catalog } = i18nCatalogModule;

  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  const parsed = ts.createSourceFile(
    "App.tsx",
    app,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TSX,
  );
  const nodes = [];
  function visit(node) {
    nodes.push(node);
    ts.forEachChild(node, visit);
  }
  visit(parsed);
  const declaration = (name) =>
    nodes.find((node) => ts.isVariableDeclaration(node) && node.name.getText(parsed) === name);
  const attribute = (node, name) =>
    node.attributes.properties.find(
      (property) => ts.isJsxAttribute(property) && property.name.getText(parsed) === name,
    );
  const generateButton = nodes.find(
    (node) =>
      ts.isJsxOpeningElement(node) &&
      attribute(node, "onClick")?.initializer?.getText(parsed).includes("void generate()"),
  );
  const readyState = (overrides = {}) => ({
    ready: true,
    busy: false,
    finished: false,
    download: null,
    folder: { name: "inert folder" },
    inputMode: "packages",
    nspNames: ["base.nsp", "update.nsp", "addon.nsp"],
    customizationValidation: { options: {}, error: "" },
    customizationError: "",
    signing: false,
    signingReady: true,
    signingFiles: { identity: false, profile: false },
    ...overrides,
  });
  function rendered(overrides = {}) {
    const context = {
      ...readyState(overrides),
      getGenerationReadiness,
      t: (key) => key,
    };
    context.generationReadiness = vm.runInNewContext(
      declaration("generationReadiness").initializer.getText(parsed),
      context,
    );
    const message = vm.runInNewContext(
      declaration("generationReadinessMessage").initializer.getText(parsed),
      context,
    );
    const disabled = vm.runInNewContext(
      attribute(generateButton, "disabled").initializer.expression.getText(parsed),
      context,
    );
    return { ...context.generationReadiness, disabled, message };
  }

  test("three discovered packages with untouched 1.0 defaults allow unsigned generation for either target", () => {
    for (const profileId of BUNDLED_SHELL_PROFILE_IDS) {
      const defaults = resolveAppCustomizationDefaults(profileId);
      assert.equal(defaults.version, "1.0");
      for (const draft of [{}, { ...defaults }]) {
        const options = normalizeAppCustomizationDraft(draft, defaults);
        assert.deepEqual(options, {});
        for (const legacyAstc of [false, true]) {
          const result = rendered({
            profileId,
            legacyAstc,
            customizationValidation: { options, error: "" },
            signingReady: false,
          });
          assert.equal(result.disabled, false);
          assert.equal(result.canGenerate, true);
          assert.equal(result.message, "dmWebGenerateReady");
        }
      }
    }
    assert.match(app, /get\("signing"\) === "1"/);
    assert.equal(rendered({ inputMode: "extracted", nspNames: [] }).disabled, false);
  });

  test("folder readiness cannot hide enabled signing's exact missing-file requirement", () => {
    for (const [identity, profile, messageKey] of [
      [false, false, "dmWebGenerateSigningFilesNeeded"],
      [false, true, "dmWebGenerateSigningIdentityNeeded"],
      [true, false, "dmWebGenerateSigningProfileNeeded"],
      [true, true, "dmWebGenerateReady"],
    ]) {
      const result = rendered({ signing: true, signingFiles: { identity, profile } });
      assert.equal(result.disabled, !identity || !profile);
      assert.equal(result.message, messageKey);
    }
    const unavailable = rendered({ signing: true, signingReady: false });
    assert.equal(unavailable.disabled, true);
    assert.equal(unavailable.message, "dmWebGenerateSigningUnavailable");
    const signingOff = rendered({ signing: false, signingReady: false });
    assert.equal(signingOff.disabled, false);
  });

  test("customization blocks honestly and restoring shell defaults clears the blocker", () => {
    const defaults = resolveAppCustomizationDefaults(BUNDLED_SHELL_PROFILE_IDS[0]);
    let error;
    try {
      normalizeAppCustomizationDraft({ version: "1.2" }, defaults);
      assert.fail("an invalid custom version must be rejected");
    } catch (failure) {
      error = appCustomizationFailureMessage(failure);
    }
    for (const state of [{ customizationValidation: { error } }, { customizationError: error }]) {
      const result = rendered(state);
      assert.equal(result.disabled, true);
      assert.equal(result.message, `dmWebCustomCorrect ${error}`);
      assert.doesNotMatch(result.message, /dmWebGenerateReady/);
    }
    assert.deepEqual(normalizeAppCustomizationDraft({ version: "1.0" }, defaults), {});
    assert.equal(rendered().disabled, false);
  });

  test("missing input and active, unready, or terminal states stay blocked", () => {
    for (const state of [{ folder: null }, { inputMode: null }, { nspNames: [] }]) {
      const result = rendered(state);
      assert.equal(result.disabled, true);
      assert.equal(result.message, "dmWebGenerateChooseFolder");
    }
    for (const state of [
      { busy: true },
      { ready: false },
      { finished: true },
      { download: { url: "blob:inert" } },
    ]) {
      const result = rendered(state);
      assert.equal(result.disabled, true);
      assert.equal(result.message, "", "the live operation status takes precedence");
    }
    assert.match(app, /if \(!generationReadiness.canGenerate \|\| !packager.current/);
    assert.match(app, /aria-describedby=\{[\s\S]*?"generation-readiness" : "packager-status"/);
    assert.match(
      app,
      /id="generation-readiness"[\s\S]*?role="status"[\s\S]*?\{generationReadinessMessage\}/,
    );
  });

  test("real file callbacks update each selection, removal and operation guard independently", () => {
    for (const id of ["signing-identity", "signing-profile"]) {
      const input = nodes.find(
        (node) =>
          ts.isJsxSelfClosingElement(node) && attribute(node, "id")?.initializer?.text === id,
      );
      const handler = attribute(input, "onChange").initializer.expression.getText(parsed);
      for (const identity of [false, true]) {
        for (const profile of [false, true]) {
          const writes = [];
          const context = {
            identityInput: { current: { files: identity ? [{}] : [] } },
            profileInput: { current: { files: profile ? [{}] : [] } },
            operationGate: { current: { busy: false } },
            setSigningFiles: (value) => writes.push({ ...value }),
          };
          vm.runInNewContext(handler, context)();
          assert.deepEqual(writes, [{ identity, profile }]);
          context.operationGate.current.busy = true;
          vm.runInNewContext(handler, context)();
          assert.equal(writes.length, 1);
        }
      }
    }
  });

  test("turning signing off clears both file-selection flags and transient credentials", () => {
    const clear = nodes.find(
      (node) =>
        ts.isFunctionDeclaration(node) && node.name?.getText(parsed) === "clearSigningInputs",
    );
    const writes = [];
    const context = {
      mounted: { current: true },
      identityInput: { current: { value: "inert selection" } },
      profileInput: { current: { value: "inert selection" } },
      passwordInput: { current: { value: "inert input" } },
      setSigningFiles: (value) => writes.push({ ...value }),
    };
    vm.runInNewContext(`${clear.getText(parsed)}\nclearSigningInputs();`, context);
    assert.deepEqual(writes, [{ identity: false, profile: false }]);
    for (const ref of [context.identityInput, context.profileInput, context.passwordInput])
      assert.equal(ref.current.value, "");
    assert.equal(rendered({ signing: false, signingFiles: writes[0] }).disabled, false);
  });

  test("folder summaries never claim global readiness and every blocker is localized", () => {
    for (const key of ["dmWebPackagesReady", "dmWebExtractedReady"])
      for (const text of catalog[key]) assert.doesNotMatch(text, /ready|就绪|可以生成/i);
    for (const key of [
      "dmWebGenerateChooseFolder",
      "dmWebGenerateReady",
      "dmWebGenerateSigningFilesNeeded",
      "dmWebGenerateSigningIdentityNeeded",
      "dmWebGenerateSigningProfileNeeded",
      "dmWebGenerateSigningUnavailable",
    ]) {
      assert.equal(catalog[key]?.length, 2, key);
      assert.ok(
        catalog[key].every((text) => typeof text === "string" && text.length > 0),
        key,
      );
    }
  });
}

// ignore-cache-ui
{
  // 仅测试界面状态/选项及固定排障文字，不访问运行时或私有文件。
  // UI state/options and fixed troubleshooting text only; no runtime or private file access.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { default: vm } = nodeVmModule;
  const { default: ts } = typescriptModule;
  const { readFile } = promisesModule;
  const { catalog } = i18nCatalogModule;
  const { packagingActivity } = packagingDiagnosticsModule;

  const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
  const parsed = ts.createSourceFile(
    "App.tsx",
    app,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TSX,
  );
  const nodes = [];
  function visit(node) {
    nodes.push(node);
    ts.forEachChild(node, visit);
  }
  visit(parsed);
  const property = (object, key) =>
    object.properties.find((node) => node.name?.getText(parsed) === key);

  test("ignore cache starts off and remains inside Advanced, without deleting existing files", () => {
    assert.match(app, /\[ignoreCache, setIgnoreCache\] = useState\(false\)/);
    const input = nodes.find(
      (node) =>
        ts.isJsxSelfClosingElement(node) &&
        node.tagName.getText(parsed) === "Checkbox" &&
        node.attributes.properties.some(
          (attribute) =>
            attribute.name.getText(parsed) === "id" &&
            attribute.initializer?.text === "ignore-cache",
        ),
    );
    assert.ok(input);
    let panel;
    for (let parent = input.parent; parent; parent = parent.parent)
      if (
        ts.isJsxElement(parent) &&
        parent.openingElement.tagName.getText(parsed) === "AccordionItem"
      ) {
        panel = parent;
        break;
      }
    assert.equal(
      panel.openingElement.attributes.properties.find(
        (attribute) => attribute.name.getText(parsed) === "value",
      ).initializer.text,
      "advanced",
    );
    assert.doesNotMatch(app, /removeEntry\(|deleteDirectory\(/);
  });

  test("both cache policies are frozen per job and reach the actual session.generate call, not bootstrap", () => {
    const snapshot = nodes
      .find((node) => ts.isVariableDeclaration(node) && node.name.getText(parsed) === "jobOptions")
      .initializer.getText(parsed);
    const generate = nodes.find(
      (node) =>
        ts.isCallExpression(node) && node.expression.getText(parsed) === "jobPackager.generate",
    );
    const forwarded = property(generate.arguments[0], "ignoreCache").initializer.getText(parsed);
    const bootstrap = nodes.find(
      (node) =>
        ts.isCallExpression(node) && node.expression.getText(parsed) === "prepareBrowserPackager",
    );
    assert.equal(property(bootstrap.arguments[0], "ignoreCache"), undefined);
    for (const ignoreCache of [false, true]) {
      const state = {
        ignoreCache,
        profileId: "inert",
        legacyAstc: false,
        signing: false,
        astcPreset: "thorough",
        skipResources: false,
        inputMode: "packages",
      };
      const jobOptions = vm.runInNewContext(snapshot, state);
      assert.equal(Object.isFrozen(jobOptions), true);
      state.ignoreCache = !ignoreCache;
      assert.equal(vm.runInNewContext(forwarded, { jobOptions }), ignoreCache);
    }
  });

  test("cache help clearly separates manual private-cache cleanup from files the user must retain", () => {
    for (const key of [
      "dmWebIgnoreCache",
      "dmWebIgnoreCacheHelp",
      "dmWebIgnoreCacheActive",
      "dmWebFaqCacheQuestion",
      "dmWebFaqCacheAnswer",
    ])
      assert.ok(catalog[key]?.length === 2 && catalog[key].every((text) => text.length > 0), key);
    assert.match(
      catalog.dmWebIgnoreCacheHelp[0],
      /not deleted.*may update.*public runtime downloads/,
    );
    for (const text of catalog.dmWebFaqCacheAnswer) {
      assert.match(text, /D2R-cache/);
      assert.match(text, /D2R-resources-\*/);
      assert.match(text, /Legacy Device Pack/);
      assert.match(text, /data/);
    }
    assert.match(
      catalog.dmWebFaqCacheAnswer[0],
      /stop generation and close other packager tabs.*delete only the D2R-cache folder inside your selected game folder/,
    );
    assert.match(
      catalog.dmWebFaqCacheAnswer[0],
      /Do not delete.*original game files, keys, or saves/,
    );
    assert.match(
      catalog.dmWebFaqCacheAnswer[1],
      /先停止生成并关闭其他打包页面.*仅删除所选游戏文件夹内的 D2R-cache/,
    );
    assert.match(catalog.dmWebFaqCacheAnswer[1], /不要删除.*原始游戏文件、密钥或存档/);
  });

  test("cache unavailability is a fixed localized notice and never an overall progress milestone", () => {
    const notice = packagingActivity({
      phase: "cache",
      status: "unavailable",
      message: "untrusted raw filesystem text",
      path: "untrusted-path",
    });
    assert.ok(catalog.dmWebCacheUnavailable.includes(notice));
    assert.doesNotMatch(notice, /untrusted/);
    assert.equal(
      packagingActivity({
        phase: "cache",
        status: "untrusted-status",
        stage: "assets",
        state: "complete",
      }),
      null,
    );
    const handler = nodes.find(
      (node) => ts.isJsxAttribute(node) && node.name.getText(parsed) === "onProgress",
    );
    // 会话进度是对象回调，不是 DOM 处理器。
    // Session progress is an object callback, not a DOM handler.
    assert.equal(handler, undefined);
    const callback = nodes.find(
      (node) =>
        ts.isPropertyAssignment(node) &&
        node.name.getText(parsed) === "onProgress" &&
        node.initializer.getText(parsed).includes("acceptingProgress"),
    ).initializer;
    const cacheBranch = callback.body.statements.find(
      (node) =>
        ts.isIfStatement(node) && node.expression.getText(parsed) === 'event.phase === "cache"',
    );
    assert.match(cacheBranch.getText(parsed), /appendActivity\(cacheLabel, true\)/);
    assert.match(cacheBranch.getText(parsed), /return;/);
    assert.doesNotMatch(
      cacheBranch.getText(parsed),
      /tracker.observe|tracker.generated|publishProgress/,
    );
  });
}

// ui-operation-gate
{
  // 合成异步交错测试，不模拟 React 渲染或真实浏览器事件。
  // Synthetic async interleavings, not React-rendering or real-browser event proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { createUiOperationGate } = uiOperationGateModule;

  test("synchronous duplicate starts cannot replace the active cancellation controller", () => {
    const gate = createUiOperationGate(),
      first = gate.begin();
    assert.equal(gate.begin(), null);
    assert.equal(first.isCurrent(), true);
    assert.equal(gate.busy, true);
    first.controller.abort();
    assert.equal(gate.begin(), null);
    first.finish();
    assert.equal(gate.busy, false);
    assert.ok(gate.begin());
  });
  test("late completion from an unmounted operation cannot clear a replacement lease", async () => {
    const gate = createUiOperationGate(),
      old = gate.begin();
    let resolve;
    const pending = new Promise((done) => {
      resolve = done;
    });
    let status = "new operation";
    const task = (async () => {
      await pending;
      if (old.isCurrent()) status = "stale result";
      old.finish();
    })();
    gate.cancel();
    assert.equal(old.controller.signal.aborted, true);
    const replacement = gate.begin();
    resolve();
    await task;
    assert.equal(status, "new operation");
    assert.equal(replacement.isCurrent(), true);
    assert.equal(gate.busy, true);
    replacement.finish();
  });
  test("revocation precedes abort callbacks and repeat cancellation is harmless", () => {
    const gate = createUiOperationGate(),
      lease = gate.begin();
    lease.controller.signal.addEventListener("abort", () => assert.equal(lease.isCurrent(), false));
    gate.cancel();
    gate.cancel();
    lease.finish();
    assert.equal(gate.busy, false);
  });
  test("UI shares operation lease across folder, generation and signing retry handlers", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    assert.equal(
      (source.match(/const operation = operationGate.current.begin\(\)/g) ?? []).length,
      3,
    );
    assert.match(source, /operations\.cancel\(\)/);
    assert.match(source, /if \(!current\(\)\) return;\s+setSkipResources\(false\)/);
    assert.match(
      source,
      /if \(!current\(\)\) return;\s+acceptingProgress = false;\s+pendingGenerationStatus.current = null;\s+generationCompleted = true/,
    );
    assert.match(source, /if \(!current\(\)\) return;\s+const names = files\.nsps/);
  });

  test("stale option events cannot change configuration while a lease owns the UI", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    for (const anchor of [
      'id="ios-target"',
      "checked={legacyAstc}",
      'id="astc-preset"',
      "checked={skipResources}",
    ]) {
      const start = source.indexOf(anchor);
      assert.ok(start >= 0);
      const control = source.slice(start, source.indexOf("/>", start));
      const handler = control.match(
        /on(?:Change|CheckedChange)=\{\((?:event|checked)\) => \{([\s\S]*?)\}\}/,
      );
      assert.ok(handler, anchor);
      assert.match(handler[1], /operationGate\.current\.busy/);
    }
  });

  // 源码契约检查只约束稳定键和复制文本，不宣称实际 React 耗时。
  // Source-contract checks constrain stable keys and copied text, not actual React timing.
  test("bounded activity log keeps monotonic row identities and copies text only", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    assert.match(
      source,
      /activityRevision\.current \+= 1;[\s\S]*?activityBuffer\.current\.push\(\{\s*id: activityRevision\.current,/,
    );
    assert.match(
      source,
      /if \(activityBuffer\.current\.length > 200\) activityBuffer\.current\.shift\(\)/,
    );
    assert.match(source, /<li key=\{line\.id\}>\{line\.text\}<\/li>/);
    assert.match(source, /activityBuffer\.current\.map\(\(line\) => line\.text\)\.join\("\\n"\)/);
    assert.match(source, /setTimeout\(flushActivity, 250\)/);
    assert.doesNotMatch(source, /activityRevision\.current\s*=\s*0/);
  });

  // 检查真实处理器的关闭条件与等待顺序；不声称已运行浏览器目录选取器。
  // Check the actual handler's close condition and await ordering, not a real directory-picker run.
  test("terminal folder cancellation releases the captured packager before clearing busy state", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const choose = source.slice(
      source.indexOf("async function choose()"),
      source.indexOf("function prepareDownload"),
    );
    assert.match(choose, /const jobPackager = packager\.current/);
    assert.match(
      choose,
      /if \(controller\.signal\.aborted && current\(\)\)\s*\{[\s\S]*?await jobPackager\?\.close\(\)/,
    );
    const cleanup = choose.indexOf("await jobPackager?.close()");
    assert.ok(cleanup > 0);
    assert.match(choose.slice(cleanup), /if \(current\(\)\)\s*\{\s*setBusy\(false\)/);
  });
}
