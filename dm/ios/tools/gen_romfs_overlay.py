#!/usr/bin/env python3
# 构建时生成 App 内 RomFS 覆盖层（romfs_overlay/data/{.build.config, 新数据档}）：底部 UI v4（NativeHUD/v4，见 gen_hud_v4.py）。
# 用法：gen_romfs_overlay.py <原版 RomFS> <输出目录>。底图写进借用路径（dm/data/casc_overlay/borrowed_paths.tsv），布局替换手柄 HUDPanelHD.json；
# 其余数据档仍由基底 RomFS 提供（App 启动按 DM_ROMFS_OVERLAY 先查本层），所以只拷新 .build.config 与本次新写的那一个数据档。
# Generates the in-App RomFS overlay at build time (romfs_overlay/data/{.build.config, new data archive}): bottom UI v4 (NativeHUD/v4, see gen_hud_v4.py).
# Usage: gen_romfs_overlay.py <original game RomFS> <output dir>. The base image is written into the borrowed path (dm/data/casc_overlay/borrowed_paths.tsv), the layout replaces the controller HUDPanelHD.json;
# the other data archives are still supplied by the base RomFS (at launch the App checks this layer first via DM_ROMFS_OVERLAY), so only the new .build.config and the one data archive newly written this time are copied.
import re
import json, os, struct, subprocess, sys, tempfile, shutil
from PIL import Image
HERE=os.path.dirname(os.path.abspath(__file__)); V4=os.environ.get('DM_HUD_V4_DIR') or __import__('gen_paths').gen_dir('NativeHUD','v4')   # 试调时可指向别的产物目录 | Can point to another build output directory when tuning
romfs, out = sys.argv[1], sys.argv[2]
if os.environ.get('DM_OVERLAY_PC') == '1':
    import pc_desktop_layout
    pc_desktop_layout.main(romfs, out)
    sys.exit(0)
sys.path.insert(0,HERE); import gen_assets; gen_assets.ensure(romfs)   # 要读的生成资源（底栏 v4、选角右侧面板…）先确保齐全；输入没变不重出（gen_assets.py 文件头） | First make sure the generated resources to be read (bottom bar v4, character select right panel...) are complete; not regenerated if inputs are unchanged (gen_assets.py file header)
REPL={'data/hd/global/ui/panel/hud_02/bugbtn.sprite':'base.sprite',            # 借用路径（登记表「当前用途」= 底部 UI v4 底图） | Borrowed path (registry 「当前用途」(current use) = bottom UI v4 base image)
      'data/hd/global/ui/panel/hud_02/front_panel.sprite':'close.sprite',       # 借用路径：局内面板外框右上角的关闭按钮图（NativeHUD/touch/close.png，gen_close_btn.py） | Borrowed path: the close button image at the top-right corner of the in-game panel outer frame (NativeHUD/touch/close.png, gen_close_btn.py)
      'data/global/ui/layouts/controller/hudpanelhd.json':os.path.join(V4,'hud.json')}
with tempfile.TemporaryDirectory() as t:
    # 系统通知框（如改难度后的「游戏难度设为 N…」）：左边 = 七格行最左格（难度格）左外沿，右边 = 最右格（红十字）右外沿，停留 5 秒；高度不动，纵向比原版上移 100 单位（原位 y=-500 压住底栏上方「Lv · 经验」那行）
    # （原版宽 1200 单位居中屏幕、停 10 秒）。几何从 hud_v4_ui/ui.json 的七格算，不手写：布局 1 单位 = 屏高/2160（gen_hud_v4.py 实测），框锚点在屏幕水平中心。
    # 落点按真机量（界面按游戏内部分辨率画，落到 2868 宽屏幕上约 1.8 px 一格取整）：边框亮线两侧各有 1 px 暗边，宽度两边各放 1 px；
    # 中心再左移 1 单位才落到左亮线 x=973、右外沿 x=1807（= 七格外沿 973 / 1807）。四档实测
    # System notification box (e.g. 「游戏难度设为 N…」(Game difficulty set to N...) after changing difficulty): left edge = left outer edge of the leftmost slot of the seven-slot row (difficulty slot), right edge = right outer edge of the rightmost slot (red cross), stays 5 seconds; height unchanged, vertically moved up 100 units from the original game (the original position y=-500 covers the "Lv · experience" line above the bottom bar)
    # (the original game's is 1200 units wide, centered on screen, staying 10 seconds). Geometry is computed from the seven slots in hud_v4_ui/ui.json, not hand-written: 1 layout unit = screen height/2160 (measured in gen_hud_v4.py), the box anchor is at the horizontal center of the screen.
    # Placement is measured on a real device (the UI is drawn at the game's internal resolution, landing on the 2868-wide screen rounded at about 1.8 px per cell): the border's bright line has a 1 px dark edge on each side, so the width is extended 1 px on each side;
    # the center is then moved 1 more unit left to land on left bright line x=973, right outer edge x=1807 (= seven-slot outer edges 973 / 1807). Measured on four profiles
    ui=json.load(open(os.path.join(V4,'hud_v4_ui','ui.json'))); ref_w,ref_h=ui['ref']; k=2160/ref_h
    x0,x1=ui['slots'][0][0]-1,ui['slots'][-1][2]+1
    nw=round((x1-x0)*k); nx=round(((x0+x1)/2-ref_w/2)*k)-1
    # 系统提示框：整份自己写、不继承原版那份——原版带一个铺满的 Confirm 按钮（继承来的子控件删不掉），提示在屏上时它接住功能键和点击：
    # 按功能键 / 点它压着的地方都只是把提示关掉（面板开不了、下面的格子点不到）。触屏上提示到点自己消失，不要这个按钮
    # 停靠高度 y −790（原版 −500；先前为让开经验条提到 −600，仍压着底栏上方那行「生命 / 法力」数值——数值位数多时字伸进框里，触屏版那行更高、直接叠在框中间）：
    # 框的下沿要高过数值行的上沿，触屏版需再上提约 173 单位、手柄版约 77 单位，取一个两版都让开的值。只改布局里的停靠位置，滑入滑出仍是游戏自己的动画
    # System notice box: written entirely by us, not inheriting the original - the original has a full-size Confirm button (inherited child widgets cannot be removed), which takes the function keys and taps while a notice is on screen:
    # pressing a function key / tapping where it covers only dismisses the notice (panels cannot be opened, the cells below cannot be tapped). On touch the notice disappears by itself on time, and this button is not wanted
    # Docking height y -790 (original -500; it was raised to -600 earlier to clear the experience bar, but still covered the "life / mana" numbers above the bottom bar - with many digits the text ran into the box, and the touch layout's row is higher and sat right in the middle of the box):
    # the lower edge of the box has to be above the upper edge of the number row; the touch layout needs about 173 more units, the gamepad layout about 77, so one value that clears both is taken. Only the docking position in the layout changes; slide-in and slide-out are still the game's own animation
    note={'type':'NotificationPanel','name':'NotificationPanel',
          'fields':{'priority':9002,'notificationSeconds':5.0,'transitionSeconds':0.8,'rect':{'x':nx,'y':-790,'width':nw,'height':244},'anchor':{'x':0.5,'y':1.0}},
          'children':[{'type':'NineTileImageWidget','name':'NotificationBorder',
                       'fields':{'rect':{'x':0,'y':0,'width':nw,'height':244},'anchor':{'x':-0.5,'y':0.0},'filename':'PANEL/TooltipBorder/TooltipBorder','backgroundColor':[0.0,0.0,0.0,0.70],'tileScale':1},
                       'children':[{'type':'TextBoxWidget','name':'Prompt','fields':{'fitToParent':True,'style':'$StyleNotification'}}]}]}
    json.dump(note,open(os.path.join(t,'notification.json'),'w'),indent=1)
    REPL['data/global/ui/layouts/controller/notificationpanelhd.json']=os.path.join(t,'notification.json')
    # 全触摸方案：前端与通用弹窗上的手柄键位提示挪出屏幕（按钮本身可点之后，字形就是错误信息）；清单与做法见 touch_layout.py
    # Full-touch scheme: controller button hints on the front end and generic popups are moved off screen (once the buttons themselves are tappable, the glyphs are misinformation); list and method in touch_layout.py
    sys.path.insert(0,HERE); import touch_layout
    tl,ntl=touch_layout.build(romfs,t); REPL.update(tl)
    REPL.update(touch_layout.build_pcfront(romfs,t))   # 选角页改用键鼠版界面（盖掉上面 build 对这几份的改动） | Character select page switched to the keyboard-and-mouse UI (overrides the changes the build above made to these files)
    tp,ntp=touch_layout.build_present(romfs,t); REPL.update(tp)   # 局内：页签条两端 L / R、人物页耐力行 | In-game: L / R at the two ends of the tab strip, stamina row on the character page
    ti,nti=touch_layout.build_ingame(romfs,t); REPL.update(ti)   # 局内：各面板右上角的关闭按钮（同一落点、各发各的消息）与手柄版删掉的功能按钮（修理 / 翻页 / 高级属性 / 选项页应用·默认·放弃）；要在 build_present 之后（同一文件以这份为准） | In-game: the close button at each panel's top-right corner (same position, each sending its own message) and the function buttons removed in the controller version (repair / page turn / advanced stats / options page apply·default·discard); must come after build_present (for the same file this copy wins)
    import merc_slots; REPL.update(merc_slots.build(romfs,t,REPL))   # 佣兵面板加六个装备位（底图 + 布局；要在 build_ingame 给它插关闭钮之后） | Add six equipment slots to the mercenary panel (base image + layout; must come after build_ingame has inserted the close button into it)
    REPL.update(touch_layout.build_cursor(romfs,t))   # 面板里的手柄光标圆环换成透明 | The controller cursor ring in panels replaced with a transparent one
    REPL.update(touch_layout.build_glyphs(romfs,t))   # 22 张手柄键图标换成透明 | 22 controller button icons replaced with transparent ones
    if os.environ.get('DM_OVERLAY_MAC'):   # Mac App：再出一套键盘键帽，放在触屏键帽同一个目录、文件名「<平台>k<名字>」（宿主 dm_padmode.c 的键盘键帽路径 Ztouch_controller_buttons_ui/%sK%s） | Mac App: additionally produce a set of keyboard key caps, placed in the same directory as the touch key caps with file names "<platform>k<name>" (the keyboard key-cap path of the host's dm_padmode.c is Ztouch_controller_buttons_ui/%sK%s)
        import kbd_glyphs; REPL.update({'data/hd/global/ui/ztouch_controller_buttons_ui/switchk'+os.path.basename(k):v for k,v in kbd_glyphs.build(romfs,t).items()})
        universal, inputglyphs = kbd_glyphs.build_universal(romfs, t)
        REPL.update({'data/hd/global/ui/ztouch_controller_buttons_ui/switchk' + name.lower(): path
                     for name, path in universal.items()})
        print('Teclas reasignables: %d sprites y 1296 etiquetas nativas' % len(universal))
    REPL.update(touch_layout.build_profile(romfs,t))   # 大字号 | Large font size
    import touch_strings; ts,nts=touch_strings.build(romfs,t); REPL.update(ts)   # 排版用的串（大字号）；手柄说法 → 触摸说法改由宿主按模式换（旁路表 touch_strings.tsv，见 emit） | Strings used for typesetting (large font size); controller wording → touch wording is now switched by the host according to mode (sidecar table touch_strings.tsv, see emit)
    TS=touch_strings.build_sidecar(romfs)
    import dm_i18n; si,nsi=dm_i18n.build(romfs,t,REPL); REPL.update(si)   # 我们自己的界面文字（中英表 dm/i18n/ui_text.tsv）追加进原版 ui.json，布局写 @键、宿主按键取串 | our own UI text (Chinese-English string table dm/i18n/ui_text.tsv) is appended to the original ui.json; layouts write @key and the host looks strings up by key
    import drop_light; dl,dls=drop_light.build(romfs,t,REPL); REPL.update(dl)   # 掉落光的带光模型变体（新增路径，常驻；宿主运行时按规则 / 开关改指，dm_dropfilter.c） | Lit model variants for drop lights (new paths, always present; the host repoints at runtime by rule / switch, dm_dropfilter.c)
    import pouch_layout; pl,pls=pouch_layout.build(romfs,t,REPL); REPL.update(pl)   # 百宝囊框与「存放」按钮（物品栏，叠在触摸布局之后） | Pouch box and 「存放」(Deposit) button (inventory, stacked after the touch layout)
    import wide_stash; ws,wss=wide_stash.build(romfs,t,REPL); REPL.update(ws)
    # 手柄版布局：手柄模式读原版路径上的布局（触屏版挪进了触屏目录，见下面 TDIR）。我们的功能改动两种模式都要——符文之语清单、百宝囊框与「存放」、
    # 储藏箱 14 列（数据表已是 14 列，布局只有 10 列时第 11–14 列的物品看不到）与宽外框、编年史仓库页签——所以按同一套功能模块在原版布局上再出一份，放在原版路径；触屏改动不带
    # Controller layouts: controller mode reads the layouts at the original paths (the touch version was moved into the touch directories, see TDIR below). Our
    # feature changes are needed in both modes: the runeword list, the pouch frame and "Deposit",
    # the 14-column stash (the data table already has 14 columns; with a 10-column layout the items in columns 11 to 14 are invisible) with its wide frame,
    # and the Chronicle vault tab. So another copy is generated on top of the original layouts with the same set of feature modules and placed at the
    # original paths; touch changes are not included
    tpad=os.path.join(t,'pad'); os.makedirs(tpad); PR={}
    lp,lt=touch_layout.read_layout(romfs,pouch_layout.INV); o=os.path.join(tpad,'pad_inv.json'); open(o,'w',encoding='utf-8').write(touch_layout.pad_inv_rw(lt)); PR[lp.lower()]=o
    for rel,gd in (('controller/pauselayoutgardenhd.json',1),('controller/pauselayouthd.json',0)):   # 手柄版暂停菜单：一键重开 / 切换难度 / 存放 / 分享诊断日志 | Controller pause menu: one-tap restart / change difficulty / Deposit / Share Diagnostics
        lp,lt=touch_layout.read_layout(romfs,rel); o=os.path.join(tpad,'pad_pause%d.json'%gd); open(o,'w',encoding='utf-8').write(touch_layout.pad_pause(lt,gd,share=not os.environ.get('DM_OVERLAY_MAC'))); PR[lp.lower()]=o
    # 大秘境面板（借「消息记录」面板外壳，touch_layout._rift_panel）：手柄版同一份。少了它手柄模式点秘境门开出来的是空的原版消息记录面板、进不了秘境；手柄键在 dm_rift_ui.c
    # 手柄版的系统提示框：原版那份（停靠 y −500）下沿压着底栏上方那行「生命 / 法力」数值，数值位数多时字伸进框里。只把停靠位置上提 90 单位（下沿高过数值行），其余原样
    # Greater Rift panel (borrowing the shell of the "message log" panel, touch_layout._rift_panel): the same for the gamepad layout. Without it, tapping a rift door in gamepad mode opens the empty original message log panel and the rift cannot be entered; gamepad keys are in dm_rift_ui.c
    # The gamepad layout's system notice box: in the original (docked at y -500) its lower edge sits on the "life / mana" numbers above the bottom bar, and with many digits the text runs into the box. Only the docking position is raised by 90 units (lower edge above the number row); everything else as in the original
    lp,lt=touch_layout.read_layout(romfs,'controller/notificationpanelhd.json'); assert lt.count('"y": -500')==1, lp   # 原版手柄版提示框的停靠位置应是 y -500（原包变了就停在这里） | the original gamepad notice box should dock at y -500 (if the original package changes, stop here)
    o=os.path.join(tpad,'pad_notification.json'); open(o,'w',encoding='utf-8').write(lt.replace('"y": -500','"y": -590')); PR[lp.lower()]=o
    lp,lt=touch_layout.read_layout(romfs,'controller/messagelogpanelhd.json'); o=os.path.join(tpad,'pad_rift.json'); open(o,'w',encoding='utf-8').write(touch_layout._rift_panel(lt)[0]); PR[lp.lower()]=o
    PR.update(merc_slots.build_pad(romfs,tpad))   # 手柄版佣兵面板：同样加六个装备位 | Controller layout mercenary panel: likewise add six equipment slots
    # 手柄版职业选择页：八个职业的点击矩形同样改成按实际站位分的竖条（touch_layout.CLASS_STRIPS，原因见那里）。手柄用左右键选、原版没暴露；
    # Mac App 是手柄版布局 + 鼠标，点德鲁伊出术士、右半边四个职业都错一格
    # Gamepad-layout class selection page: the click rectangles of the eight classes are likewise changed to vertical strips split by where the characters actually stand (touch_layout.CLASS_STRIPS, see there for the reason). A gamepad selects with left / right and the original never exposed this;
    # the Mac App is the gamepad layout plus a mouse, where clicking the Druid selected the Warlock and all four classes on the right half were off by one
    lp,lt=touch_layout.read_layout(romfs,'controller/console/charactercreatepanelhd.json')
    for nm,x0,x1 in touch_layout.CLASS_STRIPS: lt=touch_layout.set_rect(lt,nm,x0,450,x1-x0,1300)
    o=os.path.join(tpad,'pad_charcreate.json'); open(o,'w',encoding='utf-8').write(lt); PR[lp.lower()]=o
    PR.update(touch_layout.build_ingame(romfs,tpad,touch_layout.PAD_VENDOR,present=False)[0])   # 手柄版商人页：刷新货架钮 | Controller layout vendor page: the refresh shop stock button
    lp,lt=touch_layout.read_layout(romfs,'controller/console/mainmenupanelhd.json'); o=os.path.join(tpad,'pad_mainmenu.json'); open(o,'w',encoding='utf-8').write(touch_layout.pad_mainmenu(lt)); PR[lp.lower()]=o   # 手柄版主菜单：构建号与新版本提示 | Controller layout main menu: build number and new version notice
    lp,lt=touch_layout.read_layout(romfs,'controller/console/mainmenubuttonribbonhd.json'); o=os.path.join(tpad,'pad_ribbon.json'); open(o,'w',encoding='utf-8').write(touch_layout.pad_ribbon(lt)); PR[lp.lower()]=o   # 手柄版主菜单按钮栏：拿掉「电影动画」 | Controller layout main menu button ribbon: remove 「电影动画」(Cinematics)
    lp,lt=touch_layout.read_layout(romfs,'controller/hudmonsterhealthhd.json'); o=os.path.join(tpad,'pad_bosspct.json'); open(o,'w',encoding='utf-8').write(touch_layout.pad_boss_pct(lt)); PR[lp.lower()]=o   # 手柄版怪物血条：加百分比 | Controller layout monster health bar: add the percentage
    lp,lt=touch_layout.read_layout(romfs,'controller/console/characteroptionspanelhd.json'); o=os.path.join(tpad,'pad_charopt.json'); open(o,'w',encoding='utf-8').write(touch_layout.pad_bliss(lt)); PR[lp.lower()]=o   # 手柄版建角模式页：加「极乐世界」一行 | Controller-version character-creation mode page: add the "Elysium" row
    for rel,fn in (('controller/console/settingspanelhd.json',touch_layout.pad_settings),('controller/console/videooptionspanelhd.json',touch_layout.pad_custom_page)):   # 手柄版选项页：「视频」页签换成「自定义」页 | Controller options page: the "Video" tab is replaced by the "Custom" page
        lp,lt=touch_layout.read_layout(romfs,rel); o=os.path.join(tpad,'pad_'+rel.replace('/','_')); open(o,'w',encoding='utf-8').write(fn(lt)); PR[lp.lower()]=o
    PR.update(pouch_layout.build(romfs,tpad,PR)[0]); PR.update(wide_stash.build(romfs,tpad,PR)[0])   # 储藏箱 10 → 14 列（编年史仓库第 1 步）：表、底图、宽外框、布局；要在触摸布局之后（储藏箱 / 外框布局叠在那份上） | Stash 10 → 14 columns (chronicle vault step 1): table, base image, wide outer frame, layout; must come after the touch layout (the stash / outer frame layout stacks on that copy)
    if os.environ.get('DM_OVERLAY_MAC'):
        legend = 'data/global/ui/layouts/controller/controlleroverlayhd.json'
        target = PR[legend]
        with open(target, encoding='utf-8') as f: legend_text = touch_strings.mac_legend(f.read())
        with open(target, 'w', encoding='utf-8') as f: f.write(legend_text)
    cb=Image.open(os.path.join(HERE,'..','NativeHUD','touch','close.png')).convert('RGBA')
    open(os.path.join(t,'close.sprite'),'wb').write(b'SpA1'+struct.pack('<HHIIIIII',31,cb.width,cb.width,cb.height,0,1,0,0)+struct.pack('<II',cb.width*cb.height*4,4)+cb.tobytes())
    # 选角页右侧面板底图：去掉「转换角色」的槽与十字花、只留居中一个槽（原图解出后改的，同尺寸）
    # Character select page right panel base image: remove the 「转换角色」(Convert Character) slot and the cross ornament, leaving only one centered slot (modified after decoding the original image, same size)
    shutil.copy(os.path.join(__import__('gen_paths').gen_dir('NativeHUD','touch'),'fe_rightpanel.sprite'),os.path.join(t,'fe_rightpanel.sprite'))   # 构建时由 gen_fe_rightpanel.py 从原图生成（DXT5 + 块线性）；原始 RGBA 的 v31 在 401×1080 这个尺寸上游戏读出竖条花屏 | Generated at build time by gen_fe_rightpanel.py from the original image (DXT5 + block-linear); raw RGBA v31 at this 401×1080 size is read by the game as vertical-stripe garbage
    REPL['data/hd/global/ui/frontend/hd/final/frontend_rightpanel.sprite']='fe_rightpanel.sprite'
    im=Image.open(os.path.join(V4,'base.png')).convert('RGBA'); w,h=im.size
    # SpA1 v31 原始 RGBA：头 0x28 字节（版本 31、帧宽、宽、高、帧数 1、像素字节数），游戏实测可读
    # SpA1 v31 raw RGBA: 0x28-byte header (version 31, frame width, width, height, frame count 1, pixel byte count), measured to be readable by the game
    open(os.path.join(t,'base.sprite'),'wb').write(b'SpA1'+struct.pack('<HHIIIIII',31,w,w,h,0,1,0,0)+struct.pack('<II',w*h*4,4)+im.tobytes())
    # 老机型补充包（显卡不支持 BC 的机型）：环境变量 DM_ASTC_PACK = 补充包目录（dm/tools/astc_pack.py + casc_overlay.py pack 的产物，含 pack_index.tsv / pack.json）。
    # 设了就再出一套 ASTC 版覆盖层（out_astc、out_astc_variants），索引里那 2.7 万张贴图指向补充包的数据档（内容不复制进 App）；App 在不支持 BC 且补充包校验通过时挑这一套。
    # 没设 = 两个目录照建但为空（App 认为本包不带 ASTC 版）。
    # Legacy Device Pack (devices whose GPU does not support BC): environment variable DM_ASTC_PACK = Legacy Device Pack directory (build output of dm/tools/astc_pack.py + casc_overlay.py pack, containing pack_index.tsv / pack.json).
    # When set, an additional ASTC overlay set is produced (out_astc, out_astc_variants), in whose index those 27 thousand textures point to the Legacy Device Pack's data archive (contents are not copied into the App); the App picks this set when BC is unsupported and the Legacy Device Pack passes verification.
    # Not set = the two directories are still created but empty (the App takes it that this package carries no ASTC version).
    ASTC=os.environ.get('DM_ASTC_PACK','')
    # 触屏版布局放进自己的目录（手柄支持 P2）：原版手柄布局 layouts/controller/ 原样保留，触屏改过的与没改的一并放到三个新目录，
    # 宿主按模式把原版拼路径用的三个前缀串改写成同长的新目录名（dm_padmode.c）再让游戏重载布局：
    #   Controller/ → Xtouch_lay/、Controller/Console/ → Ztouch_console_lay/、Controller/Console/Switch/ → 0touch_console_switch_lay/
    # 新目录只能是 layouts 下一层、首字母不能与 layouts 现有子项相同（casc_overlay.py 新增路径的限制；layouts 下 a–y 与 _ 都用了）。
    # _profile*.json 按固定路径读、PC 专用子目录（pc/）Switch 不用，都留在原处。覆盖层根目录放标记文件 ctrl_touch：宿主见到它才改写前缀
    # Touch layouts go into their own directories (controller support P2): the original controller layouts layouts/controller/ are kept as is, and touch layouts,
    # both modified and unmodified, are all put into three new directories;
    # by mode, the host rewrites the three prefix strings the original uses to build paths into new directory names of the same length (dm_padmode.c) and
    # then makes the game reload its layouts:
    #   Controller/ → Xtouch_lay/, Controller/Console/ → Ztouch_console_lay/, Controller/Console/Switch/ → 0touch_console_switch_lay/
    # A new directory must be exactly one level under layouts and its first character must differ from every existing child of layouts (limitation of new
    # paths in casc_overlay.py; a–y and _ under layouts are all taken).
    # _profile*.json is read from a fixed path and the PC-only subdirectory (pc/) is not used on Switch, so both stay where they are. The marker file
    # ctrl_touch is placed in the overlay root: the host rewrites the prefixes only when it sees it
    LAY='data/global/ui/layouts/'
    TDIR=[('controller/console/switch/','0touch_console_switch_lay/'),('controller/console/','ztouch_console_lay/'),('controller/','xtouch_lay/')]
    import romfs_art
    MAN={f[9].lower().split(':',1)[-1] for f in (l.rstrip('\n').split('|') for l in open(os.path.join(romfs,'data','build_manifest.psv'),encoding='utf-8')) if len(f)>=11}
    # 键位图标与手柄光标（touch_layout 换成透明的那几张）同样挪到新目录，宿主改写原版拼贴图路径的串（dm_padmode.c）：
    #   Controller/ControllerButtons/%s/%s → Ztouch_controller_buttons_ui/%s_%s（平台 / 名字之间的「/」换成「_」，只开一层目录）、Controller/Cursor/X → Xtouch_cursor_lay/X
    # Button icons and the controller cursor (the ones touch_layout replaces with transparent images) are likewise moved to new directories, and the host
    # rewrites the strings the original uses to build texture paths (dm_padmode.c):
    #   Controller/ControllerButtons/%s/%s → Ztouch_controller_buttons_ui/%s_%s (the "/" between platform and name becomes "_", so only one directory level
    #   is created), Controller/Cursor/X → Xtouch_cursor_lay/X
    UI='data/hd/global/ui/'
    def touch_path(k):   # 原版路径 → 触屏目录里的路径；不归触屏目录管的返回 None | Original path → path in the touch directories; returns None for paths the touch directories do not cover
        if k.startswith(UI+'controller/controllerbuttons/'):
            r=k[len(UI+'controller/controllerbuttons/'):]; return UI+'ztouch_controller_buttons_ui/'+r.replace('/','_') if r.count('/')==1 else None
        if k.startswith(UI+'controller/cursor/') and '/' not in k[len(UI+'controller/cursor/'):]: return UI+'xtouch_cursor_lay/'+k[len(UI+'controller/cursor/'):]
        if not k.startswith(LAY) or '/_profile' in k: return None
        r=k[len(LAY):]
        for a,b in TDIR:
            if r.startswith(a): rest=r[len(a):]; return None if '/' in rest else LAY+b+rest
        return None
    ORIG=sorted(q for q in MAN if touch_path(q) and q.startswith(LAY))   # 布局要整套搬（重载时按目录找）；贴图只搬换过的那几张，其余平台 / 贴图触屏模式用不到 | Layouts must be moved as a complete set (looked up by directory on reload); for textures only the replaced ones are moved, the other platforms / textures are not used in touch mode
    def to_touch(repl):
        r={}
        for k,v in repl.items(): tp=touch_path(k); r[tp or k]=v
        for q in ORIG:
            k=touch_path(q)
            if k not in r:
                o=os.path.join(t,'orig_'+q[len(LAY):].replace('/','__'))
                if not os.path.exists(o): open(o,'wb').write(romfs_art.read(romfs,q))
                r[k]=o
        return r
    # 两种模式本来就该共用的（我们自己的页，手柄 / 触屏同一份布局、宿主按模式摆）：编年史三份
    # 还没分开的存量：两份字号档（开机只读一次）
    # Meant to be shared by both modes all along (our own pages, one layout for controller / touch, placed by the host according to mode): the three Chronicle ones
    # Existing stock not yet split: the two font size profiles (read only once at boot)
    SHARED_TODO=('chroniclebaselistpanelhd.json','chroniclepanelhd.json','controller/_profileconsolehd.json','controller/_profilehandheldhd.json','dropdownlistcontentshd.json')
    PADROOT=touch_layout.build_pad_root(romfs,tpad); PADROOT_KEYS={k[len(LAY):] for k in PADROOT}   # 三份选项页：原路径放手柄那份，触屏那份挪到触屏这边 | The three options pages: the controller copy goes at the original path, the touch copy moves to the touch side
    UNSHARE=('confirmationmodalhd.json','dismissablemodalhd.json','helpbuttonpanelhd.json','npcdialogpanelhd.json','youdiedmodalhd.json')   # 只该给触屏的：挪到触屏这边（见 emit） | Touch-only ones: moved to the touch side (see emit)
    PADLAY={k:v for k,v in PR.items() if touch_path(k)}   # 手柄版只取布局（数据表、图与触屏版同一份） | The controller version takes only layouts (data tables and images are the same copy as the touch version)
    # 手柄专用清单：手柄模式下不是原版字节的布局就这几份，一份不许多、一份不许少（改上面某一行时把相邻一行整行替掉过，商人页的刷新钮就从包里掉了）
    # Controller-only list: these are the only layouts that are not original bytes in controller mode, not one more and not one fewer (once, when a line above was edited, the adjacent line was replaced whole, and the vendor page's refresh button dropped out of the package)
    PAD_PAGES=('controller/bankexpansionlayouthd.json','controller/console/charactercreatepanelhd.json','controller/console/characteroptionspanelhd.json','controller/console/mainmenubuttonribbonhd.json','controller/console/mainmenupanelhd.json','controller/console/settingspanelhd.json','controller/console/switch/playerinventoryexpansionlayouthd.json','controller/console/videooptionspanelhd.json',
               'controller/controlleroverlayhd.json','controller/hirelinginventorypanelhd.json','controller/hudmonsterhealthhd.json','controller/messagelogpanelhd.json','controller/notificationpanelhd.json','controller/pauselayoutgardenhd.json','controller/pauselayouthd.json','controller/vendorpanellayouthd.json')
    if sorted(k[len(LAY):] for k in PADLAY)!=sorted(PAD_PAGES): raise SystemExit('gen_romfs_overlay：手柄专用布局与清单 PAD_PAGES 不一致：多 %s，少 %s' % (sorted(set(k[len(LAY):] for k in PADLAY)-set(PAD_PAGES)), sorted(set(PAD_PAGES)-set(k[len(LAY):] for k in PADLAY))))
    def emit(repl, dst, tag, prebuilt=None):   # repl → 覆盖层（新 .build.config + 一个新数据档）写到 dst/data | repl → overlay (a new .build.config + one new data archive) written to dst/data
        repl=to_touch(repl); repl.update(PADLAY)
        # 只该给触屏的根目录布局（不在 Controller/ 前缀下、原先两种模式共用一份改过的）：改过的那份挪到触屏这边，原路径不再替换——手柄模式加载到原版字节。
        # 触屏目录里有页继承它的（basedOn 它的名字）：放 1touchbase_lay/ 并把那些页的 basedOn 改指过去；没有页继承的（游戏直接按名字加载）：放进触屏目录 xtouch_lay/，触屏模式先在那里找到
        # Root-directory layouts meant only for touch (not under the Controller/ prefix, previously one modified copy shared by both modes): the modified copy moves to the touch side and the original path is no longer replaced -- controller mode loads original bytes.
        # If pages in the touch directory inherit from it (basedOn its name): put it in 1touchbase_lay/ and repoint those pages' basedOn there; if no page inherits from it (the game loads it directly by name): put it in the touch directory xtouch_lay/, where touch mode finds it first
        for name in UNSHARE+tuple(sorted(PADROOT_KEYS)):
            k=LAY+name; mod=repl.pop(k); pat=re.compile(r'("basedOn"\s*:\s*")(%s)(")' % re.escape(name), re.I); inh=[]
            for tk,tv in list(repl.items()):
                if not any(tk.startswith(LAY+d) for _,d in TDIR): continue
                try: txt=open(os.path.join(t,tv),encoding='utf-8').read()
                except UnicodeDecodeError: continue
                if pat.search(txt): inh.append((tk,txt))
            if inh:
                repl[LAY+'1touchbase_lay/'+name]=mod
                for tk,txt in inh:
                    o=os.path.join(t,tag+'unshare_'+tk[len(LAY):].replace('/','__')); open(o,'w',encoding='utf-8').write(pat.sub(lambda m: m.group(1)+'1touchbase_lay/'+m.group(2)+m.group(3), txt)); repl[tk]=o
            else: assert LAY+'xtouch_lay/'+name not in repl, name; repl[LAY+'xtouch_lay/'+name]=mod
            if name in PADROOT_KEYS: repl[LAY+name]=PADROOT[LAY+name]   # 原路径换成手柄那份（触屏模式已不读原路径这一份） | The original path gets the controller copy (touch mode no longer reads the copy at the original path)
            print('按模式分开：%s → %s（继承它的触屏页 %d 个；0 = 触屏模式直接按名字加载）' % (name, '1touchbase_lay/' if inh else 'xtouch_lay/', len(inh)))
        # 手柄模式 = 原版：手柄模式能加载到的布局（触屏三个目录之外的）要么是原版字节（不在替换表里），要么在手柄专用清单 PR 里。
        # SHARED_TODO 是还没按模式分开的存量（两种模式共用一份改过的），只许减不许加；新增共用布局在这里红
        # Controller mode = original: the layouts that controller mode can load (those outside the three touch directories) are either original bytes (not in the replacement table) or in the controller-only list PR.
        # SHARED_TODO is the existing stock not yet split by mode (both modes share one modified copy); it may only decrease, never increase; a newly added shared layout fails here
        shared=sorted(k[len(LAY):] for k in repl if k.startswith(LAY) and k in MAN and k not in PADLAY and k[len(LAY):] not in PADROOT_KEYS and not any(k.startswith(LAY+d) for _,d in TDIR))
        if shared!=sorted(SHARED_TODO): raise SystemExit('gen_romfs_overlay：两种模式共用的改过的布局与存量清单不一致（多出的要按模式分开：改过的放触屏目录 / 另起新名，原路径留原版）：多 %s，少 %s' % (sorted(set(shared)-set(SHARED_TODO)), sorted(set(SHARED_TODO)-set(shared))))
        with open(os.path.join(t,tag+'repl.tsv'),'w') as f:
            for k,v in repl.items(): f.write(f'{"" if k in MAN else "+"}{k}\t{os.path.join(t,v)}\n')   # 新增路径前加「+」（casc_overlay.py） | New paths are prefixed with "+" (casc_overlay.py)
        subprocess.run([sys.executable,os.path.join(HERE,'..','..','tools','casc_overlay.py'),romfs,os.path.join(t,tag+'ov'),os.path.join(t,tag+'repl.tsv')]+(['--prebuilt',prebuilt] if prebuilt else []),check=True,stdout=subprocess.DEVNULL)
        src=os.path.join(t,tag+'ov','data'); new=[n for n in os.listdir(src) if n.endswith('.data') and not os.path.islink(os.path.join(src,n))]
        assert len(new)==1, new
        shutil.rmtree(dst,ignore_errors=True); os.makedirs(os.path.join(dst,'data'))
        for n in ('.build.config',new[0]): shutil.copyfile(os.path.join(src,n),os.path.join(dst,'data',n))
        with open(os.path.join(dst,'touch_strings.tsv'),'w',encoding='utf-8') as f:   # 键 \t 原文校验（SHA-1 前 8 位） \t 触摸说法（\\ \\n \\t 转义）；宿主 dm_padmode.c 读 | key <TAB> source checksum (first 8 hex digits of SHA-1) <TAB> touch wording (backslash, newline and tab are escaped); read by the host's dm_padmode.c
            for k,o,v in TS: f.write('%s\t%s\t%s\n' % (k,o,v.replace('\\','\\\\').replace('\n','\\n').replace('\t','\\t')))
        if os.environ.get('DM_OVERLAY_MAC'): open(os.path.join(dst,'kbd_glyphs'),'w').write('ztouch_controller_buttons_ui/switchk*.sprite\n')   # 标记文件：覆盖层带键盘键帽，宿主 dm_padmode.c 见到它才用 | marker file: the overlay carries keyboard key caps; the host's dm_padmode.c uses them only when it sees this
        if os.environ.get('DM_OVERLAY_MAC'):
            shutil.copyfile(inputglyphs, os.path.join(dst, 'inputglyphs.tsv'))
        open(os.path.join(dst,'ctrl_touch'),'w').write('触屏版布局在 layouts/xtouch_lay、ztouch_console_lay、0touch_console_switch_lay（dm_padmode.c 见到本文件才改写前缀）\n')
        return new[0], os.path.getsize(os.path.join(dst,'data',new[0]))
    n0,z0=emit(REPL,out,'')
    print(f'romfs_overlay：{n0} {z0} 字节，底图 {w}x{h}；前端布局 {len(tl)} 个、挪走键位提示 {ntl} 处')
    for d in (out+'_astc',out+'_astc_variants'): shutil.rmtree(d,ignore_errors=True); os.makedirs(d)
    if ASTC:
        idx=os.path.join(ASTC,'pack_index.tsv'); assert os.path.isfile(idx) and os.path.isfile(os.path.join(ASTC,'pack.json')), 'DM_ASTC_PACK 目录里缺 pack_index.tsv / pack.json：'+ASTC
        na,za=emit(REPL,out+'_astc','astc_',idx); shutil.copyfile(os.path.join(ASTC,'pack.json'),os.path.join(out+'_astc','pack.json'))
        assert na==n0, f'El archivo ASTC {na} y el BC {n0} deben compartir nombre para la precarga'
        print(f'romfs_overlay_astc：{na} {za} 字节（贴图指向补充包 {ASTC}）')
    # 走覆盖层的自定义开关（按组合出全部 2^n 份，App 启动按 dm_custom_get 挑一份，重启生效；开关到第 4 个再换「拼表」方案）。
    # 每项 = (自定义页的键, 模块名)：模块提供 build(romfs, 临时目录, 前面已换的 repl) → ({包内逻辑路径: 本地文件}, 统计)，在传入的 repl 之上改（同一文件多个特性都改时自然叠加）。
    # out 本身 = 全关（宿主验收 / 触摸链用它）；其余组合写到 out_variants/<键+键…>/data，各带 features.txt。
    # Custom page switches that go through the overlay (all 2^n copies are produced by combination; at launch the App picks one by dm_custom_get, effective after restart; switch to the "table-stitching" scheme once there is a 4th switch).
    # Each entry = (customization page key, module name): the module provides build(romfs, temp dir, the repl replaced so far) → ({logical path in package: local file}, stats), modifying on top of the passed-in repl (naturally stacking when several features change the same file).
    # out itself = all off (host acceptance / the touch chain use it); the other combinations are written to out_variants/<key+key...>/data, each with its features.txt.
    FEATURES=[('item_notes','annot_strings')]   # 物品注释与旧译名；以后走覆盖层的开关在这里追加一项 | Item notes and old translated names; future switches that go through the overlay append an entry here
    import importlib, itertools
    shutil.rmtree(out+'_variants',ignore_errors=True)
    for r in range(1,len(FEATURES)+1):
        for combo in itertools.combinations(FEATURES,r):
            RS=dict(REPL); touched=set(); stats={}
            for key,mod in combo:
                ta,st=importlib.import_module(mod).build(romfs,t,RS); RS.update(ta); touched|=set(ta); stats[key]=st
            # 除特性文件外逐文件相同：没被本组合的特性改过的路径必须原样指向全关那份的文件
            # File-by-file identical except for feature files: paths not changed by this combination's features must point unchanged to the files of the all-off copy
            diff=[k for k in RS if k not in touched and RS[k]!=REPL.get(k)]
            if diff: raise SystemExit('gen_romfs_overlay：组合 %s 除特性文件外与全关那份不一致：%s' % ([k for k,_ in combo],diff))
            name='+'.join(k for k,_ in combo); dst=os.path.join(out+'_variants',name)
            n1,z1=emit(RS,dst,name+'_')
            for key,mod in combo:   # 特性模块自带的旁路文件（不进 CASC 的，如物品注释的文字框对照表）写在变体目录根上，宿主按 DM_ROMFS_OVERLAY 找 | Sidecar files that feature modules bring along (those not in CASC, e.g. the item notes' text box mapping table) are written at the variant directory root; the host finds them via DM_ROMFS_OVERLAY
                m=importlib.import_module(mod)
                if hasattr(m,'write_textbox_table'): m.write_textbox_table(dst)
            with open(os.path.join(dst,'features.txt'),'w') as f: f.write('\n'.join(k for k,_ in combo)+'\n'+''.join(f'{p}\n' for p in sorted(touched)))
            print(f'romfs_overlay_variants/{name}：{n1} {z1} 字节；改了 {len(touched)} 个文件；{stats}')
            if ASTC:   # 同一组合的 ASTC 版：索引另出，旁路文件（features.txt、文字框对照表…）照抄 | ASTC version of the same combination: the index is emitted separately, sidecar files (features.txt, text box mapping table...) are copied as is
                da=os.path.join(out+'_astc_variants',name); emit(RS,da,name+'_astc_',os.path.join(ASTC,'pack_index.tsv'))
                for f in os.listdir(dst):
                    if f!='data': shutil.copyfile(os.path.join(dst,f),os.path.join(da,f))
