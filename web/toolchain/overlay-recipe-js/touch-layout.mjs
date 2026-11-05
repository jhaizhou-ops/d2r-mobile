// 固定源版本的原生触控布局配方；静态控件来自受信源码，而非游戏资源。
// 运行时读取由经过认证的覆盖层归档提供。
// Pinned native touch-layout recipe. Static widgets are generated from trusted source,
// never from game assets; runtime reads are supplied by the authenticated overlay archive.
import data from "./touch-data.json" with { type: "json" };
import { parseInventoryJson, pythonJsonDumps } from "./inventory.mjs";
import { PythonFloat } from "../art-recipes-js/native-art-util.mjs";

export const TOUCH_SOURCE_SHA256 = Object.freeze(data.sourceSha256);
export const CLASS_STRIPS = Object.freeze(data.CLASS_STRIPS);
export const PAD_VENDOR = Object.freeze(data.PAD_VENDOR);
const encoder = new TextEncoder();
const fail = (message) => {
  throw new Error("touch_layout: " + message);
};
const requireValue = (test, message) => {
  if (!test) fail(message);
};
const escape = (value) => value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const count = (s, literal) => s.split(literal).length - 1;
const endOf = (m) => m.index + m[0].length;
const trimSpaces = (s) => s.replace(/ +$/, "");
const search = (pattern, s, start = 0, end = s.length) => {
  const m = new RegExp(pattern, "d").exec(s.slice(start, end));
  if (m) {
    m.index += start;
    for (const span of m.indices)
      if (span) {
        span[0] += start;
        span[1] += start;
      }
  }
  return m;
};
const sub1 = (s, a, b) => {
  requireValue(count(s, a) === 1, "expected original literal: " + a);
  return s.replace(a, () => b);
};
const replaceSpan = (s, span, value) => s.slice(0, span[0]) + value + s.slice(span[1]);
const compose = (a, b) => (s) => {
  const r1 = a(s),
    r2 = b(r1[0]);
  return [r2[0], r1[1] + r2[1]];
};
const mustCount = (result, expected, name) => {
  requireValue(result[1] === expected, name + " count changed");
  return result;
};

export function blockEnd(s, i) {
  let depth = 0,
    quoted = false;
  for (; i < s.length; i++) {
    const c = s[i];
    if (quoted) {
      if (c === "\\") i++;
      else if (c === '"') quoted = false;
    } else if (c === '"') quoted = true;
    else if (s.slice(i, i + 2) === "//") {
      i = s.indexOf("\n", i);
      requireValue(i >= 0, "unclosed object comment");
    } else if (c === "{") depth++;
    else if (c === "}" && --depth === 0) return i;
  }
  fail("unmatched braces");
}

export function offscreen(
  s,
  types = ["ButtonLegendWidget", "ButtonLegendScrollWidget"],
  names = [],
) {
  let n = 0;
  const patterns = types
    .map((t) => '"type"\\s*:\\s*"' + escape(t) + '"')
    .concat(
      names.map(
        (name) => '"type"\\s*:\\s*"[A-Za-z0-9_]+"\\s*,\\s*"[Nn]ame"\\s*:\\s*"' + escape(name) + '"',
      ),
    );
  for (const pattern of patterns) {
    let pos = 0;
    for (;;) {
      const m = search(pattern, s, pos);
      if (!m) break;
      const line0 = s.lastIndexOf("{", m.index - 1);
      const next = search('"(type|children)"\\s*:', s, endOf(m));
      const f = search(
        '"fields"\\s*:\\s*\\{',
        s,
        line0,
        Math.max(next?.index ?? s.length, endOf(m)),
      );
      if (f) {
        const a = endOf(f) - 1,
          b = blockEnd(s, a);
        let body = s.slice(a + 1, b),
          depth = 0;
        const keys = new Map();
        for (let i = 0; i < body.length; i++) {
          const c = body[i];
          if (c === '"') {
            let j = body.indexOf('"', i + 1);
            while (j >= 0 && body[j - 1] === "\\") j = body.indexOf('"', j + 1);
            requireValue(j >= 0, "unclosed field string");
            if (
              depth === 0 &&
              body
                .slice(j + 1)
                .trimStart()
                .startsWith(":")
            ) {
              const key = body.slice(i + 1, j);
              if (!keys.has(key)) keys.set(key, i);
            }
            i = j;
          } else if (body.slice(i, i + 2) === "//") {
            i = body.indexOf("\n", i);
            requireValue(i >= 0, "unclosed field comment");
          } else if (c === "{" || c === "[") depth++;
          else if (c === "}" || c === "]") depth--;
        }
        if (keys.has("rect")) {
          const r0 = body.indexOf("{", keys.get("rect"));
          requireValue(r0 >= 0, "rectangle is not an object");
          body = body.slice(0, keys.get("rect")) + data.OFF + body.slice(blockEnd(body, r0) + 1);
        } else body = " " + data.OFF + "," + body;
        body = body.replace(/"fitToParent"\s*:\s*true/, '"fitToParent": false');
        s = s.slice(0, a + 1) + body + s.slice(b);
        pos = a + 1 + body.length;
      } else {
        const ins = ' "fields": { ' + data.OFF + " },";
        let at = endOf(m);
        if (s[at] === ",") at++;
        s = s.slice(0, at) + (s[at - 1] === "," ? ins : "," + ins) + s.slice(at);
        pos = at + ins.length + 1;
      }
      n++;
    }
  }
  return [s, n];
}

export function setRect(s, name, x, y, w, h) {
  const m = search('"name"\\s*:\\s*"' + escape(name) + '"', s);
  requireValue(m, "missing widget " + name);
  const r = search('"rect"\\s*:\\s*\\{[^}]*\\}', s, endOf(m));
  requireValue(r && r.index - endOf(m) < 200, "rectangle changed for " + name);
  return replaceSpan(
    s,
    r.indices[0],
    `"rect": { "x": ${Math.trunc(x)}, "y": ${Math.trunc(y)}, "width": ${Math.trunc(w)}, "height": ${Math.trunc(h)} }`,
  );
}
function insertBefore(s, pattern, add, suffix = "        ") {
  const matches = [...s.matchAll(new RegExp(pattern, "g"))];
  requireValue(matches.length === 1, "insertion anchor count changed");
  const at = matches[0].index;
  return s.slice(0, at) + add.trimStart() + suffix + s.slice(at);
}
function appendLast(s, add, suffix = "    ") {
  const i = s.trimEnd().lastIndexOf("]");
  requireValue(s.trimEnd().endsWith("}") && i > 0, "layout children terminator changed");
  return trimSpaces(s.slice(0, i)) + add + suffix + s.slice(i);
}
function finalChildren(s) {
  const end = s.trimEnd().replace(/}+$/, "").trimEnd();
  requireValue(end.endsWith("]"), "layout children terminator changed");
  return end.length - 1;
}
function widgetBlock(s, name) {
  const m = search(
    '\\{\\s*"type"\\s*:\\s*"[A-Za-z]+"\\s*,\\s*"name"\\s*:\\s*"' + escape(name) + '"',
    s,
  );
  requireValue(m, "missing native widget " + name);
  return s.slice(m.index, blockEnd(s, m.index) + 1);
}
export function padBliss(s) {
  [s] = mustCount(
    offscreen(s, [], ["LadderFlag", "LadderToggle", "NonLadderToggle", "LadderCenter"]),
    4,
    "ladder widgets",
  );
  let n = 0;
  s = s.replace(
    /\{\s*"action"\s*:\s*"option2"\s*,\s*"description"\s*:\s*"@LegacyGameVersionSwitch"\s*\}\s*,?/g,
    () => {
      n++;
      return "";
    },
  );
  requireValue(n === 1, "legacy version action changed");
  const [at, add] = data.INSERT["controller/console/characteroptionspanelhd.json"];
  return insertBefore(s, at, add, data.blissKey + "        ");
}
export function padRibbon(s) {
  for (const [a, b] of [
    ['"rect": { "x": 164, "y": 1175 },', '"rect": { "x": 99999, "y": 99999 },'],
    ['"rect": { "x": 164, "y": 1325 },', '"rect": { "x": 164, "y": 1175 },'],
    ['"down": { "name": "Cinematics" }', '"down": { "name": "Credits" }'],
    ['"up": { "name": "Cinematics" }', '"up": { "name": "Settings" }'],
  ])
    s = sub1(s, a, b);
  return s;
}
export const padMainmenu = (s) => appendLast(s, data._UPDATE_PAD);
export const padInvRw = (s) => appendLast(s, data._RW);
export const padSettings = (s) => sub1(s, '"@OptionsMenuVideo"', '"@KeyBindingTypeCustom"');
function customPage(s, upstream = true) {
  requireValue(
    s.includes('"name": "VideoOptionsPanel"') && s.includes('"name": "OptionsTable"'),
    "console video layout changed",
  );
  requireValue(upstream === true, "OVERLAY_RECIPE_REVISION");
  return [...data.customPage];
}
export const padCustomPage = (s, upstream = true) => customPage(s, upstream)[0];
export function riftPanel(s) {
  for (const a of [
    '"type": "MessageLogPanel", "name": "MessageLogPanel"',
    '"name": "ListContainer"',
    '"name": "ScrollBar"',
    '"name": "ChatEntryTemplate"',
  ])
    requireValue(count(s, a) === 1, "message log layout changed: " + a);
  return [...data.riftPanel];
}
function pcMainmenu(s) {
  s = sub1(s, '"rect": { "x": -1010, "y": 1865 },', '"rect": { "x": -303, "y": 1865 },');
  s = sub1(s, '"textString": "@PlayTab",', '"textString": "@PlayButton",');
  const [out, n] = mustCount(
    offscreen(s, [], ["OnlinePlaySettingsButton", "Divider", "SecondaryContextButton"]),
    3,
    "PC main menu widgets",
  );
  return [appendLast(out, data._UPDATE), n + 2];
}
function pcCharselect(s) {
  s = sub1(s, '"rect": { "x": -801, "y": 0 },', '"rect": { "x": -1001, "y": 0 },');
  let n;
  [s, n] = mustCount(offscreen(s, [], ["Tabs", "ConvertCharacter"]), 2, "PC character tabs");
  for (const [a, b] of [
    ['"rect": { "x": 315, "y": 1940 },', '"rect": { "x": 431, "y": 1940 },'],
    [
      '"itemSelectionMessage": "CharacterSelect:SelectCharacter",',
      '"itemSelectionMessage": "PanelManager:ClosePanel:TouchNoop",',
    ],
    [
      '"onClickMessage": "FrontEndNavigation:ToCharacterCreate",',
      '"onClickMessage": "FrontEndNavigation:ToCharacterCreate:1",',
    ],
  ])
    s = sub1(s, a, b);
  for (const [name, variable, size] of [
    ["Title", "$XMediumFontSize", 30],
    ["Name", "$XMediumLargeFontSize", 34],
    ["Level&Class", "$XMediumFontSize", 30],
  ]) {
    const i = s.indexOf('"type": "TextBoxWidget", "name": "' + name + '"');
    requireValue(i >= 0, "missing PC character label " + name);
    const m = search('"pointSize":\\s*"(\\$[A-Za-z]+)"', s, i);
    requireValue(m && m[1] === variable, "PC character label size changed");
    s = replaceSpan(s, m.indices[0], '"pointSize": ' + size);
    n++;
  }
  return [s, n + 1];
}
function charNoStamina(s) {
  let n;
  [s, n] = mustCount(
    offscreen(
      s,
      [],
      ["Row06", "Divider06", "Divider07", "StaminaTitle", "MaxStamina", "CurrentStamina"],
    ),
    6,
    "stamina widgets",
  );
  for (const [name, y] of [
    ["Row07", 1075],
    ["Divider08", 1092],
    ["Divider09", 1092],
    ["HPTitle", 1089],
    ["MaxHP", 1093],
    ["CurrentHP", 1093],
  ]) {
    const i = s.indexOf('"name": "' + name + '"');
    requireValue(i >= 0, "missing life widget " + name);
    const m = search('"y":\\s*(\\d+)', s, i);
    requireValue(m && Number(m[1]) === y, "life row position changed");
    s = replaceSpan(s, m.indices[1], String(y - 106));
    n++;
  }
  return [s, n];
}
function charTitleUp(s) {
  let n = 0;
  s = s.replace(
    /("type": "TextBoxWidget", "name": "AttackDamageTitle",\s*"fields": \{\s*"rect": \{ "x": 0, "y": )0(, "width": \d+, "height": 84 \})/g,
    (_, a, b) => {
      n++;
      return a + "-12" + b;
    },
  );
  return mustCount([s, n], 2, "damage titles");
}
function pauseNoHelp(ys, hide = ["Help"], restartY = null, extra = []) {
  return (s) => {
    s = sub1(s, '"textString": "@minipanelmessage",', '"textString": "@dmPauseShareLog",');
    s = sub1(
      s,
      '"onClickMessage": "PanelManager:OpenPanel:MessageLogPanel",',
      '"onClickMessage": "PanelManager:ClosePanel:TouchShareLog",',
    );
    let n;
    [s, n] = mustCount(offscreen(s, [], hide), hide.length, "pause hidden buttons");
    for (const [name, [y0, y1]] of Object.entries(ys)) {
      const m = search(
        '"type":\\s*"ButtonWidget",\\s*"name":\\s*"' +
          escape(name) +
          '",\\s*"fields":\\s*\\{\\s*("rect":\\s*\\{ "x": 564, "y": (\\d+)\\s*\\})',
        s,
      );
      requireValue(m && Number(m[2]) === y0, "pause button position changed: " + name);
      s = replaceSpan(s, m.indices[1], '"rect": { "x": 564, "y": ' + y1 + " }");
      n++;
    }
    const clones =
      restartY === null ? extra : [["TouchRestart", "@dmPauseRestart", restartY], ...extra];
    for (const [name, text, y] of clones) {
      const b = widgetBlock(s, "Exit"),
        e = s.indexOf(b) + b.length;
      let r = sub1(
        sub1(
          sub1(b, '"name": "Exit"', '"name": "' + name + '"'),
          '"textString": "@SaveAndExit"',
          '"textString": "' + text + '"',
        ),
        '"onClickMessage": "PausePanelMessage:ExitGame"',
        '"onClickMessage": "PanelManager:ClosePanel:' + name + '"',
      );
      r = r.replace(/"rect":\s*\{ "x": 564, "y": \d+ \}/, '"rect": { "x": 564, "y": ' + y + " }");
      s = s.slice(0, e) + ",\n        " + r + s.slice(e);
      n++;
    }
    return [s, n];
  };
}
export function padPause(s, garden, share = true) {
  const extra = [
    ["TouchPadDiff", "", 278],
    ["TouchPadStore", "@strGoldDeposit", 778],
  ];
  const hidden = share ? [] : ["MessageLog"];
  if (garden)
    s = pauseNoHelp(
      {
        Settings: [428, 653],
        Exit: [728, 403],
        Chronicle: [1028, 903],
        ...(share ? { MessageLog: [278, 1028] } : {}),
      },
      ["Help", "LootFilter", ...hidden],
      528,
      extra,
    )(s)[0];
  else {
    s = sub1(
      s,
      '"filename": "Controller/Panel/EscapeMenu/EscapeOptionsBGClassic"',
      '"filename": "Controller/Panel/EscapeMenu/EscapeOptionsBG"',
    );
    s = pauseNoHelp(
      { Settings: [578, 653], Exit: [878, 403], ...(share ? { MessageLog: [428, 903] } : {}) },
      ["Help", ...hidden],
      528,
      extra,
    )(s)[0];
  }
  return appendLast(s, data.padDiffText, "");
}
function chronicleTouch(s) {
  requireValue(
    s.includes('"onClickMessage": "PanelManager:ClosePanel:ChroniclePanel"'),
    "chronicle close message changed",
  );
  const at = finalChildren(s);
  return [s.slice(0, at).trimEnd() + data.chronicleClose + s.slice(at), 1];
}
function chronicleKillboard(s) {
  for (const [a, b] of [
    ['"tabSize": { "x": 266, "y": 121 },', '"tabSize": { "x": 320, "y": 121 },'],
    ['"tabCount": 3,', '"tabCount": 4,'],
    ['"inactiveFrames": [ 0, 0, 0],', '"inactiveFrames": [ 0, 0, 0, 0],'],
    ['"activeFrames": [ 1, 1, 1],', '"activeFrames": [ 1, 1, 1, 1],'],
    [
      '"textStrings": [ "@Unique", "@Sets", "@Runewords"],',
      '"textStrings": [ "@Unique", "@Sets", "@dmChronRunewords", "@Bosses"],',
    ],
    [
      '"ChroniclePanelMessage:ToggleChildPanel:ChronicleRunewordsListPanel"',
      '"ChroniclePanelMessage:ToggleChildPanel:ChronicleRunewordsListPanel",\n                    "ChroniclePanelMessage:ToggleChildPanel:TouchKillBoard"',
    ],
  ])
    s = sub1(s, a, b);
  const at = finalChildren(s);
  return [
    s.slice(0, at).trimEnd() + ",\n        " + data.killboardJson + "\n    " + s.slice(at),
    7,
  ];
}
function chronicleNoSearch(s) {
  for (const variable of ['"$ChronicleSearchBox"', '"$ChronicleSearchInput"'])
    s = sub1(s, '"rect": ' + variable, data.OFF);
  for (const x of [80, 486, 1030])
    s = sub1(
      s,
      `"rect": { "x": ${x}, "y": 7, "height": 50 },`,
      `"rect": { "x": ${x}, "y": -5, "height": 50 },`,
    );
  return [s, 5];
}
function literalPairs(pairs) {
  return (s) => {
    for (const [a, b] of pairs) s = sub1(s, a, b);
    return [s, pairs.length];
  };
}
function bossPctWidget() {
  return {
    type: "TextBoxWidget",
    name: "TouchBossPct",
    fields: {
      anchor: { x: 1, y: new PythonFloat(0.5) },
      rect: { x: 24, y: 0 },
      style: {
        pointSize: "$LargeFontSize",
        fontColor: { r: 212, g: 196, b: 145, a: 255 },
        alignment: { h: "left", v: "center" },
      },
    },
  };
}
const parseLayoutJson = (s) => parseInventoryJson(s.replace(/,(\s*[\]}])/g, "$1"));
const dumpLayoutJson = (j) => pythonJsonDumps(j, { ensureAscii: false, indent: 1 });
function setDefault(object, key, value) {
  if (!Object.hasOwn(object, key)) object[key] = value;
  return object[key];
}
function addInteger(value, delta) {
  requireValue(
    typeof value === "number" ||
      typeof value === "bigint" ||
      typeof value === "boolean" ||
      value instanceof PythonFloat,
    "rectangle coordinate is not numeric",
  );
  if (value instanceof PythonFloat) return new PythonFloat(Number(value) + delta);
  if (typeof value === "bigint") return value + BigInt(delta);
  const result = Number(value) + delta;
  return Number.isSafeInteger(result) ? result : BigInt(value) + BigInt(delta);
}
function bossBarTop(text) {
  const j = parseLayoutJson(text),
    sa = j.children.find((c) => c.name === "ScreenAnchored"),
    ta = j.children.find((c) => c.name === "TargetAttached");
  requireValue(sa && ta, "boss bar anchors changed");
  const moved = new Map(ta.children.map((c) => [c.name, c]));
  requireValue(
    moved.size === 3 &&
      ["UniqueMonsterBackground", "NormalMonsterBackground", "Health"].every((k) => moved.has(k)) &&
      sa.children.map((c) => c.name).join(",") === "Name,Unique,Additional",
    "boss bar widgets changed",
  );
  for (const c of moved.values()) {
    const r = Object.hasOwn(c, "rect")
      ? c.rect
      : setDefault(setDefault(c, "fields", {}), "rect", {});
    r.y = addInteger(Object.hasOwn(r, "y") ? r.y : 0, 86);
  }
  const hp = moved.get("Health"),
    f = hp.fields;
  requireValue(Object.hasOwn(f, "filename"), "boss health filename missing");
  delete f.filename;
  Object.assign(f.rect, { width: 45, height: 57 });
  f.rect.y = addInteger(f.rect.y, 8);
  f.normalColor = { r: new PythonFloat(0.18), g: 0, b: 0 };
  f.transparency = new PythonFloat(0.75);
  setDefault(hp, "children", []).push(bossPctWidget());
  const [uniq, add] = sa.children.slice(1);
  uniq.fields.rect = { y: 100 };
  add.fields.rect = { y: 160 };
  sa.children = [
    moved.get("UniqueMonsterBackground"),
    moved.get("NormalMonsterBackground"),
    hp,
    uniq,
    add,
  ];
  ta.children = [];
  return [dumpLayoutJson(j), 1];
}
export function padBossPct(text) {
  const j = parseLayoutJson(text),
    ta = j.children.find((c) => c.name === "TargetAttached");
  requireValue(ta, "boss target anchor missing");
  const hp = ta.children.find((c) => c.name === "Health");
  requireValue(hp, "boss health missing");
  setDefault(hp, "children", []).push(bossPctWidget());
  return dumpLayoutJson(j);
}
function settingsTabs(text) {
  let n = 0,
    k = 0;
  let out = text.replace(
    /("(?:inactiveFrames|activeFrames|textStrings)"\s*:\s*\[)([^\]]*)(\])/g,
    (_, a, body, b) => {
      const items = body
        .trim()
        .replace(/,+$/, "")
        .split(/,\s*/)
        .filter((v) => v.trim());
      n++;
      return a + " " + items.slice(1).join(", ") + " " + b;
    },
  );
  out = out.replace(
    /("tabMessages"\s*:\s*\[)([^\]]*)(\])/g,
    (_, a, body, b) =>
      a +
      "\n" +
      body
        .split(",")
        .map((v) => v.trim())
        .filter((v) => v && !v.includes("VideoOptionsPanel"))
        .join(",\n") +
      ",\n" +
      b,
  );
  out = out.replace(/"tabCount"\s*:\s*5/g, () => {
    k++;
    return '"tabCount": 4';
  });
  requireValue(
    count(out, '"@Controls"') === 1 &&
      count(out, "SettingsPanelMessage:CheckChanges:KeyBindingMenu") === 1 &&
      count(out, '"@GameSettingsTab"') === 1,
    "settings tabs changed",
  );
  out = out
    .replace('"@GameSettingsTab"', '"@dmTabGameSettings"')
    .replace('"@Controls"', '"@dmTabCustom"')
    .replace(
      "SettingsPanelMessage:CheckChanges:KeyBindingMenu",
      "SettingsPanelMessage:CheckChanges:VideoOptionsPanel",
    );
  return [out, n + k + 1];
}
function dropRows(rows) {
  return (text) => {
    let n = 0;
    for (const row of rows) {
      const ms = [
        ...text.matchAll(
          new RegExp(
            '\\{\\s*"type"\\s*:\\s*"TableRowWidget"\\s*,\\s*"name"\\s*:\\s*"Row ' +
              escape(row) +
              '"',
            "g",
          ),
        ),
      ];
      requireValue(ms.length === 1, "option row count changed: " + row);
      let e = blockEnd(text, ms[0].index) + 1;
      while (text[e] === " " || text[e] === "\t") e++;
      if (text[e] === ",") e++;
      text = text.slice(0, ms[0].index) + text.slice(e);
      n++;
    }
    text = text.replace(/[ \t]*"tooltipString"\s*:\s*"[^"\n]*",?[ \t]*\r?\n/g, () => {
      n++;
      return "";
    });
    return [text, n];
  };
}
const soundRows = ["LanguageOptions", "VoiceLanguage", "TextLanguage", "Controller Feedback"];
const gameplayRows = [
  "AutoPartyInvite",
  "ItemHotkeyTooltipAppender",
  "ChatFontSize",
  "ChatBackground",
  "Controller Options",
  "ControllerCursorSpeed",
  "ControllerWeaponSwapSeparateBindings",
  "ControllerRumbleEnabled",
  "ControllerSwapThumbsticks",
  "ShowParty",
  "ShowNames",
  "ShowRoomName",
  "LowVisionMode",
  "ColorBlindCalibration",
];
const onlineRows = ["OfflineDifficultyScaling", "OfflineSettingsEffectNote"];
const padRoot = {
  "soundoptionspanelhd.json": dropRows(soundRows),
  "gameplayoptionspanelhd.json": dropRows([
    "AutoPartyInvite",
    "ItemHotkeyTooltipAppender",
    "ChatFontSize",
    "ChatBackground",
    "ShowParty",
    "ShowNames",
    "ShowRoomName",
    "LowVisionMode",
    "ControllerCursorSpeed",
    "ControllerWeaponSwapSeparateBindings",
    "ControllerRumbleEnabled",
  ]),
  "onlinegameoptionspanelhd.json": dropRows(onlineRows),
};
const rowHeight = (fn) => (text) => {
  const [out, n] = fn(text),
    a = '"rowHeight": "$OptionsTableRowHeight"';
  requireValue(out.includes(a), "option row height changed");
  return [out.replaceAll(a, '"rowHeight": 180'), n + count(out, a)];
};
function modalPrompt(old, next) {
  return literalPairs([
    [old, next],
    [
      '"style": "$StyleModalDialogDescription"',
      '"style": { "fontColor": "$ModalDescriptionTextColor", "alignment": { "h": "center", "v": "center" }, "options": { "lineWrap": true, "newlineHandling": "standard" }, "pointSize": 30, "spacing": "$ReducedSpacing" }',
    ],
  ]);
}
const present = {
  "controller/uiswitcherhd.json": (s) =>
    offscreen(s, [], ["CycleLeftIndicator", "CycleRightIndicator"]),
  "controller/characterstatspanelhd.json": compose(charNoStamina, charTitleUp),
  "controller/tooltipspanelhd.json": literalPairs([
    ['"screenEdgePadding": 50,', '"screenEdgePadding": 190,'],
    ['"showDelayMilliseconds": 400,', '"showDelayMilliseconds": 0,'],
  ]),
  "controller/dropgoldmodalhd.json": (s) => offscreen(s),
  "controller/bankexpansionlayouthd.json": (s) => offscreen(s, [], ["gold_max"]),
  "npcdialogpanelhd.json": literalPairs([
    [
      '"padding": { "left": 32, "top": 24, "right": 32, "bottom": 24 },',
      '"padding": { "left": 150, "top": 24, "right": 150, "bottom": 24 },',
    ],
    ['"listEntryPadding": { "x": 0, "y": 10 },', '"listEntryPadding": { "x": 0, "y": 62 },'],
  ]),
  "chroniclebaselistpanelhd.json": chronicleNoSearch,
  "dropdownlistcontentshd.json": (s) => {
    const a = '"rect": { "width": 625, "height": 40 }';
    requireValue(count(s, a) === 2, "dropdown row height changed");
    return [s.replaceAll(a, '"rect": { "width": 625, "height": 56 }'), 2];
  },
  "chroniclepanelhd.json": compose(chronicleTouch, chronicleKillboard),
  "controller/messagelogpanelhd.json": riftPanel,
  "controller/pauselayoutgardenhd.json": pauseNoHelp(
    { Settings: [428, 293], Exit: [728, 473], Chronicle: [1028, 833], MessageLog: [278, 1013] },
    ["Help", "LootFilter"],
    653,
  ),
  "controller/pauselayouthd.json": pauseNoHelp(
    { Settings: [578, 428], Exit: [878, 578], MessageLog: [428, 878] },
    ["Help"],
    728,
  ),
  "helpbuttonpanelhd.json": (s) => offscreen(s, [], ["Help"]),
  "controller/buttonbindingmodalhd.json": (s) =>
    mustCount(offscreen(s, [], ["SkillButton6", "AltSkillButtonRT"]), 2, "binding sixth column"),
  "youdiedmodalhd.json": (s) => offscreen(s, [], ["SubText"]),
  "controller/controlleroverlayhd.json": (s) => mustCount(offscreen(s), 1, "overlay legend"),
  "controller/console/switch/playerinventoryexpansionlayouthd.json": (s) => {
    const [out, n] = offscreen(s);
    return [padInvRw(out), n + 1];
  },
  "controller/playerinventoryexpansionlayouthd.json": (s) => offscreen(s),
  "controller/hudmonsterhealthhd.json": bossBarTop,
  "controller/console/switch/npcdialogpanelhd.json": literalPairs([
    [
      '"basedOn": "NpcDialogPanelHD.json",',
      '"basedOn": "NpcDialogPanelHD.json",\n    "fields": { "listEntryPadding": { "x": 0, "y": 124 } },',
    ],
  ]),
  "controller/console/videooptionspanelhd.json": customPage,
  "controller/console/settingspanelhd.json": settingsTabs,
  "confirmationmodalhd.json": modalPrompt(
    '"rect": { "x": 78, "y": 85, "width": 1076, "height": 170 },',
    '"rect": { "x": 78, "y": 28, "width": 1076, "height": 264 },',
  ),
  "dismissablemodalhd.json": modalPrompt(
    '"rect": { "x": 140, "y": 70, "width": 966, "height": 212 },',
    '"rect": { "x": 140, "y": 28, "width": 966, "height": 264 },',
  ),
  "controller/settingspanelhd.json": (s) =>
    mustCount(offscreen(s, ["ButtonLegendWidget"]), 1, "settings legend"),
  "soundoptionspanelhd.json": rowHeight(dropRows(soundRows)),
  "gameplayoptionspanelhd.json": rowHeight(dropRows(gameplayRows)),
  "onlinegameoptionspanelhd.json": rowHeight(dropRows(onlineRows)),
};

export function createTouchLayouts({ readLayout, readEntry, rows, upstream = true }) {
  requireValue(upstream === true, "OVERLAY_RECIPE_REVISION");
  requireValue(typeof readLayout === "function", "authenticated layout reader required");
  const put = (out, path, text) => out.set(path.toLowerCase(), encoder.encode(text));
  const transformMap = (spec) => {
    const out = new Map();
    for (const [rel, fn] of Object.entries(spec)) {
      const [path, text] = readLayout(rel),
        [next, n] = fn(text);
      requireValue(n !== 0, "no layout changes for " + rel);
      put(out, path, next);
    }
    return out;
  };
  const blankSprites = (needle, expected, suffix = "") => {
    const out = new Map();
    requireValue(
      Array.isArray(rows) && typeof readEntry === "function",
      "authenticated sprite reader required",
    );
    for (const f of rows) {
      if (
        f.length < 11 ||
        f[0].startsWith("#") ||
        !f[9].toLowerCase().includes(needle) ||
        !f[9].toLowerCase().endsWith(suffix)
      )
        continue;
      const raw = readEntry(f);
      requireValue(raw instanceof Uint8Array && raw.length >= 40, "sprite truncated: " + f[9]);
      const view = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
      requireValue(
        raw[0] === 83 &&
          raw[1] === 112 &&
          raw[2] === 65 &&
          raw[3] === 49 &&
          view.getUint16(4, true) === 61 &&
          view.getUint32(32, true) === raw.length - 40,
        "sprite header changed: " + f[9],
      );
      const bytes = new Uint8Array(raw.length);
      bytes.set(raw.subarray(0, 40));
      const colon = f[9].indexOf(":");
      requireValue(colon >= 0, "sprite logical path missing");
      out.set(f[9].slice(colon + 1).toLowerCase(), bytes);
    }
    requireValue(
      out.size === expected,
      "sprite count changed: expected " + expected + ", got " + out.size,
    );
    return out;
  };
  return Object.freeze({
    build(files = data.FRONTEND) {
      const out = new Map();
      for (const [rel, names] of files) {
        const [path, text] = readLayout(rel);
        let [next, n] = offscreen(text, undefined, names);
        requireValue(n !== 0, "missing front-end legends: " + rel);
        if (rel === "controller/console/charactercreatepanelhd.json")
          for (const [name, x0, x1] of CLASS_STRIPS)
            next = setRect(next, name, x0, 450, x1 - x0, 1300);
        if (Object.hasOwn(data.INSERT, rel)) next = insertBefore(next, ...data.INSERT[rel]);
        put(out, path, next);
      }
      return out;
    },
    buildPcfront() {
      const out = new Map();
      for (const [pc, fn] of [
        ["mainmenupanelhd.json", pcMainmenu],
        ["characterselectpanelhd.json", pcCharselect],
        [
          "mainmenubuttonribbonhd.json",
          (s) => mustCount(offscreen(s, [], ["Ribbon", "MotDSection"]), 2, "PC ribbon"),
        ],
        ["mainmenulinkshd.json", null],
      ]) {
        const [cpath] = readLayout("controller/console/" + pc),
          [ppath, original] = readLayout(pc);
        const text = fn ? fn(original)[0] : original;
        let mid;
        if (data.PCFRONT_PLAIN.includes(pc)) mid = text;
        else {
          const slash = ppath.lastIndexOf("/");
          put(out, ppath.slice(0, slash).toLowerCase() + "/1touchbase_lay/" + pc, text);
          [, mid] = readLayout("controller/" + pc);
          [mid] = offscreen(mid);
          mid = sub1(
            mid,
            '"basedOn": "' + ppath.slice(slash + 1) + '"',
            '"basedOn": "1touchbase_lay/' + pc + '"',
          );
        }
        if (pc === "mainmenupanelhd.json") {
          const i = mid.trimEnd().lastIndexOf("]");
          mid = mid.slice(0, i) + data._UPDATE_STUB + mid.slice(i);
        }
        put(out, cpath, mid);
      }
      return out;
    },
    buildPresent: () =>
      transformMap({
        ...present,
        "controller/console/videooptionspanelhd.json": (s) => customPage(s, upstream),
      }),
    buildPadRoot: () => transformMap(padRoot),
    buildProfile() {
      const out = new Map();
      for (const [rel, from] of [
        ["controller/_profileconsolehd.json", data.BIG_FONT[0]],
        [
          "controller/_profilehandheldhd.json",
          '"XMediumLargeFontSize": 36,\n    "MediumLargeFontSize": 36,\n    "XMediumFontSize": 34,\n',
        ],
      ]) {
        const [path, text] = readLayout(rel);
        put(out, path, sub1(text.replaceAll("\r\n", "\n"), from, data.BIG_FONT[1]));
      }
      return out;
    },
    buildCursor: () => blankSprites("/ui/controller/cursor/", 3, ".sprite"),
    buildGlyphs: () => blankSprites("/ui/controller/controllerbuttons/switch/", 22),
    buildIngame(spec = null, applyPresent = true) {
      const out = new Map();
      for (const [rel, [src, items]] of Object.entries(spec ?? data.INGAME)) {
        const [path, original] = readLayout(rel),
          [, base] = readLayout(src);
        let text =
          applyPresent && Object.hasOwn(present, rel) ? present[rel](original)[0] : original;
        let add = "";
        for (const [name, newName, override, ...alt] of items) {
          let b = widgetBlock(alt.length ? readLayout(alt[0])[1] : base, name);
          requireValue(
            !search('"name"\\s*:\\s*"' + escape(newName || name) + '"', text),
            "duplicate inserted widget: " + (newName || name),
          );
          if (newName) b = b.replace('"name": "' + name + '"', () => '"name": "' + newName + '"');
          if (override) {
            for (const m of override.matchAll(/"([A-Za-z/]+)"\s*:/g))
              b = b.replace(
                new RegExp(
                  '\\n\\s*"' + escape(m[1]) + '"\\s*:\\s*(\\{[^{}]*\\}|"[^"]*"|[^,\\n]+),?',
                ),
                "",
              );
            const f = search('"fields"\\s*:\\s*\\{', b);
            requireValue(f, "inserted widget fields missing");
            b =
              b.slice(0, endOf(f)) +
              "\n                " +
              override.replace(/,+$/, "") +
              "," +
              b.slice(endOf(f));
          }
          add += "        " + b + ",\n";
        }
        const at = finalChildren(text),
          head = text.slice(0, at).trimEnd();
        const next =
          head +
          (head.endsWith(",") || head.endsWith("[") ? "" : ",") +
          "\n" +
          add +
          "    " +
          text.slice(at);
        try {
          parseInventoryJson(
            next
              .replace(/\/\*[\s\S]*?\*\//g, "")
              .replace(/^\s*\/\/[^\n]*|(?<=[,{[\s])\/\/[^\n]*/gm, "")
              .replace(/,(\s*[}\]])/g, "$1"),
          );
        } catch {
          fail("inserted layout is invalid: " + rel);
        }
        put(out, path, next);
      }
      return out;
    },
  });
}
