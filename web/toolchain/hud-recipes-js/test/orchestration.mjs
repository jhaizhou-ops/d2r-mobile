// 聚焦完整 HUD 的合成对比；下方字体及 PNG 适配器使用实际打包的 Pillow 参考，不验证独立字体/PNG WASM。
// Focused whole-HUD synthetic comparison. Font and PNG adapters below use the
// actual bundled Pillow oracle. This does NOT verify standalone font/PNG WASM.
import process from "node:process";
import { Buffer } from "node:buffer";
import assert from "node:assert/strict";
import { readFileSync, copyFileSync, mkdtempSync, rmSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { pathToFileURL } from "node:url";
import { createHash } from "node:crypto";
import { createImageKernels } from "../../image-kernels-rust/kernels.mjs";
import * as art from "../../art-kernels/kernels.mjs";
import { generateHud } from "../recipe.mjs";
const source = new URL("../../asset-generation-spike/public/vendor/pyodide/", import.meta.url);
const generated = new URL("../../asset-generation-spike/public/generated/", import.meta.url);
const vendor = JSON.parse(readFileSync(new URL("vendor-manifest.json", source)));
const dir = mkdtempSync(join(tmpdir(), "d2r-hud-recipe-"));
try {
  for (const [name, sha256] of Object.entries(vendor.sha256)) {
    const raw = readFileSync(new URL(name, source));
    assert.equal(createHash("sha256").update(raw).digest("hex"), sha256);
    copyFileSync(new URL(name, source), join(dir, name));
  }
  const { loadPyodide } = await import(pathToFileURL(join(dir, "pyodide.mjs")));
  const py = await loadPyodide({ indexURL: dir + "/" });
  await py.loadPackage("pillow");
  py.registerJsModule("art_kernel_bridge", art);
  for (const name of [
    "art_pixels.py",
    "hud.py",
    "hud_original.py",
    "hud_design_original.py",
    "hud_support_original.py",
  ])
    py.FS.writeFile("/home/pyodide/" + name, readFileSync(new URL(name, generated)));
  const paths = process.argv.slice(2);
  assert.equal(paths.length, 2, "Supply local test TTF and OTF paths");
  paths.forEach((path, i) => py.FS.writeFile("/font" + i, readFileSync(path)));
  py.globals.set("_metadata_text", readFileSync(new URL("hud-metadata.json", generated), "utf8"));
  const fixture = JSON.parse(
    py.runPython(`
import base64, hashlib, io, json
from PIL import Image, ImageDraw, ImageFont, features
assert Image.__version__=='10.2.0' and features.version('freetype2')=='2.6.0' and not features.check('raqm')
import hud
metadata=json.loads(_metadata_text)
patch=json.loads(metadata['patch'])
def skeleton(p):
 if p is None: return {'name':'Authored','fields':{}}
 if 'value' in p: return p['value']
 if 'dict' in p: return {k:skeleton(p['dict'][k]) if k in p['dict'] else 0 for k in p['order']}
 if 'list' in p:
  result=[{} for _ in range(1+max((x.get('base',-1) for x in p['list']),default=-1))]
  for x in p['list']:
   if 'base' in x: result[x['base']]=skeleton(x['patch'])
  return result
 raise ValueError('patch')
layout=skeleton(patch)
names={0:'ExpBar',1:'HealthBall',2:'ManaBall',3:'Overlay',4:'AuthoredUnused',16:'Belt',17:'LevelUpFlourish',18:'LevelUpButton',**{i:f'SkillButton{i-9}' for i in range(10,16)}}
for i,name in names.items():layout['children'][i]['name']=name
for i in (1,2):layout['children'][i]['children'][0]['name']='ValueDisplay'
layout['children'][16]['children'][0]['name']='HotkeyLabelTemplate'
b64=lambda raw:base64.b64encode(raw).decode()
images={}
for i,name in enumerate(('front_panel','levelupbutton','greater_healing_potion')):
 w,h=(128,64) if i==0 else (32,24)
 raw=bytes(v for y in range(h) for x in range(w) for v in ((x*7+i*41)%256,(y*11+71)%256,(x+y+39)%256,255))
 images[name]={'size':[w,h],'rgba':b64(raw)}
assets=[];atlases={}
for i in range(9):
 name=f'dm_native_skill_{i}.png';image=Image.new('RGBA',(16,8),(180+i*4,180,180,255));out=io.BytesIO();image.save(out,'PNG');raw=out.getvalue();atlases[name]=b64(raw)
 assets.append(dict(asset=name,atlas_px=[16,8],frame_width=7,frame_pitch=8,frame_count=2,decoded_png_sha256=hashlib.sha256(raw).hexdigest()))
metadata['manifest']=json.dumps({'assets':assets})
bundle=dict(metadata=metadata,layout=json.dumps(layout),fonts={name:b64(open('/font'+str(i),'rb').read()) for i,name in enumerate(('blizzardglobal-v5_81.ttf','exocetblizzardot-medium.otf'))},images=images,atlases=atlases)
options=dict(mode='switch-upscaled',metadataSha256={k:hashlib.sha256(v.encode()).hexdigest() for k,v in metadata.items()})
expected=hud.generate(json.dumps(bundle).encode(),options)
json.dumps(dict(bundle=bundle,options=options,expected={name:b64(raw) for name,raw in expected['files'].items()}))
`),
  );
  const rawResult = (script) => {
    const proxy = py.runPython(script);
    try {
      return new Uint8Array(proxy.toJs()).slice();
    } finally {
      proxy.destroy();
    }
  };
  const codec = {
    encodeRgba(raw, width, height) {
      py.globals.set("_raw", raw);
      py.globals.set("_w", width);
      py.globals.set("_h", height);
      return rawResult(
        "_out=io.BytesIO();Image.frombytes('RGBA',(_w,_h),bytes(_raw)).save(_out,'PNG');_out.getvalue()",
      );
    },
    decodeRgba(raw) {
      py.globals.set("_raw", raw);
      const [width, height] = JSON.parse(
        py.runPython(
          "_image=Image.open(io.BytesIO(bytes(_raw))).convert('RGBA');json.dumps(_image.size)",
        ),
      );
      return { width, height, channels: 4, data: rawResult("_image.tobytes()") };
    },
  };
  const font = {
    open(raw, size) {
      py.globals.set("_raw", raw);
      py.globals.set("_size", size);
      py.runPython("_active_font=ImageFont.truetype(io.BytesIO(bytes(_raw)),_size)");
    },
    close() {
      py.runPython("_active_font=None");
    },
    measure(text, { anchor = "la", stroke = 0 } = {}) {
      py.globals.set("_text", text);
      py.globals.set("_anchor", anchor);
      py.globals.set("_stroke", stroke);
      return JSON.parse(
        py.runPython(
          "json.dumps(dict(bbox=_active_font.getbbox(_text,anchor=_anchor,stroke_width=_stroke),length=_active_font.getlength(_text),metrics=_active_font.getmetrics()))",
        ),
      );
    },
    draw(raw, width, height, text, options) {
      py.globals.set("_raw", raw);
      py.globals.set("_w", width);
      py.globals.set("_h", height);
      py.globals.set("_text", text);
      py.globals.set("_opts", JSON.stringify(options));
      return rawResult(`
_o=json.loads(_opts)
_image=Image.frombytes('RGBA' if _o['channels']==4 else 'L',(_w,_h),bytes(_raw))
_ink=lambda value:tuple(value) if isinstance(value,list) else value
ImageDraw.Draw(_image).text((_o['x'],_o['y']),_text,font=_active_font,fill=_ink(_o['fill']),anchor=_o.get('anchor','la'),stroke_width=_o.get('stroke',0),stroke_fill=_ink(_o.get('strokeFill',_o['fill'])))
_image.tobytes()
`);
    },
  };
  const kernels = await createImageKernels(
    readFileSync(new URL("../../image-kernels-rust/dist/image-kernels.wasm", import.meta.url)),
  );
  const result = await generateHud(
    new TextEncoder().encode(JSON.stringify(fixture.bundle)),
    fixture.options,
    { codec, kernels, font },
  );
  assert.deepEqual(Object.keys(result.files).sort(), Object.keys(fixture.expected).sort());
  for (const [name, bytes] of Object.entries(result.files)) {
    const expected = Buffer.from(fixture.expected[name], "base64"),
      actual = Buffer.from(bytes);
    if (!actual.equals(expected)) {
      const at = actual.findIndex((byte, i) => byte !== expected[i]);
      throw new Error(
        `${name}: ${actual.length}/${expected.length} bytes; first mismatch ${at}: ${actual.subarray(Math.max(0, at - 80), at + 160).toString()}/${expected.subarray(Math.max(0, at - 80), at + 160).toString()}`,
      );
    }
  }
  console.log(
    JSON.stringify({
      files: Object.keys(result.files).length,
      exact: true,
      scope: "JS HUD orchestration + image WASM vs bundled Pillow; oracle font/PNG adapters",
      gameInputs: false,
      standaloneFontPngVerified: false,
    }),
  );
} finally {
  rmSync(dir, { recursive: true, force: true });
}
