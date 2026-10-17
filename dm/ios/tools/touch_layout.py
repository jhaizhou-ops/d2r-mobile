#!/usr/bin/env python3
# 触摸方案的界面呈现：从 Switch 真包读原版布局，把手柄键位提示挪到屏幕外，交给 gen_romfs_overlay.py 进覆盖层。
# 事实（Mac 截图对比核过）：游戏不认布局里的 "visible": false；把控件 rect 挪出屏幕有效、不报错、不会被代码挪回。
# Switch 的布局查找顺序是 Controller/Console/Switch → Controller/Console → Controller → 基础布局，所以同名文件各层都要处理。
# 布局文件带注释和尾逗号，标准 JSON 解析器读不了，这里一律按文本改。
# UI presentation for the touch scheme: read the original game's layouts from the real Switch package, move the controller button prompts off screen, and hand them to gen_romfs_overlay.py to go into the overlay.
# Facts (verified by comparing Mac screenshots): the game does not honor "visible": false in a layout; moving a widget's rect off screen works, raises no error, and is not moved back by code.
# The Switch layout lookup order is Controller/Console/Switch → Controller/Console → Controller → base layout, so a same-named file must be handled at every layer.
# Layout files carry comments and trailing commas that a standard JSON parser cannot read, so everything here is edited as text.
import json, os, re, struct, zlib, hashlib
OFF = '"rect": { "x": 99999, "y": 99999, "width": 1, "height": 1 }'

def _read_entry(romfs, f):
    ek = bytes.fromhex(f[6]); raw_len = int(f[7])
    with open(os.path.join(romfs, 'data', '%02d-%08d.data' % (ek[9], ek[10])), 'rb') as s: s.seek(int.from_bytes(ek[11:16], 'big')); raw = s.read(raw_len)
    if f[8] == 'z': data = zlib.decompress(raw)
    elif f[8] == 'n': data = raw
    else:
        hs = struct.unpack('>I', raw[4:8])[0]; pos = hs; data = b''
        for i in range(int.from_bytes(raw[9:12], 'big')):
            e, _ = struct.unpack('>II', raw[12 + i * 24:20 + i * 24]); b = raw[pos:pos + e]; data += b[1:] if b[:1] == b'N' else zlib.decompress(b[1:]); pos += e
    assert hashlib.md5(data).hexdigest() == f[4], f[9]
    return data

# 手柄键图标（全触摸方案：任何地方都不该再出现手柄键）：游戏按当前手柄类型从 Controller/ControllerButtons/switch/ 取 22 张小贴图画键位图标——
# 面板提示条、页签两端的 ZL / ZR、NPC 名字旁的 A 都是它们（后者是代码拼的，布局里没有控件可挪）。整目录换成同尺寸的全透明贴图，一次去干净。
# 贴图 = SpA1 v61：头 0x28 字节 + DXT5 块；块全 0 = 透明，头原样保留（尺寸、帧数不变，排版不动）。
# Controller key icons (full-touch scheme: controller keys should not appear anywhere any more): the game takes 22 small textures from Controller/ControllerButtons/switch/ according to the current controller type to draw the key icons --
# the panel hint bars, the ZL / ZR at both ends of the tabs and the A next to NPC names are all these (the last is assembled in code, with no widget in the layout to move). The whole directory is replaced with fully transparent textures of the same size, removing them all at once.
# Texture = SpA1 v61: 0x28-byte header + DXT5 blocks; all-zero blocks = transparent, the header is kept as-is (size and frame count unchanged, layout untouched).
def build_cursor(romfs, outdir):
    """面板里的手柄光标圆环（Controller/Cursor/Idle、Hold、Held 三张）换成透明：全触摸下光标内部照用（拖放、选中靠它移动），只是不画。
    拿在手上的物品图标是另画的，不受影响。
    The controller cursor ring in panels (the three images Controller/Cursor/Idle, Hold, Held) is replaced with transparent ones: under full touch the cursor is still used internally (drag-and-drop and selection rely on it moving), it is just not drawn.
        The icon of the item held in hand is drawn separately and is unaffected."""
    repl = {}
    for line in open(os.path.join(romfs, 'data', 'build_manifest.psv'), encoding='utf-8'):
        f = line.rstrip('\n').split('|')
        if len(f) < 11 or line.startswith('#') or '/ui/controller/cursor/' not in f[9].lower() or not f[9].lower().endswith('.sprite'): continue
        d = _read_entry(romfs, f); assert d[:4] == b'SpA1' and struct.unpack('<H', d[4:6])[0] == 61 and struct.unpack('<I', d[0x20:0x24])[0] == len(d) - 0x28, f[9]
        path = f[9].split(':', 1)[1]; o = os.path.join(outdir, 'cursor_' + os.path.basename(path)); open(o, 'wb').write(d[:0x28] + bytes(len(d) - 0x28)); repl[path.lower()] = o
    if len(repl) != 3: raise SystemExit('touch_layout：手柄光标贴图应为 3 张，找到 %d 张（原包变了？）' % len(repl))
    return repl
def build_glyphs(romfs, outdir):
    repl = {}
    for line in open(os.path.join(romfs, 'data', 'build_manifest.psv'), encoding='utf-8'):
        f = line.rstrip('\n').split('|')
        if len(f) < 11 or line.startswith('#') or '/ui/controller/controllerbuttons/switch/' not in f[9].lower(): continue
        d = _read_entry(romfs, f); assert d[:4] == b'SpA1' and struct.unpack('<H', d[4:6])[0] == 61 and struct.unpack('<I', d[0x20:0x24])[0] == len(d) - 0x28, f[9]
        path = f[9].split(':', 1)[1]; o = os.path.join(outdir, 'glyph_' + os.path.basename(path)); open(o, 'wb').write(d[:0x28] + bytes(len(d) - 0x28)); repl[path.lower()] = o
    if len(repl) != 22: raise SystemExit('touch_layout：手柄键图标应为 22 张，找到 %d 张（原包变了？）' % len(repl))
    return repl

def read_layout(romfs, rel, suffix=None):
    """rel 形如 controller/console/mainmenupanelhd.json（不分大小写）。返回 (包内逻辑路径, 文本)。suffix：改按任意路径后缀取（字符串表等非布局文件）。 | rel looks like controller/console/mainmenupanelhd.json (case-insensitive). Returns (logical path in package, text). suffix: fetch by an arbitrary path suffix instead (non-layout files such as string tables)."""
    want = (suffix or '/ui/layouts/' + rel).lower()
    for line in open(os.path.join(romfs, 'data', 'build_manifest.psv'), encoding='utf-8'):
        f = line.rstrip('\n').split('|')
        if len(f) < 11 or line.startswith('#') or not f[9].lower().endswith(want): continue
        ek = bytes.fromhex(f[6]); raw_len = int(f[7])
        with open(os.path.join(romfs, 'data', '%02d-%08d.data' % (ek[9], ek[10])), 'rb') as s: s.seek(int.from_bytes(ek[11:16], 'big')); raw = s.read(raw_len)
        if f[8] == 'z': data = zlib.decompress(raw)
        elif f[8] == 'n': data = raw
        else:
            hs = struct.unpack('>I', raw[4:8])[0]; pos = hs; data = b''
            for i in range(int.from_bytes(raw[9:12], 'big')):
                e, _ = struct.unpack('>II', raw[12 + i * 24:20 + i * 24]); b = raw[pos:pos + e]; data += b[1:] if b[:1] == b'N' else zlib.decompress(b[1:]); pos += e
        assert hashlib.md5(data).hexdigest() == f[4], rel
        return f[9].split(':', 1)[1], data.decode('utf-8-sig')
    raise SystemExit('touch_layout：包里没有布局 ' + rel)

def _block_end(s, i):   # i 指向 '{'，返回配对 '}' 的下标（跳过字符串与 // 注释） | i points at '{'; returns the index of the matching '}' (skipping strings and // comments)
    d = 0; q = False
    while i < len(s):
        c = s[i]
        if q:
            if c == '\\': i += 1
            elif c == '"': q = False
        elif c == '"': q = True
        elif c == '/' and s[i:i + 2] == '//': i = s.find('\n', i)
        elif c == '{': d += 1
        elif c == '}':
            d -= 1
            if d == 0: return i
        i += 1
    raise SystemExit('touch_layout：括号不配对')

def offscreen(s, types=('ButtonLegendWidget', 'ButtonLegendScrollWidget'), names=()):
    """把这些类型 / 名字的控件挪出屏幕。返回 (新文本, 改了几处)。只动控件自己 fields 第一层的 rect / fitToParent。 | Move widgets of these types / names off screen. Returns (new text, number of places changed). Only touches rect / fitToParent at the first level of the widget's own fields."""
    n = 0; pats = [r'"type"\s*:\s*"%s"' % re.escape(t) for t in types] + [r'"type"\s*:\s*"[A-Za-z0-9_]+"\s*,\s*"[Nn]ame"\s*:\s*"%s"' % re.escape(x) for x in names]   # 名字只认控件声明（"type" 紧跟 "name"）：导航引用里的同名项不动 | Names match only widget declarations ("type" immediately followed by "name"): same-named entries in navigation references are left alone
    for pat in pats:
        pos = 0
        while True:
            m = re.compile(pat).search(s, pos)
            if not m: break
            line0 = s.rfind('{', 0, m.start())                       # 控件对象的起点：往前最近的 '{' | Start of the widget object: the nearest '{' going backward
            nxt = re.compile(r'"(type|children)"\s*:').search(s, m.end()); end = nxt.start() if nxt else len(s)
            f = re.compile(r'"fields"\s*:\s*\{').search(s, line0, max(end, m.end()))
            if f:
                a = f.end() - 1; b = _block_end(s, a); body = s[a + 1:b]; depth = 0; i = 0; keys = {}
                while i < len(body):
                    c = body[i]
                    if c == '"':
                        j = body.find('"', i + 1)
                        while body[j - 1] == '\\': j = body.find('"', j + 1)
                        if depth == 0 and body[j + 1:].lstrip().startswith(':'): keys.setdefault(body[i + 1:j], i)
                        i = j
                    elif c == '/' and body[i:i + 2] == '//': i = body.find('\n', i)
                    elif c in '{[': depth += 1
                    elif c in '}]': depth -= 1
                    i += 1
                if 'rect' in keys: r0 = body.index('{', keys['rect']); body = body[:keys['rect']] + OFF + body[_block_end(body, r0) + 1:]
                else: body = ' ' + OFF + ',' + body
                body = re.sub(r'"fitToParent"\s*:\s*true', '"fitToParent": false', body, count=1)
                s = s[:a + 1] + body + s[b:]; pos = a + 1 + len(body)
            else:
                ins = ' "fields": { ' + OFF + ' },'; at = m.end(); at = at + 1 if s[at:at + 1] == ',' else at
                s = s[:at] + (ins if s[at - 1] == ',' else ',' + ins) + s[at:]; pos = at + len(ins) + 1
            n += 1
    return s, n

# 前端与通用弹窗（第 ② 步）：键位提示控件全部挪走；选角页 / 过场列表底部那条半透明底条（LegendBackground）一并挪走
# 选角页（主菜单 / 存档列表 / 左侧栏）不在这里：整份换成键鼠版界面，见 PCFRONT
# Front end and common popups (step ②): all button prompt widgets are moved away; the semi-transparent backing strip (LegendBackground) at the bottom of the character select page / cutscene list is moved away as well
# The character select page (main menu / save list / left sidebar) is not handled here: it is replaced entirely with the keyboard-and-mouse UI, see PCFRONT
FRONTEND = [('controller/charactercreatepanelhd.json', ()), ('controller/console/charactercreatepanelhd.json', ('GameType', 'GameTypeBackground')),   # 标题下的「离线」字样与底条 | The 「离线」(Offline) label under the title and its backing strip
            ('controller/console/charactercreatenamepanelhd.json', ('EnterNameLabel',)), ('controller/console/characteroptionspanelhd.json', ('LadderFlag', 'LadderToggle', 'NonLadderToggle', 'LadderCenter')),   # 模式页只留标准 / 专家一行（单机只支持这两种） | The mode page keeps only the standard / hardcore row (single player supports only these two)
           
            ('controller/cinematicsmodalhd.json', ('LegendBackground',)), ('controller/console/cinematicsmodalhd.json', ('LegendBackground',)),
            ('controller/creditsmodalhd.json', ()), ('controller/eulamodalhd.json', ()),
            ('controller/confirmationmodalhd.json', ()), ('controller/dismissablemodalhd.json', ()),
            ('controller/deletecharactermodalhd.json', ()), ('controller/renamecharactermodalhd.json', ())]

# 选角页（有存档时的首页）：键位提示挪走后页面上没有可点的入口，在角色卡下加三个原生按钮。
# 加在主菜单面板（角色卡 PlayerInfoContainer 所在层，水平锚在屏幕中心：卡 x 640 宽 841，按钮图宽 470 → 居中 x 825），消息用游戏已有的。
# Character select page (the home page when a save exists): after the button prompts are moved away the page has no tappable entry, so three native buttons are added under the character card.
# They are added to the main menu panel (the layer holding the character card PlayerInfoContainer, horizontally anchored at the screen center: card x 640 width 841, button image width 470 → centered at x 825), using messages the game already has.
def _btn(name, y, text, msg, x=825, anchor=True, scale=None):
    return ('        {\n            "type": "ButtonWidget", "name": "%s",\n            "fields": {\n%s                "rect": { "x": %d, "y": %d%s },\n'
            '                "filename": "FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonMed",\n                "textString": "%s",\n                "onClickMessage": "%s",\n'
            '                "textColor": "$LightButtonTextColor",\n                "text/style": "$StyleFEButtonText",\n                "hoveredFrame": 3,\n                "disabledFrame": 2,\n            }\n        },\n') % (name, '                "anchor": {"x": 0.5},\n' if anchor else '', x, y, ', "scale": %s' % scale if scale else '', text, msg)
# 职业选择 / 起名页的「返回 / 创建」：前端按钮图原版大小（同页不要两种大小；先前放大 1.4 倍的做法撤掉），手指尺寸靠命中吸附
# 「返回 / 创建」(Back / Create) on the class selection / naming pages: front-end button image at the original game's size (no two sizes on one page; the earlier 1.4× enlargement is withdrawn), with finger size handled by hit snapping
def _btn_big(name, x, y, text, msg):
    return _btn(name, y, text, msg, x=x, anchor=False)
INSERT = {
          # 职业选择页：底部键位提示原来的位置放一个「返回」（面板本身锚在屏幕中心，按钮图宽 470 → x -235）；消息同 PC 版 ToMainMenu 按钮
          # Class selection page: a 「返回」(Back) is placed where the bottom button prompts used to be (the panel itself is anchored at the screen center, button image width 470 → x -235); same message as the PC version's ToMainMenu button
          'controller/console/charactercreatepanelhd.json': (r'\{\s*"type"\s*:\s*"ButtonLegendWidget"\s*,\s*"name"\s*:\s*"CharacterCreateLegend"',
          # 「继续」居中（进模式页，消息同手柄确认；选定要有明确入口）；「返回」在它左边，宿主只在有存档时显示（dm_cursor.c）
          # 「创建」居中（宽 728 → x -364），「返回」在它左边隔 80 单位；y 1850 起高 255，在职业竖条区（y < 1750）之下。文案用「创建」
          # 「继续」(Continue) is centered (enters the mode page, same message as controller confirm; a selection needs an explicit entry); 「返回」(Back) is to its left, shown by the host only when a save exists (dm_cursor.c)
          # 「创建」(Create) is centered (width 728 → x -364), with 「返回」(Back) 80 units to its left; from y 1850 with height 255, below the class strip area (y < 1750). The wording used is 「创建」(Create)
          _btn_big('TouchNext', -235, 1900, '@UICreate', 'ConsoleCharacterCreate:ToConsoleCharacterOptions')
          + _btn_big('TouchBack', -785, 1900, '@UIBack', 'FrontEndNavigation:ToCharacterSelect')),
          # 起名页：键盘收起后要有路可走——「创建」「返回」两个按钮（发 PanelManager:ClosePanel:TouchKey<键>，宿主换成原版的 ＋ / B）；「按 Ⓐ 输入名称」那行挪走，点输入框即重新弹键盘
          # Naming page: there must be a way forward after the keyboard is dismissed -- the two buttons 「创建」(Create) and 「返回」(Back) (send PanelManager:ClosePanel:TouchKey<key>, which the host converts to the original game's + / B); the line 「按 Ⓐ 输入名称」(Press Ⓐ to enter a name) is moved away, and tapping the input box brings the keyboard up again
          'controller/console/charactercreatenamepanelhd.json': (r'\{\s*"type"\s*:\s*"ButtonLegendWidget"\s*,\s*"name"\s*:\s*"CharacterNameLegend"',
          # 两个按钮对称放在输入框下方，中间隔 120 单位；y 320 让开拒名弹窗的下沿
          # The two buttons are placed symmetrically below the input box, 120 units apart; y 320 clears the bottom edge of the name-rejected popup
          _btn_big('TouchNameBack', -659, 340, '@UIBack', 'PanelManager:ClosePanel:TouchKeyB') + _btn_big('TouchNameOK', 189, 340, '@UICreate', 'PanelManager:ClosePanel:TouchKeyPLUS'))}

# 职业选择页八个职业的点击矩形：原版布局与人物实际站位对不上（术士 x 1000–1465 与德鲁伊 980–1445 重合，人却站在 x≈924、落在法师的 590–970 里），
# 手柄用左右键选所以原版没暴露。改成按实际站位的中心线取中点分的八条竖条（Mac 截图量得中心线 -1272 -912 -588 -204 168 576 924 1392，
# 布局单位，原点 = 屏幕水平中心），上下 450–1750（上面是标题说明、下面是按钮）。真机上想点术士总是出德鲁伊或法师。
# Tap rectangles of the eight classes on the class selection page: the original game's layout does not match where the characters actually stand (warlock x 1000–1465 overlaps druid 980–1445, yet the character stands at x≈924, which falls inside the sorceress's 590–970),
# and the original game never exposed this because the controller selects with left/right keys. Changed to eight vertical strips split at the midpoints between the center lines of the actual standing positions (center lines measured from a Mac screenshot: -1272 -912 -588 -204 168 576 924 1392,
# layout units, origin = horizontal center of the screen), vertically 450–1750 (title and description above, buttons below). On the real device, trying to tap the warlock always gave the druid or the sorceress.
CLASS_STRIPS = [('Amazon', -1500, -1092), ('Assassin', -1092, -750), ('Necromancer', -750, -396), ('Barbarian', -396, -18),
                ('Paladin', -18, 372), ('Sorceress', 372, 750), ('Warlock', 750, 1158), ('Druid', 1158, 1640)]
# 建角「选择游戏模式」页的「极乐世界」勾选项（原版勾选框 + 标题 + 一行说明，放在标准 / 专家那行下面左侧；它是附加项，不是第三种模式）。
# 勾选框 = 原版开关按钮（建局高级选项 / 储藏箱页那个勾选框的图），标题与说明用本页牌子上的两个字样式。点击消息是空操作，勾选由控件自己切换，
# 宿主读它的状态（dm_cursor.c dm_fe_bliss）。位置数字（-1551, 1330）宿主那边也用到，改这里要一起改。
# The 「极乐世界」(Elysium) checkbox item on the character creation 「选择游戏模式」(Select game modes) page (original-game checkbox + title + one line of description, placed at the left below the standard / hardcore row; it is an add-on, not a third mode).
# Checkbox = the original game's toggle button (the checkbox image from the game creation advanced options / stash page), and the title and description use the two text styles from this page's plaques. The tap message is a no-op, the check state is toggled by the widget itself,
# and the host reads its state (dm_cursor.c dm_fe_bliss). The position numbers (-1551, 1330) are also used on the host side, so change them together when changing here.
_BLISS = ('        { "type": "Widget", "name": "TouchBlissWidget", "children": [\n        {\n            "type": "ToggleButtonWidget", "name": "TouchBliss",\n            "fields": {\n'
          '                "rect": { "x": -1551, "y": 1330 }, "hitMargin": { "left": 30, "top": 30, "right": 30, "bottom": 30 },\n'
          '                "filename": "Lobby\\\\CreateGame\\\\CreateGame_AdvancedCheckbox",\n                "untoggledFrame": 0, "untoggledPressedFrame": 1, "untoggledHoveredFrame": 3, "untoggledDisabledFrame": 2,\n'
          '                "toggledFrame": 4, "toggledPressedFrame": 5, "toggledHoveredFrame": 6,\n                "onClickMessage": "PanelManager:ClosePanel:TouchBliss",\n            }\n        },\n        ] },\n'
          '        { "type": "TextBoxWidget", "name": "TouchBlissTitle", "fields": { "rect": { "x": -1451, "y": 1322, "width": 690, "height": 60 }, "text": "@dmBlissTitle", "style": "$StyleCharacterModeSelectTitle" } },\n'
          '        { "type": "TextBoxWidget", "name": "TouchBlissDesc", "fields": { "rect": { "x": -1451, "y": 1400, "width": 3000, "height": 120 }, "text": "@dmBlissDesc", "style": "$StyleCharacterModeSelectDescription" } },\n')
INSERT['controller/console/characteroptionspanelhd.json'] = (r'\{\s*"type"\s*:\s*"ButtonLegendWidget"\s*,\s*"name"\s*:\s*"CharacterOptionsLegend"', _BLISS)
def pad_bliss(s):   # 手柄版建角模式页（原版布局原样）+ 「极乐世界」那一行（同触屏版定稿的那一行）+ 勾选框左边一个原版 Y 键帽（Switch 手柄左键）： | Controller-version character-creation mode page (original layout unchanged) + the "Elysium" row (the same row as finalized for the touch version) + an original Y keycap to the left of the checkbox (the left face button on a Switch controller):
    # 这一页原版只有写死的几行、没有自由光标，做不成「移上去按 A」，按 Y 勾 / 取消（dm_cursor.c；做不成可聚焦的一行就用快捷键，哪个键都行）
    # 「天梯 / 非天梯」一行同触屏版一样挪走（单机只支持标准 / 专家）；键位提示条留着（手柄版界面）
    # In the original game this page has only a few hard-coded rows and no free cursor, so "move onto it and press A" cannot be done;
    # press Y to check / uncheck (dm_cursor.c; when a focusable row cannot be made, use a shortcut key, and any key will do).
    # The "Ladder / Non-Ladder" row is moved away just as in the touch version (single player only supports Standard / Hardcore);
    # the button legend is kept (controller-version UI).
    s, n = offscreen(s, types=(), names=('LadderFlag', 'LadderToggle', 'NonLadderToggle', 'LadderCenter')); assert n == 4, n
    # 提示条里的「X 经典版」去掉（宿主同时把这一页的 X 收掉，dm_cursor.c）：单机只支持资料片的标准 / 专家角色，同触屏版
    # Remove "X Classic" from the button legend (the host also swallows X on this page, see dm_cursor.c): single player only supports
    # Standard / Hardcore expansion characters, same as the touch version.
    s, n = re.subn(r'\{\s*"action"\s*:\s*"option2"\s*,\s*"description"\s*:\s*"@LegacyGameVersionSwitch"\s*\}\s*,?', '', s); assert n == 1, n
    at, add = INSERT['controller/console/characteroptionspanelhd.json']; ms = list(re.finditer(at, s)); assert len(ms) == 1
    key = '        { "type": "ImageWidget", "name": "TouchBlissKey", "fields": { "rect": { "x": -1665, "y": 1330, "scale": 1.5 }, "filename": "Controller/ControllerButtons/Switch/ButtonLeft" } },\n'
    if os.environ.get('DM_OVERLAY_MAC'):   # Mac App：同位再带一个键盘键帽（缺省在屏外，玩家用键盘时宿主把两个对调，dm_padmode.c） | Mac App: one more keyboard key cap at the same position (off screen by default; when the player uses the keyboard the host swaps the two, dm_padmode.c)
        key += '        { "type": "ImageWidget", "name": "TouchBlissKeyK", "fields": { "rect": { "x": 99999, "y": 1330, "scale": 1.5 }, "filename": "Ztouch_controller_buttons_ui/switchKButtonLeft" } },\n'
    return s[:ms[0].start()] + add.lstrip() + key + '        ' + s[ms[0].start():]
def set_rect(s, name, x, y, w, h):
    m = re.search(r'"name"\s*:\s*"%s"' % re.escape(name), s); assert m, name
    r = re.compile(r'"rect"\s*:\s*\{[^}]*\}').search(s, m.end()); assert r and r.start() - m.end() < 200, name
    return s[:r.start()] + '"rect": { "x": %d, "y": %d, "width": %d, "height": %d }' % (x, y, w, h) + s[r.end():]

def build(romfs, outdir, files=FRONTEND):
    """生成改好的布局到 outdir，返回 {包内逻辑路径(小写): 本地文件}；每个文件至少要改到 1 处，否则报错（原版布局变了就不该悄悄漏掉）。 | Generate the modified layouts into outdir and return {logical path in package (lowercase): local file}; each file must have at least 1 place changed, otherwise it is an error (if the original game's layout has changed it must not be silently missed)."""
    repl = {}; total = 0
    for rel, names in files:
        path, text = read_layout(romfs, rel); new, n = offscreen(text, names=names)
        if n == 0: raise SystemExit('touch_layout：%s 里没找到键位提示控件（原版布局变了？）' % rel)
        if rel == 'controller/console/charactercreatepanelhd.json':
            for nm, x0, x1 in CLASS_STRIPS: new = set_rect(new, nm, x0, 450, x1 - x0, 1300)
        if rel in INSERT:
            at, add = INSERT[rel]; ms = list(re.finditer(at, new)); assert len(ms) == 1, rel; new = new[:ms[0].start()] + add.lstrip() + '        ' + new[ms[0].start():]
        o = os.path.join(outdir, rel.replace('/', '_')); open(o, 'w', encoding='utf-8').write(new); repl[path.lower()] = o; total += n
    return repl, total

# 选角页改用键鼠版界面（裁剪后只留右侧存档列表，简洁有效，也符合右手为主要操作手的习惯）。
# 做法：手柄版（controller/console/…）这几份布局整份换成同名键鼠版的内容，再做阉割。每项：手柄版路径 → (键鼠版路径, 变换函数 或 None)
# The character select page switches to the keyboard-and-mouse UI (after trimming only the save list on the right remains; simple and effective, and it also fits the habit of the right hand being the main operating hand).
# Approach: these controller layouts (controller/console/...) are replaced entirely with the content of the same-named keyboard-and-mouse layouts and then stripped down. Each entry: controller layout path → (keyboard-and-mouse layout path, transform function or None)
SAFE_X = 200   # 两侧面板整体往里收的量（布局单位，≈ 122 像素 ≈ 41 点）：收完后按钮与文字都在 62 点安全线内，面板外框可以留在安全线外 | Amount by which the two side panels are pulled inward as a whole (layout units, ≈ 122 pixels ≈ 41 points): after this the buttons and text are all within the 62-point safe line, and the panel outer frames may stay outside the safe line
def _sub1(s, old, new):
    if s.count(old) != 1: raise SystemExit('touch_layout：键鼠版前端布局里「%s」不是恰好 1 处（原版布局变了？）' % old[:60])
    return s.replace(old, new)
# 构建号 + 新版本提示：都是主菜单面板（选角页那一层）的子控件，用原版的字与「进入游戏」那张按钮图。
# 布局里文字全空、按钮与底板摆在屏幕外：字由宿主写（dm/host/dm_update.c：构建号常显；查到新版本才写提示），按钮与底板由宿主挪回下面 _UPD_POS 的位置。
# 普通档 = 左下角构建号右边一行字 + 0.6 倍的小「更新」（这一行从屏幕左边量、让开左安全区，x 由宿主算：字多宽按钮就跟到哪）；建议立即更新档 = 「进入游戏」（x -303, y 1865）正上方：半透明底 + 红字标题 + 说明 + 同大的「更新」。
# 两个「更新」都发 PanelManager:ClosePanel:TouchUpdate（关一个不存在的面板 = 空操作，宿主当信号收）。位置数字宿主那边也用到，改这里要一起改。
# Build number + new version notice: both are child widgets of the main menu panel (the layer of the character select page), using the original game's text and the 「进入游戏」(Play) button image.
# In the layout all text is empty and the button and backing are placed off screen: the text is written by the host (dm/host/dm_update.c: build number always shown; the notice is written only when a new version is found), and the button and backing are moved back by the host to the positions in _UPD_POS below.
# Normal tier = one line of text to the right of the build number at the bottom left + a small 0.6× 「更新」(Update) (this line is measured from the left edge of the screen and clears the left safe area; x is computed by the host: the button follows wherever the text width ends); recommended-immediate-update tier = right above 「进入游戏」(Play) (x -303, y 1865): semi-transparent backing + red title + description + a same-size 「更新」(Update).
# Both 「更新」(Update) buttons send PanelManager:ClosePanel:TouchUpdate (closing a panel that does not exist = no-op, which the host receives as a signal). The position numbers are also used on the host side, so change them together when changing here.
_C = '"anchor": { "x": 0.5 }, '
def _upd_text(name, x, y, w, size, color, h='left', anchor='', fit=''):
    return ('        { "type": "TextBoxWidget", "name": "%s", "fields": { %s"rect": { "x": %d, "y": %d, "width": %d, "height": 60 }, "text": "", %s'
            '"style": { "pointSize": %d, "fontColor": { "r": %d, "g": %d, "b": %d, "a": 255 }, "alignment": { "h": "%s", "v": "center" }, "dropShadow": "$DefaultDropShadow" } } },\n') % ((name, anchor, x, y, w, fit, size) + color + (h,))
def _upd_btn(name, scale, anchor=_C):
    return ('        { "type": "ButtonWidget", "name": "%s", "fields": { %s"rect": { "x": 99999, "y": 99999%s }, "filename": "FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonLarge", '
            '"textString": "@dmUpdButton", "onClickMessage": "PanelManager:ClosePanel:TouchUpdate", "text/style": "$StyleFEButtonText", "pointSize": "$ExtraLargeFontSize", "textColor": "$LightButtonTextColor", '
            '"hoveredFrame": 3, "disabledFrame": 2%s } },\n') % (name, anchor, ', "scale": %s' % scale if scale else '', ', "hitMargin": { "left": 30, "top": 40, "right": 30, "bottom": 40 }' if scale else '')
_UPDATE = (_upd_text('TouchBuild', 79, 2046, 420, 30, (150, 140, 115))
           + _upd_text('TouchUpdNote', 420, 2046, 1500, 30, (199, 179, 119), fit='"fitToText": true, ')
           + _upd_btn('TouchUpdSmall', 0.6, '')
           + '        { "type": "RectangleWidget", "name": "TouchUpdBg", "fields": { "anchor": { "x": 0.5 }, "rect": { "x": 99999, "y": 99999, "width": 1400, "height": 186 }, "color": [ 0.0, 0.0, 0.0, 0.59 ] } },\n'
           + _upd_text('TouchUpdTitle', -700, 1524, 1400, 46, (214, 74, 58), 'center', _C)
           + _upd_text('TouchUpdDesc', -700, 1610, 1400, 36, (232, 228, 214), 'center', _C)
           + _upd_btn('TouchUpdBig', None))
# 手柄版原版主菜单上的同一组控件（pad_mainmenu）：左下角是原版的按钮栏和提示条，构建号与提示字放右上角、靠右对齐。
# 原版手柄主菜单面板的矩形是 0×0，锚点无从算起：这一组都不带锚点，位置全由宿主按屏宽摆（dm_update.c），这里的坐标只是 16:9 下的初值
# The same group of widgets on the controller layout's original main menu (pad_mainmenu): the bottom left holds the original button ribbon and legend, so the build number and hint text go to the top right, right-aligned.
# The original controller main menu panel's rectangle is 0×0, with nothing to compute an anchor from: none of this group carries an anchor, their position is set entirely by the host from the screen width (dm_update.c), and the coordinates here are only initial values for 16:9
_UPDATE_PAD = (_upd_text('TouchBuild', 3341, 60, 420, 30, (150, 140, 115), 'right')
               + _upd_text('TouchUpdNote', 1761, 130, 2000, 42, (199, 179, 119), 'right')   # 手柄版没有「更新」按钮可看，这一行字放大（触屏版同色 30 号） | The controller layout has no 「更新」(Update) button to look at, so this line of text is enlarged (the touch version uses the same color at size 30)
               + _upd_btn('TouchUpdSmall', 0.6, '')
               + '        { "type": "RectangleWidget", "name": "TouchUpdBg", "fields": { "rect": { "x": 99999, "y": 99999, "width": 1400, "height": 186 }, "color": [ 0.0, 0.0, 0.0, 0.59 ] } },\n'
               + _upd_text('TouchUpdTitle', 1220, 1524, 1400, 46, (214, 74, 58), 'center')
               + _upd_text('TouchUpdDesc', 1220, 1610, 1400, 36, (232, 228, 214), 'center')
               + _upd_btn('TouchUpdBig', None, ''))
def pad_ribbon(s):   # 手柄版主菜单左侧按钮栏 = 原版布局，只拿掉「电影动画」：包里不播过场（列表里只有一行有字、选了什么都不播），是个没用的入口，同触屏版没有它。 | Controller layout main menu left button ribbon = the original layout, only removing 「电影动画」(Cinematics): the package does not play cutscenes (only one row in the list has text, and nothing plays whichever is chosen), so it is a useless entry; same as the touch version, which does not have it.
    # 「制作人员」上移到它的位置，上下焦点改成「选项 ↔ 制作人员」直连
    # 「制作人员」(Credits) moves up into its place, and up / down focus becomes a direct link 「选项 ↔ 制作人员」(Options ↔ Credits)
    s = _sub1(s, '"rect": { "x": 164, "y": 1175 },', '"rect": { "x": 99999, "y": 99999 },')
    s = _sub1(s, '"rect": { "x": 164, "y": 1325 },', '"rect": { "x": 164, "y": 1175 },')
    s = _sub1(s, '"down": { "name": "Cinematics" }', '"down": { "name": "Credits" }')
    s = _sub1(s, '"up": { "name": "Cinematics" }', '"up": { "name": "Settings" }')
    return s
def pad_mainmenu(s):   # 手柄版主菜单 = 原版布局原样 + 上面这组控件（加在根 children 末尾） | Controller layout main menu = the original layout as is + the group of widgets above (appended at the end of the root children)
    i = s.rstrip().rfind(']'); assert s.rstrip().endswith('}') and i > 0
    return s[:i].rstrip(' ') + _UPDATE_PAD + '    ' + s[i:]
_UPDATE_STUB = ''.join('        { "type": "%s", "name": "%s" },\n' % (ty, nm) for ty, nm in re.findall(r'"type": "(\w+)", "name": "(TouchBuild|TouchUpd\w+)"', _UPDATE))
assert _UPDATE_STUB.count('\n') == 7
def _pc_mainmenu(s):
    # 「进入游戏」居中（键鼠版它在左半、右半是联机大厅按钮）；下面的齿轮（联机建局设置）、分隔条、联机大厅按钮挪走
    # 「进入游戏」(Play) is centered (in the keyboard-and-mouse layout it is in the left half and the right half is the online lobby button); the gear below (online game creation settings), the divider bar and the online lobby button are moved away
    s = _sub1(s, '"rect": { "x": -1010, "y": 1865 },', '"rect": { "x": -303, "y": 1865 },')
    s = _sub1(s, '"textString": "@PlayTab",', '"textString": "@PlayButton",')
    s, n = offscreen(s, types=(), names=('OnlinePlaySettingsButton', 'Divider', 'SecondaryContextButton'))
    if n != 3: raise SystemExit('touch_layout：键鼠版主菜单要挪走的控件应为 3 个，找到 %d 个' % n)
    i = s.rstrip().rfind(']'); assert s.rstrip().endswith('}') and i > 0
    return s[:i].rstrip(' ') + _UPDATE + '    ' + s[i:], n + 2
def _pc_charselect(s):
    # 右侧存档列表：整块往里收 SAFE_X；「线上 / 离线」页签挪走（只有离线）；列表三行字用回原主机档字号（统一 40 的大字装不下三行）
    # Save list on the right: the whole block is pulled inward by SAFE_X; the 「线上 / 离线」(Online / Offline) tabs are moved away (offline only); the three text lines of the list go back to the original console profile font sizes (the uniform large size of 40 does not fit three lines)
    s = _sub1(s, '"rect": { "x": -801, "y": 0 },', '"rect": { "x": %d, "y": 0 },' % (-801 - SAFE_X))
    s, n = offscreen(s, types=(), names=('Tabs', 'ConvertCharacter'))
    if n != 2: raise SystemExit('touch_layout：键鼠版选角列表的页签 / 转换按钮没找到')
    # 「转换角色」不留，底图上它的槽和两槽之间的十字花一起去掉、只留居中一个槽（底图构建时由 gen_fe_rightpanel.py 从原图改出，gen_romfs_overlay.py 换进去）；垃圾桶移到居中槽上（+116 单位）
    # 「转换角色」(Convert Character) is not kept; its slot on the background image and the cross ornament between the two slots are removed together, leaving only one centered slot (the background image is produced at build time from the original image by gen_fe_rightpanel.py and swapped in by gen_romfs_overlay.py); the trash can moves onto the centered slot (+116 units)
    s = _sub1(s, '"rect": { "x": 315, "y": 1940 },', '"rect": { "x": 431, "y": 1940 },')
    # 再点一下已经高亮的行：列表会发 itemSelectionMessage（CharacterSelect:SelectCharacter）= 手柄流程里的「确认选择」，列表被收起、重开后顺序与高亮都变，
    # 之后「进入游戏」进的是另一个角色（真实触点 3/3）。选谁只看高亮（点未选中的行只发 UpdateCharacterInfo 就够了），确认由「进入游戏」按钮做——
    # 这条消息换成关一个不存在的面板（空操作）
    # Tapping an already highlighted row again: the list sends itemSelectionMessage (CharacterSelect:SelectCharacter) = 'confirm selection' in the controller flow; the list is collapsed, and after reopening both order and highlight change,
    # so 「进入游戏」(Play) then enters a different character (real touch points 3/3). Who is selected depends only on the highlight (tapping an unselected row only needs to send UpdateCharacterInfo), and confirmation is done by the 「进入游戏」(Play) button --
    # this message is replaced with closing a panel that does not exist (no-op)
    s = _sub1(s, '"itemSelectionMessage": "CharacterSelect:SelectCharacter",', '"itemSelectionMessage": "PanelManager:ClosePanel:TouchNoop",')
    # 「创建新角色」：键鼠版发的 ToCharacterCreate 不带参数，手柄模式下列表收起后不进职业选择页（Mac 宿主实测）；带上参数 1（同手柄版「新角色」键发的）
    # 「创建新角色」(Create New Character): the ToCharacterCreate sent by the keyboard-and-mouse layout carries no argument, and in controller mode after the list collapses it does not enter the class selection page (measured on the Mac host); argument 1 is added (same as what the controller layout's 「新角色」(New Character) key sends)
    s = _sub1(s, '"onClickMessage": "FrontEndNavigation:ToCharacterCreate",', '"onClickMessage": "FrontEndNavigation:ToCharacterCreate:1",')
    for name, var, size in (('Title', '$XMediumFontSize', 30), ('Name', '$XMediumLargeFontSize', 34), ('Level&Class', '$XMediumFontSize', 30)):
        i = s.index('"type": "TextBoxWidget", "name": "%s"' % name); m = re.compile(r'"pointSize":\s*"(\$[A-Za-z]+)"').search(s, i)
        if not m or m.group(1) != var: raise SystemExit('touch_layout：选角列表 %s 的字号不是原样（原版布局变了？）' % name)
        s = s[:m.start()] + '"pointSize": %d' % size + s[m.end():]; n += 1
    return s, n + 1
def _pc_ribbon(s):
    # 左侧栏整块不要（「选项」「退出」两个按钮、按钮区和背景图全删，标志也一起删，首页不留设置入口）；公告栏本来就是空的，一并挪走
    # The whole left sidebar block is dropped (the two buttons 「选项」(Options) and 「退出」(Quit), the button area and the background image are all deleted, the logo is deleted too, and the home page keeps no settings entry); the announcement bar was empty anyway and is moved away as well
    s, n = offscreen(s, types=(), names=('Ribbon', 'MotDSection'))
    if n != 2: raise SystemExit('touch_layout：键鼠版左侧栏应挪走 2 块，找到 %d 块' % n)
    return s, n
PCFRONT = {
    'controller/console/mainmenupanelhd.json': ('mainmenupanelhd.json', _pc_mainmenu),
    'controller/console/characterselectpanelhd.json': ('characterselectpanelhd.json', _pc_charselect),
    'controller/console/mainmenubuttonribbonhd.json': ('mainmenubuttonribbonhd.json', _pc_ribbon),
    'controller/console/mainmenulinkshd.json': ('mainmenulinkshd.json', None),
}
PCFRONT_PLAIN = ('mainmenubuttonribbonhd.json', 'mainmenulinkshd.json')
def build_pcfront(romfs, outdir):
    """键鼠版底层布局阉割后另放新目录 1touchbase_lay/<名>（原路径保持原版，手柄模式的原版页继承的是它）；手柄版（controller/console/<名>，生成器会搬进触屏目录）换成原包「键鼠界面 + 手柄焦点」那层（controller/<名>，继承键鼠版底层）——
    这样手柄键仍能在选角页上走（X 进选角页；冒烟与触屏脚本的进局键序用「X 进选角页 → LoadCharacter」）；那层里的键位提示控件照例挪走。
    注意：换成这套界面后，主菜单上按 A 没有落点——不进局、不弹难度框，只有按键事件；原版手柄布局（Mac 宿主验收跑的）下按 A 是进局。写 iOS 包的进局键序别用「主菜单按 A」。
    After being stripped down, the keyboard-and-mouse base layout is placed in a new directory 1touchbase_lay/<name> (the original path stays original; the original pages of controller mode inherit from it); the controller version (controller/console/<name>, which the generator moves into the touch directory) is replaced with the original package's "keyboard-and-mouse UI + controller focus" layer (controller/<name>, inheriting the keyboard-and-mouse base layer) --
        this way controller buttons still work on the character select page (X enters the character select page; the run-entry key sequence of the smoke and touch scripts uses "X to enter the character select page → LoadCharacter"); the button hint widgets in that layer are moved away as usual.
        Note: after switching to this UI, pressing A on the main menu lands on nothing -- no run entry, no difficulty dialog, only a key event; under the original controller layout (which the Mac host acceptance runs) pressing A enters a run. Do not use "press A on the main menu" when writing the run-entry key sequence for the iOS app."""
    repl = {}
    for rel, (pc, fn) in PCFRONT.items():
        cpath, _ = read_layout(romfs, rel); ppath, text = read_layout(romfs, pc)
        if fn: text, n = fn(text)
        # 键鼠版底层那份原版手柄版页也继承（Controller/<名> basedOn <名>）：改过的不放回原路径（手柄模式要加载到原版字节），另放新目录 1touchbase_lay/<名>，只让触屏目录里的这一页继承
        # The keyboard-and-mouse base layer copy is also inherited by the original controller layout page (Controller/<name> basedOn <name>): the modified one is not put back at the original path (controller mode must load original bytes) but placed in a new directory 1touchbase_lay/<name>, inherited only by this page in the touch directory
        base = os.path.dirname(ppath).lower() + '/1touchbase_lay/' + pc   # 新增路径只能是「已有目录 / 新子目录 / 文件」（casc_overlay.py） | A new path can only be "existing directory / new subdirectory / file" (casc_overlay.py)
        if pc in PCFRONT_PLAIN: mid = text   # 左侧栏 / 链接栏：不用手柄焦点那层（那层把缺省焦点放在左侧栏的「选项」上，栏挪走后焦点仍会落过去、确认键开出选项页）；整份放进触屏目录，不需要底层 | Left sidebar / link bar: the controller focus layer is not used (that layer puts the default focus on 「选项」(Options) in the left sidebar; after the bar is moved away the focus still lands there and the confirm key opens the options page); the whole file goes into the touch directory and needs no base layer
        else:
            o = os.path.join(outdir, 'pcfront_base_' + pc); open(o, 'w', encoding='utf-8').write(text); repl[base] = o
            _, mid = read_layout(romfs, 'controller/' + pc); mid, _n = offscreen(mid)
            mid = _sub1(mid, '"basedOn": "%s"' % os.path.basename(ppath), '"basedOn": "1touchbase_lay/%s"' % pc)
        if pc == 'mainmenupanelhd.json': i = mid.rstrip().rfind(']'); mid = mid[:i] + _UPDATE_STUB + mid[i:]   # 继承层要按名字逐个点到子控件才会建：新加的七个也列上 | The inheriting layer only builds child widgets that are named one by one: the seven newly added ones are listed too
        o = os.path.join(outdir, 'pcfront_' + rel.replace('/', '_')); open(o, 'w', encoding='utf-8').write(mid); repl[cpath.lower()] = o
    return repl

# 局内面板（第 ③ 步）：手柄版布局把键鼠版的可点按钮删了（关闭、修理、翻页…只剩手柄键能触发），触摸下要插回来。
# 做法：从同一个包的键鼠版同名布局里按控件名取出原定义（外观、位置、点击消息都是原文），加到手柄版布局根 children 的末尾。
# 带外框的五页（任务 / 角色 / 物品栏 / 技能树 / 选项）的关闭走外框右上角那一个，这里不插。
# 每项：手柄版布局 → (键鼠版布局, [(控件名, 新名 或 None, 覆盖的字段文本 或 None)…])
# In-game panels (step ③): the controller layout deleted the tappable buttons of the keyboard-and-mouse layout (close, repair, paging... only triggerable by controller keys), and under touch they must be inserted back.
# Approach: from the same-named keyboard-and-mouse layout in the same package, take the original definition by widget name (look, position and tap message are all the original text) and append it to the end of the controller layout's root children.
# Closing of the five pages with the outer frame (quests / character / inventory / skill tree / options) goes through the single one at the top-right corner of the outer frame and is not inserted here.
# Each entry: controller layout → (keyboard-and-mouse layout, [(widget name, new name or None, overriding field text or None)...])
INGAME = {
    'controller/horadriccubelayouthd.json': ('horadriccubelayouthd.json', [('close', None, '"rect": { "x": 2741, "y": -156 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0')]),
    # 两颗修理按钮：键鼠版的位置（169,1277 / 877,1277）是照键鼠版底图的凹槽定的；手柄版面板用另一张底图（Controller/Panel/Vendor/V2/VendorForge_BG，摆在 -2,100），
    # 凹槽在别处——原样搬过来按钮就歪在凹槽左下角外沿。按手柄版底图量：两个凹槽在底图里是 x 72–212 / 440–580、y 542–632 像素（1 像素 = 2 单位），
    # 中心换到面板坐标 = (282,1274) / (1018,1274)；按钮图 58 像素 = 116 单位见方 → 左上角 (224,1216) / (960,1216)
    # 交易页只留一颗修理：「全部修理」挪到左格；单件修理去掉（物品上本来就能单件修），空出的右格放「刷新货架」（同赌博页刷新钮的位置与原图）。
    # 刷新钮借单件修理那颗控件 button_repair：面板打开函数 0x409b90 按名字只在交易 / 修理页显示它，正好是刷新钮该出现的时机；图 / 音 / 说明换成赌博页刷新钮的原版值，
    # 点击发空操作消息 PanelManager:ClosePanel:TouchShopRefresh，宿主 dm_shoprefresh.c 照赌博刷新的写法再发一次「打开交易」并置这个商人的待刷新标志（原版货单机制重刷）
    # The two repair buttons: the keyboard-and-mouse positions (169,1277 / 877,1277) are set to the notches of the keyboard-and-mouse background image; the controller-layout panel uses a different background image (Controller/Panel/Vendor/V2/VendorForge_BG, placed at -2,100),
    # whose notches are elsewhere -- moved over as-is the buttons sit skewed outside the lower-left edge of the notches. Measured on the controller-layout background image: the two notches are at x 72–212 / 440–580, y 542–632 pixels in the image (1 pixel = 2 units),
    # centers converted to panel coordinates = (282,1274) / (1018,1274); the button image is 58 pixels = 116 units square → top-left corners (224,1216) / (960,1216)
    # The trade page keeps only one repair button: 「全部修理」(Repair All) moves to the left cell; single-item repair is removed (items can already be repaired individually on the item itself), and the freed right cell holds 「刷新货架」(Refresh Stock) (same position and original image as the gamble page refresh button).
    # The refresh button borrows the single-item repair widget button_repair: the panel open function 0x409b90 shows it by name only on the trade / repair page, which is exactly when the refresh button should appear; image / sound / description are replaced with the original-game values of the gamble page refresh button,
    # and the tap sends the no-op message PanelManager:ClosePanel:TouchShopRefresh; the host dm_shoprefresh.c, written the same way as the gamble refresh, sends 'open trade' once more and sets this vendor's pending-refresh flag (re-rolled by the original game's stock list mechanism)
    'controller/vendorpanellayouthd.json': ('vendorpanellayouthd.json', [('button_repair', None, '"rect": { "x": 960, "y": 1216 }, "filename": "PANEL\\\\Vendors\\\\Gambling_Refresh_Button", "sound": "cursor_gamble_refresh_hd", "tooltipString": "@refresh", "onClickMessage": "PanelManager:ClosePanel:TouchShopRefresh"'),
        # 不修理的商人（基德、阿卡拉…）不显示 button_repair：同一颗刷新钮再挂一份，名字用 button_blank——面板打开函数 0x409b90 按名字找它，
        # 在「不修理」的交易页与赌博页显示（面板模式 +0x168：0 交易不修理 / 1 交易会修理 / 2 赌博；button_repair 只在 1、button_refresh 只在 2、button_blank 在 ≠ 1）。
        # 赌博页上它与原版 button_refresh 叠在同一处、同一张图；排在 button_refresh 前面（在下层），点击落到原版那颗；万一落到它，宿主按赌博页发原版的赌博刷新
        # Vendors that do not repair (Gheed, Akara...) do not show button_repair: one more copy of the same refresh button is attached, named button_blank -- the panel open function 0x409b90 finds it by name
        # and shows it on 'non-repair' trade pages and on the gamble page (panel mode +0x168: 0 trade without repair / 1 trade with repair / 2 gamble; button_repair only in 1, button_refresh only in 2, button_blank in ≠ 1).
        # On the gamble page it overlaps the original game's button_refresh at the same spot with the same image; it is ordered before button_refresh (on the lower layer) so taps land on the original one; if a tap does land on it, the host sends the original game's gamble refresh based on being on the gamble page
        ('button_refresh', 'button_blank', '"rect": { "x": 960, "y": 1216 }, "onClickMessage": "PanelManager:ClosePanel:TouchShopRefresh"'),
        ('button_repair_all', None, '"rect": { "x": 224, "y": 1216 }'),
        # 赌博页的「刷新货架」：原版手柄靠一个手柄动作键（option3，面板消息处理 0x409338 → 0x409614）刷新，触摸下没有入口；键鼠版这颗按钮发的 VendorPanelMessage:RefreshAll
        # 走的是同一段刷新（0x408e78）。搬回来，位置同「全部修理」（键鼠版里两者就是同一个位置，按页互斥显示：面板打开函数 0x409b90 只在赌博页显示它、只在修理页显示两颗修理）
        # 「刷新货架」(Refresh Stock) on the gamble page: the original game's controller refreshes through a controller action key (option3, panel message handler 0x409338 → 0x409614), and there is no entry under touch; the VendorPanelMessage:RefreshAll sent by this button in the keyboard-and-mouse layout
        # goes through the same refresh code (0x408e78). It is brought back, at the same position as 「全部修理」(Repair All) (in the keyboard-and-mouse layout the two are at the very same position, shown mutually exclusively per page: the panel open function 0x409b90 shows it only on the gamble page and shows the two repair buttons only on the repair page)
        ('button_refresh', None, '"rect": { "x": 960, "y": 1216 }'), ('close', None, '"rect": { "x": 2741, "y": -156 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0')]),
    'controller/bankexpansionlayouthd.json': ('bankexpansionlayouthd.json', [('close', None, '"rect": { "x": 2741, "y": -156 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0', 'bankoriginallayouthd.json'),
        # 共享页翻页：手柄版里 StashLeftArrow / StashRightArrow 这两个名字已被键位提示占用，换名放在提示原位（StashNavigation 390,1146 内的 25,13 与 411,13）
        # Shared page paging: in the controller layout the names StashLeftArrow / StashRightArrow are already taken by button prompts, so they are renamed and placed at the prompts' original positions (25,13 and 411,13 inside StashNavigation 390,1146)
        ('StashLeftArrow', 'TouchStashLeft', '"rect": { "x": 415, "y": 1159, "width": 64, "height": 64 }'),
        ('StashRightArrow', 'TouchStashRight', '"rect": { "x": 801, "y": 1159, "width": 64, "height": 64 }')]),
    'controller/characterstatspanelhd.json': ('characterstatspanelhd.json', [('AdvancedStats', None, '"rect": { "x": 2644, "y": 1000 }'), ('CloseButton', 'TouchClose', '"rect": { "x": 2747, "y": -157 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:UISwitcher"')]),
    # 高级属性页：手柄版没有回来的入口（键鼠版的关闭钮没带过来）。插回原版关闭钮，原样原大（用原版自己的回法，不加「返回」）
    # Advanced stats page: the controller layout has no entry for coming back (the keyboard-and-mouse close button was not carried over). Insert the original game's close button back, same look and same size (use the original game's own way back, without adding 「返回」(Back))
    'controller/advancedstatspanelhd.json': ('advancedstatspanelhd.json', [('CloseButton', None, '"rect": { "x": 2565, "y": 20 }')]),
    'controller/console/switch/playerinventoryexpansionlayouthd.json': ('playerinventoryoriginallayouthd.json', [('close', 'TouchClose', '"rect": { "x": 2079, "y": -130 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:UISwitcher"')]),
    # 技能树「综合技能」页「物品技能」标题栏右端：原版物品栏武器位上方的 I / II 页签，原图原字（手柄版物品栏那四个控件，两帧底图 + 两个字），用来切到副手组技能。
    # 位置按页内坐标：Tab3 在 (19,5)，标题栏 (0,640) 宽 1766 高 100 ⇒ 右端 x 1785、竖直居中；底图约 215×55（物品栏里字在底图内 +12 / +117）。显示 / 换帧 / 点击都由宿主管（dm_cursor.c swaptab_*）
    # Right end of the 「物品技能」(Item Skills) title bar on the skill tree 「综合技能」(General Skills) page: the I / II tabs above the weapon slots of the original game's inventory, original image and text (the four widgets of the controller-layout inventory, a two-frame background image + two characters), used to switch to the secondary weapon set's skills.
    # Position in page coordinates: Tab3 is at (19,5), title bar (0,640) width 1766 height 100 ⇒ right end x 1785, vertically centered; background image about 215×55 (in the inventory the characters are at +12 / +117 within the background image). Show / frame change / taps are all managed by the host (dm_cursor.c swaptab_*)
    'controller/skillstreepanelhd.json': ('skillstreepanelhd.json', [('Cancel', 'TouchClose', '"rect": { "x": 2271, "y": -158 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:UISwitcher"'),
        ('background_right_arm', 'TouchSwapTab0', '"rect": { "x": 1540, "y": 668 }', 'controller/playerinventoryexpansionlayouthd.json'),
        ('background_right_arm_selected', 'TouchSwapTab1', '"rect": { "x": 1540, "y": 668 }', 'controller/playerinventoryexpansionlayouthd.json'),
        ('text_i_left', 'TouchSwapI', '"rect": { "x": 1552, "y": 665, "width": 84, "height": 48 }', 'controller/playerinventoryexpansionlayouthd.json'),
        ('text_ii_left', 'TouchSwapII', '"rect": { "x": 1657, "y": 665, "width": 84, "height": 48 }', 'controller/playerinventoryexpansionlayouthd.json')]),
    'controller/questlogpanelexpansionhd.json': ('questlogpaneloriginalhd.json', [('Cancel', 'TouchClose', '"rect": { "x": 2265, "y": -160 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:UISwitcher"')]),
    'controller/pauselayoutgardenhd.json': ('playerinventoryoriginallayouthd.json', [('close', 'TouchClose', '"rect": { "x": 2271, "y": -158 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:UISwitcher"')]),
    # 不带花园的暂停页（88 级法师这类带佣兵页签的角色打开的是 PauseLayout，不是 PauseLayoutGarden）：原先没挂 X，右上角是空框；
    # 设置页点「应用」后回到这一页、游戏一直停着，只能先切别的页签再点 X（mnc 局）。同一个 X、同一落点、同一条消息
    # Pause page without the garden (characters with a mercenary tab such as the level 88 sorceress open PauseLayout, not PauseLayoutGarden): previously no X was attached and the top-right corner was an empty frame;
    # after tapping 「应用」(Apply) on the settings page it returns to this page with the game still paused, and the only way was to switch to another tab first and then tap X (run mnc). Same X, same spot, same message
    'controller/pauselayouthd.json': ('playerinventoryoriginallayouthd.json', [('close', 'TouchClose', '"rect": { "x": 2271, "y": -158 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:UISwitcher"')]),
    # 雇佣列表 / 灌魔页 / 小站页：手柄版没有关闭入口（灌魔页进去出不来）。插回键鼠版自己的关闭钮（各发各的关闭消息），图与位置同别的面板右上角那个 X
    # Hire list / imbue page / waypoint page: the controller layout has no close entry (once in the imbue page there is no way out). Insert the keyboard-and-mouse layout's own close buttons back (each sends its own close message), with the same image and position as the X at the top-right corner of the other panels
    'controller/hiremenupanelhd.json': ('hiremenupanelhd.json', [('CloseButton', 'TouchClose', '"rect": { "x": 2490, "y": -203 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0')]),
    # 灌魔 / 打孔 / 个性化：面板与背包并排，外框跨两个半面板——X 与商人 / 仓库 / 魔盒同一落点（外框右上角凹槽）。原先的 1210,12 在左半面板的角花上，凹槽空着。
    # 凹槽那块被后开的背包面板与外框盖在上层，游戏的鼠标分发到不了这颗钮：点它由宿主按「点面板外 = 关面板」处理（dm_cursor.c ui_classify）
    # Imbue / socket / personalize: the panel sits side by side with the inventory and the outer frame spans two half panels -- the X lands at the same spot as for the vendor / stash / cube (the notch at the top-right corner of the outer frame). The previous 1210,12 was on the corner ornament of the left half panel, leaving the notch empty.
    # The notch area is covered on upper layers by the later-opened inventory panel and the outer frame, so the game's mouse dispatch cannot reach this button: tapping it is handled by the host as 'tap outside the panel = close the panel' (dm_cursor.c ui_classify)
    'controller/imbueitemspanelhd.json': ('imbueitemspanelhd.json', [('Cancel', 'TouchClose', '"rect": { "x": 2741, "y": -156 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:ImbueItemsPanel"')]),
    # 佣兵页（功能页页签条第 5 个，带活佣兵的角色才有）：外框右上角没有关闭钮。同角色页：插回键鼠版的关闭钮、放到外框右上角、发关整个功能页的消息
    # （用它自己的关闭消息只关掉里面两块内容，外框留成空壳——本机实测）
    # Mercenary page (5th item of the function page tab strip, only for characters with a living mercenary): the top-right corner of the outer frame has no close button. Same as the character page: insert the keyboard-and-mouse close button back, place it at the top-right corner of the outer frame, and send the message that closes the whole function page
    # (using its own close message closes only the two content blocks inside and leaves the outer frame as an empty shell -- measured locally)
    'controller/hirelinginventorypanelhd.json': ('hirelinginventorypanelhd.json', [('CloseButton', 'TouchClose', '"rect": { "x": 2747, "y": -157 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:UISwitcher"')]),
    'controller/waypointspanelexpansionhd.json': ('waypointspaneloriginalhd.json', [('CloseButton', 'TouchClose', '"rect": { "x": 2267, "y": -153 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0')]),
    # 小站：关闭按钮画在外框凹槽处点不到——小站面板矩形之外的点击直接落到游戏世界（角色走开、面板随之关掉），放在面板内又只能压在页签条上。暂不放，待解决。
    # 选项页的应用 / 默认 / 放弃：键鼠版原位（y 1800）在手柄版里压住列表最后一行、「放弃」被滑块盖住；挪到外框底部那条空带上（提示条挪走后空着）
    # Waypoint: a close button drawn at the outer frame notch cannot be tapped -- taps outside the waypoint panel rectangle fall straight through to the game world (the character walks away and the panel closes with it), and inside the panel it could only sit on the tab strip. Not placed for now, pending a solution.
    # Apply / Default / Discard on the options page: the keyboard-and-mouse position (y 1800) covers the last row of the list in the controller layout, and 「放弃」(Discard) is covered by the slider; moved onto the empty band at the bottom of the outer frame (empty after the button legend was moved away)
    'controller/console/settingspanelhd.json': ('settingspanelhd.json', [('ActionButtonsTable', None, '"rect": { "x": -750, "y": 1938 }'),
        # 位置按画面中线算（锚 0.5，x 1594 = 原来按 iPhone 画面宽写死的 3941 − 半宽 2347）：功能页外框是居中的，写死距左边多少在 16:9 画面（iPad）上会出屏。
        # 设置页盖住了底下功能页右上角的 X（别的四个页签页都有）。补一个同图同位的；它自己不发关闭消息——这页退出只认返回键，
        # 宿主认到点的是它就替玩家按返回（dm_cursor.c，与「应用 / 放弃」同一处），有未应用改动时照原版弹确认框
        # Position is computed from the picture's center line (anchor 0.5, x 1594 = the previous 3941 hard-coded for the iPhone picture width − half width 2347): the function page outer frame is centered, and a hard-coded distance from the left edge would go off screen on a 16:9 picture (iPad).
        # The settings page covers the X at the top-right corner of the function page underneath (the other four tab pages all have it). One with the same image and position is added; it does not send a close message itself -- this page only exits through the back key,
        # and when the host sees that the tap landed on it, it presses back on the player's behalf (dm_cursor.c, same place as 「应用 / 放弃」(Apply / Discard)), popping the confirmation box as the original game does when there are unapplied changes
        ('close', 'TouchClose', '"anchor": { "x": 0.5 }, "rect": { "x": 1594, "y": 58, "scale": 1.2 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "hoveredFrame": 0, "onClickMessage": "PanelManager:ClosePanel:TouchNoop"', 'playerinventoryoriginallayouthd.json')]),
}

# 局内呈现（第 ③ 步）：不加按钮、只改摆放的那部分。每项：手柄版布局 → 变换函数(文本) → (新文本, 改了几处)
# In-game presentation (step ③): the part that adds no buttons and only changes placement. Each entry: controller layout → transform function(text) → (new text, number of places changed)
def _char_no_stamina(s):
    """人物页去「耐力」一行（精力阉割），「生命」行上移到原耐力行的位置（与左边「活力」同高）。 | The character page drops the 「耐力」(Stamina) row (stamina is stripped out), and the 「生命」(Life) row moves up to the position of the former stamina row (same height as 「活力」(Vitality) on the left)."""
    s, n = offscreen(s, types=(), names=('Row06', 'Divider06', 'Divider07', 'StaminaTitle', 'MaxStamina', 'CurrentStamina'))
    if n != 6: raise SystemExit('touch_layout：人物页耐力行控件应为 6 个，找到 %d 个' % n)
    for name, y in (('Row07', 1075), ('Divider08', 1092), ('Divider09', 1092), ('HPTitle', 1089), ('MaxHP', 1093), ('CurrentHP', 1093)):
        i = s.index('"name": "%s"' % name); m = re.compile(r'"y":\s*(\d+)').search(s, i)
        if int(m.group(1)) != y: raise SystemExit('touch_layout：人物页 %s 的 y 不是 %d（原版布局变了？）' % (name, y))
        s = s[:m.start(1)] + str(y - 106) + s[m.end(1):]; n += 1
    return s, n
# 外框（带页签的五页与箱子 / 魔盒 / 小站等共用的 ControllerOverlay）：底部那排手柄键位提示挪走；右上角凹槽放关闭按钮（石面透出 + 贴满凹槽的旧金色 X，
# 图 = NativeHUD/touch/close.png，写在借用路径 PANEL/HUD_02/front_panel）。消息 ClosePanel:UISwitcher = 页签条、当前页、外框一起关，与按 B 的消息序列一致（物品栏实测）；
# 关闭按钮不放在外框里：外框里的按钮在箱子 / 魔盒 / 小站页会抢走点击却只发 UISwitcher（面板不关），放进页签条面板又被「选项」页签吃掉点击。
# 改为每个面板各放一个、都落在外框右上角凹槽同一处（屏幕像素包围盒 (2413,41)–(2438,78)）、各发各的关闭消息，见 INGAME。
# 角色页「<技能名> / 伤害」两行标题：两行的行高合计比格高 84 多出一截，下半行被下一格的底图压住。
# 换行是代码拼的（技能名 + 换行 + 「伤害」），改不成单行；字号（pointSize）与行距（spacing.leading）改了画面都不变（实测）。
# 文字在框里垂直居中：把这两个标题框上移 12 单位（实测：16 时上一行的顶被格子上沿压住，9 时下一行的底仍贴边），让溢出的那截落到格内上方的空白里。左右手各一处。
# Outer frame (the ControllerOverlay shared by the five tabbed pages and the chest / cube / waypoint etc.): the row of controller button prompts at the bottom is moved away; a close button is placed in the notch at the top-right corner (stone surface showing through + an old-gold X filling the notch,
# image = NativeHUD/touch/close.png, written at the borrowed path PANEL/HUD_02/front_panel). The message ClosePanel:UISwitcher = tab strip, current page and outer frame closed together, matching the message sequence of pressing B (measured on the inventory);
# the close button is not placed inside the outer frame: a button in the outer frame steals taps on the chest / cube / waypoint pages yet only sends UISwitcher (the panel does not close), and placed in the tab strip panel its taps are swallowed by the 「选项」(Options) tab.
# Instead each panel gets its own, all landing at the same spot in the top-right notch of the outer frame (screen pixel bounding box (2413,41)-(2438,78)) and each sending its own close message, see INGAME.
# Character page two-line titles 「<技能名> / 伤害」(<skill name> / Damage): the combined line height of the two lines exceeds the cell height of 84 by a margin, and the lower half-line is covered by the next cell's background image.
# The line break is assembled in code (skill name + line break + 「伤害」(Damage)) and cannot be changed to a single line; changing the font size (pointSize) and line spacing (spacing.leading) leaves the picture unchanged (measured).
# The text is vertically centered in its box: these two title boxes are moved up 12 units (measured: at 16 the top of the upper line is covered by the cell's top edge, at 9 the bottom of the lower line still touches the edge), so the overflow falls into the blank space at the top inside the cell. One place each for left and right hand.
def _char_title_up(s):
    a = re.compile(r'("type": "TextBoxWidget", "name": "AttackDamageTitle",\s*"fields": \{\s*"rect": \{ "x": 0, "y": )0(, "width": \d+, "height": 84 \})')
    s, n = a.subn(lambda m: m.group(1) + '-12' + m.group(2), s)
    if n != 2: raise SystemExit('touch_layout：角色页「伤害」标题不是原样（找到 %d 处，应 2 处）' % n)
    return s, n
def _overlay_touch(s):
    s, n = offscreen(s)
    if n != 1: raise SystemExit('touch_layout：外框底部提示条应为 1 个，找到 %d 个' % n)
    return s, n
# 物品栏：换武器的手柄键位提示挪走；外框底部空出来的那一条写本页的触摸手势（物品手势）
# Inventory: the controller button prompt for weapon swap is moved away; the strip freed up at the bottom of the outer frame carries this page's touch gestures (item gestures)
def _inv_touch(hint):   # hint 已不用：底部手势提示字去掉（手势不言自明，界面更简洁） | hint is no longer used: the gesture hint text at the bottom is removed (the gestures are self-explanatory and the UI is cleaner)
    def fn(s):
        s, n = offscreen(s)
        return s, n
    return fn
# 符文之语助手清单：物品栏面板下的一组原版控件，全摆在屏幕外、字是空的；
# 长按匹配底材时宿主（dm/host/dm_runeword.c）写字、量宽、摆位置，「一键合成」按钮只在符文齐且此刻可取用的那几行挪回来。
# 每个符文之语两行：名字（金）+ 需要等级（灰）一行；符文串一行，每枚符文一个文字框（「#13 夏」），缺的那枚换成红色那一份。
# 原版文字框不认行内颜色码（真机：U+E07E 显示成「*」）、也不换行不裁剪，所以一种颜色一个控件；符文框开 fitToText，宿主按游戏量出的宽度（+0x78）逐个往右排，保证不出底板、不压按钮。
# 全部包在一个容器 TouchRwBox 里：直接挂物品栏下会让面板直接子控件过 256 个，安全阀的面板巡检（dm_cursor.c ui_panel_heal，子控件数 > 256 判不可见）
# 就把开着的物品栏判成「屏上没面板」补发 ClosePanel（真机 y2–y6 四局）。容器不设 rect，子控件坐标仍按物品栏面板算。
# 底板 = 半透明深色矩形（同原版说明框的观感）；按钮 = 难度选择框那颗原版按钮图（FrontEnd_ButtonMed）。按钮发 PanelManager:ClosePanel:TouchRwGo<行号>（空操作，宿主当信号收）
# Runeword helper list: a group of original-game widgets under the inventory panel, all placed off screen with empty text;
# on a long press that matches a base item the host (dm/host/dm_runeword.c) writes the text, measures widths and places them, and the 「一键合成」(Craft) button is moved back only for the rows whose runes are complete and available right now.
# Two lines per runeword: name (gold) + required level (gray) on one line; the rune string on one line, one text box per rune (「#13 夏」(#13 Shael)), with a missing rune replaced by its red copy.
# The original game's text box does not honor inline color codes (real device: U+E07E shows as '*'), and neither wraps nor clips, so there is one widget per color; rune boxes enable fitToText, and the host lays them out to the right one by one using the width measured by the game (+0x78), making sure they do not leave the backing or cover the button.
# Everything is wrapped in one container TouchRwBox: hanging them directly under the inventory would push the panel's direct child widgets past 256, and the safety valve's panel patrol (dm_cursor.c ui_panel_heal, more than 256 child widgets is judged invisible)
# would then judge the open inventory as 'no panel on screen' and send an extra ClosePanel (four real-device runs y2-y6). The container has no rect, and child widget coordinates are still relative to the inventory panel.
# Backing = semi-transparent dark rectangle (same look as the original game's tooltip); button = the original-game button image from the difficulty selection box (FrontEnd_ButtonMed). The button sends PanelManager:ClosePanel:TouchRwGo<row number> (a no-op that the host receives as a signal)
_RW_N = 15
def _rw_text(name, w, size, color, h='left', fit=False, ht=56):
    return ('        { "type": "TextBoxWidget", "name": "%s", "fields": { "rect": { "x": 99999, "y": 99999, "width": %d, "height": %d }, "text": "", %s'
            '"style": { "pointSize": %d, "fontColor": { "r": %d, "g": %d, "b": %d, "a": 255 }, "alignment": { "h": "%s", "v": "center" } } } },\n') % ((name, w, ht, '"fitToText": true, ' if fit else '', size) + color + (h,))
_RW = ('        { "type": "Widget", "name": "TouchRwBox", "children": [\n'
       + '        { "type": "RectangleWidget", "name": "TouchRwBg", "fields": { "rect": { "x": 99999, "y": 99999, "width": 1125, "height": 400 }, "color": [ 0.055, 0.055, 0.055, 0.92 ] } },\n'
       + _rw_text('TouchRwTitle', 1125, 38, (235, 235, 235), 'center')
       # 翻页一排（一页放不下时宿主才挪回来）：照搬键鼠版仓库页 StashNavigation 的两颗箭头与页码字（同图、同帧号、同字样式），点击改发空操作消息给宿主
       # Paging row (the host moves it back only when one page is not enough): copied from the two arrows and page number text of StashNavigation on the keyboard-and-mouse stash page (same images, same frame numbers, same text style), with taps changed to send a no-op message to the host
       + ''.join('        { "type": "ButtonWidget", "name": "TouchRwPg%s", "fields": { "rect": { "x": 99999, "y": 99999, "width": 64, "height": 64 }, "filename": "PANEL\\\\Stash\\\\Stash_%sArrow", '
                 '"normalFrame": 0, "pressedFrame": 2, "hoveredFrame": 1, "disabledFrame": 3, "onClickMessage": "PanelManager:ClosePanel:TouchRwPg%s" } },\n' % (k, a, k) for k, a in (('Prev', 'Left'), ('Next', 'Right')))
       # 条目之间的分隔线（每个符文之语之间隔一行）：包里没有虚线素材，用原版暂停菜单列表的分隔线图 PauseMenu/Divider_Medium，调暗不抢字
       # Divider lines between entries (one line between each runeword): the package has no dashed-line asset, so the original game's pause menu list divider image PauseMenu/Divider_Medium is used, dimmed so it does not compete with the text
       + ''.join('        { "type": "ImageWidget", "name": "TouchRwSep%d", "fields": { "rect": { "x": 99999, "y": 99999, "scale": 1.25 }, "filename": "PauseMenu\\\\Divider_Medium", "transparency": 0.35 } },\n' % k for k in range(7))
       + '        { "type": "TextBoxWidget", "name": "TouchRwPage", "fields": { "text": "", "rect": { "x": 99999, "y": 99999, "width": 230, "height": 64 }, '
         '"style": { "fontColor": "$PanelTitleColor", "pointSize": "$MediumFontSize", "alignment": { "v": "center", "h": "center" }, "spacing": "$MinimumSpacing", "dropShadow": "$DefaultDropShadow" } } },\n'
       # 每行的控件（15 行 × 名字 / 等级 / 12 枚符文 / 灰字 / 按钮 = 240 个）另包一层容器 TouchRwRows：全挂在 TouchRwBox 下会过 256 个，
       # 交互层与触点脚本按名字找控件的遍历（子控件数 > 256 当不可见）就找不到它们（加了 TouchRwHint 后 257 个，tap_ui TouchRwGo0 全部找不到）
       # The widgets of each row (15 rows × name / level / 12 runes / gray text / button = 240) are wrapped in another container TouchRwRows: hanging them all under TouchRwBox would exceed 256,
       # and the traversal by which the interaction layer and touch point scripts find widgets by name (more than 256 child widgets counts as invisible) would then not find them (257 after adding TouchRwHint, and tap_ui TouchRwGo0 could find none of them)
       + '        { "type": "Widget", "name": "TouchRwRows", "children": [\n'
       + ''.join(_rw_text('TouchRwName%d' % i, 520, 36, (199, 179, 119), fit=True, ht=46) + _rw_text('TouchRwLv%d' % i, 300, 28, (160, 160, 160), fit=True, ht=40)
                 + ''.join(_rw_text('TouchRwR%d_%d%s' % (i, j, c), 300, 30, col, fit=True, ht=40) for j in range(6) for c, col in (('w', (235, 235, 235)), ('r', (255, 90, 90))))
                 # 底材在共享仓库（原版不能直接镶）时按钮那一格换成一行灰字：原版说明框的灰 $FontColorGray
                 # When the base item is in the shared stash (the original game cannot socket it directly) the button cell is replaced with a line of gray text: the gray of the original game's tooltip, $FontColorGray
                 + ('        { "type": "TextBoxWidget", "name": "TouchRwHint%d", "fields": { "rect": { "x": 99999, "y": 99999, "width": 400, "height": 40 }, "text": "@dmRwHint", "fitToText": true, '
                    '"style": { "pointSize": 28, "fontColor": "$FontColorGray", "alignment": { "h": "left", "v": "center" } } } },\n') % i
                 + ('        { "type": "ButtonWidget", "name": "TouchRwGo%d", "fields": { "rect": { "x": 99999, "y": 99999, "width": 408, "scale": 0.75 }, "filename": "FrontEnd/HD/Final/FrontEnd_ButtonMed", "hoveredFrame": 3, '
                    '"textString": "@dmRwGo", "textColor": "$LightButtonTextColor", "text/style": "$StyleFEButtonText", "onClickMessage": "PanelManager:ClosePanel:TouchRwGo%d" } },\n') % (i, i)
                 for i in range(_RW_N))
       + '        ] },\n'
       # 手柄模式的选中框：原版手柄选中框（同编年史仓库格子的金框），框住十字键选中的那一行；触屏模式一直在屏外
       # Selection frame in controller mode: the original controller selection frame (same as the gold frame on Chronicle vault cells), framing the row selected with
       # the D-pad; always off-screen in touch mode
       + '        { "type": "NineTileImageWidget", "name": "TouchRwSel", "fields": { "rect": { "x": 99999, "y": 99999, "width": 1000, "height": 100 }, "backgroundColor": [ 0, 0, 0, 0 ], '
         '"filename": "Controller/PANEL/ControllerSelector/ControllerSelectorBorder", "tileScale": 0.5 } },\n'
       # 符文之语详情（点暗金色的名字显示该符文之语的属性）：放在最后（画在最上层）。属性文字是游戏自己生成的原版说明（宿主 rw_desc，同编年史），
       # 文字框样式照抄原版物品说明框（_profilehd.json TooltipStyle：白字、提示框字号、居中、行序倒排、处理 | 字符）；页式：清单底板里换成详情，左上角原版仓库左箭头 +「返回」
       # 字号：原版提示框字号（手柄版 42）一屏放不下长的（全表 99 条最长 24 行）；两份文字框，32 号放得下就用它，放不下换 22 号（宿主按量出的高挑）
       # Runeword details (tapping the unique-gold name shows that runeword's properties): placed last (drawn on the top layer). The property text is the original game's description generated by the game itself (host rw_desc, same as the chronicle),
       # and the text box style is copied from the original game's item tooltip (_profilehd.json TooltipStyle: white text, tooltip font size, centered, reversed line order, handling of the | character); page style: the details replace the list inside the list backing, with the original game's stash left arrow + 「返回」(Back) at the top left
       # Font size: the original game's tooltip font size (42 in the controller layout) cannot fit the long ones on one screen (longest of all 99 entries in the table is 24 lines); two text boxes, use size 32 if it fits and switch to size 22 if not (the host picks by the measured height)
       + ''.join('        { "type": "TextBoxWidget", "name": "TouchRwDesc%s", "fields": { "rect": { "x": 99999, "y": 99999, "width": 1000, "height": 600 }, "text": "", "fitToText": true, '
                 '"style": { "fontColor": "$FontColorWhite", "pointSize": %d, "alignment": { "h": "center", "v": "top" }, "options": { "pipeCharacterBehavior": "handle", "newlineHandling": "reversed" } } } },\n' % (k, pt)
                 for k, pt in (('', 32), ('S', 22)))
       + '        { "type": "ButtonWidget", "name": "TouchRwBack", "fields": { "rect": { "x": 99999, "y": 99999, "width": 64, "height": 64 }, "filename": "PANEL\\\\Stash\\\\Stash_LeftArrow", '
         '"normalFrame": 0, "pressedFrame": 2, "hoveredFrame": 1, "disabledFrame": 3, "onClickMessage": "PanelManager:ClosePanel:TouchRwBack" } },\n'
       + _rw_text('TouchRwBackTxt', 300, 34, (199, 179, 119), fit=True, ht=64).replace('"text": "",', '"text": "@Return",')
       + ('        { "type": "ButtonWidget", "name": "TouchRwDetGo", "fields": { "rect": { "x": 99999, "y": 99999, "width": 408, "scale": 0.75 }, "filename": "FrontEnd/HD/Final/FrontEnd_ButtonMed", "hoveredFrame": 3, '
          '"textString": "@dmRwGo", "textColor": "$LightButtonTextColor", "text/style": "$StyleFEButtonText", "onClickMessage": "PanelManager:ClosePanel:TouchRwDetGo" } },\n')
       + '        ] },\n')
def _rw_children_check():
    """清单控件各层容器的直接子控件数都要 ≤ 256：交互层 / 触点脚本 / 安全阀遍历面板树时，子控件数 > 256 的容器当不可见（踩到过两次）。 | The number of direct child widgets of each container level of the list widget must be ≤ 256: when the interaction layer / touch point script / safety valve traverse the panel tree, a container with more than 256 child widgets is treated as invisible (hit twice)."""
    import json
    t = '{"c":[' + _RW.replace('        ] },\n', ']},').rstrip().rstrip(',') + ']}'
    def walk(w, path):
        ch = w.get('children', [])
        if len(ch) > 256: raise SystemExit('touch_layout：符文之语清单容器 %s 直接子控件 %d 个，超过 256' % (path, len(ch)))
        for c in ch: walk(c, path + '/' + c.get('name', '?'))
    walk(json.loads(re.sub(r',(\s*[}\]])', r'\1', t)), '')
def _inv_rw(s):
    _rw_children_check()
    s, n = offscreen(s)
    i = s.rstrip().rfind(']'); assert s.rstrip().endswith('}') and i > 0
    return s[:i].rstrip(' ') + _RW + '    ' + s[i:], n + 1
def pad_inv_rw(s):   # 手柄版物品栏（原版布局，不挪任何东西）+ 符文之语清单控件：手柄模式按住对比键打开同一份清单（gen_romfs_overlay.py 手柄版布局） | Controller inventory (original layout, nothing moved) + runeword list widgets: in controller mode holding the compare button opens the same list (gen_romfs_overlay.py controller layouts)
    _rw_children_check()
    i = s.rstrip().rfind(']'); assert s.rstrip().endswith('}') and i > 0
    return s[:i].rstrip(' ') + _RW + '    ' + s[i:]
# 技能绑定页（技能树里给技能键配技能的框，两排各六格）：格下原来是手柄键图标（已整体透明），换成屏幕上技能键的名字。
# 格 1–6 = A B Y X R ZR；触摸技能键 = 攻击(A) F1(B) F2(X) F3(Y) F4(R)（dm_touch_controls.c skill[]），第二套 F5–F8。第 3、4 格对调位置，屏上顺序就是 F1 F2 F3 F4。
# 第 6 格（ZR）触摸下没有对应的技能键，不标字。
# Skill binding page (the box in the skill tree for assigning skills to skill buttons, two rows of six cells each): under the cells there used to be controller key icons (already made transparent as a whole), replaced with the names of the on-screen skill buttons.
# Cells 1–6 = A B Y X R ZR; touch skill buttons = Attack(A) F1(B) F2(X) F3(Y) F4(R) (dm_touch_controls.c skill[]), second set F5-F8. Cells 3 and 4 swap positions so the on-screen order is F1 F2 F3 F4.
# Cell 6 (ZR) has no corresponding skill button under touch and gets no label.
def _bind_touch(s):
    n = 0
    for pre, names in (('SkillButton', ('@skillname0', 'F1', 'F3', 'F2', 'F4')), ('AltSkillButton', ('@skillname0', 'F5', 'F7', 'F6', 'F8'))):   # @skillname0 = 原版「攻击」 | @skillname0 = the original game's "Attack"
        for i, label in enumerate(names, 1):
            pn = '%s%dPrompt' % (pre, i) if pre == 'SkillButton' else ('Bottom', 'Right', 'Left', 'Top', 'RB')[i - 1] + 'ButtonPrompt'   # 第二排的提示控件按键位方位起名 | The prompt widgets of the second row are named by button position
            m = re.search(r'\{\s*"type":\s*"SkillButtonPromptWidget",\s*"name":\s*"%s"' % pn, s)
            if not m: raise SystemExit('touch_layout：技能绑定页没有 %s（原版布局变了？）' % pn)
            s = s[:m.start()] + ('{\n                                    "type": "TextBoxWidget", "name": "TouchLabel",\n                                    "fields": {\n'
                '                                        "anchor": { "x": 0.5, "y": 1.0 }, "rect": { "x": -70, "y": 2, "width": 140, "height": 44 }, "text": "%s",\n'
                '                                        "style": { "fontColor": "$FontColorGoldYellow", "pointSize": "$SmallFontSize", "alignment": { "h": "center", "v": "center" } },\n'
                '                                    }\n                                },\n                                ') % label + s[m.start():]; n += 1
        for a_, b_ in ((3, 965), (4, 830)):   # 第 3 格（Y = F3）与第 4 格（X = F2）对调 | Cell 3 (Y = F3) and cell 4 (X = F2) are swapped
            m = re.search(r'("name":\s*"%s%d",\s*"fields":\s*\{\s*"rect":\s*\{ "x": )(\d+)' % (pre, a_), s)
            if not m or int(m.group(2)) != (830 if a_ == 3 else 965): raise SystemExit('touch_layout：技能绑定页 %s%d 的位置不是原样' % (pre, a_))
            s = s[:m.start(2)] + str(b_) + s[m.end(2):]; n += 1
    return s, n
# 帮助页整页切掉（帮助页不留）：去掉的是入口——暂停菜单里的「帮助」按钮（两份布局）与 HUD 的帮助按钮面板；剩下的按钮按原间距 150 重新排匀、整组仍以 y 653 为中心，不留空洞。
# The help page is cut out entirely (the help page is not kept): what is removed is the entries -- the 「帮助」(Help) button in the pause menu (two layouts) and the HUD's help button panel; the remaining buttons are re-spaced evenly at the original spacing of 150 and the whole group is still centered on y 653, leaving no hole.
def _pause_no_help(ys, hide=('Help',), restart_y=None, extra=()):
    # 不换原版按钮的图和字（换成前端按钮图 + 放大白字的做法观感差，已撤）；「消息记录」入口不要。
    # 手指尺寸不靠改外观：条目少了以后把间距拉开（中心距 240 单位 ≈ 9.4 毫米），命中外扩交给输入层的就近吸附
    # 「分享诊断日志」：借原版「消息记录」按钮（同一张按钮图、同一种字），换字、
    # 点击改发 PanelManager:ClosePanel:TouchShareLog（关一个不存在的面板 = 空操作，宿主当信号收，dm/host/diag.c → 外壳 dm/ios/Sources/dm_sharelog.m 弹系统分享面板）
    # Do not replace the image and text of the original game's buttons (the approach of swapping in the front-end button image + enlarged white text looked bad and was withdrawn); the 「消息记录」(Message Log) entry is dropped.
    # Finger size is not handled by changing the look: with fewer entries the spacing is opened up (center spacing 240 units ≈ 9.4 mm), and hit expansion is left to the input layer's nearest-target snapping
    # 「分享诊断日志」(Share Diagnostics): borrows the original game's 「消息记录」(Message Log) button (same button image, same font), changes the text,
    # and the tap is changed to send PanelManager:ClosePanel:TouchShareLog (closing a panel that does not exist = no-op, which the host receives as a signal; dm/host/diag.c → the shell dm/ios/Sources/dm_sharelog.m pops up the system share sheet)
    def fn(s):
        s = _sub1(s, '"textString": "@minipanelmessage",', '"textString": "@dmPauseShareLog",')
        s = _sub1(s, '"onClickMessage": "PanelManager:OpenPanel:MessageLogPanel",', '"onClickMessage": "PanelManager:ClosePanel:TouchShareLog",')
        s, n = offscreen(s, types=(), names=hide)
        if n != len(hide): raise SystemExit('touch_layout：暂停菜单要挪走的按钮应为 %d 个，找到 %d 个' % (len(hide), n))
        for name, (y0, y1) in ys.items():
            m = re.search(r'"type":\s*"ButtonWidget",\s*"name":\s*"%s",\s*"fields":\s*\{\s*("rect":\s*\{ "x": 564, "y": (\d+)\s*\})' % name, s)
            if not m or int(m.group(2)) != y0: raise SystemExit('touch_layout：暂停菜单按钮 %s 的位置不是原样（原版布局变了？）' % name)
            s = s[:m.start(1)] + '"rect": { "x": 564, "y": %d }' % y1 + s[m.end(1):]; n += 1
        # 「一键重开」：复制原版「保存并退出」按钮（同图同字体），换字、紧挨着放在它下面，
        # 点击改发 PanelManager:ClosePanel:TouchRestart（空操作，宿主当信号收：dm_cursor.c 一键重开，替玩家走原版「保存并退出 → 选同一角色同一难度进局」）
        # 「一键重开」(Quick Restart): copy the original game's 「保存并退出」(Save and Exit) button (same image, same font), change the text, and place it right below that button,
        # with the tap changed to send PanelManager:ClosePanel:TouchRestart (a no-op that the host receives as a signal: quick restart in dm_cursor.c walks the original game's 'Save and Exit → pick the same character and same difficulty to enter a run' on the player's behalf)
        if restart_y is not None:
            b = _widget_block(s, 'Exit'); e = s.index(b) + len(b)
            r = _sub1(_sub1(_sub1(b, '"name": "Exit"', '"name": "TouchRestart"'), '"textString": "@SaveAndExit"', '"textString": "@dmPauseRestart"'), '"onClickMessage": "PausePanelMessage:ExitGame"', '"onClickMessage": "PanelManager:ClosePanel:TouchRestart"')
            r = re.sub(r'"rect":\s*\{ "x": 564, "y": \d+ \}', '"rect": { "x": 564, "y": %d }' % restart_y, r, count=1)
            s = s[:e] + ',\n        ' + r + s[e:]; n += 1
        # 手柄版另加的行（pad_pause）：同样复制「保存并退出」按钮，换名、换字、换消息（空操作，宿主当信号收）
        # Rows added for the controller version (pad_pause): likewise copy the "Save and Exit" button and change its name, text and message (a no-op, which the
        # host receives as a signal)
        for name, text, y in extra:
            b = _widget_block(s, 'Exit'); e = s.index(b) + len(b)
            r = _sub1(_sub1(_sub1(b, '"name": "Exit"', '"name": "%s"' % name), '"textString": "@SaveAndExit"', '"textString": "%s"' % text), '"onClickMessage": "PausePanelMessage:ExitGame"', '"onClickMessage": "PanelManager:ClosePanel:%s"' % name)
            r = re.sub(r'"rect":\s*\{ "x": 564, "y": \d+ \}', '"rect": { "x": 564, "y": %d }' % y, r, count=1)
            s = s[:e] + ',\n        ' + r + s[e:]; n += 1
        return s, n
    return fn
# 手柄版暂停菜单（「难度、百宝囊存放挂暂停菜单」）：原版布局 + 我们的功能行，放原版路径（gen_romfs_overlay.py 手柄版布局）；触屏插回的关闭钮不带。
# 行 = 游戏难度 / 保存并退出 / 一键重开 / 选项 / 存放 / 编年史（经典版没有）/ 分享诊断日志；帮助、战利品过滤器、消息记录照的裁定不留。
#   「选项」固定在第四行（y 653）：原版手柄版开暂停菜单时自由光标在屏幕正中、压着哪行哪行高亮，直接按确认就点它——正中那行只许是按了没后果的。
#   「游戏难度：N」= 按一下人数档 +1（8 之后回 1），当前档位写在这一行上；「存放」= 物品栏「存放」钮同一件事（手柄焦点到不了那颗钮），字用原版串 @strGoldDeposit。
# 按钮图高 120 单位：带编年史那版七行在原版首末两行之间（278–1028）等距 125；经典版原底图只放得下四行，换用带编年史那版的底图（同尺寸），同一套行距排前六行（少编年史那行，后一行上移）
# Gamepad pause menu ("difficulty and trove deposit hang on the pause menu"): the original layout + our feature rows, placed at the original path (gamepad layout in gen_romfs_overlay.py); the close button inserted for touch is not included.
# Rows = Game Difficulty / Save and Exit / Quick Restart / Options / Deposit / Chronicle (absent in the classic version) / Share Diagnostics; Help, Loot Filter and Message Log are not kept, as ruled.
#   "Options" is fixed on the fourth row (y 653): when the original gamepad UI opens the pause menu the free cursor is at the centre of the screen and highlights whichever row it rests on, and pressing confirm straight away activates it -- the centre row may only be one whose press has no consequence.
#   "Game Difficulty: N" = one press raises the player-count setting by 1 (back to 1 after 8), with the current setting shown on this row; "Deposit" = the same thing as the "Deposit" button of the inventory (the gamepad focus cannot reach that button), using the original string @strGoldDeposit.
# Button image height is 120 units: in the version with the Chronicle, seven rows are spaced evenly by 125 between the original first and last rows (278–1028); the classic version's own background only fits four rows, so the background of the Chronicle version (same size) is used and the first six rows are laid out with the same spacing (without the Chronicle row, the following row moves up)
def pad_pause(s, garden, share=True):   # share=False（Mac App）：没有系统分享面板，「分享诊断日志」行不放 | share=False (Mac App): there is no system share sheet, so the "Share Diagnostics" row is left out
    ex = (('TouchPadDiff', '', 278), ('TouchPadStore', '@strGoldDeposit', 778)); y = 278
    sh = lambda y0, y1: {'MessageLog': (y0, y1)} if share else {}; hs = () if share else ('MessageLog',)
    if garden: s = _pause_no_help({'Settings': (428, 653), 'Exit': (728, 403), 'Chronicle': (1028, 903), **sh(278, 1028)}, hide=('Help', 'LootFilter') + hs, restart_y=528, extra=ex)(s)[0]
    else:
        s = _sub1(s, '"filename": "Controller/Panel/EscapeMenu/EscapeOptionsBGClassic"', '"filename": "Controller/Panel/EscapeMenu/EscapeOptionsBG"')
        s = _pause_no_help({'Settings': (578, 653), 'Exit': (878, 403), **sh(428, 903)}, hide=('Help',) + hs, restart_y=528, extra=ex)(s)[0]
    # 「游戏难度：N」的字：菜单开着时游戏停拍、原版系统通知要关了菜单才出，所以当前档位直接写在这一行上——按钮自己不带字，上面叠一个同字体的文字控件，宿主写（dm_padmode.c）
    # Text of "Game Difficulty: N": while the menu is open the game stops ticking and the original system notification only appears once the menu is closed,
    # so the current setting is written directly on this row. The button itself carries no text; a text widget with the same font is stacked on top and
    # written by the host (dm_padmode.c)
    i = s.rstrip().rfind(']'); assert s.rstrip().endswith('}') and i > 0
    return (s[:i].rstrip(' ') + '        { "type": "TextBoxWidget", "name": "TouchPadDiffTxt", "fields": { "rect": { "x": 564, "y": %d, "width": 664, "height": 120 }, "text": "@OptionsMenuGameDifficulty", '
            '"style": { "fontColor": "$LightButtonTextColor", "pointSize": "$MediumLargeFontSize", "alignment": { "h": "center", "v": "center" } } } },\n    ' % y + s[i:])
# 编年史保留。只有键鼠版布局，套在手柄外框里；列表行 / 分类筛选 / 查看奖励 / 下拉都认点击。页内的关闭按钮被外框盖住：在外框右上角凹槽放关闭按钮，
# 发原 CloseButton 的消息，关掉后回到暂停菜单；单放一个加宽的容器，2868×1320 下金色包围盒 (2413,40)–(2438,77)。
# 页签不在这里处理：点页签由触摸层翻成 ZL / ZR（一件事只留一个执行者；先前盖在页签上的透明按钮与它并存、一次点击切两回，已撤）
# Chronicle is kept. It only has a keyboard-and-mouse layout, wrapped in the controller outer frame; list rows / category filters / view rewards / dropdowns all respond to taps. The in-page close button is covered by the outer frame: a close button is placed in the notch at the top-right corner of the outer frame,
# sending the original CloseButton's message and returning to the pause menu after closing; a widened container is placed on its own, with the gold bounding box (2413,40)-(2438,77) at 2868×1320.
# Tabs are not handled here: tapping a tab is translated by the touch layer into ZL / ZR (only one executor per job; the transparent buttons previously laid over the tabs coexisted with it and one tap switched twice, so they were withdrawn)
def _chronicle_touch(s):
    if '"onClickMessage": "PanelManager:ClosePanel:ChroniclePanel"' not in s: raise SystemExit('touch_layout：编年史关闭消息不是原样（原版布局变了？）')
    add = (',\n        {\n            "type": "Widget", "name": "TouchCloseBox",\n            "fields": { "rect": { "x": -998, "y": 256, "width": 2600, "height": 121, "scale": "$ConsoleUIScale" }, "anchor": { "x": 0.5 } },\n            "children": [\n'
           '                { "type": "ButtonWidget", "name": "TouchClose", "fields": { "rect": { "x": 2236, "y": -171 }, "filename": "PANEL\\\\HUD_02\\\\front_panel", "sound": "cursor_close_window_hd", "onClickMessage": "PanelManager:ClosePanel:ChroniclePanel" } }\n'
           '            ]\n        }\n    ')
    end = s.rstrip().rstrip('}').rstrip(); assert end.endswith(']'); at = len(end) - 1
    return s[:at].rstrip() + add + s[at:], 1
# 编年史第四个页签「首领」= 刷怪积分牌。页签条加第四项，消息发 ToggleChildPanel:TouchKillBoard——
# 游戏照常关掉当前列表页、去开一个不存在的面板（宿主实测：中间留空，不报错）；那块空地由这里加的控件 TouchKillBoard 占：
# 照原版列表页（ChronicleBaseListPanelHD.json）的摆法与美术——普通 / 噩梦 / 地狱三个勾选、右上下拉（排序）、左列五幕、左下总数框、右列每个首领一行（任务日志图标 + 两行字）。
# 名单与字由宿主 dm_kills.c 写（键 = dm_kills.c 的 k_rows），显示 / 隐藏、切幕、排序、滚动高度也由它管；点击一律发无害的 ClosePanel:TouchKB_*（同自定义页）
# Chronicle fourth tab 「首领」(Bosses) = the kill board. A fourth item is added to the tab strip, and its message sends ToggleChildPanel:TouchKillBoard --
# the game closes the current list page as usual and goes to open a panel that does not exist (measured on the host: the middle is left empty, no error); that empty area is occupied by the widget TouchKillBoard added here:
# following the placement and art of the original game's list page (ChronicleBaseListPanelHD.json) -- three checkboxes Normal / Nightmare / Hell, top-right dropdown (sort), five acts in the left column, total count box at the bottom left, one row per boss in the right column (quest log icon + two lines of text).
# The roster and text are written by the host dm_kills.c (keys = k_rows in dm_kills.c), which also manages show / hide, act switching, sorting and scroll height; all taps send a harmless ClosePanel:TouchKB_* (same as the custom page)
KB_ROW_H = 188   # 原版行底图的高（底图不拉伸）。三行字：名字 + 击杀 / 首杀 + 暗金套装 / 最快 · 最近 · 平均；宿主 dm_kills.c KB_ROW_H 同值 | Height of the original game's row background image (the background image is not stretched). Three lines of text: name + kills / first kill + unique and set / fastest, latest, average; same value as KB_ROW_H in the host dm_kills.c
# 每行的原版图标（hd/global/ui/QuestIcons，第 0 帧 = 金色）：按原版任务认，没有对应任务的用 Done（任务日志里的暗色骷髅章）
# Each row's original-game icon (hd/global/ui/QuestIcons, frame 0 = gold): matched by the original game's quest; rows with no corresponding quest use Done (the dark skull stamp in the quest log)
KB_ICON = {'andariel': 'a1q6', 'countess': 'a1q4', 'bloodraven': 'a1q2', 'cowking': 'Misc\\CowKing', 'corpsefire': 'a1q1', 'smith': 'a1q5', 'griswold': 'a1q3', 'rakanishu': 'a1q3',
           'duriel': 'a2q6', 'summoner': 'a2q5', 'radament': 'a2q1', 'fangskin': 'a2q3',
           'mephisto': 'a3q6', 'council': 'a3q5', 'bremm': 'a3q6', 'wyand': 'a3q6', 'maffer': 'a3q6',
           'diablo': 'a4q2', 'vizier': 'a4q2', 'deseis': 'a4q2', 'infector': 'a4q2', 'izual': 'a4q1', 'hephasto': 'a4q3',
           'baal': 'a5q6', 'nihlathak': 'a5q4', 'shenk': 'a5q1', 'frozenstein': 'a5q3', 'ancients': 'a5q5',
           'colenzo': 'a5q6', 'achmel': 'a5q6', 'bartuc': 'a5q6', 'ventar': 'a5q6', 'lister': 'a5q6'}
def _kb_keys():   # 名单的键取自 dm_kills.c（一处定义） | The keys of the roster are taken from dm_kills.c (defined in one place)
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'host', 'dm_kills.c'), encoding='utf-8').read()
    ks = re.findall(r'\{"([a-z]+)", (?:"[^"]*"|NULL), ([1-5]), K_', src)
    if len(ks) < 60 or len({k for k, _ in ks}) != len(ks): raise SystemExit('touch_layout：从 dm_kills.c 读到的积分牌名单不对（%d 行）' % len(ks))
    return [(k, int(a)) for k, a in ks]
def _kb_text(name, x, y, w, h, color, size, align='left', text=''):
    return {'type': 'TextBoxWidget', 'name': name, 'fields': {'rect': {'x': x, 'y': y, 'width': w, 'height': h}, 'text': text,
            'style': {'fontColor': color, 'pointSize': size, 'alignment': {'h': align, 'v': 'center'}}}}
def _kb_check(name, x, label):
    return [{'type': 'ToggleButtonWidget', 'name': name, 'fields': {'rect': {'x': x, 'y': 0, 'width': 65, 'height': 65}, 'sound': 'cursor_menu_select_hd',
             'filename': 'Lobby\\CreateGame\\CreateGame_AdvancedCheckbox', 'untoggledFrame': 0, 'untoggledPressedFrame': 1, 'untoggledHoveredFrame': 3, 'untoggledDisabledFrame': 2,
             'toggledFrame': 4, 'toggledPressedFrame': 5, 'toggledHoveredFrame': 6, 'onClickMessage': 'PanelManager:ClosePanel:' + name}},
            {'type': 'TextBoxWidget', 'name': name + '_Text', 'fields': {'text': label, 'rect': {'x': x + 80, 'y': -5, 'height': 50}, 'style': '$StyleSettingsTitle', 'fitToText': True}}]
_KB_SPACER = {'type': 'Widget', 'name': 'spacer', 'fields': {'rect': {'width': 20, 'height': 65}}}
def _kb_row(key):
    ic = KB_ICON.get(key, 'Done')
    return {'type': 'Widget', 'name': 'TouchKB_' + key, 'fields': {'rect': {'x': 0, 'y': 0, 'width': 1157, 'height': KB_ROW_H}}, 'children': [
        {'type': 'ImageWidget', 'name': 'Bg', 'fields': {'filename': 'PANEL\\Chronicle\\Slot_list_item_XL'}},
        {'type': 'ImageWidget', 'name': 'Icon', 'fields': {'rect': {'x': 22, 'y': 24, 'scale': 0.83}, 'filename': 'QuestIcons\\' + ic}},
        _kb_text('Name', 180, 8, 640, 60, '$FontColorWhite', '$LootFilterMediumLargeFontSize'),
        _kb_text('Kills', 760, 8, 360, 60, '$FontColorGoldYellow', '$LootFilterMediumLargeFontSize', 'right'),
        _kb_text('First', 180, 76, 560, 48, '$FontColorMoreLightGray', '$LootFilterSmallFontSize'),
        _kb_text('Drops', 740, 76, 380, 48, '$FontColorMoreLightGray', '$LootFilterSmallFontSize', 'right'),
        _kb_text('Times', 180, 126, 940, 48, '$FontColorMoreLightGray', '$LootFilterSmallFontSize')]}
def _kb_board():
    acts = []
    for i in range(6):   # 第一至五幕 + 秘境（秘境成绩挂在积分牌第六项） | Acts one to five + rift (rift results hang on the sixth item of the kill board)
        y = 80 + 100 * i
        acts += [{'type': 'ImageWidget', 'name': 'TouchKB_ActArrow%d' % i, 'fields': {'rect': {'x': 0, 'y': y}, 'filename': 'PANEL\\Chronicle\\Btn_collapse'}},
                 {'type': 'ToggleButtonWidget', 'name': 'TouchKB_Act%d' % i, 'fields': {'rect': {'x': 100, 'y': y, 'width': 360, 'height': 100}, 'sound': 'cursor_menu_select_hd',
                  'filename': 'PANEL\\Chronicle\\Slot_list_small_golddeco', 'untoggledFrame': 0, 'toggledFrame': 1, 'onClickMessage': 'PanelManager:ClosePanel:TouchKB_Act%d' % i},
                  'children': [_kb_text('Text', 35, 0, 315, 100, '$FontColorWhite', '$MediumFontSize', text=('@dmKbAct%d' % (i + 1)) if i < 5 else '@dmKbRift')]}]
    # 外层 TouchKillBoard 用原版列表页同一个矩形变量（各档位的位置 / 缩放跟原版一致）；里层 TouchKB_Body 缺省挪出屏幕（游戏不认 visible: false），
    # 宿主切到这一页时挪回 (0, 0)：面板刚打开那一帧宿主还够不着它，放在屏外就不会闪一帧
    # The outer TouchKillBoard uses the same rectangle variable as the original game's list pages (position / scaling in each profile match the original game); the inner TouchKB_Body is moved off screen by default (the game does not honor visible: false),
    # and the host moves it back to (0, 0) when switching to this page: on the frame the panel has just opened the host cannot reach it yet, and keeping it off screen avoids a one-frame flash
    return {'type': 'Widget', 'name': 'TouchKillBoard', 'fields': {'rect': '$OptionsPanelRect', 'anchor': {'x': 0.5}}, 'children': [
        {'type': 'Widget', 'name': 'TouchKB_Body', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1730, 'height': 1284}}, 'children':
        [{'type': 'StackWidget', 'name': 'TouchKB_DiffStack', 'fields': {'rect': {'x': 0, 'y': 0, 'height': 65, 'width': 1300}, 'direction': 'LeftRight', 'padding': 15},   # 照原版 QualityStack | Follows the original game's QualityStack
          'children': _kb_check('TouchKB_D0', 0, '@strCreateGameNormalText') + [_KB_SPACER] + _kb_check('TouchKB_D1', 406, '@strCreateGameNightmareText') + [_KB_SPACER] + _kb_check('TouchKB_D2', 950, '@strCreateGameHellText')}] + [
        {'type': 'DropdownListWidget', 'name': 'TouchKB_Sort', 'fields': {'rect': {'x': -390, 'y': 0, 'width': 610, 'height': 105, 'scale': 0.65},
            'background': {'rect': {'width': 610, 'height': 105}, 'filename': 'PANEL/DropdownButton_Large_Center', 'leftCap/filename': 'PANEL/DropdownButton_Large_Left', 'rightCap/filename': 'PANEL/DropdownButton_Large_Right'},
            # 选了哪项宿主按显示的字认（dm_kills.c kb_sort）。onUpdateMessage 带的是选项号：发 ClosePanel 会让游戏把选项号当面板名、取空崩（宿主实测），这里发一条没人收的
            # The host recognizes which item was selected by the displayed text (dm_kills.c kb_sort). onUpdateMessage carries the option index: sending ClosePanel would make the game treat the option index as a panel name and crash on a null fetch (measured on the host), so here a message nobody receives is sent
            'pressedFrame': 1, 'disabledFrame': 2, 'hoveredFrame': 3, 'anchor': {'x': 1.0}, 'states': ['@dmKbSortAct', '@dmKbSortCount', '@dmKbSortFast'],
            'onUpdateMessage': 'TouchKillBoardMessage:Sort', 'text/rect': {'scale': 1.538}, 'text/anchor': {'x': 0, 'y': -0.3}, 'textMargin': {'left': 20, 'right': 290},
            'text/style': '$StyleFilterRuleListButtonTextSmall', 'text/truncateWithEllipsis': True, 'textColor': '$FontColorLightGold'}},
        {'type': 'ImageWidget', 'name': 'TouchKB_TypeBar', 'fields': {'rect': {'x': 466, 'y': 80}, 'filename': 'Panel\\Chronicle\\Chronicle_ScrollBar_Small'}}] + acts + [
        {'type': 'ImageWidget', 'name': 'TouchKB_TotalBox', 'fields': {'rect': {'x': 0, 'y': 1000, 'width': 450, 'height': 110}, 'filename': 'PANEL\\Chronicle\\Footer_BG_Progress'}, 'children': [
            {'type': 'TextBoxWidget', 'name': 'TouchKB_Total', 'fields': {'text': '', 'rect': {'y': 15, 'width': 450, 'height': 40}, 'style': {'pointSize': '$MediumFontSize', 'alignment': {'h': 'center', 'v': 'center'}}}},
            {'type': 'ImageWidget', 'name': 'FillBg', 'fields': {'rect': {'x': 25, 'y': 65, 'width': 450, 'height': 80}, 'filename': 'PANEL\\Chronicle\\ProgressBar_Small_2'}}]},
        ] + [_kb_page(i) for i in range(5)] + [_kb_rift_page()]}]}
# 右列每幕一页（滚动条 + 滚动区 + 本幕各行）：滚动位置存在游戏的滚动控件里、宿主改不了，切幕时只换页（不在屏上的页挪到屏外），各幕的滚动位置各自留着
# Right column, one page per act (scrollbar + scroll area + that act's rows): the scroll position is stored in the game's scroll widget and the host cannot change it, so switching acts only swaps pages (pages not on screen are moved off screen), and each act keeps its own scroll position
def _kb_page(i):
    return {'type': 'Widget', 'name': 'TouchKB_Page%d' % i, 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1730, 'height': 1284}}, 'children': [
        {'type': 'ImageWidget', 'name': 'TouchKB_ListBar', 'fields': {'rect': {'x': -20, 'y': 80}, 'anchor': {'x': 1.0}, 'filename': 'Panel\\Chronicle\\Chronicle_Scrollbar_Large'}, 'children': [
            {'type': 'ScrollControllerWidget', 'name': 'TouchKB_ScrollController%d' % i, 'fields': {'rect': {'x': -31, 'y': 29, 'width': 26, 'height': 1095}, 'anchor': {'x': 1.0},
             'upArrowFilepath': 'FrontEnd\\HD\\Final\\FrontEnd_ScrollUpBtn', 'downArrowFilepath': 'FrontEnd\\HD\\Final\\FrontEnd_ScrollDownBtn', 'barFilepath': 'PauseMenu\\VerticalIndicator',
             'viewName': 'TouchKB_Scroll%d' % i, 'wheelScrollSound': 'cursor_scroll_hd', 'buttonScrollSound': 'cursor_scroll_hd'}}]},
        {'type': 'Widget', 'name': 'TouchKB_ListBox', 'fields': {'rect': {'x': 530, 'y': 80, 'width': 1157, 'height': 1200}}, 'children': [
            {'type': 'ScrollViewWidget', 'name': 'TouchKB_Scroll%d' % i, 'fields': {'fitToParent': True, 'scrollControllerName': 'TouchKB_ScrollController%d' % i}, 'children': [
                {'type': 'Widget', 'name': 'TouchKB_List', 'fields': {'rect': {'x': 0, 'y': 0, 'width': 1157, 'height': KB_ROW_H * sum(1 for _, a in _kb_keys() if a == i + 1)}},
                 'children': [_kb_row(k) for k, a in _kb_keys() if a == i + 1]}]}]}]}
# 编年史第五页签「秘境」（选层页要像编年史仓库 / 积分牌一样用原版控件和图元）。宿主 dm_rift_ui.c 切页、写字、收点按（ClosePanel:TouchRF_*）。
# 左列：选层（原版大厅大箭头 ±1、前端小按钮 ±10、前端中按钮「开始」，都带按下态）+ 本层说明 + 安亚赌博率；进行中时换成「继续 / 放弃」（宿主挪进挪出）。
# 右列：10 档首通奖励，一行一项（积分牌同款行底图），物品用原版物品图。图标路径与 dm_rift.c 里程碑物品对应
# 尺寸口径：图元像素 × 2 = 布局单位（行底图 Slot_list_item_XL 578×92 像素 = 原版列表行 1157×188）。行内几何取原版编年史列表行 ChronicleItemListItem：
#   物品格 = 行底图左侧那块暗格，物品图容器 (20, 20, 140, 140)，文字从 x 180 起；列表区 (530, 80, 1157, 1200)，左栏 0–460（原版编年史列表页 / 积分牌同值）。
# 间距一套：8 / 16 / 24 / 40（组内行距 64、组间 40、标题到内容 15 = 原版标题条 65 → 内容 80）。
# 第一行那排小标题（选择层数 / 秘境进行中 / 本层已完成、首通奖励、每档另得…）上抬 24（「第一行文字整体压线了，往上抬一些」）。
# 压的不是金线：Mac 截图逐行量，标题字下沿到金线的间距和「本层」小标题一样（都是 3 像素）；压的是面板底图自己在内容区顶上那道横向接缝
# （截图 y 173–176，上面是浅色横带、下面是深色内容底），字的下半截骑在接缝上。上抬 24 单位（约 8 像素）后字整个落在接缝上方的浅色带里，
# 字下沿离接缝 4 像素；金线和下面的内容不动
# Fifth Chronicle tab "Rift" (the level selection page must use original widgets and graphics, like the Chronicle vault / kill board). The host's
# dm_rift_ui.c switches pages, writes text and receives taps (ClosePanel:TouchRF_*).
# Left column: level selection (original lobby large arrows ±1, front-end small buttons ±10, front-end medium button "Start", all with pressed states) +
# level description + Anya gamble rate; while in progress it is replaced by "Continue / Abandon" (the host moves them in and out).
# Right column: 10 tiers of first-clear rewards, one per row (same row background as the kill board), items shown with original item images. Icon paths
# correspond to the milestone items in dm_rift.c
# Size convention: graphic pixels × 2 = layout units (row background Slot_list_item_XL 578×92 pixels = original list row 1157×188). In-row geometry is
# taken from the original Chronicle list row ChronicleItemListItem:
#   item cell = the dark cell on the left of the row background, item image container (20, 20, 140, 140), text starts at x 180; list area (530, 80,
#   1157, 1200), left column 0–460 (same values as the original Chronicle list page / kill board).
# One spacing scale: 8 / 16 / 24 / 40 (row pitch within a group 64, between groups 40, title to content 15 = original title bar 65 → content 80).
# The row of small headings on the first line (select level / Rift in progress / level completed, first-clear rewards, each tier also grants...) is
# raised by 24 ("the whole first line of text sits on the line, raise it a bit").
# What it sat on was not the gold line: measured row by row on a Mac screenshot, the gap from the bottom of the heading text to the gold line is the
# same as for the "this level" small heading (3 pixels each); it sat on the horizontal seam of the panel background itself at the top of the content
# area
# (screenshot y 173 to 176, a light horizontal band above and the dark content background below), with the lower half of the text riding on the seam.
# After raising by 24 units (about 8 pixels) the text falls entirely inside the light band above the seam,
# with the bottom of the text 4 pixels from the seam; the gold line and the content below are unchanged
RF_T0 = -24
RF_EN_CAP, RF_EN_NOTE = 31, 26   # 英文版：按钮上方说明、右上那句的字号（点数） | English version: font sizes (points) of the caption above the buttons and of the top-right sentence
RF_EN_PT = 32   # 英文版左栏标签的字号（点数）：$SmallFontSize 在这块面板里和中号几乎一样大（Mac 截图，换了没变化），直接给数 | font size (points) of the left column labels in the English version: in this panel $SmallFontSize is almost the same size as medium (Mac screenshot, swapping it made no difference), so a number is given directly
RF_SLOT = (20, 20, 140)   # 原版物品格：左上 (20, 20)、边长 140 | Original game's item cell: top-left (20, 20), side length 140
RF_MS = [(10, '@dmRiftMsItem10', '@dmRiftMsSub10', 'Items/misc/charm/charm_large', (49, 147), (2, 4, 48, 146)), (20, '@dmRiftMsItem20', '', 'Items/misc/quest/mephisto_soul_stone', (49, 49), (9, 4, 39, 49)),
         (30, '@dmRiftMsItem30', '', 'Items/misc/torch/torch', (49, 98), (9, 5, 38, 95)), (40, '@dmRiftMsItem40', '@dmRiftMsSub40', 'Items/weapon/polearm/war_scythe', (98, 196), (3, 3, 93, 192)),
         (50, '@dmRiftMsItem50', '@dmRiftMsSub50', 'Items/misc/body_part/fragment_magic', (49, 49), (2, 0, 48, 48), 1.3), (60, '@dmRiftMsItem60', '', 'Items/weapon/orb/dragon_stone', (49, 147), (5, 7, 46, 136)),
         (70, '@dmRiftMsItem70', '', 'Items/armor/circlet/diadem', (98, 98), (0, 20, 95, 87)), (80, '@dmRiftMsItem80', '', 'Items/armor/armor/victors_silk', (98, 147), (2, 10, 94, 139)),
         (90, '@dmRiftMsItem90', '@dmRiftMsSub90', 'Items/misc/charm/charm_large', (49, 147), (2, 4, 48, 146)), (100, '@dmRiftMsItem100', '@dmRiftMsSub100', 'Items/misc/rune/zod_rune', (49, 49), (3, 4, 47, 48))]   # 物品图像素尺寸（从原版 .sprite 读出的帧宽 × 高）；先祖暗金珠宝用原版暗金图表 hd/items/uniques.json 给这六件配的图（body_part/fragment_*，三种造型），不是底材 cjw 的钻石图 | Item image pixel sizes (frame width × height read from the original game's .sprite); the ancestral unique jewels use the images that the original game's unique table hd/items/uniques.json assigns to these six items (body_part/fragment_*, three looks), not the diamond image of the base item cjw
def _rf_btn(name, x, y, sprite, frames, scale, text='', pt='$MediumLargeFontSize'):
    f = {'rect': {'x': x, 'y': y, 'scale': scale}, 'filename': sprite, 'onClickMessage': 'PanelManager:ClosePanel:' + name, 'sound': 'cursor_menu_select_hd'}
    f.update(frames)
    if text: f.update({'textString': text, 'pointSize': pt, 'textColor': '$LightButtonTextColor', 'text/style': '$StyleFEButtonText'})   # 照原版暂停菜单按钮的写法 | Written the same way as the original game's pause menu buttons
    return {'type': 'ButtonWidget', 'name': name, 'fields': f}
_RF_ARROW = {'hoveredFrame': 1, 'pressedFrame': 2, 'disabledFrame': 3}   # 大厅大箭头 4 帧：暗 / 亮 / 更亮 / 灰（常态第 0 帧） | Lobby large arrow, 4 frames: dark / bright / brighter / grayed (normal state is frame 0)
_RF_BTN = {'pressedFrame': 1, 'disabledFrame': 2, 'hoveredFrame': 3}     # 前端按钮 4 帧：常态 / 按下 / 灰 / 悬停 | Front-end button, 4 frames: normal / pressed / grayed / hover
def _rf_icon(icon, px, bb, up=1.0):   # up：方形小件允许放大到的倍数（先祖暗金珠宝原图偏小，放大到与别的行视觉一致、不出格）   # 物品图按「可见部分」（不透明像素外框 bb，原版图读出）缩进原版物品格、居中；细长物品按格子高度定比例（可见高占格高 95%），方形小件不放大 | up: the factor up to which small square items may be enlarged (the original image of the ancestral unique jewel is on the small side; enlarged until visually consistent with the other rows without leaving the cell) # the item image is fitted by its 'visible part' (bounding box bb of the opaque pixels, read from the original game's image) into the original game's item cell and centered; slender items are scaled by the cell height (visible height takes 95% of the cell height), small square items are not enlarged
    x0, y0, side = RF_SLOT; bw, bh = (bb[2] - bb[0]) * 2, (bb[3] - bb[1]) * 2
    k = side * 0.95 / bh if bh > bw * 1.4 and px[1] * 2 > side else min(side * 0.95 / bw, side * 0.95 / bh, up)   # 细长 = 可见高 > 宽 1.4 倍且原图比格子高 | Slender = visible height > 1.4× width and the original image is taller than the cell
    return {'type': 'ImageWidget', 'name': 'Icon', 'fields': {'rect': {'x': round(x0 + side / 2 - (bb[0] * 2 + bw / 2) * k), 'y': round(y0 + side / 2 - (bb[1] * 2 + bh / 2) * k), 'scale': round(k, 4)}, 'filename': icon}}
def _rf_row(i, m, name, sub, icon, px, bb, up=1.0):
    # 两行字：第一行「层数 + 物品名」（MediumLarge，物品名原版暗金色），第二行小一号说明；两行左对齐 x 180。状态字右对齐、整行垂直居中，右留白 20 = 左边距
    # Two lines of text: the first line 'level number + item name' (MediumLarge, item name in the original game's unique gold), the second line a description one size smaller; both lines left-aligned at x 180. Status text is right-aligned and vertically centered in the whole row, with right padding 20 = left margin
    dx = 34 if m >= 100 else 0   # 三位数的层号多一个数字宽，物品名右移（Mac 截图：「100 层」压住物品名） | A three-digit level number is one digit wider, so the item name shifts right (Mac screenshot: 「100 层」(100) covered the item name)
    return {'type': 'Widget', 'name': 'TouchRF_Row%d' % i, 'fields': {'rect': {'x': 0, 'y': KB_ROW_H * i, 'width': 1157, 'height': KB_ROW_H}}, 'children': [
        {'type': 'ImageWidget', 'name': 'Bg', 'fields': {'filename': 'PANEL\\Chronicle\\Slot_list_item_XL'}}, _rf_icon(icon, px, bb, up),
        _kb_text('Tier', 180, 30 if sub else 0, 150 + dx, 70 if sub else 188, '$FontColorWhite', '$MediumLargeFontSize', text='@dmRiftMsTier%d' % m),
        _kb_text('Name', 330 + dx, 30 if sub else 0, 520 - dx, 70 if sub else 188, '$FontColorGoldYellow', '$MediumLargeFontSize', text=name),
        _kb_text('Sub', 180, 100, 670, 56, '$FontColorMoreLightGray', '$MediumFontSize', text=sub),
        _kb_text('State', 850, 0, 287, 188, '$FontColorMoreLightGray', '$MediumLargeFontSize', 'right', text='@dmRiftMsNo')]}
def _rf_kv(k, y, label, vcolor='$FontColorWhite'):   # 「标签 + 值」一行：标签灰、左列左对齐；值（缺省白）右列右对齐（左栏 0–460） | One 'label + value' row: label gray, left-aligned in the left column; value (white by default) right-aligned in the right column (left column 0–460)
    # 英文版：同一句英文比中文宽，标签写全称（Enemy Power / Next Unique）用中号字会顶到数值。每行另放一个小一号的标签控件 …LE，
    # 缺省摆在屏外；游戏语言不是中文时宿主把它挪进来、把中号那个挪出去（dm_rift_ui.c）。中文画面不变
    # English version: the same sentence is wider in English than in Chinese, and a full-name label (Enemy Power / Next Unique) in the medium font would run
    # into the value. Each row gets an extra, one-size-smaller label widget ...LE,
    # placed off screen by default; when the game language is not Chinese the host moves it in and moves the medium one out (dm_rift_ui.c). The Chinese
    # screen is unchanged
    return [_kb_text('TouchRF_%sL' % k, 0, y, 250, 56, '$FontColorMoreLightGray', '$MediumFontSize', 'left', label),
            _kb_text('TouchRF_%sLE' % k, 99999, y, 330, 56, '$FontColorMoreLightGray', RF_EN_PT, 'left', label),
            _kb_text('TouchRF_%s' % k, 230, y, 250, 56, vcolor, '$MediumFontSize', 'right')]
RF_LW = 500                         # 左栏宽（列表区 530 起，栏间 30） | Left column width (list area starts at 530, column gap 30)
RF_X10_K = 0.66                     # ±10：前端小按钮 366×92 × 0.66 = 242×61，字 52（约按钮高的 45%） | ±10: front-end small button 366×92 × 0.66 = 242×61, text 52 (about 45% of the button height)
RF_X10 = (0, 258)                   # 两钮间隔 16，左右与箭头组对齐（0–500） | Gap of 16 between the two buttons, left and right aligned with the arrow group (0–500)
RF_DIV_W = 1590                     # 原版 PauseMenu/Divider 在 scale 1 时的长度（布局单位，截图量出） | Length of the original game's PauseMenu/Divider at scale 1 (layout units, measured from a screenshot)
def _rf_div(name, x, y, w=500):   # 分隔金线按所在栏宽缩放 | The gold divider line is scaled to the width of its column
    return {'type': 'ImageWidget', 'name': name, 'fields': {'rect': {'x': x, 'y': y, 'scale': round(w / RF_DIV_W, 4)}, 'filename': 'PauseMenu\\Divider'}}
def _rf_key(name, cap, by):   # 手柄模式下贴在按钮左端的原版键帽（Switch 手柄：右 = A、上 = X；48×45 原图放大 1.5）。缺省在屏外，宿主在手柄模式挪进来（dm_rift_ui.c）；这一页原版底部提示条不在屏上 | The original button glyph placed at the left end of the button in controller mode (Switch controller: right = A, top = X; the 48x45 source image scaled by 1.5). Off screen by default, moved in by the host in controller mode (dm_rift_ui.c); on this page the original bottom button legend is not on screen
    return {'type': 'ImageWidget', 'name': name, 'fields': {'rect': {'x': 99999, 'y': by + 26, 'scale': 1.5}, 'filename': 'Controller/ControllerButtons/Switch/' + cap}}
def _rf_keys(name, cap, by):   # Mac App 的覆盖层（DM_OVERLAY_MAC）再带一个同位的键盘键帽（名字加 K，图在 kbd_glyphs.py 那一套里）：玩家用键盘时宿主显示它、藏手柄那个。iOS 的覆盖层不带 | the Mac App's overlay (DM_OVERLAY_MAC) additionally carries a keyboard key cap at the same position (name with K appended, images from the kbd_glyphs.py set): when the player uses the keyboard the host shows it and hides the gamepad one. The iOS overlay does not carry it
    k = _rf_key(name, cap, by); return [k] + ([dict(k, name=name + 'K', fields=dict(k['fields'], filename='Ztouch_controller_buttons_ui/switchK' + cap))] if os.environ.get('DM_OVERLAY_MAC') else [])
def _rift_board():
    # 左栏：标题 0–65 → 箭头组 80–188（大厅大箭头 94×108 原尺寸，层数大字在两箭头正中）→ ±10 两个等宽钮 212–267（前端小按钮 366×92 × 0.596 = 218×55，间隔 24，左右与箭头组对齐）
    #   → 本层信息 5 行（行距 64）307–627 → 组间 40 → 安亚赌博 667–851 → 「开始」贴底：底边 = 右列列表底边 1280，宽 460（前端中按钮 470×118 × 0.979）
    # 重排：三组（选择层数 / 本层 / 安亚赌博），每组标题金字 + 原版暂停菜单分隔金线；组内行距 60、组间 48；±10 用小一号按钮 + 正常字号（不顶满）
    # Left column: title 0–65 → arrow group 80–188 (lobby large arrows 94×108 at original size, large level number text exactly between the two arrows) → two equal-width ±10 buttons 212–267 (front-end small button 366×92 × 0.596 = 218×55, gap 24, left and right aligned with the arrow group)
    #   → this-level info 5 rows (row pitch 64) 307–627 → group gap 40 → Anya gamble 667–851 → 「开始」(Enter Rift) flush against the bottom: bottom edge = bottom edge of the right column list 1280, width 460 (front-end medium button 470×118 × 0.979)
    # Re-layout: three groups (select level / this level / Anya gamble), each with a gold title + the original game's pause menu gold divider line; row pitch within a group 60, between groups 48; ±10 uses a one-size-smaller button + normal font size (not filling the full width)
    sel = [_kb_text('TouchRF_PickT', 0, RF_T0, 500, 60, '$FontColorGoldYellow', '$MediumFontSize', 'left', '@dmRiftSelT'), _rf_div('TouchRF_DivA', 0, 62),
           _rf_btn('TouchRF_M1', 0, 96, 'Lobby/Final/LobbyArrowLargeL', _RF_ARROW, 1.0), _rf_btn('TouchRF_P1', 406, 96, 'Lobby/Final/LobbyArrowLargeR', _RF_ARROW, 1.0),
           _kb_text('TouchRF_Num', 94, 96, 312, 108, '$FontColorLightGold', '$HugeFontSize', 'center', '1'),
           _rf_btn('TouchRF_M10', RF_X10[0], 224, 'FrontEnd/HD/Final/FrontEnd_ButtonSmall', _RF_BTN, RF_X10_K, '-10', 52), _rf_btn('TouchRF_P10', RF_X10[1], 224, 'FrontEnd/HD/Final/FrontEnd_ButtonSmall', _RF_BTN, RF_X10_K, '+10', 52)]
    sel += [_kb_text('TouchRF_InfT', 0, 332, 500, 60, '$FontColorGoldYellow', '$MediumFontSize', 'left', '@dmRiftInfT'), _rf_div('TouchRF_DivB', 0, 394)]
    for n, (k, lab) in enumerate((('Range', '@dmRiftRange'), ('Best', '@dmRiftBest'), ('Str', '@dmRiftStr'), ('Lvl', '@dmRiftLvl'), ('Time', '@dmRiftTime'))): sel += _rf_kv(k, 414 + 64 * n, lab)
    sel += [_kb_text('TouchRF_GamT', 0, 760, 500, 60, '$FontColorGoldYellow', '$MediumFontSize', 'left', '@dmRiftGamT'), _rf_div('TouchRF_DivC', 0, 822)] + []
    # 四行（行距 64，同「本层」）：左边灰字标签（暗金 / 套装 / 通关后暗金 / 通关后套装）+ 右对齐的值（暗金色、套装绿各一个控件，宿主填其一）。
    # 两行整句、两列并排都超出栏宽（Mac 截图：被截断 / 互相压住）；五行带小标题在真机上太挤（行距 56、末行贴着「开始」）
    # Four rows (row pitch 64, same as 「本层」(This Level)): gray label on the left (unique / set / unique after clear / set after clear) + right-aligned value (one widget each in unique gold and set green; the host fills one of them).
    # Two rows of full sentences and two side-by-side columns both exceed the column width (Mac screenshot: truncated / overlapping each other); five rows with a subtitle are too cramped on the real device (row pitch 56, last row touching 「开始」(Enter Rift))
    for n in range(4): sel += [_kb_text('TouchRF_Gam%dL' % n, 0, 842 + 64 * n, 300, 56, '$FontColorMoreLightGray', '$MediumFontSize', 'left'), _kb_text('TouchRF_Gam%dLE' % n, 99999, 842 + 64 * n, 330, 56, '$FontColorMoreLightGray', RF_EN_PT, 'left'), _kb_text('TouchRF_Gam%dU' % n, 230, 842 + 64 * n, 250, 56, '$FontColorGoldYellow', '$MediumFontSize', 'right'), _kb_text('TouchRF_Gam%dS' % n, 230, 842 + 64 * n, 250, 56, '$FontColorGreen', '$MediumFontSize', 'right')]
    sel += [_rf_btn('TouchRF_Start', 0, 1155, 'FrontEnd/HD/Final/FrontEnd_ButtonMed', _RF_BTN, 1.064, '@dmRiftBtnStart')] + _rf_keys('TouchRF_KeyA', 'ButtonRight', 1155)
    # 进行中页：层数大字 96–204 → 「本层」小节（标题 332、金线 394、三行 414 / 478 / 542，与选层页同位）→ 放弃一组（说明 746、按钮 818–943）→ 继续一组贴底（说明 1083、按钮 1155，同「开始」）。
    # 信息—放弃—继续三段间距相等（各 140）；按钮上方的说明都是四到八个字的短句、居中（放弃那句与「本层」三行宿主按「守护者打倒没有」换说法）
    # In-progress page: large level number text 96–204 → 「本层」(This Level) section (title 332, gold line 394, three rows 414 / 478 / 542, same positions as the level selection page) → abandon group (caption 746, button 818–943) → continue group flush against the bottom (caption 1083, button 1155, same as 「开始」(Enter Rift)).
    # The three sections info - abandon - continue are equally spaced (140 each); the captions above the buttons are all short phrases of four to eight characters, centered (for the abandon caption and the three 「本层」(This Level) rows the host changes the wording depending on whether the guardian has been defeated)
    cho = [_kb_text('TouchRF_ChT', 0, RF_T0, 500, 60, '$FontColorGoldYellow', '$MediumFontSize', 'left', '@dmRiftTitleRun'), _rf_div('TouchRF_DivD', 0, 62),
           _kb_text('TouchRF_ChTier', 0, 96, 500, 108, '$FontColorLightGold', '$HugeFontSize', 'center'),
           _kb_text('TouchRF_ChInfT', 0, 332, 500, 60, '$FontColorGoldYellow', '$MediumFontSize', 'left', '@dmRiftInfT'), _rf_div('TouchRF_DivE', 0, 394)]
    for n, (k, lab) in enumerate((('ChLeft', '@dmRiftTimeLeft'), ('ChProg', '@dmRiftProgress'), ('ChNote', ''))): cho += _rf_kv(k, 414 + 64 * n, lab)
    # 按钮上方两句说明的英文版控件（…E，字号 RF_EN_CAP，缺省在屏外；游戏语言不是中文时宿主换用，见 dm_rift_ui.c）：英文句子比中文宽，中号小字放不下
    # English version widgets for the two caption sentences above the buttons (...E, font size RF_EN_CAP, off screen by default; the host switches to them
    # when the game language is not Chinese, see dm_rift_ui.c): English sentences are wider than Chinese and do not fit in the medium-size small text
    cho += [_kb_text('TouchRF_AbTE', 99999, 746, 500, 56, '$FontColorMoreLightGray', RF_EN_CAP, 'center'), _kb_text('TouchRF_ContTE', 99999, 1083, 500, 56, '$FontColorMoreLightGray', RF_EN_CAP, 'center')]
    cho += [_kb_text('TouchRF_AbT', 0, 746, 500, 56, '$FontColorMoreLightGray', '$SmallFontSize', 'center', '@dmRiftCapVoid'),
            _rf_btn('TouchRF_Abandon', 0, 818, 'FrontEnd/HD/Final/FrontEnd_ButtonMed', _RF_BTN, 1.064, '@dmRiftBtnAbandon'),
            _kb_text('TouchRF_ContT', 0, 1083, 500, 56, '$FontColorMoreLightGray', '$SmallFontSize', 'center', '@dmRiftCapCont'),
            _rf_btn('TouchRF_Cont', 0, 1155, 'FrontEnd/HD/Final/FrontEnd_ButtonMed', _RF_BTN, 1.064, '@dmRiftBtnCont'),
            # 本层已完成时次按钮换成「重新选层」（进度已记，叫「放弃」读着不对）。按钮上的字宿主改不了，所以放一个同位置的按钮，宿主按「守护者打倒没有」挪进挪出
            # When this level is completed the secondary button becomes "Reselect level" (progress is already recorded, so "Abandon" reads wrong). The host cannot
            # change button text, so a button is placed at the same position and the host moves it in and out according to "has the Guardian been defeated"
            _rf_btn('TouchRF_Resel', 99999, 818, 'FrontEnd/HD/Final/FrontEnd_ButtonMed', _RF_BTN, 1.064, '@dmRiftBtnResel'),
            ] + _rf_keys('TouchRF_KeyA2', 'ButtonRight', 1155) + _rf_keys('TouchRF_KeyX', 'ButtonTop', 818)
    right = [_kb_text('TouchRF_MsT', 530, RF_T0, 500, 60, '$FontColorGoldYellow', '$MediumFontSize', 'left', '@dmRiftMsT'), _rf_div('TouchRF_DivR', 530, 62, 1157),
             _kb_text('TouchRF_MsN', 1030, RF_T0, 657, 60, '$FontColorMoreLightGray', '$SmallFontSize', 'right', '@dmRiftMsN'),
             # 英文版的这一句写物品原名（Token of Absolution），比中文长一倍：另放一个更宽、字号更小的控件，缺省在屏外，英文时宿主换用
             # In the English version this sentence uses the item's original name (Token of Absolution) and is twice as long as the Chinese: a separate, wider widget
             # with a smaller font is added, off screen by default, and the host switches to it for English
             _kb_text('TouchRF_MsNE', 99999, RF_T0, 757, 60, '$FontColorMoreLightGray', RF_EN_NOTE, 'right', '@dmRiftMsNEn'),
             {'type': 'ImageWidget', 'name': 'TouchRF_ListBar', 'fields': {'rect': {'x': -20, 'y': 80}, 'anchor': {'x': 1.0}, 'filename': 'Panel\\Chronicle\\Chronicle_Scrollbar_Large'}, 'children': [
                 {'type': 'ScrollControllerWidget', 'name': 'TouchRF_ScrollController', 'fields': {'rect': {'x': -31, 'y': 29, 'width': 26, 'height': 1095}, 'anchor': {'x': 1.0},
                  'upArrowFilepath': 'FrontEnd\\HD\\Final\\FrontEnd_ScrollUpBtn', 'downArrowFilepath': 'FrontEnd\\HD\\Final\\FrontEnd_ScrollDownBtn', 'barFilepath': 'PauseMenu\\VerticalIndicator',
                  'viewName': 'TouchRF_Scroll', 'wheelScrollSound': 'cursor_scroll_hd', 'buttonScrollSound': 'cursor_scroll_hd'}}]},
             {'type': 'Widget', 'name': 'TouchRF_ListBox', 'fields': {'rect': {'x': 530, 'y': 80, 'width': 1157, 'height': 1200}}, 'children': [
                 {'type': 'ScrollViewWidget', 'name': 'TouchRF_Scroll', 'fields': {'fitToParent': True, 'scrollControllerName': 'TouchRF_ScrollController'}, 'children': [
                     {'type': 'Widget', 'name': 'TouchRF_List', 'fields': {'rect': {'x': 0, 'y': 0, 'width': 1157, 'height': KB_ROW_H * len(RF_MS)}},
                      'children': [_rf_row(i, *r) for i, r in enumerate(RF_MS)]}]}]}]
    return {'type': 'Widget', 'name': 'TouchRift', 'fields': {'rect': '$OptionsPanelRect', 'anchor': {'x': 0.5}}, 'children': [
        {'type': 'Widget', 'name': 'TouchRF_Body', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1730, 'height': 1284}}, 'children': [
            {'type': 'Widget', 'name': 'TouchRF_Sel', 'fields': {'rect': {'x': 0, 'y': 0, 'width': 500, 'height': 1284}}, 'children': sel},
            {'type': 'Widget', 'name': 'TouchRF_Cho', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 500, 'height': 1284}}, 'children': cho}] + right}]}
def _rift_panel(s):
    # 独立「大秘境」面板（点门开独立面板，不走编年史的页签）。借原版建好、我们不用的「消息记录」面板外壳（MessageLogPanel；暂停菜单那颗按钮已换成分享诊断日志，见 _pause_no_help），
    # 布局整份换掉：外框照编年史面板（全屏 0.7 暗底 + 选项底图 + 标题 + 关闭钮 + 内容底图，ChroniclePanelHD.json 原样抄），没有页签条，内容 = _rift_board。
    # 面板类要的原版子控件（ClickCatcher / ListContainer / ScrollBar 等）照留、挪到屏外（类代码按名字找它们）
    # Standalone 「大秘境」(Greater Rift) panel (tapping the portal opens a standalone panel, not through the chronicle's tabs). It borrows the shell of the 「消息记录」(Message Log) panel that the original game builds and we do not use (MessageLogPanel; that button in the pause menu has been replaced with Share Diagnostics, see _pause_no_help),
    # and the layout is replaced entirely: the outer frame follows the chronicle panel (full-screen 0.7 dark backdrop + options background image + title + close button + content background image, copied as-is from ChroniclePanelHD.json), with no tab strip, content = _rift_board.
    # The original-game child widgets required by the panel class (ClickCatcher / ListContainer / ScrollBar etc.) are kept and moved off screen (the class code finds them by name)
    for a in ('"type": "MessageLogPanel", "name": "MessageLogPanel"', '"name": "ListContainer"', '"name": "ScrollBar"', '"name": "ChatEntryTemplate"'):
        if s.count(a) != 1: raise SystemExit('touch_layout：消息记录面板不是原样（%s），大秘境面板没换' % a)
    off = {'rect': {'x': 99999, 'y': 0, 'width': 10, 'height': 10}}
    native = [{'type': 'ClickCatcherWidget', 'name': 'ClickCatcher', 'fields': off},
              {'type': 'RectangleWidget', 'name': 'MessageLogBackground', 'fields': off},
              {'type': 'ImageWidget', 'name': 'Background', 'fields': off},
              {'type': 'TextBoxWidget', 'name': 'title', 'fields': dict(off, text='')},
              {'type': 'ChatListWidget', 'name': 'ListContainer', 'fields': off, 'children': [
                  {'type': 'ListItemWidget', 'name': 'ChatEntryTemplate', 'fields': {'rect': {'x': 0, 'y': 0, 'width': 10, 'height': 10}}, 'children': [
                      {'type': 'TextBoxWithLinksWidget', 'name': 'MessageText', 'fields': {'fitToParent': True}}]}]},
              {'type': 'ImageWidget', 'name': 'ScrollBarBackground', 'fields': off, 'children': [
                  {'type': 'ScrollControllerWidget', 'name': 'ScrollBar', 'fields': {'rect': {'x': 0, 'y': 0, 'width': 10, 'height': 10}, 'viewName': 'ListContainer'}}]}]
    close = 'PanelManager:ClosePanel:MessageLogPanel'
    frame = [{'type': 'RectangleWidget', 'name': 'TouchRF_Dim', 'fields': {'fitToScreen': True, 'color': [0.0, 0.0, 0.0, 0.7]}, 'children': [
                 {'type': 'ClickCatcherWidget', 'name': 'TouchRF_Catcher', 'fields': {'fitToParent': True}},
                 {'type': 'Widget', 'name': 'TouchRF_Anchor', 'fields': {'anchor': {'x': 0.5, 'y': 0.5}, 'rect': '$SettingsPanelAnchorRect'}, 'children': [
                     {'type': 'ImageWidget', 'name': 'TouchRF_FrameBg', 'fields': {'filename': '\\PANEL\\Options\\FrontEndOptionsBG'}},
                     {'type': 'TextBoxWidget', 'name': 'TouchRF_Title', 'fields': {'rect': {'x': 0, 'y': 45, 'width': 1950, 'height': 103}, 'text': '@dmRiftPanelTitle', 'style': '$StyleTitleBlock'}},
                     {'type': 'ButtonWidget', 'name': 'TouchRF_CloseBtn', 'fields': {'rect': {'x': 1868, 'y': 8}, 'filename': 'PANEL\\closebtn_4x', 'hoveredFrame': 3, 'onClickMessage': close,
                      'tooltipString': '@strClose', 'sound': 'cursor_close_window_hd', 'acceptsEscKeyEverywhere': True}},
                     # 触摸关闭钮：外框右上角同一处只留一个——手柄 / 键鼠显示上面那个原版红框，触屏显示这个金色 X（与别的面板同图），宿主按模式把另一个挪出屏（dm_rift_ui.c）。
                     # 位置：标题条（y 45 起、高 103，深色底）那一行的右端、面板内侧——金 X（24×33 像素 = 48×66 单位，单位是外框贴图像素的 2 倍）垂直居中于标题条：
                     # y = 45 + 103 / 2 − 33 = 64；x = 1766：整个 X 落在标题条右端角花（约 1832 起）以左的深色底上，不伸出外框、不压角花。原先对准红框中心，一半伸出外框、压着石角花纹，几乎看不见
                     # 点按热区：金 X 本体只有约 10×13 点，另放一个透明的按钮罩在它上面当热区（借触屏那套透明键帽图 48×45 像素，放大 2.4 倍 = 约 46×43 点），中心对准 X 的中心（x 1705 是按「按名字取到的控件中心」实测校的：1675 时偏左 6 点）
                     # Touch close button: only one stays at the same place at the top right of the frame - gamepad / keyboard-and-mouse show the original red frame above, touch shows this gold X (same image as other panels), and the host moves the other one off screen by mode (dm_rift_ui.c).
                     # Position: at the right end of the title bar row (starting at y 45, 103 high, dark background), inside the panel - the gold X (24x33 pixels = 48x66 units, a unit being twice a pixel of the frame texture) is centred vertically on the title bar:
                     # y = 45 + 103 / 2 - 33 = 64; x = 1766: the whole X sits on the dark background to the left of the corner ornament at the right end of the title bar (starting at about 1832), neither sticking out of the frame nor covering the ornament. It used to be aligned with the centre of the red frame, half outside the frame and on top of the stone corner pattern, nearly invisible
                     # Tap hot zone: the gold X itself is only about 10x13 points, so a transparent button is placed over it as the hot zone (borrowing the touch layout's transparent key-cap image of 48x45 pixels, enlarged 2.4 times = about 46x43 points), centred on the centre of the X (x 1705 was calibrated by measuring "the widget centre fetched by name": at 1675 it was 6 points to the left)
                     {'type': 'ImageWidget', 'name': 'TouchRF_CloseX', 'fields': {'rect': {'x': 1766, 'y': 64}, 'filename': 'PANEL\\HUD_02\\front_panel'}},
                     {'type': 'ButtonWidget', 'name': 'TouchClose', 'fields': {'rect': {'x': 1705, 'y': -11, 'scale': 2.4}, 'filename': 'Ztouch_controller_buttons_ui/switch_ButtonRight', 'sound': 'cursor_close_window_hd', 'onClickMessage': close}}]}]},
             {'type': 'ImageWidget', 'name': 'TouchRF_ContentBg', 'fields': {'rect': '$SettingsPanelBackgroundRect', 'anchor': {'x': 0.5}, 'filename': 'Controller/Panel/Options/Panel_Options_BG'}},
             ]
    board = _rift_board(); board['children'][0]['fields']['rect']['x'] = 0; board['children'][0]['fields']['rect']['y'] = -54   # 没有页签条：内容上移 54，吃掉标题下的空带，底边留出与左边距相当的空（原先上空下贴底）   # 内容缺省就在屏上（编年史版要切页签才挪进来） | No tab strip: content moves up 54, taking up the empty band under the title, leaving a gap at the bottom edge comparable to the left margin (previously empty at the top and flush against the bottom) # the content is on screen by default (the chronicle version only moves it in when the tab is switched)
    lay = {'basedOn': 'MessageLogPanel_800x600HD.json', 'type': 'MessageLogPanel', 'name': 'MessageLogPanel',
           'fields': {'priority': 9002, 'fitToParent': True, 'defaultWidget': 'TouchRF_Start'}, 'children': native + frame + [board]}
    return json.dumps(lay, ensure_ascii=False, indent=1), 1
# 积分牌「秘境」页（第六项）：一行总览 + 每层一行（只显示限时内通关过的层，宿主按层号从高到低排、其余挪出屏），行样式同首领行
# Kill board 「秘境」(Rift) page (sixth item): one summary row + one row per level (only levels cleared within the time limit are shown; the host sorts by level number from high to low and moves the rest off screen), row style same as the boss rows
RIFT_ROWS = 100
def _kb_rift_row(name):
    return {'type': 'Widget', 'name': name, 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1157, 'height': KB_ROW_H}}, 'children': [
        {'type': 'ImageWidget', 'name': 'Bg', 'fields': {'filename': 'PANEL\\Chronicle\\Slot_list_item_XL'}},
        {'type': 'ImageWidget', 'name': 'Icon', 'fields': {'rect': {'x': 22, 'y': 24, 'scale': 0.83}, 'filename': 'QuestIcons\\Done'}},
        _kb_text('Name', 180, 8, 640, 60, '$FontColorWhite', '$LootFilterMediumLargeFontSize'),
        _kb_text('Kills', 760, 8, 360, 60, '$FontColorGoldYellow', '$LootFilterMediumLargeFontSize', 'right'),
        _kb_text('First', 180, 76, 560, 48, '$FontColorMoreLightGray', '$LootFilterSmallFontSize'),
        _kb_text('Drops', 740, 76, 380, 48, '$FontColorMoreLightGray', '$LootFilterSmallFontSize', 'right'),
        _kb_text('Times', 180, 126, 940, 48, '$FontColorMoreLightGray', '$LootFilterSmallFontSize')]}
def _kb_rift_page():
    return {'type': 'Widget', 'name': 'TouchKB_Page5', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1730, 'height': 1284}}, 'children': [
        {'type': 'ImageWidget', 'name': 'TouchKB_ListBar', 'fields': {'rect': {'x': -20, 'y': 80}, 'anchor': {'x': 1.0}, 'filename': 'Panel\\Chronicle\\Chronicle_Scrollbar_Large'}, 'children': [
            {'type': 'ScrollControllerWidget', 'name': 'TouchKB_ScrollController5', 'fields': {'rect': {'x': -31, 'y': 29, 'width': 26, 'height': 1095}, 'anchor': {'x': 1.0},
             'upArrowFilepath': 'FrontEnd\\HD\\Final\\FrontEnd_ScrollUpBtn', 'downArrowFilepath': 'FrontEnd\\HD\\Final\\FrontEnd_ScrollDownBtn', 'barFilepath': 'PauseMenu\\VerticalIndicator',
             'viewName': 'TouchKB_Scroll5', 'wheelScrollSound': 'cursor_scroll_hd', 'buttonScrollSound': 'cursor_scroll_hd'}}]},
        {'type': 'Widget', 'name': 'TouchKB_ListBox', 'fields': {'rect': {'x': 530, 'y': 80, 'width': 1157, 'height': 1200}}, 'children': [
            {'type': 'ScrollViewWidget', 'name': 'TouchKB_Scroll5', 'fields': {'fitToParent': True, 'scrollControllerName': 'TouchKB_ScrollController5'}, 'children': [
                {'type': 'Widget', 'name': 'TouchKB_List', 'fields': {'rect': {'x': 0, 'y': 0, 'width': 1157, 'height': KB_ROW_H * (RIFT_ROWS + 2)}},
                 'children': [_kb_rift_row('TouchKB_RiftSum'), _kb_rift_row('TouchKB_RiftSum2')] + [_kb_rift_row('TouchKB_RiftT%d' % t) for t in range(1, RIFT_ROWS + 1)]}]}]}]}
def _chronicle_killboard(s):
    n = 0
    # 页签宽度 266 → 320：原版三个页签每个固定宽 266、字用「中号」；我们整套界面用大字号（中号 28 → 40）之后，
    # 英文 Runewords 九个字母放不下，显示成「Runewo…」（中文四个字放得下，所以一直没人看见；英文版第一阶段合入后线上就这样）。
    # 四个页签统一加宽、整排仍居中（中文那排也跟着变宽，有意改动）；4 × 320 + 间隔 = 1286，面板内宽 1730，iPad 档同一套单位
    # Tab width 266 → 320: each of the original three tabs has a fixed width of 266 and uses the "medium" font; after we moved the whole UI to large fonts
    # (medium 28 → 40),
    # the nine letters of the English Runewords no longer fit and it shows as "Runewo..." (the four Chinese characters fit, which is why nobody ever saw it;
    # this is how released builds looked after phase one of the English version was merged).
    # All four tabs are widened uniformly and the row stays centered (the Chinese row gets wider as well, an intentional change); 4 × 320 + gaps = 1286,
    # panel inner width 1730, same units for the iPad profile
    for a, b in (('"tabSize": { "x": 266, "y": 121 },', '"tabSize": { "x": 320, "y": 121 },'), ('"tabCount": 3,', '"tabCount": 4,'), ('"inactiveFrames": [ 0, 0, 0],', '"inactiveFrames": [ 0, 0, 0, 0],'), ('"activeFrames": [ 1, 1, 1],', '"activeFrames": [ 1, 1, 1, 1],'),
                 ('"textStrings": [ "@Unique", "@Sets", "@Runewords"],', '"textStrings": [ "@Unique", "@Sets", "@Runewords", "@Bosses"],'),
                 ('"ChroniclePanelMessage:ToggleChildPanel:ChronicleRunewordsListPanel"', '"ChroniclePanelMessage:ToggleChildPanel:ChronicleRunewordsListPanel",\n                    "ChroniclePanelMessage:ToggleChildPanel:TouchKillBoard"')):
        if s.count(a) != 1: raise SystemExit('touch_layout：编年史页签条 %s 不是原样（原版布局变了？）' % a)
        s = s.replace(a, b); n += 1
    end = s.rstrip().rstrip('}').rstrip(); assert end.endswith(']'); at = len(end) - 1
    return s[:at].rstrip() + ',\n        ' + json.dumps(_kb_board(), ensure_ascii=False) + '\n    ' + s[at:], n + 1
# 下拉列表（编年史排序、选项页各下拉共用）：每行高 40，大字号（40）下几行字叠在一起——行高改 56
# Dropdown list (shared by the chronicle sort and the dropdowns on the options pages): each row is 40 high, and at the large font size (40) several lines of text overlap -- row height changed to 56
def _dropdown_rows(s):
    a = '"rect": { "width": 625, "height": 40 }'
    if s.count(a) != 2: raise SystemExit('touch_layout：下拉列表行高不是原样（原版布局变了？）')
    return s.replace(a, '"rect": { "width": 625, "height": 56 }'), 2
# 编年史列表页的搜索框去掉（mini 真触点：点了出 TextInputModal、画面变暗但不弹输入框，点哪都没反应，只能靠手柄「返回」退）。三个列表页都继承这份
# The search box on the chronicle list pages is removed (real touch points on the second Mac: tapping it brings up TextInputModal, the picture dims but no input box pops up, nothing responds to taps, and the only way out is the controller 「返回」(Back)). All three list pages inherit this file
def _chronicle_no_search(s):
    n = 0
    for var in ('"$ChronicleSearchBox"', '"$ChronicleSearchInput"'):   # 这两个 rect 写的是变量名（offscreen 只认花括号写法），直接换 | These two rects are written as variable names (offscreen only recognizes the curly-brace form), so replace them directly
        if s.count('"rect": ' + var) != 1: raise SystemExit('touch_layout：编年史搜索框的 rect 不是原样（原版布局变了？）')
        s = s.replace('"rect": ' + var, '"rect": { "x": 99999, "y": 99999, "width": 1, "height": 1 }'); n += 1
    # 「普通 / 优异 / 精英」三个勾选框旁的字：大字号下字比勾选框低半个字、下沿碰到下面的列表；字框上移 12 单位与勾选框对齐
    # Text next to the three checkboxes 「普通 / 优异 / 精英」(Normal / Exceptional / Elite): at the large font size the text sits half a character lower than the checkbox and its bottom edge touches the list below; the text box is moved up 12 units to align with the checkbox
    for x in (80, 486, 1030):
        a = '"rect": { "x": %d, "y": 7, "height": 50 },' % x
        if s.count(a) != 1: raise SystemExit('touch_layout：编年史品质勾选框文字的 rect 不是原样（原版布局变了？）')
        s = s.replace(a, '"rect": { "x": %d, "y": -5, "height": 50 },' % x); n += 1
    return s, n
# 难度选择弹窗（普通 / 噩梦 / 地狱）：三个按钮间距 131 单位 ≈ 5.1 毫米、按钮高 4.6 毫米。底框是定长贴图，单拉间距按钮会掉出框——整个框连同里面的按钮放大 1.6 倍
# （前端页 1 单位 = 0.0337 毫米：间距 131 × 1.6 ≈ 7.1 毫米）；放大以左上角为原点，框原位 (-272, -270) 是居中的，按比例改成 (-435, -432)
# NPC 对话菜单（原版很窄、手指难点）：框加宽、行加高到手指好点；字号不动，框仍是原版九宫格底图自己拉伸。
# 行距 = 字高 + 2 × listEntryPadding.y：10 → NPC_PAD_Y；左右留白 32 → NPC_PAD_X
# Difficulty selection popup (Normal / Nightmare / Hell): the three buttons are spaced 131 units ≈ 5.1 mm apart and are 4.6 mm high. The background frame is a fixed-length texture, so stretching only the spacing would drop buttons out of the frame -- the whole frame together with the buttons inside is scaled up 1.6×
# (on front-end pages 1 unit = 0.0337 mm: spacing 131 × 1.6 ≈ 7.1 mm); scaling uses the top-left corner as origin, and the frame's original position (-272, -270) is centered, so it is changed proportionally to (-435, -432)
# NPC dialog menu (very narrow in the original game and hard to tap with a finger): the frame is widened and rows made taller until they are easy to tap; font size unchanged, and the frame is still the original game's nine-slice background image stretching itself.
# Row pitch = text height + 2 × listEntryPadding.y: 10 → NPC_PAD_Y; left/right padding 32 → NPC_PAD_X
NPC_PAD_X, NPC_PAD_Y = 150, 62
def _npc_menu(s):
    for a, b in (('"padding": { "left": 32, "top": 24, "right": 32, "bottom": 24 },', '"padding": { "left": %d, "top": 24, "right": %d, "bottom": 24 },' % (NPC_PAD_X, NPC_PAD_X)),
                 ('"listEntryPadding": { "x": 0, "y": 10 },', '"listEntryPadding": { "x": 0, "y": %d },' % NPC_PAD_Y)):
        if s.count(a) != 1: raise SystemExit('touch_layout：NPC 对话菜单 %s 不是原样（原版布局变了？）' % a)
        s = s.replace(a, b)
    return s, 2
# 说明框离屏幕边的最小距离 50 → 120 单位：手机上长说明（技能树里没学的技能，十几行）下沿伸出屏幕底、最后几行看不到。
# 游戏按这个值把说明框往回推（上移 / 换边）。2868×1320 实量同一条说明：文字下沿 1268 → 1225（下安全线 1260）
# 真机更正：120 时说明框本身没出屏（真机与 Mac 宿主同为文字下沿 1225 / 1320），是被面板底部那条空带上的提示字（绑定提示、物品手势）压住最后一行。
# 改 190：说明框下沿停在底部空带之上（空带上沿约 1202 像素）
# Minimum distance of the tooltip from the screen edge 50 → 120 units: on a phone the bottom edge of a long description (an unlearned skill in the skill tree, a dozen or so lines) extends past the bottom of the screen and the last few lines cannot be seen.
# The game pushes the tooltip back by this value (moves it up / switches sides). The same description measured at 2868×1320: text bottom edge 1268 → 1225 (lower safe line 1260)
# Real-device correction: at 120 the tooltip itself was not off screen (text bottom edge 1225 / 1320 on both the real device and the Mac host); the last line was covered by the hint text on the empty band at the bottom of the panel (binding hint, item gestures).
# Changed to 190: the bottom edge of the tooltip stops above the bottom empty band (top edge of the empty band is at about 1202 pixels)
def _tooltip_pad(s):
    a = '"screenEdgePadding": 50,'
    if s.count(a) != 1: raise SystemExit('touch_layout：说明框 screenEdgePadding 不是原样（原版布局变了？）')
    s = s.replace(a, '"screenEdgePadding": 190,')
    # 悬停延时：手柄版 400 毫秒（键鼠版是 0）。触摸下点了就该立刻出说明
    # Hover delay: 400 ms in the controller layout (0 in the keyboard-and-mouse layout). Under touch the description should appear immediately on tap
    b = '"showDelayMilliseconds": 400,'
    if s.count(b) != 1: raise SystemExit('touch_layout：说明框 showDelayMilliseconds 不是原样（原版布局变了？）')
    return s.replace(b, '"showDelayMilliseconds": 0,'), 2
PRESENT = {
    'controller/uiswitcherhd.json': lambda s: offscreen(s, types=(), names=('CycleLeftIndicator', 'CycleRightIndicator')),   # 页签条两端的 L / R 字形 | The L / R glyphs at the two ends of the tab strip
    'controller/characterstatspanelhd.json': lambda s: (lambda r1: (lambda r2: (r2[0], r1[1] + r2[1]))(_char_title_up(r1[0])))(_char_no_stamina(s)),
    'controller/tooltipspanelhd.json': _tooltip_pad,
    'controller/dropgoldmodalhd.json': lambda s: offscreen(s),   # 金币数量框：原版大小与样子（去掉 − / + / 一半 / 全部，只留数字框、好、取消）；这里只挪走「好 / 取消」上的手柄键位提示 | Gold amount box: the original game's size and look (− / + / half / all removed, keeping only the number box, OK and Cancel); here only the controller button prompts on 「好 / 取消」(OK / Cancel) are moved away
    'controller/bankexpansionlayouthd.json': lambda s: offscreen(s, types=(), names=('gold_max',)),   # 储藏箱「金币最大数: N」这行去掉（40 号字在框内放不下） | The stash line 「金币最大数: N」(Max gold: N) is removed (40-point text does not fit inside the frame)
    'npcdialogpanelhd.json': _npc_menu,
    'chroniclebaselistpanelhd.json': _chronicle_no_search,
    'dropdownlistcontentshd.json': _dropdown_rows,
    'chroniclepanelhd.json': lambda s: (lambda a: (lambda b: (b[0], a[1] + b[1]))(_chronicle_killboard(a[0])))(_chronicle_touch(s)),
    'controller/messagelogpanelhd.json': _rift_panel,
    # 战利品过滤器去掉、编年史保留、消息记录不要；另加第四个「分享诊断日志」（占原「消息记录」按钮）。
    # 暂停菜单 = 选项 / 保存并退出 / 编年史 / 分享诊断日志（中心距仍 240，四个落在原版六个的上下两端之内）；
    # 带佣兵页签的角色用的经典版没有编年史 = 选项 / 保存并退出 / 分享诊断日志（底图只放得下原版四个的跨度，中心距 225，正好铺满原版首末两个的位置）
    # 加「一键重开」后五个 / 四个按钮在原来首尾之间等距：带编年史那版中心距 180，经典版 150
    # Loot filter removed, chronicle kept, message log dropped; a fourth button 「分享诊断日志」(Share Diagnostics) is added (taking the place of the original 「消息记录」(Message Log) button).
    # Pause menu = Options / Save and Exit / Chronicle / Share Diagnostics (center spacing still 240; the four fall within the top and bottom ends of the original six);
    # the classic version used by characters with a mercenary tab has no chronicle = Options / Save and Exit / Share Diagnostics (the background image only fits the span of the original four; center spacing 225, exactly filling the positions of the original first and last)
    # After adding 「一键重开」(Quick Restart) the five / four buttons are spaced evenly between the original first and last: center spacing 180 for the version with chronicle, 150 for the classic version
    'controller/pauselayoutgardenhd.json': _pause_no_help({'Settings': (428, 293), 'Exit': (728, 473), 'Chronicle': (1028, 833), 'MessageLog': (278, 1013)}, hide=('Help', 'LootFilter'), restart_y=653),
    'controller/pauselayouthd.json': _pause_no_help({'Settings': (578, 428), 'Exit': (878, 578), 'MessageLog': (428, 878)}, hide=('Help',), restart_y=728),
    'helpbuttonpanelhd.json': lambda s: offscreen(s, types=(), names=('Help',)),
    'controller/buttonbindingmodalhd.json': _bind_touch,
    'youdiedmodalhd.json': lambda s: offscreen(s, types=(), names=('SubText',)),   # 死亡画面第二行「按 ＋ 继续。」（＋ = 手柄键图标）：下面就有「继续」按钮，这句不要（点一下能完成的提示直接去掉） | Second line of the death screen 「按 ＋ 继续。」(Press + to continue.) (+ = controller key icon): there is a 「继续」(Continue) button right below, so this sentence is dropped (hints for something done with one tap are removed outright)
    'controller/controlleroverlayhd.json': _overlay_touch,
    'controller/console/switch/playerinventoryexpansionlayouthd.json': _inv_rw,   # 物品栏 + 符文之语清单控件（见 _RW） | Inventory + runeword list widgets (see _RW)
    'controller/playerinventoryexpansionlayouthd.json': _inv_touch(False),
}
# 字号（先用大字号）：产品走主机档 _profileConsoleHD（小 26 / 中 28 / 中大 32），前端按钮字在 2868×1320 上约 1.1 毫米高。
# 换成原包掌机大字档 _profilehandheldlv.json 的那组值（Blizzard 给小屏做的，统一 40）。只换字号，面板矩形与缩放不动。
# Font sizes (use the large sizes first): the product uses the console profile _profileConsoleHD (small 26 / medium 28 / medium-large 32), and front-end button text is about 1.1 mm high at 2868×1320.
# Replaced with the set of values from the original package's handheld large-font profile _profilehandheldlv.json (made by Blizzard for small screens, uniformly 40). Only font sizes are replaced; panel rectangles and scaling are unchanged.
BIG_FONT = (('"XMediumFontSize": 30,\n    "MediumFontSize": 28,\n    "SmallFontSize": 26,\n'),
            ('"SmallFontSize": 40,\n    "MediumFontSize": 40,\n    "MediumLargeFontSize": 40,\n    "XMediumFontSize": 40,\n    "XMediumLargeFontSize": 40,\n    "LargeFontSize": 40,\n    "SmallPanelFontSize": 32,\n    "TooltipFontSize": 42,\n    "OptionsDividerRect": { "x": 0, "y": 150 },\n'))   # TooltipFontSize：物品 / 技能说明框的字（基础档 36；42 = 原包主机大字档 _profileconsolelv.json 的值） | TooltipFontSize: text of the item / skill tooltip (base profile 36; 42 = the value in the original package's console large-font profile _profileconsolelv.json)
# OptionsDividerRect：选项页分节标题下的横线。行距拉到 180 之后标题字在行内垂直居中（约 y 90），原来 y 90 的横线正好压在字上——挪到 y 150（字的下方）
# OptionsDividerRect: the horizontal line under the section headers of the options page. After the row pitch is stretched to 180 the header text is vertically centered in the row (about y 90), and the line originally at y 90 sits right on the text -- moved to y 150 (below the text)
def build_profile(romfs, outdir):
    path, text = read_layout(romfs, 'controller/_profileconsolehd.json')
    text = text.replace('\r\n', '\n')   # 这份原版是 CRLF | This original-game file uses CRLF
    if text.count(BIG_FONT[0]) != 1: raise SystemExit('touch_layout：_profileConsoleHD 的字号段不是原样（原版变了？）')
    o = os.path.join(outdir, 'profile_consolehd.json'); open(o, 'w', encoding='utf-8').write(text.replace(BIG_FONT[0], BIG_FONT[1])); repl = {path.lower(): o}
    # 掌机档（DM_OPMODE=0：过热降级会切过去，触屏脚本也用它）走另一份 _profileHandheldHD，上面的改动到不了：同一组值也写进去，
    # 否则掌机档下字是小的、选项页分节横线穿过标题字（在 Mac 上的 iOS 包截到）
    # The handheld profile (DM_OPMODE=0: overheat downgrade switches to it, and touch scripts use it too) goes through a separate _profileHandheldHD that the changes above do not reach: the same set of values is written there too,
    # otherwise under the handheld profile the text is small and the options page section divider line runs through the header text (captured on the iOS app on the Mac)
    path, text = read_layout(romfs, 'controller/_profilehandheldhd.json'); text = text.replace('\r\n', '\n')
    a = '"XMediumLargeFontSize": 36,\n    "MediumLargeFontSize": 36,\n    "XMediumFontSize": 34,\n'
    if text.count(a) != 1: raise SystemExit('touch_layout：_profileHandheldHD 的字号段不是原样（原版变了？）')
    o = os.path.join(outdir, 'profile_handheldhd.json'); open(o, 'w', encoding='utf-8').write(text.replace(a, BIG_FONT[1])); repl[path.lower()] = o
    return repl

# NPC 对话菜单（交谈 / 交易 / 雇佣…）的行距：原版每行约 66 单位 = 2.6 毫米，相邻行必然点串；行间距 10 → 124，行距约 180 单位 = 7 毫米（手指目标下限，热区审计）。
# 只改行距。选项本身不认鼠标点击：把 Switch 版关掉的 isHoverList 改回 true 也没用（Mac 宿主注入点击实测，点了无消息、高亮不动）——点选要由触摸层换成方向键下 ×N + A。
# Row pitch of the NPC dialog menu (talk / trade / hire...): the original game uses about 66 units per row = 2.6 mm, so adjacent rows are inevitably mis-tapped; row spacing 10 → 124, row pitch about 180 units = 7 mm (lower limit for a finger target, hit-area audit).
# Only the row pitch is changed. The options themselves do not respond to mouse clicks: changing isHoverList, which the Switch version turned off, back to true does not help either (measured with injected clicks on the Mac host: clicking produces no message and the highlight does not move) -- selecting by tap has to be converted by the touch layer into D-pad down ×N + A.
def _npc_dialog_touch(s):
    b = '"basedOn": "NpcDialogPanelHD.json",'
    if s.count(b) != 1: raise SystemExit('touch_layout：NPC 对话菜单的 basedOn 不是原样（原版布局变了？）')
    return s.replace(b, b + '\n    "fields": { "listEntryPadding": { "x": 0, "y": 124 } },'), 1
# ---------- 顶部首领血条：手柄版把进度条挂在目标头顶（TargetAttached），首领在画面边上 / 出画面时只剩顶部的名字词缀、看不到进度；
# 键鼠版的血条本来就固定在顶部中间。这里照键鼠版的位置把头顶那块（条框、金边装饰、进度条连同条里的名字）整个挪进顶部那块（ScreenAnchored），
# 顶部原来那行单独的名字去掉（条里已有）、词缀两行往下让；条右边加一个空字的 TouchBossPct，宿主每帧写「NN%」（dm_cursor.c ui_boss_pct_pump，
# 值取进度条控件自己的比例，与条完全一致）。头顶那块留空：手柄版代码按名字找控件，两块都得在 ----------
# ---------- Top boss health bar: the controller layout hangs the progress bar above the target's head (TargetAttached), so when the boss is at the edge of / outside the picture only the name and affixes at the top remain and the progress cannot be seen;
# the keyboard-and-mouse layout's health bar is fixed at the top center anyway. Here, following the keyboard-and-mouse position, the whole above-head block (bar frame, gold trim, progress bar together with the name inside the bar) is moved into the top block (ScreenAnchored),
# the separate name line that used to be at the top is removed (the bar already has it) and the two affix lines move down to make room; an empty-text TouchBossPct is added to the right of the bar, and the host writes 'NN%' every frame (dm_cursor.c ui_boss_pct_pump,
# the value is taken from the progress bar widget's own ratio, fully consistent with the bar). The above-head block is left empty: the controller-layout code finds widgets by name, so both blocks must exist ----------
def _boss_bar_top(text):
    j = json.loads(re.sub(r',(\s*[\]}])', r'\1', text))
    sa, ta = (next(c for c in j['children'] if c.get('name') == n) for n in ('ScreenAnchored', 'TargetAttached'))
    moved = {c['name']: c for c in ta['children']}
    if set(moved) != {'UniqueMonsterBackground', 'NormalMonsterBackground', 'Health'} or [c['name'] for c in sa['children']] != ['Name', 'Unique', 'Additional']:
        raise SystemExit('touch_layout：首领血条布局不是原样（%s / %s）' % (sorted(moved), [c['name'] for c in sa['children']]))
    dy = 86   # 进度条原在 y −86、条框在 −26：整体下移 86，条顶贴面板顶，条框与条的相对位置不变 | The progress bar was originally at y −86 and the bar frame at −26: shift everything down by 86 so the bar top touches the panel top, with the relative position of bar frame and bar unchanged
    for c in moved.values():
        r = c.setdefault('rect', {}) if 'rect' in c else c.setdefault('fields', {}).setdefault('rect', {}); r['y'] = r.get('y', 0) + dy
    # 进度段不用手柄版那张贴图、改成键鼠版原版的画法（HUDMonsterHealthHD.json 根下那份就没有贴图）：有贴图时进度段靠「设剪裁区」画出比例，
    # iPad 真全屏把这块面板上提到界面坐标的负 y 后，游戏给出的剪裁区是空的、整段不画（黑条）；没贴图时游戏直接按比例画一个矩形，不经剪裁区，上提后照常。
    # 宽 45 是键鼠版原值（条宽跟名字走，这个数是两侧留白）；高 57、下移 8 是按手柄版条框内沿量出来的（iPad 档逐行取色：进度段上下沿与原贴图画法同一行）
    # 颜色：不带贴图时进度段的颜色取 normalColor（不给就是绿的），游戏把它落到调色板里最近的一格。实测（iPhone 档截图取色）：
    # 0.18 → (47, 0, 0)、0.217 → (63, 18, 9) 偏棕、0.28 → (75, 24, 9) 偏棕、键鼠版原值 0.31 → (90, 0, 0) 偏亮；原贴图那段是 (63, 0, 0)。
    # 取色相不变的那一格 0.18（纯暗红、比原贴图略深）；transparency 用键鼠版原值 0.75
    # The progress segment does not use the controller layout's texture and switches to the keyboard-and-mouse version's original drawing (the copy at the root, HUDMonsterHealthHD.json, has no texture): with a texture the progress segment draws the ratio by "setting a scissor rectangle",
    # and once iPad true fullscreen lifts this panel to negative y in UI coordinates, the scissor rectangle the game gives is empty and the whole segment is not drawn (black bar); without a texture the game draws a rectangle directly by ratio, with no scissor rectangle, and it works as usual after lifting.
    # Width 45 is the keyboard-and-mouse version's original value (the bar width follows the name; this number is the padding on both sides); height 57 and the downshift of 8 were measured from the inner edge of the controller layout's bar frame (iPad profile, colors sampled row by row: the top and bottom edges of the progress segment are on the same rows as with the original textured drawing)
    # Color: without a texture the progress segment's color comes from normalColor (green if not given), and the game maps it to the nearest entry in the palette. Measured (colors sampled from iPhone profile screenshots):
    # 0.18 → (47, 0, 0), 0.217 → (63, 18, 9) brownish, 0.28 → (75, 24, 9) brownish, the keyboard-and-mouse version's original value 0.31 → (90, 0, 0) on the bright side; the original textured segment is (63, 0, 0).
    # The entry that keeps the hue, 0.18, is used (pure dark red, slightly darker than the original texture); transparency uses the keyboard-and-mouse version's original value 0.75
    f = moved['Health']['fields']; f.pop('filename'); f['rect'].update(width=45, height=57); f['rect']['y'] += 8; f['normalColor'] = {'r': 0.18, 'g': 0, 'b': 0}; f['transparency'] = 0.75
    moved['Health'].setdefault('children', []).append({'type': 'TextBoxWidget', 'name': 'TouchBossPct', 'fields': {
        'anchor': {'x': 1, 'y': 0.5}, 'rect': {'x': 24, 'y': 0},
        'style': {'pointSize': '$LargeFontSize', 'fontColor': {'r': 212, 'g': 196, 'b': 145, 'a': 255},   # 条里名字的颜色（真机 / 宿主截图取色），与名字同字号 | Color of the name inside the bar (color picked from real-device / host screenshots), same font size as the name
                  'alignment': {'h': 'left', 'v': 'center'}}}})
    uniq, add = sa['children'][1], sa['children'][2]
    uniq['fields']['rect'] = {'y': 100}; add['fields']['rect'] = {'y': 160}
    sa['children'] = [moved['UniqueMonsterBackground'], moved['NormalMonsterBackground'], moved['Health'], uniq, add]
    ta['children'] = []
    return json.dumps(j, ensure_ascii=False, indent=1), 1
PRESENT['controller/hudmonsterhealthhd.json'] = _boss_bar_top
def pad_boss_pct(text):   # 手柄版怪物血条 = 原版布局原样（条挂在目标头顶），只在进度条右边加同一个空字的 TouchBossPct，宿主每帧写「NN%」（同触屏版，dm_cursor.c ui_boss_pct_pump 按名字找，不分模式） | Controller layout monster health bar = the original layout as is (the bar hangs above the target's head), only adding the same empty-text TouchBossPct to the right of the progress bar, into which the host writes "NN%" every frame (same as the touch version; dm_cursor.c ui_boss_pct_pump finds it by name regardless of mode)
    j = json.loads(re.sub(r',(\s*[\]}])', r'\1', text))
    ta = next(c for c in j['children'] if c.get('name') == 'TargetAttached'); hp = next(c for c in ta['children'] if c.get('name') == 'Health')
    hp.setdefault('children', []).append({'type': 'TextBoxWidget', 'name': 'TouchBossPct', 'fields': {
        'anchor': {'x': 1, 'y': 0.5}, 'rect': {'x': 24, 'y': 0},
        'style': {'pointSize': '$LargeFontSize', 'fontColor': {'r': 212, 'g': 196, 'b': 145, 'a': 255}, 'alignment': {'h': 'left', 'v': 'center'}}}})
    return json.dumps(j, ensure_ascii=False, indent=1)
PRESENT['controller/console/switch/npcdialogpanelhd.json'] = _npc_dialog_touch


# 「重新绑定技能」框只留 1–5 列：第 6 列（ZR）已定成回城快捷键放在底部 UI，不再是技能位。两排各挪走一格。
# The 「重新绑定技能」(Press button to rebind skill) box keeps only columns 1–5: column 6 (ZR) has been fixed as the town portal shortcut placed in the bottom UI and is no longer a skill slot. One cell is moved away from each of the two rows.
def _bind_modal_5col(s):
    s, n = offscreen(s, types=(), names=('SkillButton6', 'AltSkillButtonRT'))
    if n != 2: raise SystemExit('touch_layout：绑定框第 6 列应为 2 格，找到 %d 格（原版布局变了？）' % n)
    return s, n
PRESENT['controller/buttonbindingmodalhd.json'] = _bind_modal_5col
def build_present(romfs, outdir):
    """局内呈现改动。返回 ({包内逻辑路径(小写): 本地文件}, 改了几处)；某文件一处没改到即报错。 | In-game presentation changes. Returns ({logical path in package (lowercase): local file}, number of places changed); it is an error if any file has no place changed."""
    repl = {}; total = 0
    for rel, fn in PRESENT.items():
        path, text = read_layout(romfs, rel); new, n = fn(text)
        if n == 0: raise SystemExit('touch_layout：%s 一处也没改到（原版布局变了？）' % rel)
        o = os.path.join(outdir, 'present_' + rel.replace('/', '_')); open(o, 'w', encoding='utf-8').write(new); repl[path.lower()] = o; total += n
    return repl, total

# ---------- 选项页：视频页签整页去掉（iPhone 自己能调亮度），其余页签里与触摸产品无关的项藏掉 ----------
# ---------- Options pages: the whole video tab page is removed (the iPhone can adjust brightness itself), and items in the other tabs unrelated to the touch product are hidden ----------
def _settings_tabs(text):
    """页签条去掉第一个（视频）：个数与四个并列数组各去首项；第四项「控制」换成「自定义」。 | Remove the first item (video) from the tab strip: drop the first entry from the count and from each of the four parallel arrays; the fourth item 「控制」(Controls) is replaced with 「自定义」(Custom)."""
    n = 0
    def drop_first(m):
        nonlocal n; items = [x for x in re.split(r',\s*', m.group(2).strip().rstrip(',')) if x.strip()]; n += 1
        return m.group(1) + ' ' + ', '.join(items[1:]) + ' ' + m.group(3)
    new = re.sub(r'("(?:inactiveFrames|activeFrames|textStrings)"\s*:\s*\[)([^\]]*)(\])', drop_first, text)
    new = re.sub(r'("tabMessages"\s*:\s*\[)([^\]]*)(\])', lambda m: m.group(1) + '\n' + ',\n'.join(x for x in [y.strip() for y in m.group(2).split(',')] if x and 'VideoOptionsPanel' not in x) + ',\n' + m.group(3), new)
    new, k = re.subn(r'"tabCount"\s*:\s*5', '"tabCount": 4', new)
    # 第四项原是「控制」（KeyBindingMenu，本平台不出现）：改成「自定义」，打开借来的视频页（见 _custom_page）
    # The fourth item was originally 「控制」(Controls) (KeyBindingMenu, which does not appear on this platform): changed to 「自定义」(Custom), opening the borrowed video page (see _custom_page)
    if new.count('"@Controls"') != 1 or new.count('SettingsPanelMessage:CheckChanges:KeyBindingMenu') != 1: raise SystemExit('touch_layout：页签条第四项不是原样「控制」，自定义页签没换')
    # 第三项「游戏设定」：原版英文串 Game Settings 在 266 宽的页签里折成两行、第二行被截成「Se…」（英文版审计）；触屏页签条换成我们表里的串（中文不变、英文 Settings）。手柄版页签条是原版，不动
    # Third item 「游戏设定」(Settings): the original English string Game Settings wraps to two lines in the 266-wide tab and the second line is truncated to "Se..." (English version audit); the touch tab bar uses the string from our table instead (Chinese unchanged, English Settings). The controller layout tab bar is the original and is left alone
    if new.count('"@GameSettingsTab"') != 1: raise SystemExit('touch_layout：页签条第三项不是原样「游戏设定」（原版布局变了？）')
    new = new.replace('"@GameSettingsTab"', '"@dmTabGameSettings"')
    new = new.replace('"@Controls"', '"@KeyBindingTypeCustom"').replace('SettingsPanelMessage:CheckChanges:KeyBindingMenu', 'SettingsPanelMessage:CheckChanges:VideoOptionsPanel')
    return new, n + k + 1
def _drop_rows(*rows):
    """把选项表里的整行（TableRowWidget "Row <名>"）删掉：表格按剩下的行重排，不留空行。每个名字必须恰好删到一行。 | Delete whole rows (TableRowWidget "Row <name>") from the options table: the table re-lays out the remaining rows and leaves no empty row. Each name must delete exactly one row."""
    def fn(text):
        n = 0
        for r in rows:
            ms = list(re.finditer(r'\{\s*"type"\s*:\s*"TableRowWidget"\s*,\s*"name"\s*:\s*"Row %s"' % re.escape(r), text))
            if len(ms) != 1: raise SystemExit('touch_layout：选项表里「Row %s」有 %d 行（应恰好 1 行，原版布局变了？）' % (r, len(ms)))
            e = _block_end(text, ms[0].start()) + 1
            while e < len(text) and text[e] in ' \t': e += 1
            if text[e:e + 1] == ',': e += 1
            text = text[:ms[0].start()] + text[e:]; n += 1
        # 选项的悬停说明（tooltipString）：手柄版一直给「光标所在行」出说明，浮在控件上方压住分节横线；触摸下没有悬停，整页去掉
        # Hover descriptions of options (tooltipString): the controller layout always shows the description for the 'row under the cursor', floating above the widgets and covering the section divider line; there is no hover under touch, so they are removed from the whole page
        text, k = re.subn(r'[ \t]*"tooltipString"\s*:\s*"[^"\n]*",?[ \t]*\r?\n', '', text)
        return text, n + k
    return fn
# 「自定义」页：选项面板第四个页签。布局覆盖层只能替换已有路径，所以借原版「视频」页
# （主机版 VideoOptionsPanelHD.json，触摸版已去掉视频页签）：页签条第四项（原「控制」= KeyBindingMenu，本平台不出现）改名「自定义」、打开 VideoOptionsPanel；
# 视频页的表格整个换成我们的行：分节标题 + 每个开关一行原版勾选框（照「游戏」页「自动拾取金币」那一行的字段）。控件名 TouchCustom_<键>，
# 宿主 dm_custom.c 读勾没勾、按应用 / 放弃 / 默认处理。勾选框的消息发一条无害的 ClosePanel（同「极乐世界」），游戏不认
# 「自定义」(Custom) page: the fourth tab of the options panel. A layout overlay can only replace existing paths, so it borrows the original game's 「视频」(Video) page
# (console version VideoOptionsPanelHD.json; the touch version has already removed the video tab): the fourth item of the tab strip (originally 「控制」(Controls) = KeyBindingMenu, which does not appear on this platform) is renamed 「自定义」(Custom) and opens VideoOptionsPanel;
# the video page's table is replaced entirely with our rows: section header + one original-game checkbox per switch (following the fields of the 「自动拾取金币」(Auto Gold Pickup) row on the 「游戏」(Game) page). Widget names are TouchCustom_<key>,
# and the host dm_custom.c reads whether each is checked and handles apply / discard / default. The checkbox message sends a harmless ClosePanel (same as 「极乐世界」(Elysium)), which the game does not recognize
_CUSTOM_KEYS = ('auto_mana', 'merc_pot', 'drop_light', 'map_mark', 'item_notes', 'icloud', 'pot_pick', 'drop_haptic', 'gem_pick', 'store_btn', 'eco_fps', 'q3d', 'loot_filter', 'auto_buff', 'touch_swap_above')
# 字在中英表（dm/i18n/ui_text.tsv 的 dmCustom<键名驼峰>），布局写 @键、跟原版语言走（英文版）；行高按中文行数分，英文必须同样行数（不然换语言排版变）
# The text lives in the Chinese-English string table (dmCustom<key name in camel case> in dm/i18n/ui_text.tsv); layouts write @key and follow the
# original game's language (English version); row height is assigned by the number of Chinese lines, and English must have the same number of lines
# (otherwise the layout changes with the language)
def _custom_key(key): return 'dmCustom' + ''.join(w.capitalize() for w in key.split('_'))   # auto_mana → dmCustomAutoMana（布局里 @键 遇下划线就断，键名不能带 _） | auto_mana → dmCustomAutoMana (in layouts @key stops at an underscore, so key names cannot contain _)
def _custom_label(key):
    import dm_i18n
    t = {k: (zh, en) for k, zh, en in dm_i18n.rows()}[_custom_key(key)]
    if t[0].count('\n') != t[1].count('\n'): raise SystemExit('touch_layout：自定义页 %s 的中英行数不同：%r / %r' % (key, t[0], t[1]))
    return t[0]
_CUSTOM_ROWS = tuple((k, '@' + _custom_key(k)) for k in _CUSTOM_KEYS)
# 视频页的类会把它不认识的表格行整行隐藏（控件树：我们的行 +0x50 = 0，只剩没有勾选框的标题行可见），所以各行不进表格；
# 但它不动滚动区里别的控件——各行包在一个容器 TouchCustomRows 里、挂在原版滚动区 ScrollView 下，内容多高原版就滚多高、滚出列表框的由原版裁掉
# （和别的选项页同一套滚动：拖动、滚动条、滚轮）。原先各行直接钉在面板上、不在滚动区里，列表框只有 1260 高，11 行把行高压到了 98。
# 尺寸取自同一控件树里表格排出来的值：行高 180；文字 1000×180；勾选框在行内 x 1315、70×70 垂直居中。标题行 180（下面的分隔金线按 180 行高摆）。
# 第一行是语言开关（宿主键表里也排第一），之后是 _CUSTOM_KEYS 各行；加行 = 往 _CUSTOM_KEYS 加键（宿主 dm_custom.c 的 k_keys 同序加）
# The video page's class hides any table row it does not recognize as a whole row (widget tree: our rows have +0x50 = 0, and only the header rows without checkboxes stay visible), so the rows do not go into the table;
# but it does not touch other widgets in the scroll area -- the rows are wrapped in a container TouchCustomRows hung under the original scroll area ScrollView, so the original game scrolls as far as the content is tall and clips whatever scrolls out of the list box
# (the same scrolling as the other option pages: drag, scrollbar, wheel). Previously the rows were pinned directly on the panel, outside the scroll area; the list box is only 1260 high, and 11 rows squeezed the row height down to 98.
# Dimensions are taken from the values the table lays out in the same widget tree: row height 180; text 1000×180; checkbox at x 1315 within the row, 70×70, vertically centered. Header row 180 (the gold divider line below it is placed for a 180 row height).
# The first row is the language switch (also first in the host's key table), followed by the rows of _CUSTOM_KEYS; adding a row = adding a key to _CUSTOM_KEYS (and to k_keys in the host's dm_custom.c in the same order)
_CUSTOM_TITLE_H, _CUSTOM_LINE_H = 180, 180
_CUSTOM_ALL = (('lang_en', '@dmCustomLang'),) + _CUSTOM_ROWS
_CUSTOM_TEST_ROWS = int(os.environ.get('DM_CUSTOM_TEST_ROWS', '0'))   # 测试（造现场）：末尾多摆这么多行假行，验更多行时的滚动；产品不设 | test (test setup): append this many dummy rows at the end, to verify scrolling with more rows; not set in the product
# 「3D 画质」（q3d）是单选的两项、每项下面一行说明小字，占两行高（_CUSTOM_Q3D_H）；其余各行一行高
# "3D Quality" (q3d) is a radio with two options, each with a line of small description text below, taking two rows of height (_CUSTOM_Q3D_H); every other row is one row high
_CUSTOM_Q3D_H = 2 * _CUSTOM_LINE_H
_CUSTOM_ROW_H = [(_CUSTOM_Q3D_H if k in ('q3d', 'touch_swap_above') else _CUSTOM_LINE_H) for k, _ in _CUSTOM_ALL] + [_CUSTOM_LINE_H] * _CUSTOM_TEST_ROWS
_CUSTOM_Y = [_CUSTOM_TITLE_H + sum(_CUSTOM_ROW_H[:i]) for i in range(len(_CUSTOM_ROW_H))]
_CUSTOM_H = _CUSTOM_TITLE_H + sum(_CUSTOM_ROW_H)
def _custom_toggle_row(i, key, label):
    y, h = _CUSTOM_Y[i], _CUSTOM_LINE_H
    return ('        { "type": "TextBoxWidget", "name": "TouchCustom_%s_Text", "fields": { "rect": { "x": 0, "y": %d, "width": 1000, "height": %d }, "text": "%s", "style": "$StyleSettingsText", "style/options/newlineHandling": "standard" } },\n'
            '        { "type": "ToggleButtonWidget", "name": "TouchCustom_%s", "fields": {\n'
            '            "rect": { "x": 1315, "y": %d, "width": 70, "height": 70 }, "sound": "cursor_menu_select_hd",\n'
            '            "focusIndicatorPadding": "$OptionsToggleFocusIndicatorPadding", "focusIndicatorFilename": "$OptionsToggleFocusIndicatorFilename", "focusIndicatorOffset": { "x": 0, "y": -4 },\n'
            '            "filename": "Lobby\\\\CreateGame\\\\CreateGame_AdvancedCheckbox",\n'
            '            "untoggledFrame": 0, "untoggledPressedFrame": 1, "untoggledHoveredFrame": 3, "untoggledDisabledFrame": 2,\n'
            '            "toggledFrame": 4, "toggledPressedFrame": 5, "toggledHoveredFrame": 6,\n'
            '            "onClickMessage": "PanelManager:ClosePanel:TouchCustom_%s" } },\n') % (key, y, h, label, key, y + (h - 70) // 2, key)
# 语言开关（第一行，普通行）：勾上 = 英文界面 + 英文语音，下次启动生效（宿主 dm_custom.c 的 lang_en、hle_fs.c settings_lang_apply）。
# 「重启游戏后生效」那句先放在屏外，勾选与当前语言不一致时宿主挪到这一行文字的下半（横坐标取标签的）
# Language switch (first row, an ordinary row): checked = English UI + English voice, effective at the next launch (lang_en in the host's dm_custom.c, settings_lang_apply in hle_fs.c).
# The line 「重启游戏后生效」(Takes effect after restarting the game) is first placed off screen; when the checkbox does not match the current language the host moves it to the lower half of this row's text (x taken from the label)
_CUSTOM_LANG_HINT = ''.join('        { "type": "TextBoxWidget", "name": "TouchCustomLangHint%s", "fields": { "rect": { "x": 99999, "y": %d, "width": 1400, "height": 60 }, "text": "@dmCustomLangHint%s", "style": "$StyleSettingsText", "style/fontFace": "BlizzardGlobal" } },\n'
                            % (n, _CUSTOM_Y[0] + 112, n) for n in ('Zh', 'En'))   # 两句各一个控件，宿主按选中的语言摆其中一句 | One control per sentence; the host places one of them according to the chosen language
# 语言那一行是并排的两个选项「中文 / English」（单选：宿主 dm_custom.c 让两个勾选框互斥，存的仍是 lang_en 一个键）。
# 这一行的字两种界面都要写汉字：英文界面缺省的 Formal 字体没有汉字（缺字画不出），所以这几个文字控件指定原版的全球字体 BlizzardGlobal
# （原版标题页、队伍名等同样按控件指定它）。English 的勾选框留在各行勾选框那一列，「中文」的在它左边
# The language row is two options side by side, 「中文 / English」 (single choice: the host dm_custom.c makes the two checkboxes mutually exclusive, and what is stored is still the one key lang_en).
# The text of this row has to show Chinese characters in both UIs: the default Formal font of the English UI has no Chinese characters (missing glyphs cannot be drawn), so these text controls specify the original global font BlizzardGlobal
# (the original title page, party names and so on specify it per control in the same way). The checkbox of English stays in the column where every row has its checkbox; the one of 「中文」 is to its left
_LANG_ZH_X, _LANG_EN_X = (770, 905), (1100, 1315)   # （文字 x，勾选框 x） | (text x, checkbox x)
def _custom_lang_row():
    y, h = _CUSTOM_Y[0], 112; cy = y + (h - 70) // 2   # 标签、两个选项排在这一行的上 112，下面 68 留给提示那一句 | The label and the two options go in the top 112 of this row; the bottom 68 is left for the hint sentence
    def text(name, x, w, key): return ('        { "type": "TextBoxWidget", "name": "%s", "fields": { "rect": { "x": %d, "y": %d, "width": %d, "height": %d }, "text": "@%s", "style": "$StyleSettingsText", "style/fontFace": "BlizzardGlobal" } },\n' % (name, x, y, w, h, key))
    def box(name, x): return ('        { "type": "ToggleButtonWidget", "name": "%s", "fields": {\n'
            '            "rect": { "x": %d, "y": %d, "width": 70, "height": 70 }, "sound": "cursor_menu_select_hd",\n'
            '            "focusIndicatorPadding": "$OptionsToggleFocusIndicatorPadding", "focusIndicatorFilename": "$OptionsToggleFocusIndicatorFilename", "focusIndicatorOffset": { "x": 0, "y": -4 },\n'
            '            "filename": "Lobby\\\\CreateGame\\\\CreateGame_AdvancedCheckbox",\n'
            '            "untoggledFrame": 0, "untoggledPressedFrame": 1, "untoggledHoveredFrame": 3, "untoggledDisabledFrame": 2,\n'
            '            "toggledFrame": 4, "toggledPressedFrame": 5, "toggledHoveredFrame": 6,\n'
            '            "onClickMessage": "PanelManager:ClosePanel:%s" } },\n') % (name, x, cy, name)
    return (text('TouchCustom_lang_en_Text', 0, 700, 'dmCustomLang')
            + text('TouchCustomLangZh_Text', _LANG_ZH_X[0], _LANG_ZH_X[1] - _LANG_ZH_X[0] - 10, 'dmCustomLangZh') + box('TouchCustom_lang_zh', _LANG_ZH_X[1])
            + text('TouchCustomLangEn_Text', _LANG_EN_X[0], _LANG_EN_X[1] - _LANG_EN_X[0] - 10, 'dmCustomLangEnOpt') + box('TouchCustom_lang_en', _LANG_EN_X[1]))
# 「3D 画质」（流畅优先（缺省，掉帧自动降 3D 比例）/ 画质优先（不自动降））：上下两项单选，每项标题下一行说明小字。
# 存的是一个键 q3d（1 = 画质优先）：「画质优先」的勾选框叫 TouchCustom_q3d、「流畅优先」的叫 TouchCustom_q3d_smooth（它的反面），宿主 dm_custom.c 让两框互斥（同语言行）。
# TouchCustom_q3d_Text 是空字、高度盖住两项的框：手柄选中框按「文字框与勾选框的上下沿取并」框住整行（dm_custom.c），所以用它把两项一起框进去
# "3D Quality" (Smoothness first (default, automatically lowers the 3D scale on frame drops) / Quality first (no automatic lowering)): a stacked two-option radio, each option with a line of small description text below its title.
# It stores a single key q3d (1 = quality first): the "Quality first" checkbox is TouchCustom_q3d and the "Smoothness first" one is TouchCustom_q3d_smooth (its inverse); the host's dm_custom.c makes the two boxes mutually exclusive (like the language row).
# TouchCustom_q3d_Text is an empty-text box tall enough to cover both options: the controller selection frame wraps the whole row by "the union of the top/bottom edges of the text box and the checkbox" (dm_custom.c), so it is used to frame both options together
def _custom_q3d_row(i, setting="q3d"):
    prefix = "TouchCustom_" + setting
    y = _CUSTOM_Y[i]; h = _CUSTOM_LINE_H
    def text(name, yy, hh, key, small): return ('        { "type": "TextBoxWidget", "name": "%s", "fields": { "rect": { "x": 0, "y": %d, "width": 1200, "height": %d }, "text": "@%s", "style": %s } },\n'
                                                % (name, yy, hh, key, '{ "fontColor": "$FontColorGray", "pointSize": 30, "alignment": { "h": "left", "v": "center" } }' if small else '"$StyleSettingsText"'))
    def box(name, yy): return ('        { "type": "ToggleButtonWidget", "name": "%s", "fields": {\n'
            '            "rect": { "x": 1315, "y": %d, "width": 70, "height": 70 }, "sound": "cursor_menu_select_hd",\n'
            '            "focusIndicatorPadding": "$OptionsToggleFocusIndicatorPadding", "focusIndicatorFilename": "$OptionsToggleFocusIndicatorFilename", "focusIndicatorOffset": { "x": 0, "y": -4 },\n'
            '            "filename": "Lobby\\\\CreateGame\\\\CreateGame_AdvancedCheckbox",\n'
            '            "untoggledFrame": 0, "untoggledPressedFrame": 1, "untoggledHoveredFrame": 3, "untoggledDisabledFrame": 2,\n'
            '            "toggledFrame": 4, "toggledPressedFrame": 5, "toggledHoveredFrame": 6,\n'
            '            "onClickMessage": "PanelManager:ClosePanel:%s" } },\n') % (name, yy, name)
    out = '        { "type": "TextBoxWidget", "name": "%s_Text", "fields": { "rect": { "x": 0, "y": %d, "width": 1000, "height": %d }, "text": "" } },\n' % (prefix, y, _CUSTOM_Q3D_H)
    choices = (('dmCustomQ3dSmooth', prefix + '_smooth'), ('dmCustomQ3dQuality', prefix)) if setting == 'q3d' else (('dmCustomTouchSwapNear', prefix + '_near'), ('dmCustomTouchSwapFar', prefix))
    for k, (key, nm) in enumerate(choices):
        yy = y + k * h
        out += text(nm + '_Label', yy + 10, 100, key, False) + text(nm + '_Hint', yy + 100, 70, key + 'Hint', True) + box(nm, yy + 15)
    return out
def _custom_page(text):
    if '"name": "VideoOptionsPanel"' not in text or '"name": "OptionsTable"' not in text: raise SystemExit('touch_layout：主机版视频页不是原样（原版布局变了？），自定义页没换')
    title = ('{ "type": "TableRowWidget", "name": "Row TouchCustomTitle", "children": [\n'
             '    { "type": "TextBoxWidget", "name": "TouchCustomTitle", "fields": { "text": "@KeyBindingTypeCustom", "style": "$StyleSettingsTitle" }, "children": [\n'
             '        { "type": "ImageWidget", "name": "Divider", "fields": { "rect": "$OptionsDividerRect", "filename": "PauseMenu\\\\Divider" } },\n'
             '    ] },\n'
             '] },\n')
    out = ('{\n    "basedOn": "VideoOptionsPanelHD.json",\n    "type": "VideoOptionsPanel", "name": "VideoOptionsPanel",\n    "children": [\n'
           '        { "type": "ImageWidget", "name": "ScrollBarBackground", "children": [ { "type": "ScrollControllerWidget", "name": "ScrollController" }, ] },\n'
           '        { "type": "ScrollViewWidget", "name": "ScrollView", "children": [\n'
           '            { "type": "TableWidget", "name": "OptionsTable", "fields": { "columns": [ "$OptionsTableColumn1", "$OptionsTableColumn2" ], "rowHeight": %d }, "children": [\n' % _CUSTOM_TITLE_H
           + title + '            ] },\n'
           + '            { "type": "Widget", "name": "TouchCustomRows", "fields": { "rect": { "x": 0, "y": 0, "width": 1500, "height": %d } }, "children": [\n' % _CUSTOM_H
           + _custom_lang_row() + ''.join((_custom_q3d_row(i, k) if k in ('q3d', 'touch_swap_above') else _custom_toggle_row(i, k, l)) for i, (k, l) in enumerate(_CUSTOM_ALL) if i) + _CUSTOM_LANG_HINT
           + ''.join('        { "type": "TextBoxWidget", "name": "TouchCustomTest%d_Text", "fields": { "rect": { "x": 0, "y": %d, "width": 1000, "height": %d }, "text": "@KeyBindingTypeCustom", "style": "$StyleSettingsText" } },\n'
                     % (n, _CUSTOM_Y[len(_CUSTOM_ALL) + n], _CUSTOM_LINE_H) for n in range(_CUSTOM_TEST_ROWS)) +
           # 手柄模式的选中框（同符文之语清单 TouchRwSel）：框住十字键选中的那一行，宿主摆（dm_custom.c）；触屏模式一直在屏外（只把 x 挪出去、y 留在内容范围内：滚动区按子控件的范围算内容高度，y 放到 99999 会把可滚范围撑到十万）。和各行在同一个容器里，跟着滚
           # Selection frame in controller mode (same as TouchRwSel of the runeword list): frames the row selected with the D-pad, positioned by the host (dm_custom.c); always off-screen in touch mode (only x is moved out while y stays within the content range: the scroll area computes the content height from the extent of its child widgets, and a y of 99999 would stretch the scrollable range to a hundred thousand). It is in the same container as the rows and scrolls with them
           '        { "type": "NineTileImageWidget", "name": "TouchCustomSel", "fields": { "rect": { "x": 99999, "y": 0, "width": 1000, "height": 100 }, "backgroundColor": [ 0, 0, 0, 0 ], '
           '"filename": "Controller/PANEL/ControllerSelector/ControllerSelectorBorder", "tileScale": 0.5 } },\n'
           '            ] },\n        ] },\n'
           '    ]\n}\n')
    return out, len(_CUSTOM_ALL) + 2
PRESENT['controller/console/videooptionspanelhd.json'] = _custom_page
# 手柄版选项页：原版页签条不动，只把第一项「视频」换成「自定义」——它打开的视频页就是上面那份自定义页（两种模式同一份内容）
# Controller options page: the original tab bar is untouched, only the first item "Video" is replaced by "Custom"; the video page it opens is the Custom
# page above (same content in both modes)
def pad_settings(s): return _sub1(s, '"@OptionsMenuVideo"', '"@KeyBindingTypeCustom"')   # 原版串：自定义 / Custom（跟游戏语言走） | original string: "Custom" in Chinese / English (follows the game language)
def pad_custom_page(s): return _custom_page(s)[0]
# 挂进 PRESENT：build_ingame 对同一份文件（往选项页插「应用 / 默认 / 放弃」）会先套这里的变换再插按钮，不会出两份覆盖互相盖掉
# Hooked into PRESENT: for the same file (inserting 「应用 / 默认 / 放弃」(Apply / Default / Discard) into the options page) build_ingame applies the transform here first and then inserts the buttons, so two overlays that overwrite each other are not produced
PRESENT['controller/console/settingspanelhd.json'] = _settings_tabs
# 选项页底部的手柄提示条（「默认」「返回」两段说明字；键图标已透明）：局内被外框底带挡着看不出，前端选项页没有外框、字露在三个按钮下面——挪走
# The controller button legend at the bottom of the options page (the two caption texts 「默认」(Default) and 「返回」(Back); the key icons are already transparent): in-game it is hidden by the bottom band of the outer frame, but the front-end options page has no outer frame and the text shows below the three buttons -- moved away
def _settings_no_legend(s):
    s, n = offscreen(s, types=('ButtonLegendWidget',))
    if n != 1: raise SystemExit('touch_layout：选项页提示条应为 1 个，找到 %d 个' % n)
    return s, n
# 确认框正文：统一大字号（40）后正文一行约 87 单位高，而确认框的底图是固定大小的，框顶内沿到按钮上沿只有约 265 单位 = 三行；
# 专家模式警告是四行，最后一行被「好 / 取消」压住。只加高正文区不够（标题顶出框上沿、第四行仍被压）；原版更大的那张底图也只多 62 单位。
# 定法（与「同页不要两种大小」的原则有出入，属例外）：这个框的正文字号 40 → 30（一行约 65 单位，四行 260），
# 正文区改成框顶内沿到按钮上沿的整段（y 28 高 264，仍垂直居中）。底图与按钮不动。手柄版确认框、删除角色确认框继承这一份；单按钮的提示框同样处理。
# Confirmation box body: after unifying on the large font size (40) one body line is about 87 units high, while the confirmation box background image has a fixed size, with only about 265 units = three lines from the inner top edge of the box to the top edge of the buttons;
# the hardcore mode warning is four lines, and the last line is covered by 「好 / 取消」(OK / Cancel). Making only the body area taller is not enough (the title is pushed past the top edge of the box and the fourth line is still covered); the original game's larger background image also adds only 62 units.
# Decision (it deviates from the 'no two sizes on one page' principle and is an exception): this box's body font size 40 → 30 (one line about 65 units, four lines 260),
# and the body area becomes the whole span from the inner top edge of the box to the top edge of the buttons (y 28, height 264, still vertically centered). Background image and buttons unchanged. The controller-layout confirmation box and the delete-character confirmation box inherit this one; the single-button prompt box is handled the same way.
def _modal_prompt(old, new):
    def fn(s):
        a = '"style": "$StyleModalDialogDescription"'
        if s.count(old) != 1 or s.count(a) != 1: raise SystemExit('touch_layout：确认框正文区 %s 不是原样（原版布局变了？）' % old)
        s = s.replace(old, new)
        return s.replace(a, '"style": { "fontColor": "$ModalDescriptionTextColor", "alignment": { "h": "center", "v": "center" }, "options": { "lineWrap": true, "newlineHandling": "standard" }, "pointSize": 30, "spacing": "$ReducedSpacing" }'), 2
    return fn
PRESENT['confirmationmodalhd.json'] = _modal_prompt('"rect": { "x": 78, "y": 85, "width": 1076, "height": 170 },', '"rect": { "x": 78, "y": 28, "width": 1076, "height": 264 },')
PRESENT['dismissablemodalhd.json'] = _modal_prompt('"rect": { "x": 140, "y": 70, "width": 966, "height": 212 },', '"rect": { "x": 140, "y": 28, "width": 966, "height": 264 },')
PRESENT['controller/settingspanelhd.json'] = _settings_no_legend
# 去掉的项：语言只留简中、手柄专用、联网、离线人数（只留底栏难度格一个入口）
# 英文版：r34 起原版切语言不再剪；「语言」分节与「文字语言」一行仍藏，等我们自己的文字进中英表（第二阶段）一起放出（切到英文看到一半中文不行）
# Removed items: language keeps only Simplified Chinese, controller-only items, online, offline player count (only the difficulty cell in the bottom bar remains as the single entry)
# English version: since r34 the original game's language switch is no longer cut; the 「语言」(Language) section and the 「文字语言」(Text Language) row stay hidden until our own text enters the Chinese-English table (phase two) and are released together (switching to English and seeing half Chinese is not acceptable)
PRESENT['soundoptionspanelhd.json'] = _drop_rows('LanguageOptions', 'VoiceLanguage', 'TextLanguage', 'Controller Feedback')
PRESENT['gameplayoptionspanelhd.json'] = _drop_rows('AutoPartyInvite', 'ItemHotkeyTooltipAppender', 'ChatFontSize', 'ChatBackground', 'Controller Options', 'ControllerCursorSpeed',
                                                    'ControllerWeaponSwapSeparateBindings', 'ControllerRumbleEnabled', 'ControllerSwapThumbsticks', 'ShowParty', 'ShowNames', 'ShowRoomName',
                                                    # 大字体模式（原版低视力档）：一应用游戏就换成另一份界面布局档（_profileLV）整套重建，那份档我们没做过触摸布局——
                                                    # 点「应用」后面板只剩一块空石板、点哪都没反应。我们已自带大字号，这一项去掉
                                                    # Large font mode (the original game's low-vision profile): as soon as it is applied the game switches to another UI layout profile (_profileLV) and rebuilds everything, and we never made a touch layout for that profile --
                                                    # after tapping 「应用」(Apply) the panel is left as a blank stone slab and nothing responds to taps. We already ship large font sizes, so this item is removed
                                                    'LowVisionMode',
                                                    # 色盲「校准」：点进去是「色盲校准」整页，页外 / 右上角 / 「关闭」/「保存并关闭」都点不动，只能强退。
                                                    # 这一行只有「色盲」两个字加这个按钮，整行去掉（色盲的模式与程度都在那一页里，设置表里没有别的色盲行）
                                                    # Color Blind 「校准」(Calibrate): tapping it opens the full-page 「色盲校准」(Color Blind Calibration), where outside the page / the top-right corner / 「关闭」(Close) / 「保存并关闭」(Save and Close) all do not respond to taps, so the only way out is a force quit.
                                                    # This row has only the two characters 「色盲」(Color Blind) plus this button, so the whole row is removed (colorblind mode and strength are both on that page, and the settings table has no other colorblind row)
                                                    'ColorBlindCalibration')
PRESENT['onlinegameoptionspanelhd.json'] = _drop_rows('OfflineDifficultyScaling', 'OfflineSettingsEffectNote')
# 手柄模式的三份选项页（放在原路径，手柄模式加载；触屏那份由生成器挪到触屏这边）：原版删去两种模式都不支持的行——语言、联机、聊天、键鼠热键提示、离线人数档，
# 和大字体模式（一应用就换成另一套布局档，再碰屏切回触屏会坏）。手柄模式比触屏多留的行见下
# The three options pages for controller mode (placed at the original path and loaded by controller mode; the touch copy is moved to the touch side by the generator): the original with the rows that neither mode supports removed -- language, online, chat, keyboard-and-mouse hotkey hints, offline /players setting,
# and large font mode (applying it swaps in another set of layout profiles, and touching the screen to switch back to touch then breaks). The rows controller mode keeps beyond touch are listed below
PAD_ROOT = {
    'soundoptionspanelhd.json': _drop_rows('LanguageOptions', 'VoiceLanguage', 'TextLanguage', 'Controller Feedback'),
    # 手柄设置里只恢复实跑证实生效的：「互换左右摇杆」（开了之后左摇杆不走路、右摇杆走路）；色盲校准（手柄能进能出）。
    # 不恢复：震动（宿主不支持震动，hle_hid.c 里是空实现）、手柄反馈与光标速度（改了值、没量出效果：面板里手柄光标是按格吸附的）、切换武器分开绑定（没验）
    # Of the controller settings only those confirmed effective in actual runs are restored: 「互换左右摇杆」(Swap Sticks) (when on, the left stick no longer walks and the right stick does); colorblind calibration (the controller can get in and out).
    # Not restored: vibration (the host does not support vibration; it is an empty implementation in hle_hid.c), controller feedback and cursor speed (the value changed but no effect was measured: in panels the controller cursor snaps cell by cell), separate binding for weapon swap (not verified)
    'gameplayoptionspanelhd.json': _drop_rows('AutoPartyInvite', 'ItemHotkeyTooltipAppender', 'ChatFontSize', 'ChatBackground', 'ShowParty', 'ShowNames', 'ShowRoomName', 'LowVisionMode',
                                              'ControllerCursorSpeed', 'ControllerWeaponSwapSeparateBindings', 'ControllerRumbleEnabled'),
    'onlinegameoptionspanelhd.json': _drop_rows('OfflineDifficultyScaling', 'OfflineSettingsEffectNote'),
}
def build_pad_root(romfs, outdir):
    repl = {}
    for rel, fn in PAD_ROOT.items():
        path, text = read_layout(romfs, rel); new, n = fn(text)
        if n == 0: raise SystemExit('touch_layout：手柄版 %s 一处也没改到（原版布局变了？）' % rel)
        o = os.path.join(outdir, 'padroot_' + rel.replace('/', '_')); open(o, 'w', encoding='utf-8').write(new); repl[path.lower()] = o
    return repl
# 选项页的行距：原版每行 100 单位 = 3.9 毫米，相邻两行必然点串（热区审计）；改成 180 单位 = 7 毫米。列表本来就滚动，行内控件位置不动。
# Row pitch of the options pages: the original game uses 100 units per row = 3.9 mm, so adjacent rows are inevitably mis-tapped (hit-area audit); changed to 180 units = 7 mm. The list scrolls anyway, and widget positions within a row are unchanged.
def _row_height(fn):
    def g(text):
        text, n = fn(text); a = '"rowHeight": "$OptionsTableRowHeight"'
        if a not in text: raise SystemExit('touch_layout：选项表的 rowHeight 不是原样（原版布局变了？）')
        return text.replace(a, '"rowHeight": 180'), n + text.count(a)
    return g
for _k in ('soundoptionspanelhd.json', 'gameplayoptionspanelhd.json', 'onlinegameoptionspanelhd.json'): PRESENT[_k] = _row_height(PRESENT[_k])

def _widget_block(s, name):
    m = re.search(r'\{\s*"type"\s*:\s*"[A-Za-z]+"\s*,\s*"name"\s*:\s*"%s"' % re.escape(name), s)
    if not m: raise SystemExit('touch_layout：键鼠版布局里没有控件 ' + name)
    return s[m.start():_block_end(s, m.start()) + 1]

# 手柄版商人页的「刷新货架」钮（不占键，光标移上去按 A，同原版手柄界面点别的按钮）：就是上面触屏版那颗——同一位置、同一张原版图、同一条消息，
# 两个名字各挂一份的原因也同上（会修理的商人显示 button_repair、不修理的显示 button_blank）。手柄版不搬「全部修理」（原版手柄是按住左摇杆）、赌博页刷新（原版手柄有自己的键）
# The "refresh the shop stock" button on the controller vendor page (takes no key; move the cursor onto it and press A, like clicking any other
# button in the original controller UI): it is the very same button as the touch one above -- same position, same original image, same message,
# and it is attached once under each of two names for the same reason as above (vendors that repair show button_repair, those that do not show
# button_blank). The controller layout does not carry over "Repair All" (hold the left stick on the original controller scheme) or the gamble
# page refresh (the original controller scheme has its own key)
PAD_VENDOR = {'controller/vendorpanellayouthd.json': ('vendorpanellayouthd.json', [INGAME['controller/vendorpanellayouthd.json'][1][0], INGAME['controller/vendorpanellayouthd.json'][1][1]])}
def build_ingame(romfs, outdir, spec=None, present=True):   # present=False：不套触屏的摆放变换（给手柄版布局插按钮用） | present=False: do not apply the touch placement transform (used for inserting buttons into the controller layout)
    """局内面板插回原生按钮。返回 ({包内逻辑路径(小写): 本地文件}, 插了几个)。 | Insert native buttons back into the in-game panels. Returns ({logical path in package (lowercase): local file}, number inserted)."""
    repl = {}; total = 0
    for rel, (src, items) in (INGAME if spec is None else spec).items():
        path, text = read_layout(romfs, rel); _, base = read_layout(romfs, src); add = ''
        if present and rel in PRESENT: text = PRESENT[rel](text)[0]   # 同一份布局在 PRESENT 里也改了摆放：先套那里的变换 | The same layout also has its placement changed in PRESENT: apply that transform first
        for name, new_name, override, *alt in items:
            b = _widget_block(read_layout(romfs, alt[0])[1] if alt else base, name)   # 第 4 项 = 这个控件另取自哪份键鼠版布局（资料片版继承经典版，按钮写在经典版那份里） | Item 4 = which other keyboard-and-mouse layout this widget is taken from (the expansion version inherits the classic version, and the button is written in the classic one)
            if re.search(r'"name"\s*:\s*"%s"' % re.escape(new_name or name), text): raise SystemExit('touch_layout：%s 里已有同名控件 %s' % (rel, new_name or name))
            if new_name: b = b.replace('"name": "%s"' % name, '"name": "%s"' % new_name, 1)
            if override:   # 覆盖 fields 里的若干键：先删原键再在 fields 开头加 | Override some keys in fields: delete the original key first, then add it at the start of fields
                for k in re.findall(r'"([A-Za-z/]+)"\s*:', override): b = re.sub(r'\n\s*"%s"\s*:\s*(\{[^{}]*\}|"[^"]*"|[^,\n]+),?' % re.escape(k), '', b, count=1)
                f = re.search(r'"fields"\s*:\s*\{', b); b = b[:f.end()] + '\n                ' + override.rstrip(',') + ',' + b[f.end():]
            add += '        ' + b + ',\n'; total += 1
        end = text.rstrip().rstrip('}').rstrip(); assert end.endswith(']'), rel        # 根对象的 children 数组必须是最后一项 | The root object's children array must be the last item
        at = len(end) - 1; head = text[:at].rstrip(); new = head + ('' if head.endswith((',', '[')) else ',') + '\n' + add + '    ' + text[at:]
        import json as _json
        try: _json.loads(re.sub(r',(\s*[}\]])', r'\1', re.sub(r'(?m)^\s*//[^\n]*|(?<=[,{\[\s])//[^\n]*', '', re.sub(r'/\*[\s\S]*?\*/', '', new))))   # 原版布局里也有 /* */ 块注释（手柄版佣兵页），校验前一并去掉 | The original game's layouts also contain /* */ block comments (controller-layout mercenary page); strip them too before validating
        except ValueError as e: raise SystemExit('touch_layout：%s 插回后不是合法布局（%s）——游戏会整个面板不显示且不报错' % (rel, e))
        o = os.path.join(outdir, 'ingame_' + rel.replace('/', '_')); open(o, 'w', encoding='utf-8').write(new); repl[path.lower()] = o
    return repl, total

if __name__ == '__main__':
    import sys, tempfile
    r, n = build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp()); print('前端布局 %d 个，挪走键位提示 %d 处' % (len(r), n))
    for k, v in r.items(): print(k, '←', v)
