// 无游戏文件的窄范围原生对照：文本字节、数值格式、库存表与输入守卫。
// Narrow native parity without game files: exact text, numeric formatting, inventory tables and input guards.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import {
  INVENTORY_SOURCE_SHA256,
  DROP_TIERS,
  mercenaryRects,
  mercenaryTransform,
  pouchWidgets,
  invLayout,
  troveSlotsLayout,
  bankLayout,
  vaultTab,
  overlayLayout,
  patchInventory,
  addLight,
  variantPath,
  selectDropModels,
  parseInventoryJson,
  pythonJsonDumps,
} from "../inventory.mjs";
import { troveSlots } from "../../remaining-art-js/recipes.mjs";
const md5 = (bytes) => createHash("md5").update(bytes).digest("hex");
const tools = fileURLToPath(new URL("../../../../dm/ios/tools/", import.meta.url));

export function inventoryFixtures() {
  const names = [
    "head",
    "torso",
    "right_arm",
    "left_arm",
    "neck",
    "gloves",
    "right_hand",
    "belt",
    "left_hand",
    "feet",
  ];
  const player =
    '{ "children": [\n' +
    names
      .map(
        (name, i) =>
          ` { "type": "InventorySlotWidget", "name": "slot_${name}", "fields": { "rect": { "x": ${200 + i * 30}, "y": ${50 + i * 20}, "width": 98, "height": 196 }, "backgroundFilename": "Authored/${name}" } }`,
      )
      .join(",\n") +
    "\n]}";
  const band = [
    "CharacterName",
    "HireTypeText",
    "XpProgress",
    "NextLevelXP",
    "CurrentXP",
    "Skill0",
    "Skill1",
    "Skill2",
  ];
  const stats = [
    "HPTitle",
    "HPStat",
    "StrengthTitle",
    "StrengthStat",
    "DexTitle",
    "DexStat",
    "DamageTitle",
    "Damage",
    "ArmorClassTitle",
    "ArmorClass",
    "FireResistanceTitle",
    "FireText",
    "ColdResistanceTitle",
    "ColdText",
    "LightningResistanceTitle",
    "LightningText",
    "PoisonResistanceTitle",
    "PoisonText",
  ];
  const statRows = [902, 903, 905, 994, 993, 995, 1088, 1089, 1090, 1183, 1185, 1279];
  const rows = band.map(
    (name, i) =>
      ` { "type": "TextBoxWidget", "name": "${name}", "fields": { "rect": { "x": -2, "y": ${650 + i}, "width": 300, "height": 50 }, "v": "bottom", "test": "escaped \\" brace }", // } comment\n "kept": true } }`,
  );
  rows.push(
    ...stats.map(
      (name, i) =>
        ` { "type": "TextBoxWidget", "name": "${name}", "fields": { "rect": { "x": 2, "y": ${statRows[i % statRows.length]}, "width": 300, "height": 50 } } }`,
    ),
  );
  rows.push(
    ...names
      .slice(0, 4)
      .map(
        (name) =>
          ` { "type": "InventorySlotWidget", "name": "slot_${name}", "fields": { "rect": { "x": 20, "y": 30, "width": 98, "height": 196 }, "navigation": { "down": { "name": "old" }, "text": "{" } } }`,
      ),
  );
  const mercenary = '{ "children": [\n' + rows.join(",\n") + "\n]}";
  const slots = Object.keys(troveSlots());
  const containers = ["advancedstash_gems", "advancedstash_materials", "advancedstash_runes"].map(
    (name, n) => {
      const items = slots
        .filter((_, i) => i % 3 === n)
        .map(
          (code) =>
            ` { "type": "AdvancedStashSlotWidget", "name": "${code}", "fields": { "rect": { "x": 1, "y": 2, "width": 98, "height": 98 }, "itemCode": "${code}", }, },`,
        )
        .join("\n");
      return ` { "type": "Widget", "name": "${name}", "children": [\n${items}\n] },`;
    },
  );
  const widgets = [
    "gold_amount",
    "gold_withdraw",
    "StashNavigation",
    "ShowPreviousLadderSeasonToggle",
    "TouchStashLeft",
    "TouchStashRight",
    "close",
    "advancedstash_horadriccube",
  ].map(
    (name) =>
      ` { "type": "Widget", "name": "${name}", "fields": { "rect": { "x": -10, "y": 4 } }, "children": [] },`,
  );
  const tabs = ` { "type": "TabBarWidget", "name": "BankTabs", "fields": {
 "tabCount": 5,
 "inactiveFrames": [ 0, 0, 0, 0, 0 ],
 "activeFrames": [ 1, 1, 1, 1, 1 ],
 "disabledFrames": [-1, 0, 0, 0, 0 ],
 "textStrings": [ "@personal", "@shared", "@gems", "@materials", "@runes" ],
 "tabRightIndicatorPosition": "$HalfPanelRightTabButtonPromptPosition",
 } },`;
  const bank =
    '{ "cellCount": { "x": 10, "y": 10 }, "children": [\n' +
    widgets.join("\n") +
    "\n" +
    containers.join("\n") +
    "\n" +
    tabs +
    "\n    ]\n}\n";
  const overlay =
    '{ "children": [\n { "type": "ImageWidget", "name": "Background", "fields": { "filename": "Synthetic" } },\n { "type": "TabBarWidget", "name": "Tabs", "fields": { "rect": { "x": 0 } } },\n]}\n';
  const inventory = new Uint8Array(4 + 27 * 240),
    view = new DataView(inventory.buffer);
  for (const row of [10, 25]) inventory.set([10, 10], 4 + row * 240 + 16);
  for (const row of [11, 26])
    for (let k = 4; k < 10; k++)
      for (let i = 0; i < 4; i++) view.setInt32(4 + row * 240 + 0x28 + k * 0x14 + i * 4, -1, true);
  for (let k = 4; k < 10; k++)
    for (let i = 0; i < 5; i++)
      view.setInt32(4 + 0x28 + k * 0x14 + i * 4, 330 + k * 17 + i * 11, true);
  return { player, mercenary, bank, overlay, inventory };
}

export function runInventoryParity() {
  for (const [name, expected] of Object.entries(INVENTORY_SOURCE_SHA256))
    assert.equal(
      createHash("sha256")
        .update(readFileSync(resolve(tools, name)))
        .digest("hex"),
      expected,
      `source pin ${name}`,
    );
  const f = inventoryFixtures(),
    requests = [];
  const add = (op, args, rejects = false) => requests.push({ op, args, rejects });
  add("pouchWidgets", []);
  add("mercenaryRects", [f.player]);
  add("mercenaryTransform", [f.mercenary, f.player]);
  add("mercenaryTransform", [
    f.mercenary.replaceAll("\n", "\r\n"),
    f.player.replaceAll("\n", "\r\n"),
  ]);
  add("mercenaryTransform", [f.mercenary.replace('"CharacterName"', '"NotName"'), f.player], true);
  add("mercenaryTransform", [f.mercenary.replace('"v": "bottom"', '"v": "top"'), f.player], true);
  add("mercenaryTransform", [f.mercenary.replace('"y": 902', '"y": 901'), f.player], true);
  add("mercenaryTransform", [f.mercenary, f.player.replace('"x": 200', '"x": -1')], true);
  for (const text of [
    '{ "children": [\n    ] }\n',
    '{ "children": [\r\n\t    ] }\r\n',
    '{ "children": [ {},\n    ] }\u0085',
  ])
    add("invLayout", [text]);
  add("invLayout", ['{ "name": "TouchPouchBox", "children": [] }'], true);
  add("invLayout", ["[]"], true);
  add("troveSlotsLayout", [f.bank]);
  add("troveSlotsLayout", [f.bank.replace('"itemCode": "gcw"', '"itemCode": "nope"')], true);
  add("troveSlotsLayout", [f.bank.replace('"name": "gcw"', '"name": "other"')], true);
  add("bankLayout", [f.bank]);
  add("bankLayout", [f.bank.replaceAll("\n", "\r\n")]);
  add("bankLayout", [
    f.bank.replaceAll(
      / \{ "type": "Widget", "name": "(?:TouchStashLeft|TouchStashRight|close)"[^\n]*\n/g,
      "",
    ),
  ]);
  add("bankLayout", [f.bank.replace('"x": 10, "y": 10', '"x": 11, "y": 10')], true);
  add("bankLayout", [f.bank.replace('"name": "gold_amount"', '"name": "not_gold"')], true);
  add(
    "bankLayout",
    [
      f.bank.replace(
        '"name": "gold_amount", "fields": { "rect": { "x": -10, "y": 4 } }',
        '"name": "gold_amount", "fields": {}',
      ),
    ],
    true,
  );
  add("vaultTab", [f.bank]);
  add("vaultTab", [f.bank.replace('"tabCount": 5,', '"tabCount": 4,')], true);
  add(
    "vaultTab",
    [f.bank.replace('"name": "BankTabs",', '"name": "BankTabs",' + " ".repeat(2501))],
    true,
  );
  add("overlayLayout", [f.overlay]);
  add(
    "overlayLayout",
    [f.overlay.replace('"name": "Background"', '"name": "NotBackground"')],
    true,
  );
  add("overlayLayout", [f.overlay.slice(0, f.overlay.indexOf('"rect"'))], true);
  add("patchInventory", [Buffer.from(f.inventory).toString("base64"), "synthetic/inventory.bin"]);
  const invalid = f.inventory.slice();
  invalid[4 + 25 * 240 + 16] = 9;
  add("patchInventory", [Buffer.from(invalid).toString("base64"), "synthetic/inventory.bin"], true);
  invalid[4 + 25 * 240 + 16] = 10;
  invalid[4 + 11 * 240 + 0x28 + 4 * 0x14] = 0;
  add("patchInventory", [Buffer.from(invalid).toString("base64"), "synthetic/inventory.bin"], true);
  for (const tier of [1, 2]) {
    add("variantPath", ["weapon", "Authored/Ä😀", String(tier)]);
    for (const extra of [
      '"normal":1.0,"negative":-0.0,"small":1e-7,"large":1e20',
      '"2":0,"1":1,"0":2,"__proto__":{},"big":123456789012345678901234567890',
      '"unicode":"中文😀\\u007f\\ud800","same":{},"same":1',
    ]) {
      add("addLight", [
        `{"type":"UnitDefinition","dependencies":{},"entities":[],${extra}}`,
        DROP_TIERS[tier],
        `data/hd/items/weapon/zswg/${tier}__authored.json`,
      ]);
    }
    add("addLight", [
      `{"type":"UnitDefinition","dependencies":{"particles":[{"path":"${DROP_TIERS[tier].toUpperCase()}"}]},"entities":[]}`,
      DROP_TIERS[tier],
      `data/hd/items/weapon/zswg/${tier}__authored.json`,
    ]);
  }
  add(
    "addLight",
    ['{"type":"Other","dependencies":{},"entities":[]}', DROP_TIERS[1], "invalid"],
    true,
  );
  for (const model of [
    '{"type":"UnitDefinition","dependencies":[],"entities":[]}',
    '{"type":"UnitDefinition","dependencies":{"particles":null},"entities":[]}',
    '{"type":"UnitDefinition","dependencies":{"particles":[{"path":null}]},"entities":[]}',
    '{"type":"UnitDefinition","dependencies":{},"entities":{}}',
  ])
    add("addLight", [model, DROP_TIERS[1], "invalid"], true);
  add("pythonJsonMutate", ['{"10":true,"2":false,"filename":"old","rect":{"x":1.0}}']);
  const values = [
    '{"10":true,"2":false,"a":[0,0.0,-0.0,1.234e-5,1e-4,1e15,1e16,1e20,1e30,5e-324,1.7976931348623157e308,NaN,Infinity,-Infinity],"unicode":"\\u0000\\b\\f\\n\\r\\t\\u007f中文😀\\ud800"}',
    '{"a":{},"a":1,"b":[1],"b":{"2":{},"1":["a"]}}',
  ];
  for (const text of values)
    for (const options of [
      {},
      { indent: 2 },
      { indent: 0 },
      { indent: "\t", ensureAscii: false },
      { separators: [",", ":"], ensureAscii: false },
    ])
      add("pythonJsonDumps", [text, options]);
  const code = String.raw`
import base64,hashlib,json,os,sys,tempfile
sys.path.insert(0,sys.argv[1])
import merc_slots as M,pouch_layout as P,wide_stash as W,drop_light as D
functions={'pouchWidgets':P.widgets,'mercenaryRects':M.rects,'mercenaryTransform':M.transform,'invLayout':P.inv_layout,'troveSlotsLayout':W.trove_slots,'bankLayout':W.bank_layout,'vaultTab':W.vault_tab,'overlayLayout':W.overlay_layout,'addLight':D.add_light,'variantPath':D.variant_path}
answers=[]
for req in json.load(sys.stdin):
 try:
  op=req['op']; args=req['args']
  if op=='pythonJsonMutate':
   value=json.loads(args[0]); del value['filename']; value['normalColor']=[1.0,0.0,1.0,0.5]; value['children']=[]
   result=json.dumps(value,ensure_ascii=False,indent=2)
  elif op=='pythonJsonDumps':
   options={ {'ensureAscii':'ensure_ascii'}.get(k,k):v for k,v in args[1].items()}
   result=json.dumps(json.loads(args[0]),**options)
  elif op=='patchInventory':
   binary=base64.b64decode(args[0]); path=args[1]
   old_entry,old_read,old_sprites,old_copy,old_bank,old_overlay=W._entry,W.touch_layout.read_layout,W.SPRITES,W.shutil.copyfile,W.bank_layout,W.overlay_layout
   try:
    W._entry=lambda *unused:(path,binary); W.touch_layout.read_layout=lambda *unused:('layout','{}'); W.SPRITES={}; W.shutil.copyfile=lambda *unused:None; W.bank_layout=W.overlay_layout=lambda s:s
    with tempfile.TemporaryDirectory() as directory:
     W.build('unused',directory,{})
     result=base64.b64encode(open(os.path.join(directory,'ws_inventory.bin'),'rb').read()).decode()
   finally: W._entry,W.touch_layout.read_layout,W.SPRITES,W.shutil.copyfile,W.bank_layout,W.overlay_layout=old_entry,old_read,old_sprites,old_copy,old_bank,old_overlay
  else: result=functions[op](*args)
  answers.append({'result':result})
 except (Exception,SystemExit) as error: answers.append({'error':str(error)})
print(json.dumps(answers,ensure_ascii=True))
`;
  const native = spawnSync("python3", ["-c", code, tools], {
    input: JSON.stringify(requests),
    encoding: "utf8",
    maxBuffer: 20 * 1024 * 1024,
  });
  assert.equal(native.status, 0, native.stderr);
  const answers = JSON.parse(native.stdout);
  const functions = {
    pouchWidgets,
    mercenaryRects,
    mercenaryTransform,
    invLayout,
    troveSlotsLayout,
    bankLayout,
    vaultTab,
    overlayLayout,
    variantPath,
  };
  for (let i = 0; i < requests.length; i++) {
    const { op, args, rejects } = requests[i];
    let result, error;
    try {
      if (op === "patchInventory")
        result = Buffer.from(
          patchInventory(Uint8Array.from(Buffer.from(args[0], "base64")), args[1]),
        ).toString("base64");
      else if (op === "addLight") result = addLight(...args, { md5 });
      else if (op === "pythonJsonMutate") {
        const object = parseInventoryJson(args[0]);
        delete object.filename;
        object.normalColor = parseInventoryJson("[1.0,0.0,1.0,0.5]");
        object.children = [];
        result = pythonJsonDumps(object, { ensureAscii: false, indent: 2 });
      } else if (op === "pythonJsonDumps")
        result = pythonJsonDumps(parseInventoryJson(args[0]), args[1]);
      else result = functions[op](...args);
    } catch (failure) {
      error = failure.message;
    }
    if (rejects) {
      assert.ok(Object.hasOwn(answers[i], "error"), `native expected rejection ${i} ${op}`);
      assert.notEqual(error, undefined, `JS expected rejection ${i} ${op}`);
      if (
        answers[i].error.startsWith("wide_stash") ||
        answers[i].error.startsWith("merc_slots") ||
        answers[i].error.startsWith("pouch_layout")
      )
        assert.equal(error, answers[i].error, `guard message ${i} ${op}`);
    } else {
      assert.equal(answers[i].error, undefined, `native ${i} ${op}: ${answers[i].error}`);
      assert.equal(error, undefined, `JS ${i} ${op}: ${error}`);
      assert.deepEqual(result, answers[i].result, `case ${i} ${op}`);
    }
  }
  const manifest = [
    "weapon/authored",
    "armor/authored",
    "misc/rune/el",
    "misc/potion/red",
    "misc/key/a",
  ]
    .map((asset) => Array(9).fill("").join("|") + "|data:data/hd/items/" + asset + ".json|")
    .join("\n");
  const selection = selectDropModels(manifest, [
    JSON.stringify([
      { a: { asset: "Authored" } },
      { b: { normal: "rune/el", uber: "potion/red", ultra: "missing" } },
      { c: { asset: "key/a" } },
    ]),
  ]);
  assert.deepEqual(
    selection.models.map((item) => [item.root, item.asset]),
    [
      ["weapon", "authored"],
      ["misc", "key/a"],
      ["misc", "rune/el"],
    ],
  );
  assert.equal(selection.skipped, 1);
  console.log(
    `inventory parity: ${requests.length} native cases plus manifest selection and ${Object.keys(INVENTORY_SOURCE_SHA256).length} source pins passed`,
  );
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url))
  runInventoryParity();
