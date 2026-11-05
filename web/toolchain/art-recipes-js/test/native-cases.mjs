// 原生图集的纯合成案例：覆盖原始生成器的完整输出、CSV 语义与拒绝边界，无游戏内容。
// Wholly synthetic native-art cases covering complete original-generator output, CSV semantics, and rejection boundaries; no game content.
import { createHash } from "node:crypto";

const utf8 = (text) => new TextEncoder().encode(text);
const b64 = (bytes) => Buffer.from(bytes).toString("base64");
const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");
const md5 = (bytes) => createHash("md5").update(bytes).digest("hex");
export const nativeHashDependencies = { sha256, md5 };

export async function createNativeArtCases(codec) {
  const raw = {
    "global/excel/skills.txt": utf8(
      '*Id\tcharclass\tskilldesc\n12\tama\t"first\nline\twith ""quote"""\n\n21\tbad\tmissing\n357\t\tcommon\n9007199254740993\tsor\thuge\n+٣_٧\tpal\tunicode\n374\twar\tendbad\n358\t\tcommon\n',
    ),
    "global/excel/skilldesc.txt": utf8(
      'skilldesc\tIconCel\n"first\nline\twith ""quote"""\t0\ncommon\t2\nhuge\t0\nhuge\t1\nunicode\t٣\nendbad\t39\n',
    ),
  };
  const assets = [];
  const rgba = {};
  for (let index = 0; index < 10; index++) {
    const name = index < 9 ? `dm_native_skill_${index}.png` : "dm_native_authored.png";
    const path = `hd/synthetic/atlas${index}.sprite`;
    const width = 80,
      height = 3;
    const pixels = Uint8Array.from(
      { length: width * height * 4 },
      (_, offset) => (offset * 13 + index * 17) % 256,
    );
    raw[path] = Uint8Array.from([...utf8("authored-atlas-"), index]);
    rgba[name] = b64(pixels);
    assets.push({
      asset: name,
      official_path: "data:data/" + path,
      official_sprite_sha256: sha256(raw[path]),
      official_sprite_md5: md5(raw[path]),
      decoded_png_sha256: sha256(await codec.encodeRgba(pixels, width, height)),
      decoded_rgba_sha256: sha256(pixels),
      atlas_px: [width, height],
      frame_width: 1.9,
      frame_pitch: 2,
      frame_count: 40,
    });
  }
  const canonicalRows =
    "[[12,0,0],[21,-1,-1],[357,8,20],[9007199254740993,1,1],[37,3,3],[374,-1,-1],[358,8,22]]";
  const manifest = {
    assets,
    official_tables: ["skills.txt", "skilldesc.txt"].map((file) => ({
      file,
      sha256: sha256(raw["global/excel/" + file]),
    })),
    descriptor_index_sha256: sha256(utf8(canonicalRows)),
  };
  const bundle = {
    metadata: {
      manifest: JSON.stringify(manifest),
      geometry: "{}\r\n",
      binding: "{}\n",
      implementation:
        "/* 纯合成实现。 | Wholly synthetic implementation. */\r\nint authored;\r\n/* 结束。 | End. */\r",
    },
    raw: Object.fromEntries(Object.entries(raw).map(([path, bytes]) => [path, b64(bytes)])),
    rgba,
  };
  function testCase(label, mutate = () => {}, rejects = false, alterOptions = () => {}) {
    const copy = structuredClone(bundle);
    mutate(copy);
    const options = {
      metadataSha256: Object.fromEntries(
        Object.entries(copy.metadata).map(([name, text]) => [name, sha256(utf8(text))]),
      ),
    };
    alterOptions(options);
    return { label, payload: utf8(JSON.stringify(copy)), options, rejects };
  }
  const changeManifest = (bundle, mutate) => {
    const manifest = JSON.parse(bundle.metadata.manifest);
    mutate(manifest);
    bundle.metadata.manifest = JSON.stringify(manifest);
  };
  const changeTables = (bundle, skills, descriptions, rows) => {
    bundle.raw["global/excel/skills.txt"] = b64(utf8(skills));
    bundle.raw["global/excel/skilldesc.txt"] = b64(utf8(descriptions));
    changeManifest(bundle, (manifest) => {
      for (const table of manifest.official_tables)
        table.sha256 = sha256(Buffer.from(bundle.raw["global/excel/" + table.file], "base64"));
      manifest.descriptor_index_sha256 = sha256(utf8(rows));
    });
  };
  return [
    testCase("complete-atlases-multiline-unicode-large-id-universal-newline"),
    testCase("quoted-CRLF-and-duplicate-headers", (bundle) => {
      changeTables(
        bundle,
        '*Id\tcharclass\tskilldesc\n12\tama\t"first\r\nline"\r\n',
        'skilldesc\tIconCel\tIconCel\r\n"first\r\nline"\t39\t0\r\n',
        "[[12,0,0]]",
      );
    }),
    testCase("empty-descriptor-rows", (bundle) => {
      changeTables(bundle, "*Id\tcharclass\tskilldesc\n", "skilldesc\tIconCel\n", "[]");
    }),
    testCase(
      "missing-trust",
      () => {},
      true,
      (options) => {
        delete options.metadataSha256;
      },
    ),
    testCase(
      "metadata-hash",
      () => {},
      true,
      (options) => {
        options.metadataSha256.manifest = "0".repeat(64);
      },
    ),
    testCase(
      "float-dimensions",
      (bundle) => {
        bundle.metadata.manifest = bundle.metadata.manifest.replace(
          '"atlas_px":[80,3]',
          '"atlas_px":[80.0,3]',
        );
      },
      true,
    ),
    testCase(
      "huge-dimensions",
      (bundle) => {
        bundle.metadata.manifest = bundle.metadata.manifest.replace(
          '"atlas_px":[80,3]',
          '"atlas_px":[9007199254740993,3]',
        );
      },
      true,
    ),
    testCase(
      "source-sha256",
      (bundle) => {
        bundle.raw["hd/synthetic/atlas0.sprite"] = "Yg==";
      },
      true,
    ),
    testCase(
      "source-md5",
      (bundle) => {
        changeManifest(bundle, (manifest) => {
          manifest.assets[0].official_sprite_md5 = "0".repeat(32);
        });
      },
      true,
    ),
    testCase(
      "decoded-rgba-sha256",
      (bundle) => {
        changeManifest(bundle, (manifest) => {
          manifest.assets[0].decoded_rgba_sha256 = "0".repeat(64);
        });
      },
      true,
    ),
    testCase(
      "encoded-png-sha256",
      (bundle) => {
        changeManifest(bundle, (manifest) => {
          manifest.assets[0].decoded_png_sha256 = "0".repeat(64);
        });
      },
      true,
    ),
    testCase(
      "canonical-row-sha256",
      (bundle) => {
        changeManifest(bundle, (manifest) => {
          manifest.descriptor_index_sha256 = "0".repeat(64);
        });
      },
      true,
    ),
    testCase(
      "strict-base64",
      (bundle) => {
        bundle.raw["hd/synthetic/atlas0.sprite"] = "Yg===";
      },
      true,
    ),
    testCase(
      "rgba-size",
      (bundle) => {
        bundle.rgba["dm_native_skill_0.png"] = "";
      },
      true,
    ),
    testCase(
      "duplicate-skill-id",
      (bundle) => {
        changeTables(
          bundle,
          "*Id\tcharclass\tskilldesc\n12\tama\tfirst\n12\tama\tfirst\n",
          "skilldesc\tIconCel\nfirst\t0\n",
          "[[12,0,0],[12,0,0]]",
        );
      },
      true,
    ),
    testCase(
      "unquoted-lone-CR",
      (bundle) => {
        changeTables(
          bundle,
          "*Id\tcharclass\tskilldesc\r12\tama\tfirst\n",
          "skilldesc\tIconCel\nfirst\t0\n",
          "[[12,0,0]]",
        );
      },
      true,
    ),
  ];
}
