/** 固定源版本的原生覆盖层编排；不使用 Python、文件系统或 fetch。
 * Source-pinned native overlay orchestration. No Python, filesystem, or fetch. */
import closeImage from "./close-image.json" with { type: "json" };
import {
  createTouchLayouts,
  padInvRw,
  padPause,
  padBossPct,
  padBliss,
  padRibbon,
  padMainmenu,
  padSettings,
  padCustomPage,
  setRect,
  riftPanel,
  CLASS_STRIPS,
  PAD_VENDOR,
} from "./touch-layout.mjs";
import {
  mercenaryTransform,
  invLayout,
  bankLayout,
  overlayLayout,
  patchInventory,
  addLight,
  variantPath,
  pythonJsonDumps,
} from "./inventory.mjs";
import { createAnnotations } from "./annotations.mjs";
import { PythonFloat } from "../art-recipes-js/native-art-util.mjs";
import {
  RELEASE_OVERLAY_REVISION,
  UPSTREAM_OVERLAY_REVISION,
  requireOverlayRevision,
} from "./revision.mjs";

const MiB = 1024 * 1024;
const LAY = "data/global/ui/layouts/",
  UI = "data/hd/global/ui/";
const INV = "controller/console/switch/playerinventoryexpansionlayouthd.json";
const MERC = "controller/hirelinginventorypanelhd.json";
const PLAYER = "controller/playerinventoryoriginallayouthd.json";
const BANK = "controller/bankexpansionlayouthd.json",
  OVERLAY = "controller/controlleroverlayhd.json";
const TDIR = [
  ["controller/console/switch/", "0touch_console_switch_lay/"],
  ["controller/console/", "ztouch_console_lay/"],
  ["controller/", "xtouch_lay/"],
];
const SHARED = [
  "chroniclebaselistpanelhd.json",
  "chroniclepanelhd.json",
  "controller/_profileconsolehd.json",
  "controller/_profilehandheldhd.json",
  "dropdownlistcontentshd.json",
];
const UNSHARE = [
  "confirmationmodalhd.json",
  "dismissablemodalhd.json",
  "helpbuttonpanelhd.json",
  "npcdialogpanelhd.json",
  "youdiedmodalhd.json",
];
const PAD_PAGES = [
  "controller/bankexpansionlayouthd.json",
  "controller/console/charactercreatepanelhd.json",
  "controller/console/characteroptionspanelhd.json",
  "controller/console/mainmenubuttonribbonhd.json",
  "controller/console/mainmenupanelhd.json",
  "controller/console/settingspanelhd.json",
  "controller/console/switch/playerinventoryexpansionlayouthd.json",
  "controller/console/videooptionspanelhd.json",
  "controller/controlleroverlayhd.json",
  "controller/hirelinginventorypanelhd.json",
  "controller/hudmonsterhealthhd.json",
  "controller/messagelogpanelhd.json",
  "controller/notificationpanelhd.json",
  "controller/pauselayoutgardenhd.json",
  "controller/pauselayouthd.json",
  "controller/vendorpanellayouthd.json",
];
const STASH_NAMES = [
  "StashPanelBG",
  "StashPanelBGPersonal",
  "StashPanelBGShared",
  "AdvancedStash_Gems_Console",
  "AdvancedStash_Consumables_Console",
  "AdvancedStash_Runes_Console",
];
export const GENERATED = Object.freeze([
  "NativeHUD/v4/base.png",
  "NativeHUD/v4/hud.json",
  "NativeHUD/v4/hud_v4_ui/ui.json",
  "NativeHUD/touch/fe_rightpanel.sprite",
  "NativeHUD/mercslots/HirelingInventoryBG.sprite",
  "NativeHUD/pouch/PouchBox.sprite",
  "NativeHUD/widestash/MainPanelBG_Wide.sprite",
  "NativeHUD/widestash/VaultBottom.sprite",
  ...STASH_NAMES.map((name) => `NativeHUD/widestash/${name}_wide.sprite`),
]);
const utf8 = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const decode = (raw) => decoder.decode(raw);
const textFile = (raw) => decode(raw).replace(/\r\n|\r/g, "\n");
const utf8sig = (raw) => decode(raw).replace(/^\ufeff/, "");
const requireValue = (test, code) => {
  if (!test) throw new Error(code);
};
const object = (x) => x !== null && typeof x === "object" && !Array.isArray(x);
const exactKeys = (x, keys) =>
  object(x) && Object.keys(x).length === keys.length && keys.every((k) => Object.hasOwn(x, k));
const sorted = (values) => [...values].sort();
const equalSet = (a, b) => JSON.stringify(sorted(a)) === JSON.stringify(sorted(b));
const update = (target, entries) => {
  for (const [key, value] of entries) target.set(key, value);
};
const escapeRegex = (value) => value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const count = (text, part) => text.split(part).length - 1;
const round = (value) => {
  const floor = Math.floor(value),
    fraction = value - floor;
  return fraction < 0.5 ? floor : fraction > 0.5 ? floor + 1 : floor + (floor % 2 === 0 ? 0 : 1);
};
function b64(text) {
  requireValue(
    typeof text === "string" && text.length % 4 === 0 && !/[^A-Za-z0-9+/=]/.test(text),
    "OVERLAY_BASE64",
  );
  const pad = text.indexOf("=");
  requireValue(
    pad < 0 || (pad >= text.length - 2 && (text.length - pad === 1 || text.endsWith("=="))),
    "OVERLAY_BASE64",
  );
  let raw;
  try {
    raw = atob(text);
  } catch {
    throw new Error("OVERLAY_BASE64");
  }
  // 直接填充有界输出，避免字符串迭代器展开为逐字符临时列表。
  // Fill the bounded output directly, without expanding a string iterator into a character list.
  const bytes = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
  return bytes;
}
export function logical(path) {
  requireValue(typeof path === "string", "OVERLAY_PATH");
  path = path.toLowerCase().replaceAll("\\", "/");
  if (path.includes(":")) path = path.slice(path.indexOf(":") + 1);
  requireValue(
    path.startsWith("data/") &&
      path.length <= 8192 &&
      path.split("/").every((p) => p && p !== "." && p !== ".."),
    "OVERLAY_PATH",
  );
  // 在规范化之后检查码点，保留前缀剥除语义且不展开长路径。
  // Count after normalization to preserve prefix stripping without expanding long paths.
  let count = 0;
  for (const char of path)
    requireValue(++count <= 4096 && char.codePointAt(0) >= 32, "OVERLAY_PATH");
  return path;
}
function decodeMap(value, limit) {
  requireValue(object(value) && Object.keys(value).length <= 8192, "OVERLAY_FILE_COUNT");
  const output = new Map();
  let size = 0;
  for (const [key, encoded] of Object.entries(value)) {
    requireValue(typeof encoded === "string" && encoded.length <= 24 * MiB, "OVERLAY_FILE_LIMIT");
    const raw = b64(encoded);
    size += raw.length;
    requireValue(size <= limit && raw.length <= 16 * MiB, "OVERLAY_BYTES_LIMIT");
    output.set(key, raw);
  }
  return output;
}
export function touchPath(path) {
  if (path.startsWith(UI + "controller/controllerbuttons/")) {
    const rest = path.slice((UI + "controller/controllerbuttons/").length);
    return count(rest, "/") === 1
      ? UI + "ztouch_controller_buttons_ui/" + rest.replaceAll("/", "_")
      : null;
  }
  if (
    path.startsWith(UI + "controller/cursor/") &&
    !path.slice((UI + "controller/cursor/").length).includes("/")
  )
    return UI + "xtouch_cursor_lay/" + path.slice((UI + "controller/cursor/").length);
  if (!path.startsWith(LAY) || path.includes("/_profile")) return null;
  const rest = path.slice(LAY.length);
  for (const [from, to] of TDIR)
    if (rest.startsWith(from)) {
      const name = rest.slice(from.length);
      return name.includes("/") ? null : LAY + to + name;
    }
  return null;
}
/** 从原生清单选择 Xbox/PS 图标，保留原始遍历顺序与 PS 回落次序。
 * Select native Xbox/PS glyphs, preserving manifest order and PS fallback order. */
export function controllerGlyphs(rows, readAsset, { playstation = true } = {}) {
  const output = new Map(),
    buttons = UI + "controller/controllerbuttons/",
    target = UI + "ztouch_controller_buttons_ui/",
    have = new Set(rows.map((row) => logical(row[9])));
  for (const row of rows) {
    if (!row[9].toLowerCase().includes("/ui/controller/controllerbuttons/switch/")) continue;
    const name = row[9].split("/").at(-1);
    const source =
      { "ButtonRight.sprite": "ButtonBottom.sprite", "ButtonBottom.sprite": "ButtonRight.sprite" }[
        name
      ] || name;
    output.set(target + "switchx" + name.toLowerCase(), readAsset(buttons + "xbox/" + source));
  }
  if (!playstation) return output;
  const face = {
    buttonright: "buttonbottom",
    buttonbottom: "buttonright",
    buttontop: "buttonleft",
    buttonleft: "buttontop",
  };
  for (const name of sorted(
    [...have]
      .filter((path) => path.startsWith(buttons + "switch/"))
      .map((path) => path.slice((buttons + "switch/").length)),
  )) {
    const stem = name.slice(0, -7),
      source = (face[stem] || stem) + ".sprite";
    const path = [
      buttons + "ps5/" + source,
      buttons + "ps5/" + source.replaceAll("rectangle", ""),
      buttons + "switch/" + name,
    ].find((path) => have.has(path));
    requireValue(path, "OVERLAY_PS_GLYPH_SOURCE");
    output.set(target + "switchp" + name, readAsset(path));
  }
  return output;
}
/** 保留插入顺序及原始控制器/共享布局集的两项门禁。
 * Preserve insertion order and both original controller/shared layout-set gates. */
export function relocateLayouts(
  replacements,
  { manifestPaths, original, readAsset, pad, padRoot },
) {
  const output = new Map();
  for (const [key, value] of replacements) output.set(touchPath(key) || key, value);
  for (const path of original) {
    const target = touchPath(path);
    if (!output.has(target)) output.set(target, readAsset(path));
  }
  update(output, pad);
  const rootKeys = new Set([...padRoot.keys()].map((path) => path.slice(LAY.length)));
  for (const name of [...UNSHARE, ...sorted(rootKeys)]) {
    const key = LAY + name,
      modified = output.get(key);
    requireValue(output.has(key), "OVERLAY_UNSHARE_SOURCE:" + name);
    output.delete(key);
    const pattern = new RegExp(`("basedOn"\\s*:\\s*")(${escapeRegex(name)})(")`, "gi");
    const inherited = [];
    for (const [path, value] of output) {
      if (!TDIR.some(([, dir]) => path.startsWith(LAY + dir))) continue;
      let text;
      try {
        text = textFile(value);
      } catch {
        continue;
      }
      pattern.lastIndex = 0;
      if (pattern.test(text)) inherited.push([path, text]);
    }
    if (inherited.length) {
      output.set(LAY + "1touchbase_lay/" + name, modified);
      for (const [path, text] of inherited)
        output.set(
          path,
          utf8.encode(
            text.replace(
              pattern,
              (_, before, value, after) => before + "1touchbase_lay/" + value + after,
            ),
          ),
        );
    } else {
      requireValue(!output.has(LAY + "xtouch_lay/" + name), "OVERLAY_UNSHARE_COLLISION");
      output.set(LAY + "xtouch_lay/" + name, modified);
    }
    if (rootKeys.has(name)) output.set(key, padRoot.get(key));
  }
  const shared = [...output.keys()]
    .filter(
      (path) =>
        path.startsWith(LAY) &&
        manifestPaths.has(path) &&
        !pad.has(path) &&
        !rootKeys.has(path.slice(LAY.length)) &&
        !TDIR.some(([, dir]) => path.startsWith(LAY + dir)),
    )
    .map((path) => path.slice(LAY.length));
  requireValue(equalSet(shared, SHARED), "OVERLAY_SHARED_LAYOUT_SET");
  return output;
}
export function rawSprite(image) {
  const { width, height, data } = image;
  requireValue(
    Number.isInteger(width) &&
      width > 0 &&
      width <= 65535 &&
      Number.isInteger(height) &&
      height > 0 &&
      width * height <= 4194304 &&
      data instanceof Uint8Array &&
      data.length === width * height * 4,
    "OVERLAY_HUD_IMAGE_LIMIT",
  );
  const result = new Uint8Array(40 + data.length),
    view = new DataView(result.buffer);
  result.set(utf8.encode("SpA1"));
  view.setUint16(4, 31, true);
  view.setUint16(6, width, true);
  view.setUint32(8, width, true);
  view.setUint32(12, height, true);
  view.setUint32(20, 1, true);
  view.setUint32(32, data.length, true);
  view.setUint32(36, 4, true);
  result.set(data, 40);
  return result;
}
function notification(raw) {
  const ui = JSON.parse(utf8sig(raw)),
    [refW, refH] = ui.ref,
    scale = 2160 / refH;
  const x0 = ui.slots[0][0] - 1,
    x1 = ui.slots.at(-1)[2] + 1;
  const width = round((x1 - x0) * scale),
    x = round(((x0 + x1) / 2 - refW / 2) * scale) - 1;
  const f = (n) => new PythonFloat(n);
  return utf8.encode(
    pythonJsonDumps(
      {
        type: "NotificationPanel",
        name: "NotificationPanel",
        fields: {
          priority: 9002,
          notificationSeconds: f(5),
          transitionSeconds: f(0.8),
          rect: { x, y: -790, width, height: 244 },
          anchor: { x: f(0.5), y: f(1) },
        },
        children: [
          {
            type: "NineTileImageWidget",
            name: "NotificationBorder",
            fields: {
              rect: { x: 0, y: 0, width, height: 244 },
              anchor: { x: f(-0.5), y: f(0) },
              filename: "PANEL/TooltipBorder/TooltipBorder",
              backgroundColor: [f(0), f(0), f(0), f(0.7)],
              tileScale: 1,
            },
            children: [
              {
                type: "TextBoxWidget",
                name: "Prompt",
                fields: { fitToParent: true, style: "$StyleNotification" },
              },
            ],
          },
        ],
      },
      { indent: 1 },
    ),
  );
}
export function createOverlayRecipe({ png, md5, sha1 } = {}) {
  requireValue(
    typeof png?.decodeRgba === "function" &&
      typeof md5 === "function" &&
      typeof sha1 === "function",
    "OVERLAY_BACKEND_REQUIRED",
  );
  return Object.freeze({
    async execute(operation, payload, options = {}) {
      requireValue(operation === "overlay-recipe", "OVERLAY_RECIPE_OPERATION");
      requireValue(
        payload instanceof Uint8Array &&
          payload.length <= 144 * MiB &&
          (exactKeys(options, []) || exactKeys(options, ["revision"])),
        "OVERLAY_RECIPE_OPTIONS",
      );
      const revision = Object.hasOwn(options, "revision")
        ? options.revision
        : RELEASE_OVERLAY_REVISION;
      requireOverlayRevision(revision);
      const playstation = revision === UPSTREAM_OVERLAY_REVISION;
      const input = JSON.parse(utf8sig(payload));
      requireValue(exactKeys(input, ["manifest", "assets", "generated"]), "OVERLAY_RECIPE_INPUT");
      requireValue(
        typeof input.manifest === "string" && utf8.encode(input.manifest).length <= 16 * MiB,
        "OVERLAY_MANIFEST_LIMIT",
      );
      const assets = decodeMap(input.assets, 96 * MiB),
        generated = decodeMap(input.generated, 48 * MiB);
      requireValue(
        [...assets.keys()].every((path) => logical(path) === path),
        "OVERLAY_ASSET_PATH",
      );
      requireValue(equalSet(generated.keys(), GENERATED), "OVERLAY_GENERATED_ART_REQUIRED");
      const hud = png.decodeRgba(generated.get("NativeHUD/v4/base.png"));
      requireValue(hud.width * hud.height <= 4194304, "OVERLAY_HUD_IMAGE_LIMIT");
      const rows = [];
      // 原生 str.splitlines 会识别这些控制分隔符。
      // Native str.splitlines recognizes these control separators.
      // eslint-disable-next-line no-control-regex
      const lines = input.manifest.split(/\r\n|[\n\r\v\f\u001c-\u001e\u0085\u2028\u2029]/);
      if (lines.at(-1) === "") lines.pop();
      if (input.manifest === "") lines.length = 0;
      for (const line of lines) {
        const row = line.split("|");
        requireValue(row.length === 11, "OVERLAY_MANIFEST_ROW");
        logical(row[9]);
        rows.push(row);
      }
      requireValue(rows.length <= 262144, "OVERLAY_MANIFEST_ROWS");
      const hashes = new Map();
      for (const [key, raw] of assets) hashes.set(key, await md5(raw));
      const firstRows = new Map();
      for (const row of rows) {
        const key = logical(row[9]);
        if (!firstRows.has(key)) firstRows.set(key, row);
      }
      const readAsset = (path) => {
        const key = logical(path.toLowerCase().startsWith("data/") ? path : "data/" + path);
        requireValue(assets.has(key), "OVERLAY_MISSING_AUTHENTICATED_ASSET:" + key);
        if (firstRows.has(key))
          requireValue(hashes.get(key) === firstRows.get(key)[4], "OVERLAY_CKEY_MISMATCH");
        return assets.get(key);
      };
      const readEntry = (row) => {
        const key = logical(row[9]),
          raw = readAsset(key);
        requireValue(hashes.get(key) === row[4], "OVERLAY_CKEY_MISMATCH");
        return raw;
      };
      const entry = (suffix) => {
        const row = rows.find((row) => row[9].toLowerCase().endsWith(suffix.toLowerCase()));
        requireValue(row, "OVERLAY_MISSING_LAYOUT:" + suffix);
        return [row[9].slice(row[9].indexOf(":") + 1), readEntry(row)];
      };
      const readLayout = (rel, suffix = null) => {
        const [path, raw] = entry(suffix || "/ui/layouts/" + rel);
        return [path, utf8sig(raw)];
      };
      const touch = createTouchLayouts({ readLayout, readEntry, rows });
      const repl = new Map([
        [UI + "panel/hud_02/bugbtn.sprite", rawSprite(hud)],
        [UI + "panel/hud_02/front_panel.sprite", rawSprite(png.decodeRgba(b64(closeImage.base64)))],
        [LAY + "controller/hudpanelhd.json", generated.get("NativeHUD/v4/hud.json")],
        [
          LAY + "controller/notificationpanelhd.json",
          notification(generated.get("NativeHUD/v4/hud_v4_ui/ui.json")),
        ],
      ]);
      const sourceText = (current, rel) => {
        const [path, original] = readLayout(rel);
        return [
          path.toLowerCase(),
          current.has(path.toLowerCase()) ? textFile(current.get(path.toLowerCase())) : original,
        ];
      };
      const applyLayout = (current, rel, transform) => {
        const [path, text] = sourceText(current, rel);
        current.set(path, utf8.encode(transform(text)));
      };
      const mercenary = (current, pad = false) => {
        if (!pad)
          current.set(
            UI + "controller/panel/hirelinginventory/v2/hirelinginventorybg.sprite",
            generated.get("NativeHUD/mercslots/HirelingInventoryBG.sprite"),
          );
        applyLayout(current, MERC, (text) => mercenaryTransform(text, readLayout(PLAYER)[1]));
      };
      const pouch = (current) => {
        current.set(
          "+" + UI + "controller/panel/zspouch/pouchbox.sprite",
          generated.get("NativeHUD/pouch/PouchBox.sprite"),
        );
        applyLayout(current, INV, invLayout);
      };
      const wide = (current) => {
        for (const sub of ["", "base/"]) {
          const [path, raw] = entry("/global/excel/" + sub + "inventory.bin");
          current.set(path.toLowerCase(), patchInventory(raw, path.toLowerCase()));
        }
        for (const name of STASH_NAMES) {
          const folder = name.startsWith("Advanced") ? "additionalstash/" : "";
          current.set(
            UI + "controller/panel/stash/v2/" + folder + name.toLowerCase() + ".sprite",
            generated.get(`NativeHUD/widestash/${name}_wide.sprite`),
          );
        }
        current.set(
          "+" + UI + "controller/panel/zswide/mainpanelbg_wide.sprite",
          generated.get("NativeHUD/widestash/MainPanelBG_Wide.sprite"),
        );
        current.set(
          "+" + UI + "controller/panel/zswide/vaultbottom.sprite",
          generated.get("NativeHUD/widestash/VaultBottom.sprite"),
        );
        applyLayout(current, BANK, bankLayout);
        applyLayout(current, OVERLAY, overlayLayout);
      };
      update(repl, touch.build());
      update(repl, touch.buildPcfront());
      update(repl, touch.buildPresent());
      update(repl, touch.buildIngame());
      mercenary(repl);
      update(repl, touch.buildCursor());
      update(repl, touch.buildGlyphs());
      update(repl, controllerGlyphs(rows, readAsset, { playstation }));
      update(repl, touch.buildProfile());
      const annotations = createAnnotations({
        readAsset,
        readReplacement: (path) => repl.get(path),
        sha1,
        release: revision === RELEASE_OVERLAY_REVISION,
      });
      const strings = await annotations.buildTouchStrings();
      update(repl, strings.replacements);
      update(repl, (await annotations.buildI18n()).replacements);
      const manifestPaths = new Set(
        rows.map((row) => row[9].toLowerCase().slice(row[9].indexOf(":") + 1)),
      );
      const modelAssets = new Set();
      for (const table of ["items.json", "uniques.json", "sets.json"])
        for (const row of JSON.parse(readLayout(null, "/hd/items/" + table)[1])) {
          requireValue(object(row) && Object.keys(row).length === 1, "OVERLAY_DROP_TABLE");
          const value = Object.values(row)[0];
          for (const asset of Object.hasOwn(value, "asset")
            ? [value.asset]
            : [value.normal, value.uber, value.ultra])
            modelAssets.add(asset.toLowerCase());
        }
      for (const asset of sorted(modelAssets)) {
        const root = ["weapon", "armor", "misc"].find((root) =>
          manifestPaths.has(`data/hd/items/${root}/${asset}.json`),
        );
        if (
          !root ||
          (root === "misc" &&
            !["rune/", "ring/", "amulet/", "charm/", "key/", "quest/"].some((prefix) =>
              asset.startsWith(prefix),
            ))
        )
          continue;
        const path = `data/hd/items/${root}/${asset}.json`;
        const text = repl.has(path)
          ? textFile(repl.get(path))
          : readLayout(null, "/" + path.slice(5))[1];
        for (const [tier, fx] of [
          ["1", "data/hd/vfx/particles/overlays/object/horadric_light/fx_horadric_light.particles"],
          [
            "2",
            "data/hd/vfx/particles/missiles/diablo_light_shaft/fx_diablo_light_shaft.particles",
          ],
        ]) {
          const dst = variantPath(root, asset, tier),
            digest = await md5(utf8.encode("droplight:" + dst));
          repl.set("+" + dst, utf8.encode(addLight(text, fx, dst, { md5: () => digest })));
        }
      }
      pouch(repl);
      wide(repl);
      const pr = new Map();
      applyLayout(pr, INV, padInvRw);
      for (const [rel, garden] of [
        ["controller/pauselayoutgardenhd.json", 1],
        ["controller/pauselayouthd.json", 0],
      ])
        applyLayout(pr, rel, (text) => padPause(text, garden, true));
      applyLayout(pr, "controller/notificationpanelhd.json", (text) => {
        requireValue(count(text, '"y": -500') === 1, "OVERLAY_PAD_NOTIFICATION");
        return text.replace('"y": -500', '"y": -590');
      });
      applyLayout(pr, "controller/messagelogpanelhd.json", (text) => riftPanel(text)[0]);
      mercenary(pr, true);
      applyLayout(pr, "controller/console/charactercreatepanelhd.json", (text) => {
        for (const [name, x0, x1] of CLASS_STRIPS)
          text = setRect(text, name, x0, 450, x1 - x0, 1300);
        return text;
      });
      update(pr, touch.buildIngame(PAD_VENDOR, false));
      for (const [rel, fn] of [
        ["controller/console/mainmenupanelhd.json", padMainmenu],
        ["controller/console/mainmenubuttonribbonhd.json", padRibbon],
        ["controller/hudmonsterhealthhd.json", padBossPct],
        ["controller/console/characteroptionspanelhd.json", padBliss],
        ["controller/console/settingspanelhd.json", padSettings],
        ["controller/console/videooptionspanelhd.json", (text) => padCustomPage(text)],
      ])
        applyLayout(pr, rel, fn);
      pouch(pr);
      wide(pr);
      repl.set(
        UI + "frontend/hd/final/frontend_rightpanel.sprite",
        generated.get("NativeHUD/touch/fe_rightpanel.sprite"),
      );
      const original = sorted(
        [...manifestPaths].filter((path) => touchPath(path) && path.startsWith(LAY)),
      );
      const padRoot = touch.buildPadRoot(),
        pad = new Map([...pr].filter(([key]) => touchPath(key)));
      requireValue(
        equalSet(
          [...pad.keys()].map((path) => path.slice(LAY.length)),
          PAD_PAGES,
        ),
        "OVERLAY_PAD_LAYOUT_SET",
      );
      const files = {},
        variants = [];
      let outputSize = 0,
        fileCount = 0;
      const addFile = (raw) => {
        requireValue(
          raw instanceof Uint8Array && raw.length <= 16 * MiB,
          "OVERLAY_REPLACEMENT_FILE",
        );
        outputSize += raw.length;
        requireValue(outputSize <= 128 * MiB, "OVERLAY_OUTPUT_LIMIT");
        const key = "file-" + fileCount++;
        files[key] = raw.slice();
        return key;
      };
      const capture = (name, replacements) => {
        const transformed = relocateLayouts(replacements, {
          manifestPaths,
          original,
          readAsset,
          pad,
          padRoot,
        });
        requireValue(transformed.size <= 4096, "OVERLAY_REPLACEMENT_COUNT");
        const inventory = [];
        for (const [rawPath, raw] of transformed) {
          const path = logical(rawPath.startsWith("+") ? rawPath.slice(1) : rawPath);
          inventory.push({ path, file: addFile(raw), add: !firstRows.has(path) });
        }
        const variant = { name, replacements: inventory, sidecars: [] };
        variants.push(variant);
        return variant;
      };
      const base = capture("base", repl);
      const notes = await annotations.buildItemNotes(),
        changed = new Map(repl);
      update(changed, notes.replacements);
      const item = capture("item_notes", changed);
      const ctrl = utf8.encode(
        "触屏版布局在 layouts/xtouch_lay、ztouch_console_lay、0touch_console_switch_lay（dm_padmode.c 见到本文件才改写前缀）\n",
      );
      for (const [variant, sidecars] of [
        [
          base,
          [
            ["ctrl_touch", ctrl],
            ["ps_glyphs", utf8.encode("ztouch_controller_buttons_ui/switchp*.sprite\n")],
            ["touch_strings.tsv", strings.touchStrings],
            ["xbox_glyphs", utf8.encode("ztouch_controller_buttons_ui/switchx*.sprite\n")],
          ],
        ],
        [
          item,
          [
            ["ctrl_touch", ctrl],
            ["dm_textbox_plain.tsv", notes.textbox],
            [
              "features.txt",
              utf8.encode(
                "item_notes\n" +
                  sorted(notes.replacements.keys())
                    .map((path) => path + "\n")
                    .join(""),
              ),
            ],
            ["ps_glyphs", utf8.encode("ztouch_controller_buttons_ui/switchp*.sprite\n")],
            ["touch_strings.tsv", strings.touchStrings],
            ["xbox_glyphs", utf8.encode("ztouch_controller_buttons_ui/switchx*.sprite\n")],
          ],
        ],
      ])
        for (const [path, raw] of sidecars) {
          if (!playstation && path === "ps_glyphs") continue;
          variant.sidecars.push({ path, file: addFile(raw) });
        }
      return { files, variants };
    },
  });
}
