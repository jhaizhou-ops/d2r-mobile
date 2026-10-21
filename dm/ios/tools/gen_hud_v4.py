#!/usr/bin/env python3
# 底部 UI v4 生成器（按 v4 效果图实装）：产出 NativeHUD/v4/base.png（底图）与 hud.json（手柄 HUD 布局）。
# 用法：python3 dm/ios/tools/gen_hud_v4.py <原版 RomFS（Update）> [输出目录] [试调 json]   构建时跑（tools/gen_assets.py），产物 Gen/assets/NativeHUD/v4，不进仓库。
# 几何与素材在 hud_v4_design.py（效果图脚本搬进仓库的那部分）：有 PC 版 D2R 文件（DM_PC_DATA）出原版 2 倍图，没有就用 Switch 素材放大。
# 几何量全部取自效果图脚本（同一套数）；底图（石条 + 七格 + 天使 / 恶魔，球心挖空透出原生球液）由 gen_romfs_overlay.py 构建时写进借用路径
# （dm/data/casc_overlay/borrowed_paths.tsv）。血 / 蓝球、腰带、红方十字（LevelUpButton）、回城（SkillButton6，ZR 位）、经验条用原生部件，只改位置与缩放。
# 布局以 touchrectv1 为底（= 原版手柄 HUD 布局 Controller/HUDPanelHD.json 套我们的补丁 touchrectv1_patch.json：根铺满全屏、部件锚底边中点；SkillButton1–5 已摆在右侧技能区，iOS 技能圈叠在其上）。
# 实测（Mac 宿主）：布局 1 单位 = 屏高/2160；v31 原始贴图 1 纹素 = 2 单位；rect.scale 按 0.1 取整生效（0.8182 按 0.8），位置须按取整后的值算。
# Bottom UI v4 generator (implemented per the v4 mockup): outputs NativeHUD/v4/base.png (base image) and hud.json (controller HUD layout).
# Usage: python3 dm/ios/tools/gen_hud_v4.py <original game RomFS (Update)> [output dir] [tuning json]   Run at build time (tools/gen_assets.py); build output Gen/assets/NativeHUD/v4, not committed to the repo.
# Geometry and assets are in hud_v4_design.py (the part of the mockup script moved into the repo): with PC D2R files (DM_PC_DATA) it produces the original 2× images, otherwise it upscales the Switch assets.
# All geometry values come from the mockup script (same set of numbers); the base image (stone strip + seven slots + angel / demon, orb centers cut out to show the native orb liquid) is written into the borrowed path by gen_romfs_overlay.py at build time
# (dm/data/casc_overlay/borrowed_paths.tsv). The life / mana orbs, belt, red square cross (LevelUpButton), town portal (SkillButton6, ZR position) and experience bar use native widgets, with only position and scale changed.
# The layout is based on touchrectv1 (= the original game's controller HUD layout Controller/HUDPanelHD.json with our patch touchrectv1_patch.json applied: root fills the full screen, widgets anchored at the bottom-edge midpoint; SkillButton1–5 are already placed in the right-side skill area with the iOS skill circles stacked on top).
# Measured (Mac host): 1 layout unit = screen height/2160; v31 raw texture 1 texel = 2 units; rect.scale takes effect rounded to 0.1 (0.8182 acts as 0.8), so positions must be computed from the rounded value.
import json, re, sys, os
import numpy as np
from PIL import Image
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
import romfs_art, hud_v4_design, layout_patch
from gen_paths import gen_dir
ROMFS=sys.argv[1]; OUT=sys.argv[2] if len(sys.argv)>2 else gen_dir('NativeHUD','v4')   # 试调时可指定输出目录 | The output directory can be specified when tuning
os.makedirs(OUT,exist_ok=True); SRC=hud_v4_design.Source(ROMFS)
def load(var='C'): return hud_v4_design.load(SRC, var)
# 试调量（腰带三项按手机实拍校正：原版小药水贴图在格内偏右下，以手机常见的小药水居中为准；其余 Mac 截图逐像素量出）：腰带位移 / 格距、经验条位移、数值文字位移与字号
# Tuning values (the three belt items are corrected from real phone captures: the original game's small potion texture sits toward the lower right within the slot, so the small potion commonly seen on the phone is taken as centered; the rest are measured pixel by pixel from Mac screenshots): belt offset / slot pitch, experience bar offset, value text offset and font size
T=dict(bx=6.5,by=5,bpx=25,bs=1.6,ex=-7,ey=-15,txt=43,tx=-238,ts=1.4)   # bs：腰带缩放（药水对效果图 78 像素图标） | bs: belt scale (potions matched to the mockup's 78-pixel icons)
if len(sys.argv)>3: T.update(json.loads(sys.argv[3]))   # 试调：覆盖上面的量 | Tuning: overrides the values above
g=load('C'); W,H=g['W'],g['H']; U=H/2160; RX0,RY0=-1473,-417
ux=lambda px:(px-W/2)/U-RX0; uy=lambda py:(py-H)/U-RY0
BASE='PANEL/HUD_02/BugBTN'   # 借用路径，见登记表 | Borrowed path, see the registry
# ---- 底图 ----
# ---- Base image ----
cv=Image.new('RGBA',(W,H))
# 效果图的暗衬只为遮掉效果图上清理旧底栏的接缝；实机没有旧底栏，不要（还会压暗数值文字）
# The mockup's dark backing exists only to hide the seam left on the mockup from cleaning out the old bottom bar; the real device has no old bottom bar, so drop it (it would also darken the value text)
cv.alpha_composite(g['bar'],(g['BAR_X0']-g['EXT'],H-g['BH']))
for x0,y0,_,_ in g['slots']: cv.alpha_composite(g['slot'],(x0,y0))
# （六格：回城书格去掉，十字挪到第 5 格。）以下为七格时的说明——第 5 格回城书、第 6 格红方十字（原版 LevelUpButton 第 0 帧）照效果图烘进底图：触屏回城走原生绑定序列、不是 ZR 位上的技能，ZR 位图标代表不了回城；
# 原生 LevelUpButton 没有未分配点数时不显示；这里要的是外观一直是红方十字、有点数时照原版亮起（原生按钮叠在上面亮）
# 十字按钮两态——有未分配的技能点 / 属性点或新任务待读时红色，否则默认色。默认色 = 原图红底换成与其余六格内框同一路的暗底，
# 金边与金十字不动；底图烘默认态，红色态由前端按 HUD 读数换图（DmTouchOverlay drawV4）
# (Six slots: the Tome of Town Portal slot is removed, the cross moves to slot 5.) The following describes the seven-slot version -- slot 5 Tome of Town Portal and slot 6 red square cross (frame 0 of the original game's LevelUpButton) are baked into the base image per the mockup: touchscreen town portal goes through the native binding sequence, not a skill on the ZR position, so the ZR position icon cannot represent town portal;
# the native LevelUpButton is not shown when there are no unassigned points; what we want here is an appearance that is always a red square cross, lighting up as in the original game when points are available (the native button lights up stacked on top)
# The cross button has two states -- red when there are unassigned skill points / attribute points or a new quest waiting to be read, otherwise the default color. Default color = the original image's red background replaced with the same kind of dark background as the inner frames of the other six slots,
# gold border and gold cross untouched; the base image bakes the default state, and the red state is swapped in by the front end according to HUD readings (DmTouchOverlay drawV4)
def dflt(im):
    a_=np.asarray(im.convert('RGBA')).astype(float); r,gc,b=a_[...,0],a_[...,1],a_[...,2]; red=(r>14)&(((r>gc*1.3)&(gc-b<0.17*r))|(r>gc*1.7)); l=r*0.5+gc*0.3+b*0.2   # 红：绿蓝接近；金：绿明显高于蓝（十字的暗金边不能当红底换掉） | Red: green and blue close; gold: green clearly higher than blue (the cross's dark gold border must not be replaced as red background)
    hh,ww=red.shape; inner=np.zeros_like(red); inner[round(hh*0.12):round(hh*0.88),round(ww*0.12):round(ww*0.88)]=True; red&=inner   # 只换金边以内的红底（金边暗部也偏红，不能动） | Only replace the red background inside the gold border (the dark parts of the gold border are also reddish and must not be touched)
    for ch,k in ((0,0.42),(1,0.40),(2,0.38)): a_[...,ch]=np.where(red,l*k+6,a_[...,ch])
    return Image.fromarray(a_.round().clip(0,255).astype('uint8'),'RGBA')
g['layer']=Image.new('RGBA',(W,H)); g['fit'](dflt(g['lvl']),5,'十字（默认态）'); cv.alpha_composite(g['layer'])   # 六格：回城不在底栏（改为右侧技能钮上方的圆钮，前端画），十字在第 5 格 | Six slots: town portal is not in the bottom bar (now a round button above the right-side skill buttons, drawn by the front end), the cross is in slot 5
yy,xx=np.mgrid[0:H,0:W]; ball=np.zeros((H,W),bool)
for c in (g['LBC'],g['RBC']): ball|=(xx-c[0])**2+(yy-c[1])**2<(g['BR']-2)**2
# UIKit 原生分辨率版分层合成：石条 / 格子在球位挖空（石条两端伸进球下，不挖会盖黑半个球），再叠雕像——雕像带 PC 前面板在球上的半透明玻璃层（高光、边缘压暗、划痕）
# UIKit native-resolution version, layered compositing: the stone strip / slots are cut out at the orb positions (the strip's ends extend under the orbs; without cutting they would black out half an orb), then the statues are overlaid -- the statues carry the PC front panel's semi-transparent glass layer over the orb (highlights, darkened edges, scratches)
g_=np.array(cv); g_[...,3][ball]=0; cvg=Image.fromarray(g_)
L,R=g['L'],g['R']
for im_ in (cv,cvg): im_.alpha_composite(L,(g['LX'],H-L.height)); im_.alpha_composite(R,(g['RX'],H-R.height))
a=np.array(cv); a_glass=np.array(cvg)
a[...,3][ball]=0      # 游戏版底图球心整个挖空（含玻璃层）：原生球在底图下层；玻璃层由 UIKit 原生分辨率版盖上，免得叠两层 | In the game version of the base image the orb center is cut out entirely (including the glass layer): the native orb is in the layer below the base image; the glass layer is laid on by the UIKit native-resolution version, to avoid stacking two layers
cv=Image.fromarray(a); bx0,by0,bx1,by1=cv.getbbox(); spr=cv.crop((bx0,by0,bx1,by1))
# 前端原生分辨率版（UIKit 直接贴效果图像素，不经游戏 1568×720 渲染与放大）：腰带第 1–4 格内框挖空透出游戏画的药水；第 6 格整格留给 UIKit 红方十字按钮
# Front-end native-resolution version (UIKit pastes the mockup pixels directly, bypassing the game's 1568×720 rendering and upscaling): the inner frames of belt slots 1–4 are cut out to show the potions drawn by the game; slot 6 is left entirely to the UIKit red square cross button
full=a_glass   # 不挖球心：玻璃层盖在游戏画的球液上（原先整个挖掉这层，球的光泽与细节差很远） | Do not cut out the orb center: the glass layer covers the orb liquid drawn by the game (previously this layer was cut out entirely, and the orb's gloss and detail were far off)
for i in (1,2,3,4): x0,y0,_,_=g['slots'][i]; full[y0+g['IY0']:y0+g['IY1']+1, x0+g['IX0']:x0+g['IX1']+1, 3]=0
x0,y0,x1,y1=g['slots'][5]; full[y0:y1,x0:x1,3]=0
FULL=Image.fromarray(full).crop((bx0,by0,bx1,by1))
# 按 1 纹素 = 2 单位重采样、布局缩放写 1.0：实测缩放字段取 0.8182 时底图窄 2.4%（≈ 按 0.8 生效），不依赖小数缩放
# Resampled at 1 texel = 2 units with the layout scale written as 1.0: measured, with the scale field at 0.8182 the base image is 2.4% narrower (≈ takes effect as 0.8), so do not rely on fractional scaling
spr=spr.resize((round(spr.width/(2*U)),round(spr.height/(2*U))),Image.LANCZOS)
spr.save(f'{OUT}/base.png')
# ---- 布局 ----
# 以 touchrectv1（手机现行底栏布局：根铺满全屏、部件锚底边中点；六个原生技能键已摆在右侧技能区，iOS 技能圈叠在其上，须原样保留）为底，只重摆底栏部件
# ---- Layout ----
# Based on touchrectv1 (the current phone bottom bar layout: root fills the full screen, widgets anchored at the bottom-edge midpoint; the six native skill buttons are already placed in the right-side skill area with the iOS skill circles stacked on top, and must be kept as is), only the bottom bar widgets are rearranged
j=layout_patch.apply(layout_patch.load_text(romfs_art.read(ROMFS,'global/ui/Layouts/Controller/HUDPanelHD.json').decode('utf-8-sig')),json.load(open(os.path.join(HERE,'touchrectv1_patch.json'),encoding='utf-8')))
X=lambda px:(px-W/2)/U; Y=lambda py:(py-H)/U          # 锚底边中点的部件坐标 | Widget coordinates anchored at the midpoint of the bottom edge
j['children']=[c for c in j['children'] if c['name'] not in ('TouchBeltFrame','BeltPopupArrow','TouchHealthBallRing','TouchManaBallRing')]   # v4 底图取代 | Replaced by the v4 base image
by={c['name']:c for c in j['children']}
def rect(n,**k):
    r=by[n]['fields'].setdefault('rect',{}); r.update({q:round(v,1) for q,v in k.items()}); by[n]['fields']['anchor']={'x':0.5,'y':1}
by['Overlay']['fields']['filename']=BASE; by['Overlay']['fields'].pop('transparency',None)
rect('Overlay',x=X(bx0),y=Y(by0)); by['Overlay']['fields']['rect'].pop('scale',None)
q=lambda v: round(v,1)        # 实测：布局 scale 字段按 0.1 取整生效，位置须按取整后的值算 | Measured: the layout scale field takes effect rounded to 0.1, so positions must be computed from the rounded value
sb=q(2*g['BR']/(348*U)*T.get('ball',1.0)); ts=T.get('ts',1.4)
for n,c in (('HealthBall',g['LBC']),('ManaBall',g['RBC'])):
    rect(n,x=X(c[0])-174*sb,y=Y(c[1])-175*sb,scale=sb); by[n]['fields']['rect'].pop('width',None); by[n]['fields']['rect'].update(width=348,height=350)
    vd=[k for k in by[n]['children'] if k['name']=='ValueDisplay'][0]['fields']      # 数值：球顶上方居中（效果图 YB 基线），底图在球之后画，原位置会被雕像盖住 | Values: centered above the orb top (mockup YB baseline); the base image is drawn after the orb, so the original position would be covered by the statue
    vd['rect']={'x':-6000,'y':0}   # 原生数值文字挪出屏幕：改由前端按效果图字形原生分辨率画（gen 产出 life / mana 字形） | Native value text moved off screen: now drawn by the front end at native resolution with the mockup glyphs (gen outputs the life / mana glyphs)
    continue
    vd['rect']={'x':T.get('tx',-174),'y':round((Y(g['YB']-T.get('txt',30))-(Y(c[1])-175*sb))/sb,1),'width':348,'scale':ts}
    vd.setdefault('style',{})['alignment']={'h':'center'}
s6=g['slots'][5]; sl=q(T.get('lvl',0.9)); lc=((s6[0]+s6[2])/2,(s6[1]+s6[3])/2)
lx,ly=X(lc[0])-62*sl+T.get('lx',0),Y(lc[1])-60*sl; rect('LevelUpButton',x=lx,y=ly,scale=sl)
rect('LevelUpFlourish',x=lx+(1179-1411)*sl,y=ly+(-1-265)*sl,scale=q(1.1*sl))
s1=g['slots'][1]; rect('Belt',x=X(s1[0])+T.get('bx',0),y=Y(s1[1])+T.get('by',0),scale=q(T.get('bs',1.0)))
by['Belt']['fields']['cellPadding']={'x':T.get('bpx',32),'y':T.get('bpy',68)}
# 行距 15 → 68：多排腰带展开时（键鼠版「显示腰带」，腰带格上滑），上面几排是腰带控件自己从底排往上按行距排的，原行距下第一排的下半截被我们的底栏上沿
# （经验条、「Lv · %」、生命 / 法力数值）压住。布局里没有「展开面板位置」这一项，能把它们抬过上沿的只有行距（排与排之间因此有缝）；底排不受影响。
# 真机 2868×1320 实量：第一排比底排高 165、排距 162（原 113 / 110）——dm_touch_controls.c 点上面几排的命中用的就是这两个数，改这里要一起改。
# Row spacing 15 → 68: when the multi-row belt is expanded (the keyboard-and-mouse version's 「显示腰带」(Show Belt), swipe up on a belt slot), the upper rows are laid out by the belt widget itself from the bottom row upward at the row spacing; at the original spacing the lower half of the first row is covered by the top edge of our bottom bar
# (experience bar, "Lv · %", life / mana values). The layout has no "expanded panel position" item; the only thing that can lift them above the top edge is the row spacing (hence the gaps between rows); the bottom row is unaffected.
# Measured on a real device at 2868×1320: the first row is 165 above the bottom row, row pitch 162 (originally 113 / 110) -- dm_touch_controls.c uses exactly these two numbers for hit-testing the upper rows; change them together with this.
by['Belt']['fields'].pop('cellOffsetInRow',None); by['Belt']['fields'].pop('itemOffsetInCell',None)
[k for k in by['Belt']['children'] if k['name']=='HotkeyLabelTemplate'][0]['fields']['rect']={'x':-6000,'y':98}   # 手柄方向键提示：触屏无意义且压住格框 | Controller d-pad hint: meaningless on a touchscreen and it overlaps the slot frame
s5=g['slots'][5]; ss6=T.get('s6',1.0); rect('SkillButton6',x=X((s5[0]+s5[2])/2)-63*ss6+T.get('s6x',0),y=Y((s5[1]+s5[3])/2)-63*ss6+T.get('s6y',0),scale=ss6)
# SkillButton6（ZR 位）实例保留供绑定读取，排在底图之前、由第 5 格底图盖住（显示的是 ZR 位绑定，不是回城）
# SkillButton6 (ZR position) instance kept for reading bindings, ordered before the base image and covered by the slot 5 base image (what it shows is the ZR position binding, not town portal)
kids=j['children']; sb6=next(c for c in kids if c['name']=='SkillButton6'); kids.remove(sb6); kids.insert(kids.index(by['Overlay']),sb6)
es=q(T.get('exp',1.5)); rect('ExpBar',x=X(g['EX0'])+T.get('ex',0),y=Y(g['EY'])+T.get('ey',0),scale=es)
# 原生 ExpBar 实例保留、排到底图之前被盖住：经验条（底槽 + 填充 + 20 等分刻度）与「Lv · %」由前端 UIKit 按效果图画（原生条与效果图几何对不上、也不带文字）
# Native ExpBar instance kept, ordered before the base image so it is covered: the experience bar (trough + fill + 20 equal tick marks) and "Lv · %" are drawn by the front-end UIKit per the mockup (the native bar's geometry does not match the mockup, and it carries no text)
eb=by['ExpBar']; kids.remove(eb); kids.insert(kids.index(by['Overlay']),eb)
eb['fields']['rect']['y']=round(Y(g['slots'][1][1]+30),1)   # 挪进石条区域（七格上沿之外底图盖不住，Mac 预览露出黄条） | Moved into the stone strip area (outside the top edge of the seven slots the base image cannot cover it, and the Mac preview shows a yellow bar)
eb['fields']['rect']['x']=-6000   # 挪出屏幕（实例仍在）。靠底图盖是盖不住的：原生条放大 1.5 倍后比底栏还长，填充随经验伸长，过了约一半就从底图右端伸出去、 | Moved off screen (instance still exists). Covering it with the base image does not work: the native bar scaled 1.5× is longer than the bottom bar, the fill grows with experience, and past about halfway it sticks out of the base image's right end,
                                  # 横穿法力球——球里有法力时被液面挡住看不出，法力见底时就是球里一条黄线、一直伸到球右边外面（真机实例：2 级、经验 73%、法力 1/21）
                                  # Runs across the mana orb -- when the orb has mana it is hidden by the liquid surface and cannot be seen; when mana bottoms out it is a yellow line inside the orb extending all the way out past the orb's right side (real device instance: level 2, experience 73%, mana 1/21)
# 原生技能键 1–5：实例保留（绑定读取），挪到第 5 格底图下藏住——右侧技能区由 iOS 画 PC 版圆形技能键，原生方框不再露在圆圈下
# Native skill buttons 1–5: instances kept (bindings are read from them), moved under the slot 5 base image to hide them -- the right-side skill area has iOS draw PC-style round skill buttons, and the native square frames no longer show under the circles
for n in ('SkillButton1','SkillButton2','SkillButton3','SkillButton4','SkillButton5'):
    w_=by[n]; kids.remove(w_); kids.insert(kids.index(by['Overlay']),w_); w_['fields']['rect']=dict(by['SkillButton6']['fields']['rect']); w_['fields']['anchor']={'x':0.5,'y':1}
json.dump(j,open(f'{OUT}/hud.json','w'),ensure_ascii=False,indent=1)

# ---- 前端 UIKit 素材（hud_v4_ui/）：几何（效果图像素，前端按 屏高/1320 换算、以屏宽中线对齐效果图 x=1434）、预渲染字形、难度数字、选择条格子 ----
# ---- Front-end UIKit assets (hud_v4_ui/): geometry (mockup pixels; the front end converts by screen height/1320 and aligns the screen-width centerline to mockup x=1434), pre-rendered glyphs, difficulty digits, selection bar slots ----
from PIL import ImageDraw, ImageFont
UI=os.path.join(OUT,'hud_v4_ui'); os.makedirs(UI,exist_ok=True)
for f in os.listdir(UI): os.remove(os.path.join(UI,f))
font=g['num'](g['BADGE'])
def glyphs(tag,fill,font=font,chars='0123456789Lv·% '):     # 与效果图 shadow_text 同一字效：右下 2px 黑影（α170）+ 1px 深描边；每字一张，记字距与基线偏移 | Same text effect as the mockup's shadow_text: lower-right 2px black shadow (α170) + 1px dark outline; one image per character, recording advance and baseline offset
    m={}
    for ch in chars:
        pad=8; w=int(font.getlength(ch))+2*pad+4; asc,desc=font.getmetrics(); im=Image.new('RGBA',(w,asc+desc+2*pad+4)); d=ImageDraw.Draw(im)
        o=(pad,pad+asc)
        d.text((o[0]+2,o[1]+2),ch,font=font,fill=(0,0,0,170),anchor='ls'); d.text(o,ch,font=font,fill=fill,anchor='ls',stroke_width=1,stroke_fill=(10,8,6,255))
        bb=im.getbbox(); name=f'g_{tag}_{ord(ch):04x}.png'
        if bb: im.crop(bb).save(os.path.join(UI,name))
        m[ch]={'file':name if bb else None,'dx':(bb[0]-o[0]) if bb else 0,'dy':(bb[1]-o[1]) if bb else 0,'adv':font.getlength(ch)}
    return m
ui={'ref':[W,H],'center_x':W/2,'slots':g['slots'],'inner':[g['IX0'],g['IY0'],g['IX1'],g['IY1']],
    'exp':{'x0':g['EX0'],'x1':g['EX1'],'y':g['EY'],'h':6,'track':[22,18,14],'edge':[70,58,40],'fill':[196,160,70],'ticks':20},
    'lv':{'x':(g['EX0']+g['EX1'])/2,'baseline':g['EY']-6,'glyphs':glyphs('lv',(205,185,140,255))},
    'badge':{'glyphs':glyphs('b',(235,235,235,255)),'dx':-3,'dy':-1},     # 右下内角锚点 = 格左上 + 内框右下 + (dx,dy)，右对齐、基线 | Lower-right inner corner anchor = slot top-left + inner frame bottom-right + (dx,dy), right-aligned, baseline
    'pick':{'center_x':1390,'pitch':120,'y':1051.5},
    'vital':{'left_x':g['LBC'][0],'right_x':g['RBC'][0],'baseline':g['YB']}}
font_v=SRC.font('blizzardglobal-v5_81.ttf',30)
import re, dm_i18n   # 生命 / 法力两行的字形：原有中文这一套 + 多语表 dmHudLife / dmHudMana 各语言列用到的字（英文 / 西语的 á é ñ 等；字形不全 v4text 会空着） | Glyphs for the life / mana rows: the existing Chinese set + characters used by each language column of the multilingual table dmHudLife / dmHudMana (English / Spanish á é ñ etc.; v4text leaves gaps where glyphs are missing)
_vt={r[0]:r[1:] for r in dm_i18n.rows()}; _vc='生命法力:/ 0123456789'
_vc+=''.join(sorted({c for k in ('dmHudLife','dmHudMana') for c in dm_i18n.FMT.sub('',''.join(_vt[k]))}-set(_vc)))
ui['vital']['glyphs']=glyphs('v',(235,235,235,255),font_v,_vc)
g['slot'].save(os.path.join(UI,'slot.png'))
FULL.save(os.path.join(UI,'base_full.png')); ui['base']={'x':bx0,'y':by0}
lvlsheet=SRC.img('levelupbutton')   # 原版 LevelUpButton 两帧（亮 / 按下），整格替换第 6 格 | The original game's LevelUpButton two frames (lit / pressed), replaces slot 6 entirely
for fi in (0,1):   # lvl_N = 红色态（有点可加 / 新任务），lvl_dN = 默认态；N = 0 常态 / 1 按住 | lvl_N = red state (points available to spend / new quest), lvl_dN = default state; N = 0 normal / 1 held
    fr=lvlsheet.crop((fi*124,0,fi*124+124,121)).resize((g['DM'],g['SH']),Image.LANCZOS); fr.save(os.path.join(UI,f'lvl_{fi}.png')); dflt(fr).save(os.path.join(UI,f'lvl_d{fi}.png'))
for n in range(1,9):      # 难度数字：效果图方案 C（Exocet + 原版标题金属字效），字高 = 安全区 78 × 85% | Difficulty digits: mockup option C (Exocet + the original game's title metal text effect), glyph height = safe area 78 × 85%
    im=g['prep'](g['metal'](str(n),220)); k=g['BOX']*0.85/im.height; im=im.resize((max(1,round(im.width*k)),round(im.height*k)),Image.LANCZOS)
    im.save(os.path.join(UI,f'diff_{n}.png'))
# 技能剪影图集：官方技能图集（Sources/TouchArt/dm_native_skill_N.png，帧位置不变）只取银色剪影（高亮、低饱和；去掉锈铁方牌与四角铆钉），
# 前端放进深色圆底 + 古铜圆环（同 PC 版圆形技能键的样式），不再把方形图标塞进圆圈
# Skill silhouette atlas: from the official skill atlas (Sources/TouchArt/dm_native_skill_N.png, frame positions unchanged) take only the silver silhouette (bright, low saturation; drop the rusty iron square plate and the four corner rivets),
# the front end puts it on a dark round base + bronze ring (same style as the PC version's round skill buttons), no longer stuffing a square icon into a circle
ART=os.path.join(HERE,'..','Sources','TouchArt'); man=json.load(open(os.path.join(ART,'native_art_manifest.json')))
for a_ in man['assets']:
    if not a_['asset'].startswith('dm_native_skill_'): continue
    im=np.asarray(Image.open(os.path.join(ART,a_['asset'])).convert('RGBA')).astype(float)/255
    mx=im[...,:3].max(-1); mn=im[...,:3].min(-1); sat=(mx-mn)/np.maximum(mx,1e-6)
    al=np.clip((mx-0.42)/0.18,0,1)*np.clip((0.32-sat)/0.12,0,1)*im[...,3]
    fw,pitch=a_['frame_width'],a_['frame_pitch']; Hh,Ww=al.shape; yy,xx=np.mgrid[0:Hh,0:Ww]
    al*=np.hypot((xx%pitch)-fw/2,yy-Hh/2)<fw*0.46
    Image.fromarray((np.dstack([im[...,:3],al])*255).astype('uint8')).save(os.path.join(UI,'sil_'+a_['asset']))
# 技能键标签（PC 定稿演示图同款：主键「攻击」、其余 F1–F4 / 第二套 F5–F8），原版界面字体加粗米色 + 右下暗影，按效果图像素预渲染
# Skill button labels (same as the final PC demo image: main button 「攻击」(Attack), the rest F1–F4 / second set F5–F8), the original game's UI font in bold beige + lower-right dark shadow, pre-rendered at mockup pixels
lf=g['num'](34)
for name,txt in [('main','攻击')]+[(f'f{n}',f'F{n}') for n in range(1,9)]:
    w=int(lf.getlength(txt))+16; im=Image.new('RGBA',(w,60)); d=ImageDraw.Draw(im)
    d.text((10,8),txt,font=lf,fill=(0,0,0,190),stroke_width=2,stroke_fill=(0,0,0,190)); d.text((8,6),txt,font=lf,fill=(226,206,160,255),stroke_width=1,stroke_fill=(30,22,12,255))
    im.crop(im.getbbox()).save(os.path.join(UI,f'lbl_{name}.png'))
json.dump(ui,open(os.path.join(UI,'ui.json'),'w'),ensure_ascii=False,indent=1)
print('NativeHUD/v4：底图',spr.size,'原点',round(X(bx0),1),round(Y(by0),1),'球缩放',sb)
