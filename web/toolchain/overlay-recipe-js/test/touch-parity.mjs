#!/usr/bin/env node
/**
 * CPython 与原生 JS 触控布局一致性比较，仅用自编合成输入，不含游戏数据。Python 是独立固定的测试参考，从不成为运行时依赖；导入本文件只定义辅助函数。
 * CPython-vs-native-JS touch layout parity, using only authored synthetic input.
 * The corpus contains no title data. Python is an independent, pinned test oracle
 * and is never a runtime dependency. Importing this file only defines helpers.
 */
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = fileURLToPath(new URL("../../../../", import.meta.url));
const data = JSON.parse(readFileSync(new URL("../touch-data.json", import.meta.url), "utf8"));
const q = JSON.stringify;
const rect = (x = 7, y = 11, width = 83, height = 47) =>
  `"rect": { "x": ${x}, "y": ${y}, "width": ${width}, "height": ${height} }`;
const widget = (name, fields = rect(), type = "Widget") =>
  `        {\n            "type": ${q(type)}, "name": ${q(name)},\n            "fields": {\n                ${fields}\n            }\n        }`;
const layout = (children = [], fields = "", basedOn = "") =>
  `{\n    ${basedOn ? `"basedOn": ${q(basedOn)},\n    ` : ""}"type": "SyntheticLayout", "name": "SyntheticRoot",\n    ${fields ? `"fields": { ${fields} },\n    ` : ""}"children": [\n${children.join(",\n")}${children.length ? "," : ""}\n    ]\n}\n`;
const legend = (name = "FixtureLegend") =>
  widget(name, rect() + ', "fitToParent": true', "ButtonLegendWidget");
const panel = (...names) => layout(names.map((name) => widget(name)));

/**
 * 新建普通相对路径到文本映射，调用方可安全修改自己的副本。
 * Fresh plain rel -> text map; callers may safely modify their own copy. */
export function createTouchCorpus() {
  const corpus = {};
  const put = (rel, text) => {
    corpus[rel] = text;
  };
  for (const [rel, names] of data.FRONTEND) {
    let ln = "FixtureLegend";
    if (rel === "controller/console/charactercreatepanelhd.json") ln = "CharacterCreateLegend";
    if (rel === "controller/console/charactercreatenamepanelhd.json") ln = "CharacterNameLegend";
    if (rel === "controller/console/characteroptionspanelhd.json") ln = "CharacterOptionsLegend";
    const children = [...names.map((n) => widget(n)), legend(ln)];
    if (rel === "controller/console/charactercreatepanelhd.json")
      children.unshift(...data.CLASS_STRIPS.map(([n]) => widget(n)));
    put(
      rel,
      layout(
        children,
        rel.endsWith("characteroptionspanelhd.json")
          ? '"actions": [ { "action": "option2", "description": "@LegacyGameVersionSwitch" }, { "action": "keep", "description": "keep me" } ]'
          : "",
      ),
    );
  }

  // 所有游戏内原生源控件均为自编，不复制游戏布局。
  // All in-game native source widgets are invented, not copied title layouts.
  const baseWidgets = new Map();
  for (const [rel, [base, items]] of Object.entries(data.INGAME)) {
    if (!corpus[rel]) put(rel, layout([legend()]));
    for (const [name, , , alternate] of items) {
      const source = alternate || base;
      if (!baseWidgets.has(source)) baseWidgets.set(source, new Set());
      baseWidgets.get(source).add(name);
    }
  }
  for (const [rel, names] of baseWidgets) {
    put(
      rel,
      layout(
        [...names].map((name) =>
          widget(
            name,
            rect() +
              ',\n                "filename": "Synthetic/Fixture",\n                "hoveredFrame": 4,\n                "onClickMessage": "Fixture:Noop",\n                "tooltipString": "Fixture tooltip",\n                "sound": "fixture_sound"',
            name.startsWith("text_") ? "TextBoxWidget" : "ButtonWidget",
          ),
        ),
      ),
    );
  }

  put(
    "mainmenupanelhd.json",
    layout([
      widget(
        "Play",
        '"rect": { "x": -1010, "y": 1865 },\n                "textString": "@PlayTab",',
        "ButtonWidget",
      ),
      ...["OnlinePlaySettingsButton", "Divider", "SecondaryContextButton"].map((n) => widget(n)),
    ]),
  );
  put(
    "characterselectpanelhd.json",
    layout([
      widget("Tabs"),
      widget("ConvertCharacter"),
      widget(
        "List",
        '"rect": { "x": -801, "y": 0 },\n                "itemSelectionMessage": "CharacterSelect:SelectCharacter",',
      ),
      widget(
        "Create",
        '"rect": { "x": 315, "y": 1940 },\n                "onClickMessage": "FrontEndNavigation:ToCharacterCreate",',
        "ButtonWidget",
      ),
      ...[
        ["Title", "$XMediumFontSize"],
        ["Name", "$XMediumLargeFontSize"],
        ["Level&Class", "$XMediumFontSize"],
      ].map(([n, size]) => widget(n, `"style": { "pointSize": ${q(size)} }`, "TextBoxWidget")),
    ]),
  );
  put("mainmenubuttonribbonhd.json", panel("Ribbon", "MotDSection"));
  put("mainmenulinkshd.json", panel("FixtureLink"));
  for (const name of [
    "mainmenupanelhd.json",
    "characterselectpanelhd.json",
    "mainmenubuttonribbonhd.json",
    "mainmenulinkshd.json",
  ]) {
    put("controller/" + name, layout([legend()], "", name));
    put("controller/console/" + name, layout([legend()], "", name));
  }
  put(
    "controller/console/mainmenubuttonribbonhd.json",
    layout(
      [
        widget("Cinematics", '"rect": { "x": 164, "y": 1175 },', "ButtonWidget"),
        widget("Credits", '"rect": { "x": 164, "y": 1325 },', "ButtonWidget"),
      ],
      '"down": { "name": "Cinematics" }, "up": { "name": "Cinematics" }',
    ),
  );

  put("controller/uiswitcherhd.json", panel("CycleLeftIndicator", "CycleRightIndicator"));
  put(
    "controller/characterstatspanelhd.json",
    layout([
      ...["Row06", "Divider06", "Divider07", "StaminaTitle", "MaxStamina", "CurrentStamina"].map(
        (n) => widget(n),
      ),
      ...[
        ["Row07", 1075],
        ["Divider08", 1092],
        ["Divider09", 1092],
        ["HPTitle", 1089],
        ["MaxHP", 1093],
        ["CurrentHP", 1093],
      ].map(([n, y]) => widget(n, rect(3, y))),
      widget("AttackDamageTitle", rect(0, 0, 93, 84), "TextBoxWidget"),
      widget("AttackDamageTitle", rect(0, 0, 117, 84), "TextBoxWidget"),
    ]),
  );
  put(
    "controller/tooltipspanelhd.json",
    layout([], '"screenEdgePadding": 50, "showDelayMilliseconds": 400,'),
  );
  put("controller/dropgoldmodalhd.json", layout([legend()]));
  put("controller/bankexpansionlayouthd.json", panel("gold_max"));
  put(
    "npcdialogpanelhd.json",
    layout(
      [],
      '"padding": { "left": 32, "top": 24, "right": 32, "bottom": 24 },\n        "listEntryPadding": { "x": 0, "y": 10 },',
    ),
  );
  put(
    "chroniclebaselistpanelhd.json",
    layout([
      widget("SearchBox", '"rect": "$ChronicleSearchBox"'),
      widget("SearchInput", '"rect": "$ChronicleSearchInput"'),
      ...[80, 486, 1030].map((x) =>
        widget("Quality" + x, `"rect": { "x": ${x}, "y": 7, "height": 50 },`),
      ),
    ]),
  );
  put(
    "dropdownlistcontentshd.json",
    layout([
      widget("Entry", '"rect": { "width": 625, "height": 40 }'),
      widget("Selected", '"rect": { "width": 625, "height": 40 }'),
    ]),
  );
  put(
    "chroniclepanelhd.json",
    layout([
      widget("Close", '"onClickMessage": "PanelManager:ClosePanel:ChroniclePanel"', "ButtonWidget"),
      widget(
        "Tabs",
        '"tabSize": { "x": 266, "y": 121 },\n                "tabCount": 3,\n                "inactiveFrames": [ 0, 0, 0],\n                "activeFrames": [ 1, 1, 1],\n                "textStrings": [ "@Unique", "@Sets", "@Runewords"],\n                "tabMessages": [ "ChroniclePanelMessage:ToggleChildPanel:ChronicleRunewordsListPanel" ]',
      ),
    ]),
  );
  corpus["chroniclepanelhd.json"] = corpus["chroniclepanelhd.json"].replace(/,\n {4}\]/, "\n    ]");
  put(
    "controller/messagelogpanelhd.json",
    layout([widget("ListContainer"), widget("ScrollBar"), widget("ChatEntryTemplate")]).replace(
      '"type": "SyntheticLayout", "name": "SyntheticRoot"',
      '"type": "MessageLogPanel", "name": "MessageLogPanel"',
    ),
  );
  for (const garden of [false, true]) {
    const ys = garden
      ? { Settings: 428, Exit: 728, Chronicle: 1028, MessageLog: 278 }
      : { Settings: 578, Exit: 878, MessageLog: 428 };
    const children = Object.entries(ys).map(([n, y]) =>
      widget(
        n,
        `"rect": { "x": 564, "y": ${y} },\n                "textString": ${q(n === "Exit" ? "@SaveAndExit" : n === "MessageLog" ? "@minipanelmessage" : n)},\n                "onClickMessage": ${q(n === "Exit" ? "PausePanelMessage:ExitGame" : n === "MessageLog" ? "PanelManager:OpenPanel:MessageLogPanel" : "Fixture:" + n)},`,
        "ButtonWidget",
      ),
    );
    children.push(widget("Help"));
    if (garden) children.push(widget("LootFilter"));
    put(
      "controller/pauselayout" + (garden ? "garden" : "") + "hd.json",
      layout(
        children,
        '"filename": "Controller/Panel/EscapeMenu/' +
          (garden ? "EscapeOptionsBG" : "EscapeOptionsBGClassic") +
          '"',
      ),
    );
  }
  put("helpbuttonpanelhd.json", panel("Help"));
  put(
    "controller/buttonbindingmodalhd.json",
    panel("SkillButton6", "AltSkillButtonRT", "UnchangedSkill"),
  );
  put("youdiedmodalhd.json", panel("SubText"));
  put("controller/controlleroverlayhd.json", layout([legend()]));
  put("controller/console/switch/playerinventoryexpansionlayouthd.json", layout([legend()]));
  // 此文件既是 PRESENT 的输入，也是技能按钮的原生来源。
  // This file is both an input to PRESENT and a native source for skill buttons.
  const invRel = "controller/playerinventoryexpansionlayouthd.json";
  put(invRel, corpus[invRel].replace('"children": [', '"children": [\n' + legend() + ","));
  put(
    "controller/hudmonsterhealthhd.json",
    JSON.stringify(
      {
        type: "SyntheticHealth",
        name: "FixtureHealth",
        children: [
          {
            type: "Widget",
            name: "ScreenAnchored",
            children: ["Name", "Unique", "Additional"].map((name) => ({
              type: "TextBoxWidget",
              name,
              fields: { rect: { x: 1, y: 2 }, text: "Fixture Ω 😀" },
            })),
          },
          {
            type: "Widget",
            name: "TargetAttached",
            children: [
              { type: "Widget", name: "UniqueMonsterBackground", rect: { x: 4, y: 8 } },
              { type: "Widget", name: "NormalMonsterBackground", fields: { rect: { x: 9 } } },
              {
                type: "Widget",
                name: "Health",
                fields: { rect: { x: 3, y: 12 }, filename: "Synthetic/Health" },
              },
            ],
          },
        ],
      },
      null,
      1,
    ),
  );
  put("controller/console/switch/npcdialogpanelhd.json", layout([], "", "NpcDialogPanelHD.json"));
  put(
    "controller/console/videooptionspanelhd.json",
    layout([widget("OptionsTable")]).replace(
      '"name": "SyntheticRoot"',
      '"name": "VideoOptionsPanel"',
    ),
  );
  put(
    "controller/console/settingspanelhd.json",
    layout(
      [legend()],
      '"tabCount": 5, "inactiveFrames": [ 0, 1, 2, 3, 4 ], "activeFrames": [ 5, 6, 7, 8, 9 ], "textStrings": [ "@OptionsMenuVideo", "@Audio", "@GameSettingsTab", "@Controls", "@Other" ], "tabMessages": [ "SettingsPanelMessage:CheckChanges:VideoOptionsPanel", "Audio", "Gameplay", "SettingsPanelMessage:CheckChanges:KeyBindingMenu", "Other" ]',
    ),
  );
  put(
    "confirmationmodalhd.json",
    layout([
      widget(
        "Prompt",
        '"rect": { "x": 78, "y": 85, "width": 1076, "height": 170 },\n                "style": "$StyleModalDialogDescription"',
        "TextBoxWidget",
      ),
    ]),
  );
  put(
    "dismissablemodalhd.json",
    layout([
      widget(
        "Prompt",
        '"rect": { "x": 140, "y": 70, "width": 966, "height": 212 },\n                "style": "$StyleModalDialogDescription"',
        "TextBoxWidget",
      ),
    ]),
  );
  put("controller/settingspanelhd.json", layout([legend()]));
  for (const [rel, names] of Object.entries({
    "soundoptionspanelhd.json": [
      "LanguageOptions",
      "VoiceLanguage",
      "TextLanguage",
      "Controller Feedback",
    ],
    "gameplayoptionspanelhd.json": [
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
    ],
    "onlinegameoptionspanelhd.json": ["OfflineDifficultyScaling", "OfflineSettingsEffectNote"],
  })) {
    put(
      rel,
      layout(
        [...names, "KeepFixtureRow"].map((n) =>
          widget(
            "Row " + n,
            rect() +
              ',\n                "tooltipString": "Synthetic row tooltip",\n                "text": "保留 Ω 😀"',
            "TableRowWidget",
          ),
        ),
        '"rowHeight": "$OptionsTableRowHeight"',
      ),
    );
  }
  put(
    "controller/_profileconsolehd.json",
    '{\r\n    "XMediumFontSize": 30,\r\n    "MediumFontSize": 28,\r\n    "SmallFontSize": 26,\r\n    "Fixture": "Ω"\r\n}\r\n',
  );
  put(
    "controller/_profilehandheldhd.json",
    '{\r\n    "XMediumLargeFontSize": 36,\r\n    "MediumLargeFontSize": 36,\r\n    "XMediumFontSize": 34,\r\n    "Fixture": "😀"\r\n}\r\n',
  );
  return corpus;
}

/**
 * 为完整覆盖配方测试合并独立背包样例；输入为 inventoryFixtures()，克隆全部字符串，不修改任何样例。
 * Merge the independent inventory fixtures for the full overlay-recipe test.
 * Input is inventoryFixtures(); all strings are cloned and no fixture is mutated.
 */
export function touchFixtures(inventory = null) {
  const corpus = createTouchCorpus();
  if (inventory) {
    const append = (text, child) => {
      const end = text.lastIndexOf("]");
      assert.ok(end >= 0, "Synthetic layout must end in a children array");
      const head = text.slice(0, end).trimEnd();
      return (
        head +
        (head.endsWith(",") || head.endsWith("[") ? "" : ",") +
        "\n" +
        child +
        ",\n    " +
        text.slice(end)
      );
    };
    corpus["controller/playerinventoryoriginallayouthd.json"] = inventory.player;
    corpus["controller/hirelinginventorypanelhd.json"] = inventory.mercenary;
    // 背包隔离测试已有触控后控件；完整流水线必须从触控插入之前开始，所以只删除那些行。
    // Inventory's isolated test already has these post-touch widgets. A complete
    // pipeline must start before touch inserts them, so remove only those lines.
    const bank = inventory.bank
      .split("\n")
      .filter((line) => !/"name": "(?:TouchStashLeft|TouchStashRight|close)"/.test(line))
      .join("\n");
    corpus["controller/bankexpansionlayouthd.json"] = append(bank, widget("gold_max"));
    corpus["controller/controlleroverlayhd.json"] = append(inventory.overlay, legend());
  }
  corpus["controller/notificationpanelhd.json"] = panel("Notification").replace(
    '"y": 11',
    '"y": -500',
  );
  return new Map(Object.entries(corpus));
}

function createSpriteFixtures() {
  const rows = [],
    entries = {};
  for (const [directory, count] of [
    ["cursor", 3],
    ["controllerbuttons/switch", 22],
  ]) {
    for (let i = 0; i < count; i++) {
      const path = `data/HD/Global/UI/Controller/${directory}/synthetic_${i}.sprite`;
      const bytes = Buffer.alloc(40 + 7 + i);
      bytes.write("SpA1");
      bytes.writeUInt16LE(61, 4);
      bytes.writeUInt32LE(bytes.length - 40, 32);
      for (let j = 40; j < bytes.length; j++) bytes[j] = ((i * 19 + j) % 255) + 1;
      const row = Array(11).fill("");
      row[4] = createHash("md5").update(bytes).digest("hex");
      row[7] = String(bytes.length);
      row[8] = "n";
      row[9] = "fixture:" + path;
      rows.push(row);
      entries[row[9]] = bytes.toString("base64");
    }
  }
  const unrelated = Array(11).fill("");
  unrelated[9] = "fixture:data/unrelated.sprite";
  rows.push(unrelated);
  const nonSprite = Array(11).fill("");
  nonSprite[9] = "fixture:data/HD/Global/UI/Controller/cursor/ignore.txt";
  rows.push(nonSprite);
  return { rows, entries };
}

const primitiveInput = layout([
  widget(
    "Legend",
    '"rect": { "x": 2, "y": 3 }, "fitToParent": true, "nested": { "rect": { "x": 5 }, "fitToParent": true }, "note": "braces { } and \\\"quote\\\""',
    "ButtonLegendWidget",
  ),
  '{ "type": "ButtonLegendScrollWidget", "name": "NoFields", "children": [] }',
  '{ "type": "Widget", "Name": "Named", "fields": { "nested": { "rect": { "x": 8 } }, "fitToParent": true } }',
  widget("Unchanged", rect()),
]);

function requests(corpus) {
  const c = (rel) => corpus[rel];
  const tests = [
    ...[
      "build",
      "buildPcfront",
      "buildPresent",
      "buildPadRoot",
      "buildProfile",
      "buildCursor",
      "buildGlyphs",
    ].map((method) => ({ name: method, method })),
    { name: "buildIngame", method: "buildIngame" },
    { name: "buildIngame-no-present", method: "buildIngame", args: [null, false] },
    { name: "buildIngame-pad-vendor", method: "buildIngame", args: [data.PAD_VENDOR, false] },
    {
      name: "padInvRw",
      method: "padInvRw",
      args: [c("controller/console/switch/playerinventoryexpansionlayouthd.json")],
    },
    ...[false, true].flatMap((garden) =>
      [false, true].map((share) => ({
        name: `padPause-${garden}-${share}`,
        method: "padPause",
        args: [c("controller/pauselayout" + (garden ? "garden" : "") + "hd.json"), garden, share],
      })),
    ),
    { name: "padBossPct", method: "padBossPct", args: [c("controller/hudmonsterhealthhd.json")] },
    {
      name: "buildPresent-exact-coordinate-numbers",
      method: "buildPresent",
      overrides: {
        "controller/hudmonsterhealthhd.json": c("controller/hudmonsterhealthhd.json")
          .replace('"y": 8', '"y": 9007199254740991')
          .replace('"y": 12', '"y": 12.0'),
      },
    },
    {
      name: "padBliss",
      method: "padBliss",
      args: [c("controller/console/characteroptionspanelhd.json")],
    },
    {
      name: "padRibbon",
      method: "padRibbon",
      args: [c("controller/console/mainmenubuttonribbonhd.json")],
    },
    {
      name: "padMainmenu",
      method: "padMainmenu",
      args: [c("controller/console/mainmenupanelhd.json")],
    },
    {
      name: "padSettings",
      method: "padSettings",
      args: [c("controller/console/settingspanelhd.json")],
    },
    {
      name: "padCustomPage",
      method: "padCustomPage",
      args: [c("controller/console/videooptionspanelhd.json")],
    },
    { name: "riftPanel", method: "riftPanel", args: [c("controller/messagelogpanelhd.json")] },
    { name: "setRect", method: "setRect", args: [panel("Near", "Other"), "Near", -11, 22, 33, 44] },
    { name: "offscreen-default", method: "offscreen", args: [primitiveInput] },
    {
      name: "offscreen-names",
      method: "offscreen",
      args: [primitiveInput, { types: [], names: ["Named"] }],
    },
    {
      name: "offscreen-empty",
      method: "offscreen",
      args: [primitiveInput, { types: [], names: [] }],
    },
    {
      name: "offscreen-comments",
      method: "offscreen",
      args: [
        layout([
          widget(
            "Comment",
            '"nested": { "value": "escaped \\\" string" }, // } this brace is inert\n                "rect": { "x": 1 }, "fitToParent": true',
            "ButtonLegendWidget",
          ),
        ]),
      ],
    },
  ];
  const bad = (name, method, rel, from, to) => ({
    name,
    method,
    overrides: { [rel]: c(rel).replace(from, to) },
    reject: true,
  });
  tests.push(
    bad(
      "guard-frontend",
      "build",
      "controller/creditsmodalhd.json",
      "ButtonLegendWidget",
      "FixtureWidget",
    ),
    bad("guard-pcfront", "buildPcfront", "mainmenupanelhd.json", '"x": -1010', '"x": -1009'),
    bad(
      "guard-profile",
      "buildProfile",
      "controller/_profileconsolehd.json",
      '"XMediumFontSize": 30',
      '"XMediumFontSize": 31',
    ),
    bad(
      "guard-present",
      "buildPresent",
      "controller/tooltipspanelhd.json",
      '"screenEdgePadding": 50',
      '"screenEdgePadding": 51',
    ),
    bad(
      "guard-pad-root",
      "buildPadRoot",
      "soundoptionspanelhd.json",
      '"Row VoiceLanguage"',
      '"ChangedRow"',
    ),
    bad(
      "guard-ingame-existing",
      "buildIngame",
      "controller/horadriccubelayouthd.json",
      '"FixtureLegend"',
      '"close"',
    ),
    { name: "guard-cursor-count", method: "buildCursor", omitEntry: 0, reject: true },
    { name: "guard-glyph-count", method: "buildGlyphs", omitEntry: 3, reject: true },
    { name: "guard-cursor-magic", method: "buildCursor", corruptEntry: 0, reject: true },
    { name: "guard-glyph-format", method: "buildGlyphs", corruptFormat: 3, reject: true },
    {
      name: "guard-setRect",
      method: "setRect",
      args: [panel("Near"), "Absent", 1, 2, 3, 4],
      reject: true,
    },
    {
      name: "guard-riftPanel",
      method: "riftPanel",
      args: [
        c("controller/messagelogpanelhd.json").replace(
          '"name": "ScrollBar"',
          '"name": "Different"',
        ),
      ],
      reject: true,
    },
    { name: "guard-padSettings", method: "padSettings", args: ['"changed"'], reject: true },
    { name: "guard-padCustomPage", method: "padCustomPage", args: [panel("Other")], reject: true },
    {
      name: "guard-padBliss",
      method: "padBliss",
      args: [
        c("controller/console/characteroptionspanelhd.json").replace(
          '"action": "option2"',
          '"action": "changed"',
        ),
      ],
      reject: true,
    },
  );
  return tests;
}

const nativeProgram = String.raw`
import base64,contextlib,hashlib,io,json,os,sys,tempfile
os.environ.pop('DM_OVERLAY_MAC',None)
os.environ.pop('DM_CUSTOM_TEST_ROWS',None)
os.environ['MOVIE_FFMPEG']='0'
sys.path.insert(0,sys.argv[1]+'/dm/ios/tools')
import touch_layout as t
payload=json.load(sys.stdin)
names={'buildPcfront':'build_pcfront','buildPresent':'build_present','buildIngame':'build_ingame','buildPadRoot':'build_pad_root','buildProfile':'build_profile','buildCursor':'build_cursor','buildGlyphs':'build_glyphs','padInvRw':'pad_inv_rw','padPause':'pad_pause','padBossPct':'pad_boss_pct','padBliss':'pad_bliss','padRibbon':'pad_ribbon','padMainmenu':'pad_mainmenu','padSettings':'pad_settings','padCustomPage':'pad_custom_page','setRect':'set_rect','riftPanel':'_rift_panel'}
results={}
for test in payload['tests']:
    corpus=dict(payload['corpus']);corpus.update(test.get('overrides',{}))
    def read(romfs,rel,suffix=None):
        path='data/HD/Global/UI/Layouts/'+rel
        if suffix:
            rel=next(k for k in corpus if ('/ui/layouts/'+k).lower().endswith(suffix.lower()))
            path='data/HD/Global/UI/Layouts/'+rel
        return path,corpus[rel]
    t.read_layout=read
    rows=[r for i,r in enumerate(payload['rows']) if i!=test.get('omitEntry')]
    def entry(romfs,row):
        d=bytearray(base64.b64decode(payload['entries'][row[9]]))
        original=payload['rows'].index(row)
        if original==test.get('corruptEntry'):d[0]=0
        if original==test.get('corruptFormat'):d[4]=62
        return bytes(d)
    t._read_entry=entry
    fn=getattr(t,names.get(test['method'],test['method']))
    try:
        with tempfile.TemporaryDirectory() as out,contextlib.redirect_stdout(io.StringIO()):
            os.mkdir(out+'/data')
            with open(out+'/data/build_manifest.psv','w',encoding='utf-8') as f:
                f.write('# synthetic-only manifest\n'+'\n'.join('|'.join(r) for r in rows)+'\n')
            if test['method'].startswith('build'):
                result=fn(out,out,*test.get('args',[]))
                if isinstance(result,tuple):result=result[0]
                result={'map':[[k,base64.b64encode(open(v,'rb').read()).decode()] for k,v in result.items()]}
            elif test['method']=='offscreen':
                args=test['args'];result=fn(args[0],**(args[1] if len(args)>1 else {}))
            else:result=fn(*test.get('args',[]))
            results[test['name']]={'value':result}
    except (SystemExit,AssertionError,ValueError,KeyError,StopIteration) as e:
        results[test['name']]={'error':type(e).__name__+': '+str(e)}
print(json.dumps(results,ensure_ascii=False))
`;

export async function runTouchParity({ nativeOnly = false, corpus = createTouchCorpus() } = {}) {
  for (const [path, sha] of Object.entries(data.sourceSha256)) {
    assert.equal(
      createHash("sha256")
        .update(readFileSync(root + path))
        .digest("hex"),
      sha,
      "Pinned source changed: " + path,
    );
  }
  const sprites = createSpriteFixtures(),
    tests = requests(corpus);
  const native = spawnSync("python3", ["-c", nativeProgram, root], {
    input: JSON.stringify({ corpus, ...sprites, tests }),
    encoding: "utf8",
    maxBuffer: 32 * 1024 * 1024,
  });
  assert.equal(native.status, 0, native.stderr);
  const expected = JSON.parse(native.stdout);
  for (const test of tests) {
    const result = expected[test.name];
    assert.ok(result, "Missing Python result: " + test.name);
    if (test.reject) assert.ok(result.error, "Python must reject: " + test.name);
    else assert.ok(!result.error, test.name + " fixture failed native guard: " + result.error);
  }
  if (nativeOnly)
    return { layouts: Object.keys(corpus).length, cases: tests.length, nativeOnly: true };
  const touch = await import("../touch-layout.mjs");
  assert.deepEqual(touch.CLASS_STRIPS, data.CLASS_STRIPS, "CLASS_STRIPS");
  assert.deepEqual(touch.PAD_VENDOR, data.PAD_VENDOR, "PAD_VENDOR");
  let outputs = 0,
    bytes = 0;
  for (const test of tests) {
    const fixture = { ...corpus, ...test.overrides };
    const rows = sprites.rows.filter((_, i) => i !== test.omitEntry);
    const pipeline = touch.createTouchLayouts({
      rows,
      readLayout(rel, suffix = null) {
        if (suffix)
          rel = Object.keys(fixture).find((r) =>
            ("/ui/layouts/" + r).toLowerCase().endsWith(suffix.toLowerCase()),
          );
        assert.ok(Object.hasOwn(fixture, rel), "Missing synthetic layout: " + rel);
        return ["data/HD/Global/UI/Layouts/" + rel, fixture[rel]];
      },
      readEntry(row) {
        const value = new Uint8Array(Buffer.from(sprites.entries[row[9]], "base64"));
        const original = sprites.rows.indexOf(row);
        if (original === test.corruptEntry) value[0] = 0;
        if (original === test.corruptFormat) value[4] = 62;
        return value;
      },
    });
    const invoke = () =>
      test.method.startsWith("build")
        ? pipeline[test.method](...(test.args || []))
        : test.method === "offscreen"
          ? touch.offscreen(test.args[0], test.args[1]?.types, test.args[1]?.names)
          : touch[test.method](...(test.args || []));
    if (test.reject) {
      assert.throws(invoke, undefined, "JS must reject: " + test.name);
      continue;
    }
    const actual = invoke(),
      want = expected[test.name].value;
    if (want && Object.hasOwn(want, "map")) {
      assert.ok(actual instanceof Map, test.name + " must return Map");
      assert.deepEqual(
        [...actual.keys()],
        want.map.map(([key]) => key),
        test.name + " path/order",
      );
      for (const [path, encoded] of want.map) {
        const output = actual.get(path),
          expectedBytes = new Uint8Array(Buffer.from(encoded, "base64"));
        assert.ok(output instanceof Uint8Array, test.name + " must return Uint8Array: " + path);
        assert.deepEqual(output, expectedBytes, test.name + " byte parity: " + path);
        outputs++;
        bytes += output.byteLength;
      }
    } else assert.deepEqual(actual, want, test.name + " result parity");
  }
  return { layouts: Object.keys(corpus).length, cases: tests.length, outputs, bytes };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const report = await runTouchParity({ nativeOnly: process.argv.includes("--native-only") });
  console.log("touch parity " + JSON.stringify(report));
}
