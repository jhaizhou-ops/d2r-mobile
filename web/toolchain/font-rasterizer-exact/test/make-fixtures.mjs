// 验证 make-fixtures 的约定与边界；依据 web/toolchain/font-rasterizer-exact/test 及本文件的测试断言。
// Verify make-fixtures contracts and boundaries against web/toolchain/font-rasterizer-exact/test and this file's assertions.
import process from "node:process";
import { Buffer } from "node:buffer";
// 使用实际打包的 Pillow/FreeType WASM 作为窄范围参考，不使用浏览器或网络。提供本地 TTF 及 CFF/OTF 路径，绝不打包或下载游戏字体。
// Narrow actual bundled Pillow/FreeType WASM oracle; no browser or network.
// Supply local TTF and CFF/OTF paths. Never package or download game fonts.
import { readFileSync, copyFileSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { tmpdir } from "node:os";
import { pathToFileURL } from "node:url";
import { createHash } from "node:crypto";
if (process.argv.length !== 4)
  throw new Error("Usage: node test/make-fixtures.mjs path/to/font.ttf path/to/font.otf");
const paths = process.argv.slice(2).map((path) => resolve(path));
const source = new URL("../../asset-generation-spike/public/vendor/pyodide/", import.meta.url);
const manifest = JSON.parse(readFileSync(new URL("vendor-manifest.json", source)));
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const dir = mkdtempSync(join(tmpdir(), "d2r-font-oracle-"));
try {
  for (const [name, sha256] of Object.entries(manifest.sha256)) {
    const raw = readFileSync(new URL(name, source));
    if (hash(raw) !== sha256) throw new Error("Oracle vendor digest mismatch");
    copyFileSync(new URL(name, source), join(dir, name));
  }
  const { loadPyodide } = await import(pathToFileURL(join(dir, "pyodide.mjs")));
  const python = await loadPyodide({ indexURL: dir + "/" });
  await python.loadPackage("pillow");
  const fonts = paths.map((path, i) => {
    const bytes = readFileSync(path);
    if (bytes.length < 1 || bytes.length > 16777216) throw new Error("Font size out of bounds");
    python.FS.writeFile(`/font${i}`, bytes);
    return { path, sha256: hash(bytes), bytes: bytes.length };
  });
  const cases = JSON.parse(
    python.runPython(`
import base64, json
from PIL import Image, ImageFont, ImageDraw, features
assert Image.__version__ == '10.2.0'
assert features.version('freetype2') == '2.6.0'
assert not features.check('raqm')
enc = lambda raw: base64.b64encode(bytes(raw)).decode()
cases=[]
# Deliberately probe negative bearings, kerning, accents, non-Latin, whitespace,
# fractional positions, all production sizes/anchors and both outline engines.
for fi in range(2):
 for size in (26,30,34,48,220):
  font=ImageFont.truetype('/font'+str(fi),size)
  for text,anchor,stroke,start in [('AV','la',0,(0,0)),('jg','ls',1,(0,0)),('生命: 55 / 55','mb',1,(0,0)),('áéñ Жизнь','mm',0,(.5,.5)),('8','la',2,(0,0)),(' ','ls',1,(-.5,-.5)),('','la',0,(0,0)),('jÁ','mm',2,(.25,.75))]:
   mask,offset=font.getmask2(text,mode='L',anchor=anchor,stroke_width=stroke,start=start)
   cases.append(dict(op='mask',font=fi,size=size,text=text,anchor=anchor,stroke=stroke,start=start,
    metrics=font.getmetrics(),length=font.getlength(text),bbox=font.getbbox(text,anchor=anchor,stroke_width=stroke),
    maskSize=mask.size,offset=offset,mask=enc(mask)))
 for ci,(mode,bg,fill,sfill,stroke,pos,anchor) in enumerate([
   ('RGBA',(91,12,234,0),(226,206,160,255),(30,22,12,255),1,(8,6),'la'),
   ('RGBA',(31,72,19,128),(0,0,0,170),(10,8,6,255),2,(8.5,54.5),'ls'),
   ('RGBA',(31,72,19,1),(226,206,160,0),(30,22,12,190),2,(-2.5,6.25),'la'),
   ('RGBA',(0,0,0,0),(0,0,0,190),(0,0,0,190),2,(70.5,40.5),'mm'),
   ('RGBA',(20,30,40,255),(226,206,160,255),(10,8,6,255),0,(95,48),'mb'),
   ('L',0,255,0,0,(8,6),'la'),
   ('L',73,170,35,2,(-2.5,6),'la')]):
  font=ImageFont.truetype('/font'+str(fi),34); text='ÁV jg / 8'
  im=Image.new(mode,(192,96),bg)
  before=im.tobytes()
  ImageDraw.Draw(im).text(pos,text,font=font,fill=fill,stroke_width=stroke,stroke_fill=sfill,anchor=anchor)
  cases.append(dict(op='draw',font=fi,size=34,text=text,anchor=anchor,stroke=stroke,pos=pos,width=192,height=96,channels=4 if mode=='RGBA' else 1,fill=fill,strokeFill=sfill,before=enc(before),pixels=enc(im.tobytes())))
json.dumps(cases,separators=(',',':'))
`),
  );
  const result = {
    backend: {
      pillow: "10.2.0",
      freetype: "2.6.0",
      layout: "BASIC",
      raqm: false,
      pyodide: "0.27.7",
    },
    fonts,
    cases,
  };
  const raw = JSON.stringify(result);
  writeFileSync(new URL("fixtures.json", import.meta.url), raw);
  const meta = {
    ...result.backend,
    fonts,
    cases: cases.length,
    bytes: Buffer.byteLength(raw),
    sha256: hash(raw),
  };
  writeFileSync(
    new URL("fixtures-meta.json", import.meta.url),
    JSON.stringify(meta, null, 2) + "\n",
  );
  console.log(JSON.stringify(meta));
} finally {
  rmSync(dir, { recursive: true, force: true });
}
