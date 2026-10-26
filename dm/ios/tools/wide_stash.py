#!/usr/bin/env python3
# 储藏箱加宽（编年史仓库第 1 步）：个人 / 共享页 10 → 14 列。
# 数据：两份 inventory.bin（excel/ 与 excel/base/）的大储藏箱页两行（第 10、25 行，+16 字节 = 列数）10 → 14，个人页与共享页都用这两行；服务端按它判格子是否合法（实测 x=12 放得进、存读档不丢）。
# 画面：储藏箱底图（个人 / 共享 / 通用）在格子区插 4 列、下段金币框居中；宝石 / 材料 / 符文三页的底图两侧本来透明，左右各补 98 像素透明 = 居中。
# 外框是 14 个面板共用的一张图（ControllerOverlay 的 Background），不改它，另加一个 WideBackground（原双面板帧左半插宽 196 像素），
# 平时放屏幕外，储藏箱开着时由宿主挪进来（dm_cursor.c ui_widestash_pump）。贴图都由 gen_widestash_art.py 预先做成原包同款 DXT5 块线性，放在 NativeHUD/widestash/，这里只拷。
# 面板矩形的左移 / 加宽、物品栏右移、标题条也在宿主做（各档位的面板矩形不同，不写死在这里）。
# 1 像素 = 2 布局单位；加宽 392 单位 = 196 像素 = 4 格。
# Stash widening (chronicle vault step 1): personal / shared pages 10 → 14 columns.
# Data: in the two inventory.bin copies (excel/ and excel/base/) the two large-stash page rows (rows 10 and 25, +16 bytes = column count) go 10 → 14, and both the personal and shared pages use these two rows; the server uses it to judge whether a cell is legal (measured: an item fits at x=12 and is not lost across save and load).
# Picture: the stash background images (personal / shared / generic) get 4 columns inserted in the cell area and the gold box in the lower section centered; the background images of the three gem / material / rune pages are transparent on both sides anyway, so 98 transparent pixels are added on each side = centered.
# The outer frame is one image shared by 14 panels (the Background of ControllerOverlay); it is not changed, and a separate WideBackground is added (the left half of the original double-panel frame widened by inserting 196 pixels),
# normally kept off screen and moved in by the host while the stash is open (dm_cursor.c ui_widestash_pump). The textures are all prepared in advance by gen_widestash_art.py as DXT5 block-linear, the same kind as the original package, and placed in NativeHUD/widestash/; here they are only copied.
# Shifting left / widening of the panel rectangle, shifting the inventory right and the title bar are also done on the host (panel rectangles differ per profile and are not hard-coded here).
# 1 pixel = 2 layout units; widening of 392 units = 196 pixels = 4 cells.
import json, os, re, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import touch_layout

HERE = os.path.dirname(os.path.abspath(__file__)); ART = __import__('gen_paths').gen_dir('NativeHUD', 'widestash')   # 构建时由 gen_widestash_art.py / gen_vault_cover.py 生成 | Generated at build time by gen_widestash_art.py / gen_vault_cover.py
COLS = 14; EX = 392   # 加宽布局单位 | Widening in layout units
BANK = 'controller/bankexpansionlayouthd.json'; OVERLAY = 'controller/controlleroverlayhd.json'
SPRITES = {n: 'controller/panel/stash/v2/' + n.lower() for n in ('StashPanelBG', 'StashPanelBGPersonal', 'StashPanelBGShared')}   # NativeHUD/widestash/<名>_wide.sprite → 原路径 | NativeHUD/widestash/<name>_wide.sprite → original path
SPRITES.update({n: 'controller/panel/stash/v2/additionalstash/' + n.lower() for n in ('AdvancedStash_Gems_Console', 'AdvancedStash_Consumables_Console', 'AdvancedStash_Runes_Console')})
WIDE_FRAME = 'controller/panel/zswide/mainpanelbg_wide'   # 新增路径（casc_overlay 要求新增文件落在新子目录里） | Newly added path (casc_overlay requires newly added files to land in a new subdirectory)

def _entry(romfs, suffix):
    for line in open(os.path.join(romfs, 'data', 'build_manifest.psv'), encoding='utf-8'):
        f = line.rstrip('\n').split('|')
        if len(f) >= 11 and not line.startswith('#') and f[9].lower().endswith(suffix): return f[9].split(':', 1)[1].lower(), touch_layout._read_entry(romfs, f)
    raise SystemExit('wide_stash：包里没有 ' + suffix)

def _shift(s, name, dx, start=0):
    """控件 name 的 rect x 加 dx（按文本改：布局带注释与尾逗号）。返回新文本。 | Add dx to the rect x of the widget named name (edited as text: layouts carry comments and trailing commas). Returns the new text."""
    m = re.search(r'"name"\s*:\s*"%s"' % re.escape(name), s[start:])
    if not m: raise SystemExit('wide_stash：储藏箱布局里没有控件 ' + name)
    i = start + m.end(); r = re.compile(r'"rect"\s*:\s*\{\s*"x"\s*:\s*(-?\d+)').search(s, i)
    if not r or s.find('"name"', i, r.start()) != -1: raise SystemExit('wide_stash：控件 %s 的 rect 不在它自己的字段里' % name)
    return s[:r.start(1)] + str(int(r.group(1)) + dx) + s[r.end(1):]

def _container_x(s, name, dx):
    """无 rect 的容器（高级页三个）加 rect；有 rect 的（魔盒）加 dx。 | Containers without a rect (the three on the advanced pages) get a rect added; those with a rect (the cube) get dx added."""
    m = re.search(r'"name"\s*:\s*"%s"' % re.escape(name), s); i = m.end()
    nxt = s.find('"children"', i); f = s.find('"fields"', i)
    if f != -1 and f < nxt: return _shift(s, name, dx)
    return s[:i] + ',\n            "fields": { "rect": { "x": %d, "y": 0 } }' % dx + s[i:]

# 珍宝匣：宝石 / 符文 / 材料三页的格位合到宝石页那一页（底图 gen_trove_art.py）。原版按物品码认格位（槽控件字段 itemCode），与控件挂在哪个容器、摆在哪里无关（实测）：
# 三个容器里的槽控件全部拿掉，按珍宝匣的几何重写进宝石容器；材料、符文两个容器留空壳（原版按名字找它们）
# Trove: the slots of the gems / runes / materials pages are merged into the gems page (background gen_trove_art.py). The game identifies slots by item code (slot widget field itemCode), regardless of which container the widget is in or where it sits (measured):
# all slot widgets in the three containers are removed and rewritten into the gem container with the Trove geometry; the materials and runes containers are left as empty shells (the game looks them up by name)
def trove_slots(s):
    import gen_trove_art as T
    pat = re.compile(r'\{\s*"type"\s*:\s*"AdvancedStashSlotWidget"\s*,\s*"name"\s*:\s*"(\w+)"\s*,\s*"fields"\s*:\s*\{\s*"rect"\s*:\s*\{[^}]*\}\s*,\s*"itemCode"\s*:\s*"(\w+)"\s*,?\s*\}\s*,?\s*\}\s*,?')
    codes = [m.group(2) for m in pat.finditer(s)]; want = T.slots()
    if sorted(codes) != sorted(want) or any(m.group(1) != m.group(2) for m in pat.finditer(s)): raise SystemExit('wide_stash：高级仓库槽控件不是原版那 91 个（找到 %d 个；原版布局变了？）' % len(codes))
    s = pat.sub('', s); i = s.index('"children"', s.index('"name": "advancedstash_gems"')); i = s.index('[', i) + 1
    new = ',\n'.join('                { "type": "AdvancedStashSlotWidget", "name": "%s", "fields": { "rect": { "width": %d, "height": %d, "x": %d, "y": %d }, "itemCode": "%s" } }' % (c, w, h, x, y, c) for c, (x, y, w, h) in sorted(want.items()))
    return s[:i] + '\n' + new + '\n            ' + s[i:]

TOUCH_ONLY = {'TouchStashLeft', 'TouchStashRight', 'close'}   # 触屏版插回的控件（touch_layout INGAME）：手柄版布局（原版 + 功能改动，gen_romfs_overlay.py）里没有，没有就不挪 | Widgets re-inserted for the touch version (touch_layout INGAME): absent from the controller layouts (original + feature changes, gen_romfs_overlay.py); if absent, nothing is moved
def _has(s, nm): return re.search(r'"name"\s*:\s*"%s"' % re.escape(nm), s) is not None
def bank_layout(text):
    s = text; n0 = s.count('"cellCount": { "x": 10, "y": 10 }')
    if n0 != 1: raise SystemExit('wide_stash：储藏箱格子 cellCount 10×10 应为 1 处，找到 %d 处（原版布局变了？）' % n0)
    s = s.replace('"cellCount": { "x": 10, "y": 10 }', '"cellCount": { "x": %d, "y": 10 }' % COLS)
    half = EX // 2
    for nm in ('gold_amount', 'gold_withdraw', 'StashNavigation', 'TouchStashLeft', 'TouchStashRight'):   # 下段居中的那排 | The centered row in the lower section
        if nm in TOUCH_ONLY and not _has(s, nm): continue
        s = _shift(s, nm, half)
    for nm in ('ShowPreviousLadderSeasonToggle', 'close'):
        if nm in TOUCH_ONLY and not _has(s, nm): continue
        s = _shift(s, nm, EX)   # 靠右的：赛季开关；关闭钮落在宽外框右上角凹槽（外框右边比原来多出 196，面板左移 196 ⇒ 面板内 +392） | Right-aligned ones: the season toggle; the close button lands in the notch at the top-right corner of the wide outer frame (the right side of the outer frame extends 196 more than before and the panel shifts left 196 ⇒ +392 within the panel)
    s = trove_slots(s)
    for nm in ('advancedstash_gems', 'advancedstash_materials', 'advancedstash_runes', 'advancedstash_horadriccube'): s = _container_x(s, nm, half)
    s = _shift(s, 'advancedstash_horadriccube', -2 * __import__('gen_trove_art').SHIFT)   # 合成格（连同转化钮）跟着底图里的台座左移 | the crafting slot (with its Transmute button) shifts left with the pedestal in the background
    s = vault_tab(s)
    return s

# 编年史仓库第 3 步（储藏箱第六个页签「编年史」）。
# 页签条 5 → 6：点第六个游戏照收、不发包（宿主实测），它在这一页画魔盒那套格子与「提取」钮——宿主在这一页把它们挪走，换上这里加的 TouchVault。
# 背景图表（backgroundFile）不能加第 6 项：加了游戏载入储藏箱布局就崩（main+0xb12024，宿主实测）；不加时第六页照常画。TouchVault 缺省挪在屏外（游戏不认 visible: false），宿主切到第六页才挪回 (0, 0)。
# 坐标按面板（加宽前，格子区 x 156–1528、y 139–1119；宿主加宽时整个面板一起挪）。文字、图、格子状态全由宿主 dm_vault_ui.c 写；点击一律发无人收的 ClosePanel:TouchV_*（同积分牌）
# Chronicle vault step 3 (the stash's sixth tab 「编年史」(Chronicle)).
# Tab strip 5 → 6: the game accepts a tap on the sixth as usual and sends no packet (measured on the host), and on this page it draws the cube's set of cells and the 「提取」(Withdraw) button -- on this page the host moves them away and puts in the TouchVault added here.
# The background image table (backgroundFile) cannot get a 6th entry: with it the game crashes as soon as it loads the stash layout (main+0xb12024, measured on the host); without it the sixth page draws normally. TouchVault is moved off screen by default (the game does not honor visible: false), and the host moves it back to (0, 0) only when switching to the sixth page.
# Coordinates are relative to the panel (before widening, cell area x 156–1528, y 139–1119; when the host widens, the whole panel moves together). Text, images and cell states are all written by the host dm_vault_ui.c; all taps send a ClosePanel:TouchV_* that nobody receives (same as the kill board)
VAULT_COLS, VAULT_ROWS, VAULT_SET_ROWS, VAULT_SET_N = 3, 4, 7, 6   # 宿主 dm_vault_ui.c 同值（暗金每页 4 行：上面让给两排部位页签） | Same values as the host dm_vault_ui.c (uniques have 4 rows per page: the space above is given to the two rows of body-part tabs)
VX, VW = 156, 1372
PARTS = ['@dmVaultPart%d' % i for i in range(11)]   # 中英表 dmVaultPart0–10（头盔 … 法杖与法器，同 gen_vault_table.py PARTS / 宿主 vt_part_name 的次序）；英文长的分两行（页签字框高 104 放得下两行） | Chinese-English string table dmVaultPart0–10 (Helms ... Staves & Casters, same order as gen_vault_table.py PARTS / the host's vt_part_name); long English names are split into two lines (the tab text box is 104 high, which fits two lines)
PT_FONT = 23   # 部位页签字号（直接给数，宿主截图量：17 → 每字 12.8 像素）：五个字的「盾牌与手套」要放进页签面（约 88 像素），原版最小档 $SmallFontSize 放不下；11 个页签同一字号 | Body-part tab font size (given directly as a number; measured from a host screenshot: 17 → 12.8 pixels per character): the five-character 「盾牌与手套」(Shields and Gloves) has to fit on the tab face (about 88 pixels), and the original game's smallest tier $SmallFontSize does not fit; all 11 tabs use the same font size
PT_Y, PT_PITCH = 228, 108   # 部位页签第一排的 y、两排间距（页签图 107×60 像素 = 214×120 单位，下沿 16 单位是选中菱形） | y of the first row of body-part tabs and the spacing between the two rows (tab image 107×60 pixels = 214×120 units, the bottom 16 units being the selection diamond)
UNIQ_Y = PT_Y + 2 * PT_PITCH + 72   # 暗金格子区的 y（宿主 dm_vault_ui.c VU_UNIQ_Y 同值） | y of the unique cell area (same value as VU_UNIQ_Y in the host dm_vault_ui.c)
def _vt_text(name, x, y, w, h, color, size, align='left', text='', trunc=True):
    return {'type': 'TextBoxWidget', 'name': name, 'fields': {'rect': {'x': x, 'y': y, 'width': w, 'height': h}, 'text': text,
            'style': {'fontColor': color, 'pointSize': size, 'alignment': {'h': align, 'v': 'center'}}, 'truncateWithEllipsis': trunc}}
def _vt_nl(w):   # 字里的 \n 照换行排（原版布局同写法 options.newlineHandling = standard；不加时 \n 画成同一行）：部位页签英文分两行 | a newline escape in the text is laid out as a line break (the original layouts use the same options.newlineHandling = standard; without it the escape is drawn on the same line): the part tabs are split into two lines in English
    w['fields']['style']['options'] = {'newlineHandling': 'standard'}; return w
def _vt_box(name, x, y, w, h, rgba, border=None):   # 原版九宫格（提示框底那种）：底色填满；border = 边框图 | Original-game nine-slice (the tooltip backing kind): background color fills it; border = border image
    f = {'rect': {'x': x, 'y': y, 'width': w, 'height': h}, 'backgroundColor': rgba}   # 不给 filename = 只画底色、没有边（宿主实测） | No filename given = only the background color is drawn, with no border (measured on the host)
    if border: f.update(filename=border, tileScale=0.5)
    return {'type': 'NineTileImageWidget', 'name': name, 'fields': f}
def _vt_tile(name, x, y, w, h):   # 一格：暗底 + 金框（存着才显示，原版手柄选中框；框控件四边各大 18，金线落在格边，见暗金格 CellGold）+ 物品图容器（宿主按图大小缩放居中） | One cell: dark backing + gold frame (shown only when stored, the original game's controller selection frame; the frame widget is 18 larger on each of the four sides so the gold line falls on the cell edge, see the unique cell CellGold) + item image container (the host scales and centers by image size)
    return [_vt_box(name, x, y, w, h, [0.03, 0.028, 0.026, 1.0]), dict(_vt_box(name + 'Gold', x - 18, y - 18, w + 36, h + 36, [0, 0, 0, 0], 'Controller/PANEL/ControllerSelector/ControllerSelectorBorder')),
            {'type': 'Widget', 'name': name + 'Box', 'fields': {'rect': {'x': x + 8, 'y': y + 8, 'width': w - 16, 'height': h - 16}},
             'children': [{'type': 'ImageWidget', 'name': name + 'Img', 'fields': {'rect': {'x': 0, 'y': 0}}}]}]
def _vt_check(name, x, y, label):
    return [{'type': 'ToggleButtonWidget', 'name': name, 'fields': {'rect': {'x': x, 'y': y, 'width': 65, 'height': 65}, 'sound': 'cursor_menu_select_hd',
             'filename': 'Lobby\\CreateGame\\CreateGame_AdvancedCheckbox', 'untoggledFrame': 0, 'untoggledPressedFrame': 1, 'untoggledHoveredFrame': 3, 'untoggledDisabledFrame': 2,
             'toggledFrame': 4, 'toggledPressedFrame': 5, 'toggledHoveredFrame': 6, 'onClickMessage': 'PanelManager:ClosePanel:' + name}},
            {'type': 'TextBoxWidget', 'name': name + '_Text', 'fields': {'text': label, 'rect': {'x': x + 75, 'y': y - 5, 'height': 50}, 'style': '$StyleSettingsTitle', 'fitToText': True}}]
def _vt_sub(name, x, label):
    return {'type': 'Widget', 'name': name, 'fields': {'rect': {'x': x, 'y': 0, 'width': 358, 'height': 94, 'scale': 0.7}},
            'children': [
                {'type': 'ImageWidget', 'name': 'Img', 'fields': {'rect': {'x': 0, 'y': 0}, 'filename': 'PANEL\\Chronicle\\Slot_list_small_golddeco', 'frame': 0}},
                _vt_box('Hit', 0, 0, 358, 94, [0, 0, 0, 0]),
                _vt_text('Text', 25, 0, 330, 94, '$FontColorWhite', '$LargeFontSize', 'center', label)]}
def vault_widgets():
    CW, CH, GAP = (VW - 2 * 8) // VAULT_COLS, 130, 8
    cells = []
    for k in range(VAULT_COLS * VAULT_ROWS):
        r, c = divmod(k, VAULT_COLS); x, y = c * (CW + GAP), r * (CH + GAP)
        cells.append({'type': 'Widget', 'name': 'TouchV_U%d' % k, 'fields': {'rect': {'x': x, 'y': y, 'width': CW, 'height': CH}}, 'children':
            # 存着 = 整格金框（CellGold，原版手柄选中框）：放在最后画在最上层，框住整格；字只排在框里那一圈之内（否则金框压字、字出框）。
            # 选中框的金线画在框控件边往里约 18 单位处（外面那圈是光晕；宿主截图量：62 像素高的框，线在上下各 8.5 像素）。框控件比格子四边各大 18，金线正好落在格边上，
            # 格里的图标、两行字、圆点都在线以内（框与格同一个矩形）；光晕落在格间缝里。图标格自己的小金框不用（只留一个框）
            # Stored = gold frame around the whole cell (CellGold, the original game's controller selection frame): placed last so it is drawn on the top layer, framing the whole cell; text is laid out only inside the ring within the frame (otherwise the gold frame covers the text or the text leaves the frame).
            # The gold line of the selection frame is drawn about 18 units inward from the edge of the frame widget (the ring outside it is glow; measured from a host screenshot: for a frame 62 pixels high, the line is 8.5 pixels in at top and bottom). The frame widget is 18 larger than the cell on each of the four sides, so the gold line falls exactly on the cell edge,
            # and the icon, two lines of text and dot in the cell are all inside the line (frame and cell are the same rectangle); the glow falls in the gaps between cells. The icon cell's own small gold frame is not used (only one frame is kept)
            [_vt_box('Bg', 0, 0, CW, CH, [0.065, 0.062, 0.058, 1.0])] + _vt_tile('Tile', 10, 10, 90, CH - 20) + [_vt_text('Name', 110, 10, CW - 160, 56, '$FontColorGoldYellow', '$MediumFontSize'),
             _vt_text('Sub', 110, 66, CW - 124, 54, '$FontColorMoreLightGray', '$SmallFontSize'),
             _vt_text('Dot', CW - 54, 8, 44, 44, '$FontColorGoldYellow', '$SmallFontSize', 'center', trunc=False),
             _vt_box('CellGold', -18, -18, CW + 36, CH + 36, [0, 0, 0, 0], 'Controller/PANEL/ControllerSelector/ControllerSelectorBorder')]})
    RH = 116; sets = []
    for r in range(VAULT_SET_ROWS):
        # BgGold = 集齐的行：不要金框，整行底色提亮成暗金（宿主按集齐与否显隐）
        # BgGold = a fully collected row: no gold frame, the whole row's background is brightened to dark gold (the host shows / hides it depending on whether it is fully collected)
        row = [_vt_box('Bg', 0, 0, VW, RH, [0.08, 0.075, 0.07, 1.0]), _vt_box('BgGold', 0, 0, VW, RH, [0.19, 0.15, 0.07, 1.0]),
               _vt_text('Name', 20, -2, 520, 60, '$FontColorGreen', '$MediumLargeFontSize'), _vt_text('Count', 20, 66, 520, 46, '$FontColorMoreLightGray', '$SmallFontSize')]   # 两行字的矩形中心相距 61（原 52）：中文字形比矩形里的字框高，原先上行的字脚贴着下行的字头 | The centers of the two text rectangles are 61 apart (was 52): Chinese glyphs are taller than the text box within the rectangle, and previously the feet of the upper line touched the heads of the lower line
        for j in range(VAULT_SET_N): row += _vt_tile('Tile%d' % j, 560 + j * 132, 6, 124, RH - 12) + [_vt_text('Tile%dDot' % j, 560 + j * 132 + 80, 0, 52, 46, '$FontColorGoldYellow', '$SmallFontSize', 'center', trunc=False)]   # 收录过没存 = 右上金点 | Collected before but not stored = gold dot at the top right
        sets.append({'type': 'Widget', 'name': 'TouchV_S%d' % r, 'fields': {'rect': {'x': 0, 'y': r * (RH + 8), 'width': VW, 'height': RH}}, 'children': row})
    return {'type': 'Widget', 'name': 'TouchVault', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1730, 'height': 1300}}, 'children': [
        _vt_box('TouchV_Back', VX - 14, 136, VW + 28, 1000, [0.13, 0.125, 0.12, 1.0]),   # 盖住储藏箱底图自带的格子线（整块暗底） | Covers the cell grid lines built into the stash background image (one whole dark backing)
        # 底部盖板：第六页没有自己的底图，游戏照画上一页的（个人 / 共享 / 魔盒凹槽各不同），还露着嵌入魔盒容器的「转化」「提取」钮；触摸由宿主挡掉（dm_vault_ui.c）。
        # 盖板是原版个人页底图格子以下整条、抹掉金币框（gen_vault_cover.py，不要大黑块），与从哪页切来无关；要在翻页条之前（翻页条画在它上面）
        # Bottom cover: the sixth page has no base image of its own, so the game draws the previous page's (personal / shared / Horadric Cube recess all differ), and the 「转化」(Transmute) and 「提取」(Withdraw) buttons embedded in the Horadric Cube container still show; touches are blocked by the host (dm_vault_ui.c).
        # The cover is the whole strip below the grid of the original personal page base image with the gold box erased (gen_vault_cover.py, no big black block), independent of which page one switched from; it must come before the page bar (the page bar is drawn on top of it)
        {'type': 'ImageWidget', 'name': 'TouchV_Cover', 'fields': {'rect': {'x': -2, 'y': 1132}, 'filename': 'Controller/Panel/zswide/VaultBottom'}},
        {'type': 'Widget', 'name': 'TouchV_Top', 'fields': {'rect': {'x': VX, 'y': 150, 'width': VW, 'height': 70}}, 'children': [
            _vt_sub('TouchV_SubU', 0, '@Unique'), _vt_sub('TouchV_SubS', 260, '@Sets'),   # 原版串（中文同字） | original strings (same characters in Chinese)
            # 搜索框：原版编年史搜索框那一套（GrowableImage 底 + InputTextBoxWidget），点它游戏开 TextInputModal、dm_textinput.c 接系统键盘（同金币数量框）。
            # 键盘是整段交回，打完点「完成」才筛（不是边打边筛）。缩 0.75 塞进这一行；字由宿主每帧读（输入框 +0x12a8）
            # Search box: the original game's chronicle search box set (GrowableImage backing + InputTextBoxWidget); tapping it makes the game open TextInputModal, and dm_textinput.c hooks up the system keyboard (same as the gold amount box).
            # The keyboard hands the text back as one whole piece, and filtering happens only after typing and tapping 「完成」(Done) (not filtering while typing). Scaled 0.75 to fit into this row; the text is read by the host every frame (input box +0x12a8)
            {'type': 'GrowableImageWidget', 'name': 'TouchV_SearchBg', 'fields': {'rect': {'x': 540, 'y': -6, 'width': 1109, 'height': 102, 'scale': 0.75},
                'filename': 'PANEL\\Chronicle\\Chronicle_InputBox_Center', 'hoveredFrame': 1, 'leftCap/filename': 'PANEL\\Chronicle\\Chronicle_InputBox_Left', 'rightCap/filename': 'PANEL\\Chronicle\\Chronicle_InputBox_Right'},
             'children': [{'type': 'InputTextBoxWidget', 'name': 'TouchV_Search', 'fields': {'rect': {'x': 40, 'y': 0, 'width': 1036, 'height': 102}, 'backgroundColor': {'r': 0, 'g': 0, 'b': 0, 'a': 0.0},
                'maxStringLength': 16, 'imeEnabled': True, 'hint': {'text': '@dmVaultSearchHint', 'style': {'fontFace': 'BlizzardGlobal', 'alignment': {'v': 'center'}, 'pointSize': '$MediumLargeFontSize', 'fontColor': {'a': 84}}},
                'fontStyle': {'fontFace': 'BlizzardGlobal', 'alignment': {'v': 'center'}, 'pointSize': '$MediumLargeFontSize'}, 'useParentRectAsFocusRect': True, 'fontType': '12ptF',
                'onUpdateMessage': 'TouchVaultMessage:Search', 'virtualKeyboardType': 'Alphabet', 'leftCursorPadding': 40}}]}]},
        # 部位页签（替掉下拉）：原版储藏箱页签图（CTRL_Tabs_215_TGA，帧 0 未选 / 帧 1 选中带菱形），两排 6 + 5，第二排错开半格；
        # 点按由宿主按 Hit 框认（dm_vault_ui.c，不发鼠标给游戏）；选中那个宿主换帧、显示白字 TextOn，其余金字 Text
        # Body-part tabs (replacing the dropdown): the original game's stash tab image (CTRL_Tabs_215_TGA, frame 0 unselected / frame 1 selected with a diamond), two rows of 6 + 5, the second row offset by half a cell;
        # taps are recognized by the host from the Hit boxes (dm_vault_ui.c, no mouse is sent to the game); for the selected one the host changes the frame and shows the white text TextOn, the rest show the gold text Text
        {'type': 'Widget', 'name': 'TouchV_Parts', 'fields': {'rect': {'x': VX, 'y': PT_Y, 'width': VW, 'height': 2 * PT_PITCH}}, 'children': [
            {'type': 'Widget', 'name': 'TouchV_PT%d' % i, 'fields': {'rect': {'x': (i % 6) * 228 + (i // 6) * 114, 'y': (i // 6) * PT_PITCH, 'width': 214, 'height': 104}}, 'children': [
                {'type': 'ImageWidget', 'name': 'Img', 'fields': {'rect': {'x': 0, 'y': 0}, 'filename': 'Controller/Panel/Stash/V2/AdditionalStash/CTRL_Tabs_215_TGA', 'frame': 0}},
                _vt_box('Hit', 0, 0, 214, 104, [0, 0, 0, 0]),
                _vt_nl(_vt_text('Text', 4, 0, 206, 104, '$FontColorGoldYellow', PT_FONT, 'center', p, trunc=False)),
                _vt_nl(_vt_text('TextOn', 4, 0, 206, 104, '$FontColorWhite', PT_FONT, 'center', p, trunc=False))]} for i, p in enumerate(PARTS)]},
        _vt_text('TouchV_Stats', VX, PT_Y + 2 * PT_PITCH + 6, 780, 56, '$FontColorBeige', '$SmallFontSize'),   # 宽到档次勾选框左沿（782）：英文统计行长（左对齐，中文不受影响） | extends to the left edge of the quality tier checkboxes (782): the English stats row is long (left-aligned, Chinese is unaffected)
        {'type': 'Widget', 'name': 'TouchV_Tiers', 'fields': {'rect': {'x': VX + VW - 590, 'y': PT_Y + 2 * PT_PITCH, 'width': 590, 'height': 65}}, 'children':
            _vt_check('TouchV_T0', 0, 0, '@dmVaultTierN') + _vt_check('TouchV_T1', 200, 0, '@dmVaultTierX') + _vt_check('TouchV_T2', 400, 0, '@dmVaultTierE')},   # 中文同原版 NormalQuality 等；英文用简写（原版全称三个挤在 590 里叠字，英文版） | Chinese is the same as the original NormalQuality etc.; English uses abbreviations (the three original full names overlap when squeezed into 590, English version)
        {'type': 'Widget', 'name': 'TouchV_Uniq', 'fields': {'rect': {'x': VX, 'y': UNIQ_Y, 'width': VW, 'height': VAULT_ROWS * 138}}, 'children': cells},
        {'type': 'Widget', 'name': 'TouchV_Sets', 'fields': {'rect': {'x': 99999, 'y': 296, 'width': VW, 'height': 870}}, 'children': sets},
        # 底部翻页条（去掉下面的空黑框，按钮下移、放大、上下居中、好点按）：页码和左右箭头排在底部装饰带的竖向正中（两枚环饰的中线 y 1262），
        # 箭头用原版大号箭头图（Lobby/Final/LobbyArrowLargeL / R，每帧 47×54 像素 = 94×108 单位，帧 0 常态 / 2 按下 / 3 不可用），放大 1.25 倍；
        # 点按区是各自一块透明的命中框 310×310 单位（iPhone 真机档位 1 单位 ≈ 0.164 点 → 约 51 点见方，16:9 档约 62 点；别贴着 44 点下限），从行区下沿往下铺、超出面板底边也算，不压最后一行的格子；宿主按命中框认（dm_vault_ui_pager_hit）。
        # 原版的小箭头 TouchStashLeft / Right 被盖板压住，这一页不用
        # Bottom page bar (remove the empty black box below; move the buttons down, enlarge them, center them vertically, make them easy to tap): the page number and left / right arrows are arranged at the vertical center of the bottom decorative band (the center line of the two ring ornaments, y 1262),
        # the arrows use the original large arrow images (Lobby/Final/LobbyArrowLargeL / R, 47×54 pixels per frame = 94×108 units, frame 0 normal / 2 pressed / 3 disabled), scaled 1.25×;
        # each tap area is its own transparent hit box of 310×310 units (on the real iPhone profile 1 unit ≈ 0.164 points → about 51 points square, about 62 points on the 16:9 profile; do not sit right at the 44-point minimum), extending downward from the bottom edge of the row area, counting even beyond the panel's bottom edge, and not overlapping the last row of cells; the host recognizes taps by the hit boxes (dm_vault_ui_pager_hit).
        # The original small arrows TouchStashLeft / Right are under the cover and not used on this page
        {'type': 'ImageWidget', 'name': 'TouchV_PgL', 'fields': {'rect': {'x': 562, 'y': 1195, 'scale': 1.25}, 'filename': 'Lobby/Final/LobbyArrowLargeL', 'frame': 0}},
        {'type': 'ImageWidget', 'name': 'TouchV_PgR', 'fields': {'rect': {'x': 992, 'y': 1195, 'scale': 1.25}, 'filename': 'Lobby/Final/LobbyArrowLargeR', 'frame': 0}},
        _vt_box('TouchV_PgLHit', 466, 1168, 310, 310, [0, 0, 0, 0]), _vt_box('TouchV_PgRHit', 896, 1168, 310, 310, [0, 0, 0, 0]),
        _vt_text('TouchV_Page', 696, 1222, 280, 80, '$FontColorGoldYellow', '$LargeFontSize', 'center'),
        {'type': 'ImageWidget', 'name': 'TouchV_Drag', 'fields': {'rect': {'x': 99999, 'y': 0}}},   # 从格子拖回背包时跟手的物品图（宿主摆） | Item image that follows the finger when dragging from a cell back to the inventory (placed by the host)
        # 说明框（第 5 步）：原版物品说明框那套（提示框九宫格边 + 八成黑底、原版说明文字倒排行序），放最后 = 画在最上面；宿主按字的大小摆，缺省在屏外
        # Tooltip (step 5): the original game's item tooltip set (tooltip nine-slice border + 80% black backing, original-game description text with reversed line order), placed last = drawn on top; the host places it according to the text size, off screen by default
        {'type': 'NineTileImageWidget', 'name': 'TouchV_TipBg', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 900, 'height': 600}, 'filename': 'PANEL/TooltipBorder/TooltipBorder', 'backgroundColor': [0.0, 0.0, 0.0, 0.85], 'tileScale': 1.0}},
        {'type': 'TextBoxWidget', 'name': 'TouchV_Tip', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1100, 'height': 600}, 'text': '', 'fitToText': True,
            'style': {'fontColor': '$FontColorWhite', 'pointSize': '$TooltipFontSize', 'alignment': {'h': 'center', 'v': 'top'}, 'options': {'pipeCharacterBehavior': 'handle', 'newlineHandling': 'reversed'}}}}]}

def vault_tab(s):
    for a, b in (('"tabCount": 5,', '"tabCount": 6,'), ('"inactiveFrames": [ 0, 0, 0, 0, 0 ],', '"inactiveFrames": [ 0, 0, 0, 0, 0, 0 ],'), ('"activeFrames": [ 1, 1, 1, 1, 1 ],', '"activeFrames": [ 1, 1, 1, 1, 1, 1 ],'),
                 ('"disabledFrames": [-1, 0, 0, 0, 0 ],', '"disabledFrames": [-1, 0, 0, 0, 0, -1 ],'), ('"textStrings": [ "@personal", "@shared", "@gems", "@materials", "@runes" ],', '"textStrings": [ "@personal", "@shared", "@dmTroveTab", "@materials", "@runes", "@dmStashVaultTab" ],')):
        if s.count(a) != 1: raise SystemExit('wide_stash：储藏箱布局 %s 不是原样（原版布局变了？）' % a.strip())
        s = s.replace(a, b)
    # 页签条右端的手柄键帽（ZR）：原版按五个页签摆在 x 1135（= 五个页签的右沿 1075 再往外 60），加了第六个页签后正好落在「编年史」页签的字上（看图发现）。
    # 现在屏上只有四个页签（珍宝匣把材料 / 符文两个页签收掉了，dm_cursor.c ui_trove_tabs_pump）：键帽放在第四个页签右沿（860）外 60；左端的 ZL 不动。只改 BankTabs 这一条（同文件里另外几条页签条不动）；触屏版键帽是透明图，位置无所谓
    # Controller keycap (ZR) at the right end of the tab bar: the game places it at x 1135 for five tabs (= the right edge of five tabs, 1075, plus 60); with a sixth tab added it landed right on the text of the "Chronicle" tab (found by looking at a screenshot).
    # There are now only four tabs on screen (the Trove removed the materials / runes tabs, dm_cursor.c ui_trove_tabs_pump): the keycap goes 60 beyond the right edge of the fourth tab (860); ZL at the left end stays. Only the BankTabs entry is changed (the other tab bars in the same file are untouched); the touch keycap is a transparent image, so its position does not matter
    i = s.index('"name": "BankTabs"'); k = '"tabRightIndicatorPosition": "$HalfPanelRightTabButtonPromptPosition",'; j = s.find(k, i)
    if j < 0 or j - i > 2500: raise SystemExit('wide_stash：BankTabs 的右端键帽位置不是原样（原版布局变了？）')
    s = s[:j] + '"tabRightIndicatorPosition": { "x": 920, "y": -2 },' + s[j + len(k):]
    # 个人仓库页码（dm_pages.c）：自己的文字控件，不再借原版共享页那块页码（SharedStashTabContainer / CurrentSharedStashText）。缺省在屏外，宿主在个人页挪进来
    # Personal stash page number (dm_pages.c): our own text widget, no longer borrowing the original game's shared page number block (SharedStashTabContainer / CurrentSharedStashText). Off screen by default; the host moves it in on personal pages
    pg = {'type': 'Widget', 'name': 'TouchPgPage', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 1730, 'height': 1300}}, 'children': [
        _vt_text('TouchPgPage_Text', 675, 1159, 322, 64, '$FontColorGoldYellow', '$MediumFontSize', 'center')]}
    end = s.rstrip().rstrip('}').rstrip(); assert end.endswith(']'); at = len(end) - 1
    return s[:at].rstrip().rstrip(',') + ',\n        ' + json.dumps(pg, ensure_ascii=False) + ',\n        ' + json.dumps(vault_widgets(), ensure_ascii=False) + '\n    ' + s[at:]

def overlay_layout(text):
    i = text.find('"name": "Background"'); j = text.find('"TabBarWidget", "name": "Tabs"')
    if i < 0 or j < 0: raise SystemExit('wide_stash：外框布局里没有 Background / Tabs（原版布局变了？）')
    k = text.rfind('{', 0, j)
    add = ('{\n                    "type": "ImageWidget", "name": "WideBackground",\n                    "fields": {\n'
           '                        "rect": { "x": 99999, "y": 0 },\n                        "filename": "Controller/Panel/zswide/MainPanelBG_Wide",\n                    },\n                },\n                ')
    text = text[:k] + add + text[k:]
    # 魔盒并排时的两个标题（dm_cursor.c 魔盒并排那段）：原版标题条此时写的是「赫拉迪克方块 | 物品栏」（魔盒面板在原版里占左边那一位），
    # 并排后左边其实是储藏箱、右边是魔盒。原版那两个字挪出屏（位置守卫），换成我们自己的两个文字控件：字用原版串（@stash / @box）、样式照面板标题，
    # 位置照储藏箱加宽后原版两个标题字的矩形（左 -196 宽 1832、右 1636 宽 1440，见 dm_cursor.c ui_wide_pump），缺省在屏外
    # The two titles while the cube is side by side (the cube side-by-side section of dm_cursor.c): the original title bar then reads 「赫拉迪克方块 | 物品栏」(Horadric Cube | Inventory) (in the original game the cube panel occupies the left position),
    # but once side by side the left is actually the stash and the right is the cube. The two original title texts are moved off screen (position guard) and replaced with two text widgets of our own: the text uses the original strings (@stash / @box), the style follows the panel title,
    # and the positions follow the rects of the two original title texts after the stash is widened (left -196 width 1832, right 1636 width 1440, see dm_cursor.c ui_wide_pump); off screen by default
    st = {'type': 'Widget', 'name': 'TouchSideTitle', 'fields': {'rect': {'x': 99999, 'y': 0, 'width': 3272, 'height': 103}}, 'children': [
        _vt_text('TouchSideTitle_L', 0, 0, 1832, 103, '$FontColorWhite', '$MediumFontSize', 'center', '@stash'),
        _vt_text('TouchSideTitle_R', 1832, 0, 1440, 103, '$FontColorWhite', '$MediumFontSize', 'center', '@box')]}
    # 插在标题条 Tabs 这个控件的后面、同一个父控件里（同一套缩放与原点，各种屏幕档位都跟着原版标题走；画在标题条之上）
    # Inserted right after the title bar's Tabs widget, in the same parent widget (same scaling and origin, so it follows the original title on every screen profile; drawn above the title bar)
    j = text.find('"TabBarWidget", "name": "Tabs"'); k = text.rfind('{', 0, j); d = 0; e = k
    while e < len(text):
        c = text[e]
        if c == '{': d += 1
        elif c == '}':
            d -= 1
            if d == 0: break
        e += 1
    if d != 0: raise SystemExit('wide_stash：外框布局里 Tabs 控件的括号没配上（原版布局变了？）')
    return text[:e + 1] + ',\n                ' + json.dumps(st, ensure_ascii=False) + text[e + 1:]

def build(romfs, outdir, repl):
    out = {}
    # 表有两份：excel/inventory.bin 与 excel/base/inventory.bin，游戏按角色所属的模式选一套数据表（取格函数 main+0x560b80 按 [单位+0x1bd] 选）。
    # 只改一份时，用另一套的角色（如 88 级法师这类老角色）界面画 14 列、游戏按 10 宽取格，指到第 10 列越界读崩 main+0x560c6c
    # There are two copies of the table: excel/inventory.bin and excel/base/inventory.bin; the game picks one set of data tables by the mode the character belongs to (the cell lookup function main+0x560b80 selects by [unit+0x1bd]).
    # If only one copy is changed, for characters using the other set (old characters such as the level 88 sorceress) the UI draws 14 columns while the game looks up cells at width 10, and pointing at column 10 reads out of bounds and crashes at main+0x560c6c
    for sub in ('', 'base_'):
        path, inv = _entry(romfs, '/global/excel/' + sub.replace('_', '/') + 'inventory.bin'); inv = bytearray(inv)
        for row in (10, 25):
            o = 4 + row * 240 + 16
            if inv[o:o + 2] != b'\x0a\x0a': raise SystemExit('wide_stash：%s 第 %d 行不是 10×10（原版表变了？）' % (path, row))
            inv[o] = COLS
        # 佣兵加装备位（秘境 10 层解锁，产物 r39 起配服务端那道门）：佣兵行（第 11 行，第二套第 26 行）原版只定义了右手 / 身 / 左手 / 头四个部位的格子，
        # 项链 / 右戒 / 左戒 / 腰带 / 鞋 / 手套（格序 4–9）全是 −1；服务端放置校验 main+0x65032c 对非玩家主人固定读第 11 行，四个坐标全负就拒。
        # 这里照玩家行（第 0 行）的相对位置补上：玩家面板原点在 x = 320，佣兵面板在 0；第二套整体再偏 (+80, +60)（两行已有四格的差）。每格 20 字节：左 右 上 下 + 1 个整数
        # Extra mercenary equipment slots (unlocked at rift level 10; paired with the server-side gate from build output r39): the mercenary row (row 11, row 26 in the second set) in the original only defines cells for the four body locations right hand / torso / left hand / head,
        # while amulet / right ring / left ring / belt / boots / gloves (cell order 4–9) are all −1; the server placement check main+0x65032c always reads row 11 for non-player owners and rejects when all four coordinates are negative.
        # Here they are filled in following the relative positions in the player row (row 0): the player panel's origin is at x = 320 and the mercenary panel's at 0; the second set is offset by a further (+80, +60) overall (the difference of the four existing cells between the two rows). 20 bytes per cell: left right top bottom + 1 integer
        import struct
        for row, dx, dy in ((11, -320, 0), (26, -240, 60)):
            base = 4 + row * 240 + 0x28
            if struct.unpack_from('<4i', inv, base + 4 * 0x14) != (-1, -1, -1, -1): raise SystemExit('wide_stash：%s 第 %d 行的项链格不是空的（原版表变了？）' % (path, row))
            for k in range(4, 10):
                l, r, t, b_, x = struct.unpack_from('<5i', inv, 4 + 0x28 + k * 0x14)
                struct.pack_into('<5i', inv, base + k * 0x14, l + dx, r + dx, t + dy, b_ + dy, x)
        p = os.path.join(outdir, 'ws_' + sub + 'inventory.bin'); open(p, 'wb').write(inv); out[path] = p
    for n, rel in SPRITES.items():
        p = os.path.join(outdir, 'ws_' + n + '.sprite'); shutil.copyfile(os.path.join(ART, n + '_wide.sprite'), p); out['data/hd/global/ui/' + rel + '.sprite'] = p
    p = os.path.join(outdir, 'ws_mainpanelbg_wide.sprite'); shutil.copyfile(os.path.join(ART, 'MainPanelBG_Wide.sprite'), p); out['+data/hd/global/ui/' + WIDE_FRAME + '.sprite'] = p
    for n in ('VaultBottom',):   # 编年史页底部盖板（gen_vault_cover.py 从宽共享页底图裁的） | Bottom cover of the chronicle page (cropped by gen_vault_cover.py from the wide shared page background image)
        p = os.path.join(outdir, 'ws_' + n.lower() + '.sprite'); shutil.copyfile(os.path.join(ART, n + '.sprite'), p); out['+data/hd/global/ui/controller/panel/zswide/' + n.lower() + '.sprite'] = p
    for rel, fn in ((BANK, bank_layout), (OVERLAY, overlay_layout)):
        logical = touch_layout.read_layout(romfs, rel)[0].lower(); src = repl.get(logical)
        text = open(src, encoding='utf-8').read() if src else touch_layout.read_layout(romfs, rel)[1]   # 叠在触摸布局已改的那份上 | Stacked on top of the copy already modified by the touch layout
        p = os.path.join(outdir, 'ws_' + os.path.basename(rel)); open(p, 'w', encoding='utf-8').write(fn(text)); out[logical] = p
    return out, {'cols': COLS, 'files': len(out)}
