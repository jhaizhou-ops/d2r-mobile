#!/usr/bin/env node
/**
 * CPython 与手写 JS 的窄范围参考对比，使用自编类似游戏的合成输入，不是真实游戏证据。Python 仅用于测试，不是生产载荷。仓库不含源文本，因此触控检查使用刻意带哈希的样例哨兵；篡改文本仍以真实 SHA-1 校验并拒绝。
 * Narrow CPython-vs-handwritten-JS oracle. Authored synthetic game-like inputs,
 * not real title evidence. Python is test-only and never a production payload.
 * Source text is unavailable in this repository, so touch checks use deliberate
 * hash-bearing fixture sentinels; tampered text still uses real SHA-1 and fails.
 */
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { createAnnotations, annotationSources } from "../annotations.mjs";
const root = fileURLToPath(new URL("../../../../", import.meta.url));
// 非 BMP 字符使用 Python 转义，避免 Bun 1.4.3 将 raw 模板字面量改写为 JS 专用转义。
// Use Python escapes for non-BMP characters; Bun 1.4.3 rewrites raw-template literals to JS-only escapes.
const program = String.raw`
import os,sys,json,base64,csv,io,tempfile,contextlib,hashlib
os.environ['MOVIE_FFMPEG']='0'
sys.path.insert(0,sys.argv[1]+'/dm/ios/tools')
import touch_layout,touch_strings,dm_i18n,annot_strings,annot_drop
asset={}; strings={}; tables={}
STR='data/local/lng/strings/'; EX='data/global/excel/'
def add(file,key,zh=None,es=None,mx=None,en=None):
    row={'id':len(strings.setdefault(file,[]))+100,'Key':key,'enUS':en or 'Name '+key,'zhCN':zh if zh is not None else '名'+key,'esES':es if es is not None else '[fs]Nombre '+key,'esMX':mx if mx is not None else '[fs]Nombre '+key}
    strings[file].append(row);return row
for file in ['item-names.json','item-runes.json','chinese-overlay.json','monsters.json','npcs.json','skills.json','levels.json','shrines.json','item-modifiers.json','ui.json','ui-controller.json']: strings[file]=[]
for file, items in touch_strings.TABLE.items():
    for key in items:
        row=add(file,key)
        for lang,tab in [('zhCN',touch_strings.TABLE)]+list(touch_strings.EXTRA):
            if key in tab.get(file,{}): row[lang]=('%s\n命中值' if key in touch_strings.FILE_ONLY else '\ue013' if key=='ControllerInteractButton' else 'HASH:'+tab[file][key][0]+':'+lang+':'+key)
add('ui.json','untouched','原文\n换行\\制表\t\U0001f600','[fs]Intacto')
for i in range(1,34):
    key='r%02d'%i; en='Rune'+chr(65+(i-1)//26)+chr(65+(i-1)%26)
    add('item-runes.json',key,'符文'+str(i),'[fs]Runa '+en,'[fs]Runa '+en)
    add('item-runes.json',key+'L','符'+str(i),en,en,en)
for i,name in enumerate(['冰球','长长长长长名','技能\U0001f600','技能四字']): add('skills.json','skillname'+str(i),name,['Orbe','A skill name that is long','Skill\U0001f600','Cuatro'][i])
add('item-modifiers.json','toa','赦免','[ms]Absolución','[ms]Absolución MX')
weapons=[]; armor=[]; misc=[]
def base(tab,code,type,norm=None,uber='',ultra='',**extra):
    row=dict(code=code,type=type,namestr=code,spawnable='1',quest='',normcode=norm or code,ubercode=uber,ultracode=ultra,speed='0',mindam='10',maxdam='99',**{'2handmindam':'','2handmaxdam':''},minac='77',maxac='111')
    row.update(extra);tab.append(row);add('item-names.json',code)
base(weapons,'7wa','axe',norm='wax',ultra='7wa',speed='10')
for code in ['ama','amf','ob1']:base(weapons,code,'javelin' if code!='ob1' else 'orb')
base(weapons,'throwpot','tpot');base(weapons,'quest','axe',quest='1');base(weapons,'disabled','axe',spawnable='0')
for code,type in [('xtp','tors'),('urn','helm'),('ci3','circ'),('glv','glov'),('glc','glov'),('shield','pala')]:base(armor,code,type,speed='5' if type=='tors' else '10' if type=='pala' else '0')
def miscadd(code,type='misc',**extra):
    row={'code':code,'namestr':code,'type':type,'name':code};row.update(extra);misc.append(row)
    if not any(r['Key']==code for r in strings['item-runes.json']):add('item-names.json',code)
for p,t in [('hp','hpot'),('mp','mpot')]:
    for i in range(1,6):miscadd(p+str(i),t)
for code in ['tsc','aqv','cqv','rvs','rvl','isc','yps']:miscadd(code)
for row in strings['item-names.json']:
    if row['Key']=='isc':row.update(zhCN='鉴定卷轴',esES='[ms]Pergamino de identificación',esMX='[ms]Pergamino de identificación')
    if row['Key']=='yps':row.update(zhCN='解毒药水',esES='[fs]Poción antídoto',esMX='[fs]Poción antídoto')
    if row['Key']=='rvs':row.update(esES='[fs]Poción pequeña',esMX='[fs]Poción parcial')
for type in annot_drop.GEM_COLOR:
    for grade in range(5):
        code=('sk'+['c','f','u','l','z'][grade]) if type=='gemz' else 'g'+['c','f','s','l','p'][grade]+type[-1]
        miscadd(code,type)
for i in range(1,34):miscadd('r%02d'%i,'rune')
for code in annot_drop.SOURCE_NOTE:miscadd(code)
for i in range(1,6):miscadd('ua'+str(i),'ques',name='Act '+str(i)+' item')
for key in ['Hellfire Torch','UniqueThree','UniqueSkip','SetOne','Unicode\U0001f600','Quoted"Key','Already']:
    add('item-names.json',key, 'ÿc5[prior]\nÿc4Existing' if key=='Already' else None)
# Duplicate keys are rewritten in-place, not parsed through a deduplicating object.
add('item-names.json','UniqueThree','重复名','[ms][mp]Duplicado')
add('chinese-overlay.json','UniqueThree','覆盖名','English placeholder')
add('chinese-overlay.json','hp5','覆盖药水','English placeholder')
add('chinese-overlay.json','7wa','覆盖斧','English placeholder')
for key in ['Oath','Death','Breath of the Dying','Incomplete']:add('item-runes.json',key)
for key in ['Countess','Summoner','Nihlathak','MonsterBoss','BackupBoss']:add('monsters.json',key)
add('npcs.json','Countess','女伯爵覆盖','[fs]Condesa')
for key in ['Area','Zero','Conflicting','Negative']:add('levels.json',key)
for i in range(1,24):add('shrines.json','ShrId'+str(i))
# Unknown effect, unused effect, frame division, reset present/absent.
tables['shrines.txt']=[{'StringName':'ShrId'+str(i),'Code':str(i),'Arg0':'15','Arg1':'40','*Effect':'(Not Used)' if i==16 else 'Unknown effect','Duration in frames':'76' if i%2 else '','reset time in minutes':'7' if i%3 else ''} for i in range(1,24)]
tables['levels.txt']=[dict(zip(['LevelName','MonLvlEx','MonLvlEx(N)','MonLvlEx(H)'],r)) for r in [('Area','1','33','85'),('Area','1','33','85'),('Zero','','',''),('Conflicting','1','2','3'),('Conflicting','4','5','6'),('Negative','-1','2','3')]]
tables['weapons.txt']=weapons;tables['armor.txt']=armor;tables['misc.txt']=misc
tables['itemtypes.txt']=[{'Code':c,'Equiv1':a,'Equiv2':b} for c,a,b in [('axe','weapon',''),('weapon','',''),('pala','shld',''),('shld','',''),('orb','weapon',''),('javelin','weapon','class'),('class','',''),('loop','loop','')]]
tables['skills.txt']=[{'*Id':str(i),'skill':'skill'+str(i)} for i in range(4)]
tables['properties.txt']=[{'code':c,'func1':f} for c,f in [('ac','1'),('dmg%','7'),('skill','22'),('oskill','22'),('ignored','15'),('charged','11'),('mystery','2'),('abs-cold','8')]]
def ranged(key,**extra):
    row={'index':key,'spawnable':'1','disabled':'','prop1':'ac','min1':'1','max1':'20','prop2':'dmg%','min2':'10','max2':'55','prop3':'skill','par3':'skill0','min3':'1','max3':'3','prop4':'mystery','min4':'2','max4':'9'};row.update(extra);return row
tables['uniqueitems.txt']=[ranged('Hellfire Torch'),ranged('UniqueThree'),ranged('UniqueSkip',disabled='1'),ranged('Already'),ranged('Unicode\U0001f600',prop1='ignored',prop2='*oskill',par2='skill1',prop3='mystery'),ranged('Quoted"Key',prop1='abs-cold',min1='5',max1='5')]
tables['setitems.txt']=[ranged('SetOne',prop1='charged',prop2='*skill',par2='skill2')]
tables['runes.txt']=[{'*Rune Name':key,'Name':key,'complete':'0' if key=='Incomplete' else '1','itype1':'axe','T1Code1':'skill','T1Param1':'skill3','T1Min1':'1','T1Max1':'3'} for key in ['Oath','Death','Breath of the Dying','Incomplete']]
tables['cubemain.txt']=[{'input 1':'r01,qty=3','input 2':next(r['code'] for r in misc if r['type']=='gema'),'output':'r02'},{'input 1':'glv,mag','mod 1':'crush'},{'input 1':'glc,mag','mod 1':'mana-kill'}]
tables['treasureclassex.txt']=[{'Treasure Class':'Direct'+code,'Item1':code} for code in annot_drop.SOURCE_NOTE]+[{'Treasure Class':'Parent'+code,'Item1':'Direct'+code} for code in annot_drop.SOURCE_NOTE]
tables['superuniques.txt']=[{'Superunique':internal,'Name':name,'TC1(H)':'Parent'+code} for code,internal,name in [('pk1','The Countess','Countess'),('pk2','The Summoner','Summoner'),('pk3','Nihlathak Boss','Nihlathak')]]
tables['monstats.txt']=[{'Id':'monster','NameStr':'MonsterBoss','TreasureClass1(H)':'Parent'+code} for code in annot_drop.SOURCE_NOTE if not code.startswith('pk')]+[{'Id':'backup','NameStr':'BackupBoss','TreasureClass1(H)':'Parenttes'}]
tables['automagic.txt']=[{'itype1':'class','mod1code':'skilltab','mod1max':'3'}]
tables['magicprefix.txt']=[{'spawnable':'1','itype1':'weapon','mod1code':'skilltab','mod1max':'2'},{'spawnable':'1','itype1':'weapon','etype1':'class','mod1code':'skilltab','mod1max':'100'}]
tables['magicsuffix.txt']=[{'spawnable':'1','itype1':'javelin','mod1code':'swing3','mod1max':'40'}]
for file,rows in strings.items():asset[STR+file]=('\ufeff'+json.dumps(rows,ensure_ascii=False,indent=2).replace('\n','\r\n')+'\r\n').encode()
for file,rows in tables.items():
    fields=list(dict.fromkeys(k for r in rows for k in r));out=io.StringIO(newline='');w=csv.DictWriter(out,fieldnames=fields,delimiter='\t');w.writeheader();w.writerows(rows);asset[EX+file]=out.getvalue().encode()
def read(romfs,rel,suffix=None):
    path=next(p for p in asset if p.endswith(suffix or '/ui/layouts/'+rel));return path,asset[path].decode('utf-8-sig')
touch_layout.read_layout=read
real_sha1=hashlib.sha1
def sha1(value):
    if value.startswith(b'HASH:'):
        class Hash:
            def hexdigest(self):return value.decode().split(':')[1]+'0'*32
        return Hash()
    return real_sha1(value)
hashlib.sha1=sha1
enc=lambda b:base64.b64encode(b).decode()
with tempfile.TemporaryDirectory() as out,contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
    touch,count=touch_strings.build(None,out);side=touch_strings.build_sidecar(None)
    ts=''.join('%s\t%s\t%s\n'%(k,h,v.replace('\\','\\\\').replace('\n','\\n').replace('\t','\\t')) for k,h,v in side).encode()
    ti={k:enc(open(v,'rb').read()) for k,v in touch.items()}
    intl,n=dm_i18n.build(None,out,touch);touch.update(intl)
    ii={k:enc(open(v,'rb').read()) for k,v in intl.items()}
    # Earlier replacement exercise, including universal-newline conversion.
    seeded=STR+'item-names.json';p=out+'/seed.json';open(p,'wb').write(asset[seeded]);touch[seeded]=p
    notes,stat=annot_strings.build(None,out,touch);annot_strings.write_textbox_table(out)
    result={'assets':{k:enc(v) for k,v in asset.items()},'touch':ti,'touchCount':count,'touchStrings':enc(ts),'i18n':ii,'i18nCount':n,'notes':{k:enc(open(v,'rb').read()) for k,v in notes.items()},'stat':stat,'textbox':enc(open(out+'/dm_textbox_plain.tsv','rb').read()),'seeded':seeded}
print(json.dumps(result,ensure_ascii=False))
`;
export const fixtureSha1 = async (bytes) => {
  const text = new TextDecoder().decode(bytes);
  return text.startsWith("HASH:")
    ? text.split(":")[1] + "0".repeat(32)
    : createHash("sha1").update(bytes).digest("hex");
};
/**
 * 供更大合成原生参考语料复用的输入。initialReplacements 只含未修改的前序物品名称替换，用于测试 CRLF；带哈希的合成源字符串必须由调用方使用 fixtureSha1。
 * Reusable inputs for a larger synthetic native-oracle corpus. initialReplacements
 * contains only an untouched earlier-stage item-name replacement (CRLF exercise).
 * The caller must use fixtureSha1 for the synthetic hash-bearing source strings.
 */
export function buildAnnotationFixture() {
  for (const [path, expected] of Object.entries(annotationSources)) {
    assert.equal(
      createHash("sha256")
        .update(readFileSync(root + path))
        .digest("hex"),
      expected,
      "Pinned source changed: " + path,
    );
  }
  const native = spawnSync("python3", ["-c", program, root], {
    encoding: "utf8",
    maxBuffer: 16 * 1024 * 1024,
  });
  assert.equal(native.status, 0, native.stderr);
  const expected = JSON.parse(native.stdout);
  const assets = new Map(
    Object.entries(expected.assets).map(([key, value]) => [key, decode(value)]),
  );
  return {
    assets,
    initialReplacements: new Map([[expected.seeded, assets.get(expected.seeded)]]),
    expected,
    sha1: fixtureSha1,
  };
}
const decode = (text) => new Uint8Array(Buffer.from(text, "base64"));
async function main() {
  const { assets, expected, sha1 } = buildAnnotationFixture();
  const replacements = new Map();
  const realSha1 = (bytes) => createHash("sha1").update(bytes).digest("hex");
  const pipeline = createAnnotations({
    readAsset: (p) => assets.get(p),
    readReplacement: (p) => replacements.get(p),
    sha1,
  });
  function compare(map, object, label) {
    assert.deepEqual([...map.keys()], Object.keys(object), label + " replacement order");
    for (const [key, bytes] of map)
      assert.deepEqual(bytes, decode(object[key]), label + " bytes: " + key);
    for (const [key, bytes] of map) replacements.set(key, bytes);
  }
  const touch = await pipeline.buildTouchStrings();
  compare(touch.replacements, expected.touch, "touch");
  assert.equal(touch.count, expected.touchCount);
  assert.deepEqual(touch.touchStrings, decode(expected.touchStrings));
  const intl = await pipeline.buildI18n();
  compare(intl.replacements, expected.i18n, "i18n");
  assert.equal(intl.count, expected.i18nCount);
  replacements.set(expected.seeded, assets.get(expected.seeded));
  const notes = await pipeline.buildItemNotes();
  compare(notes.replacements, expected.notes, "annotations");
  assert.deepEqual(notes.stat, expected.stat);
  assert.deepEqual(notes.textbox, decode(expected.textbox));
  // 来源及冲突防护必须安全拒绝，绝不应用未经检查的修改。
  // Source and collision guards must fail closed, never apply unchecked changes.
  await assert.rejects(
    createAnnotations({ readAsset: (p) => assets.get(p), sha1: realSha1 }).buildTouchStrings(),
    /TOUCH_STRING_SOURCE/,
  );
  await assert.rejects(pipeline.buildI18n(), /I18N_KEY_CLASH/);
  const badRune = new Map(assets);
  badRune.set(
    "data/local/lng/strings/item-runes.json",
    new TextEncoder().encode(
      new TextDecoder()
        .decode(assets.get("data/local/lng/strings/item-runes.json"))
        .replace("RuneAA", "Rune99"),
    ),
  );
  await assert.rejects(
    createAnnotations({ readAsset: (p) => badRune.get(p), sha1 }).buildItemNotes(),
    /ANNOT_RUNE_SOURCE/,
  );
  console.log(
    JSON.stringify(
      {
        status: "pass",
        fixture: "synthetic CPython oracle (not full-title parity)",
        touchReplacements: touch.replacements.size,
        touchRows: new TextDecoder().decode(touch.touchStrings).trimEnd().split("\n").length,
        i18nRows: intl.count,
        annotationReplacements: notes.replacements.size,
        stat: notes.stat,
        textboxBytes: notes.textbox.length,
        guards: ["source checksum", "i18n key collision", "rune source"],
      },
      null,
      2,
    ),
  );
}
if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) await main();
