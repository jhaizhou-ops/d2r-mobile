#!/usr/bin/env python3
"""全触摸方案：简中字符串里的手柄说法换成触摸说法（所有的手柄按钮操作介绍都应该换成触摸操作介绍；点一下就能完成的提示直接去掉）。
做法 = RomFS 覆盖层换字符串文件（与布局覆盖同一套机制），只改 zhCN 一列、按键名定位、原文不符即报错。
手势口径：单击看详情并比较、双击使用、拖动装备 / 丢弃 / 出售、长按药水进腰带、底栏十字按钮开面板。
Full-touch scheme: in the Simplified Chinese strings, controller wording is replaced with touch wording (every description of a controller button action should become a description of the touch action; hints for things done with a single tap are removed outright).
Approach = replace the string files through the RomFS overlay (the same mechanism as the layout overlay), change only the zhCN column, locate entries by key name, and report an error if the original text does not match.
Gesture conventions: single tap to see details and compare, double tap to use, drag to equip / drop / sell, long press a potion to put it in the belt, the cross button on the bottom bar opens panels."""
import hashlib, json, os, re
import touch_layout

U = ''   # 确认键图标； 比较键； 加号键 | Confirm key icon; compare key; plus key
# 文件 → {键: (原文校验, 新文)}；原文校验 = 游戏现行文字的 SHA-1 前 8 位（表里不存原文）；新文为空串 = 这句不要了
# file → {key: (original text check, new text)}; original text check = first 8 characters of the SHA-1 of the game's current text (the table does not store the original text); an empty new text = this sentence is dropped
TABLE = {
    'ui-controller.json': {
        'UseItemController': ('4c4adb8b', '双击使用'),
        'OpenHoradricCubeController': ('e2d89ec6', '双击打开'),
        'ReadScrollController': ('61e448cc', '双击阅读'),
        'UsePotionOfLifeController': ('2ccab0ae', '双击永久增加20点生命值'),
        'UseBookOfSkillController': ('619bdd63', '双击增加你的技能点'),
        'UseScrollOfResistanceController': ('bc185cbc', '双击施法'),
        'UseTokenOfAbsolutionController': ('9a7778b6', '双击重设属性技能点'),
        'UseTerrorTokenAct1Controller': ('aefa10c5', '双击恐惧化第一幕'),
        'UseTerrorTokenAct2Controller': ('3005cb15', '双击恐惧化第二幕'),
        'UseTerrorTokenAct3Controller': ('45921fbf', '双击恐惧化第三幕'),
        'UseTerrorTokenAct4Controller': ('5ebcc635', '双击恐惧化第四幕'),
        'UseTerrorTokenAct5Controller': ('0bd3c1fd', '双击恐惧化第五幕'),
        'comparisonTooltipController': ('0abebcab', ''),            # 单击物品就出详情并和身上的比较，不用提示 | A single tap on an item shows its details and compares it with what is equipped, so no hint is needed
        'hireComparisonTooltipController': ('2f147b1d', ''),
        'StrYouDiedConsole': ('5336efaf', '你挂了'),              # 下面就有「继续」按钮 | There is a 「继续」(Continue) button right below
        'ControllerInteractButton': ('54234e14', ''),                                    # NPC / 可交互物名字旁的确认键图标（图已透明，占位也去掉） | The confirm key icon next to the names of NPCs / interactable objects (the image is already transparent; the placeholder is removed too)
        'ControllerTitleScreenPrompt': ('d701b1f2', '点击屏幕开始'),
        'EnterCharacterName': ('c516bdc5', ''),
        # 两件可比装备（如两个戒指）之间切换 = 再单击同一件物品（行为归输入层）
        # Switching between two comparable equipped items (e.g. two rings) = single-tap the same item again (the behavior belongs to the input layer)
        'ControllerCycleComparisonTooltips': ('d53c0a25', '再点一次与另一件比较'),
        'SelectButtonNoteText': ('aa4e91fa', ''),   # 触摸下交互 = 点目标、拾取 = 点物品名，与技能键无关 | Under touch, interact = tap the target and pick up = tap the item name, unrelated to the skill buttons
        'MoviePlaybackHoldToSkip_switch': ('f3a05873', ''),                           # 过场不放（宿主 0.1 秒播完），这句不要 | Cutscenes are not played (the host finishes them in 0.1 s), so this sentence is dropped
    },
    'ui.json': {
        # 通知里不写怎么操作（界面更简洁；十字按钮变红已表达）
        # Notifications do not say how to operate (the UI is cleaner; the cross button turning red already conveys it)
        'LevelUpStatsNotificationController': ('7854f598', '你升级了。'),
        'LevelUpNotificationController': ('7ba311c9', '你升级了。'),
        'QuestLogNotificationController': ('7b65d87d', '任务已更新。'),
        'MoviePlaybackHoldToSkip': ('f3a05873', ''),
        # 大字号排版（不是手柄说法）：角色页「技能名 / 命中值」两行标题在 84 高的格子里放不下两行大字——改成单行「技能名命中值」
        # Large font layout (not controller wording): the two-line title 「技能名 / 命中值」(skill name / Attack Rating) on the character page cannot fit two lines of large text in a cell 84 high -- changed to the single line 「技能名命中值」(skill name Attack Rating)
        'strchratr': ('703341e1', '%s命中值'),
        'strchrrat': ('703341e1', '%s命中值'),
    },
}

# 英文列（英文版）：同一批键的 enUS 改成触摸说法；口径同上（空串 = 这句不要）。不在这里的键英文照原版。俄语列见下面 RU。
# 人物页「%s\n命中值」那两条是中文大字号排版问题，英文不动。西语两列见下面 ES / MX；别的语言列不改（我们自己的字只做中英西俄，别的语言按英文）
# 与中文表同一写法：原文只存校验（SHA-1 前 8 位），表里不存原版文字
# English column (English version): the same keys' enUS changed to touch wording; same rules as above (empty string = drop the sentence). Keys not listed keep the original English. The Russian column is RU below.
# The two "%s
# Attack Rating" entries on the character page are a Chinese large-font layout issue; English is untouched. The two Spanish columns are ES / MX below; other language columns are unchanged (our own text covers Chinese, English, Spanish and Russian; other languages use English)
# Same scheme as the Chinese table: only a checksum of the original is stored (first 8 hex digits of SHA-1), never the original text
EN = {
    'ui-controller.json': {
        'UseItemController': ('3c07d84d', 'Double-tap to Use'),
        'OpenHoradricCubeController': ('3c06b741', 'Double-tap to Open'),
        'ReadScrollController': ('5b76ecf7', 'Double-tap to Read'),
        'UsePotionOfLifeController': ('74604765', 'Double-tap to permanently add 20 to Life'),
        'UseBookOfSkillController': ('3a031cc6', 'Double-tap to learn skill of your choice'),
        'UseScrollOfResistanceController': ('ce192aed', 'Double-tap to Cast'),
        'UseTokenOfAbsolutionController': ('3ba19ef3', 'Double-tap to reset Stat/Skill Points'),
        'UseTerrorTokenAct1Controller': ('73cb07eb', 'Double-tap to terrorize Act 1'),
        'UseTerrorTokenAct2Controller': ('8c394882', 'Double-tap to terrorize Act 2'),
        'UseTerrorTokenAct3Controller': ('558bcf92', 'Double-tap to terrorize Act 3'),
        'UseTerrorTokenAct4Controller': ('07eadb68', 'Double-tap to terrorize Act 4'),
        'UseTerrorTokenAct5Controller': ('9f1ff50e', 'Double-tap to terrorize Act 5'),
        'comparisonTooltipController': ('48cc0c36', ''),
        'hireComparisonTooltipController': ('a97423d0', ''),
        'StrYouDiedConsole': ('e2908c71', 'You have died'),
        'ControllerInteractButton': ('54234e14', ''),
        'ControllerTitleScreenPrompt': ('75c1a3c8', 'Tap the Screen to Begin'),
        'EnterCharacterName': ('8334a399', ''),
        'ControllerCycleComparisonTooltips': ('389c3ec9', 'Tap again to compare with the other'),
        'SelectButtonNoteText': ('e0bafb15', ''),
        'MoviePlaybackHoldToSkip_switch': ('dab56a9e', ''),
    },
    'ui.json': {
        'LevelUpStatsNotificationController': ('525fb31a', "You've leveled up."),
        'LevelUpNotificationController': ('1e4882b2', "You've leveled up."),
        'QuestLogNotificationController': ('94d110a8', 'Quest has been updated.'),
        'MoviePlaybackHoldToSkip': ('dab56a9e', ''),
    },
}

# 西语两列（西语版）：同一批键的 esES / esMX 各改成触摸说法，口径同英文（空串 = 这句不要）；原版两列用词不同（Acto I / Acto 1、búsqueda / gesta），各存各的原文校验
# Two Spanish columns (Spanish version): esES / esMX of the same set of keys each changed to touch wording, same policy as English (empty string = drop this sentence); the two original columns use different wording (Acto I / Acto 1, búsqueda / gesta), each stores its own original-text check
ES = {
    'ui-controller.json': {
        'UseItemController': ('b3c337b6', 'Toca dos veces para usar'),
        'OpenHoradricCubeController': ('316e4f76', 'Toca dos veces para abrir'),
        'ReadScrollController': ('9f9ebb2f', 'Toca dos veces para leer'),
        'UsePotionOfLifeController': ('8261013a', 'Toca dos veces para conseguir 20 p. de vida permanentes'),
        'UseBookOfSkillController': ('9b9ad5b4', 'Toca dos veces para aprender una habilidad'),
        'UseScrollOfResistanceController': ('2347c7a9', 'Toca dos veces para lanzar'),
        'UseTokenOfAbsolutionController': ('cce00a79', 'Toca dos veces para reiniciar los puntos de estadísticas/habilidades'),
        'UseTerrorTokenAct1Controller': ('9c7bb697', 'Toca dos veces para aterrorizar el Acto I'),
        'UseTerrorTokenAct2Controller': ('f46a3882', 'Toca dos veces para aterrorizar el Acto II'),
        'UseTerrorTokenAct3Controller': ('773c665e', 'Toca dos veces para aterrorizar el Acto III'),
        'UseTerrorTokenAct4Controller': ('19ba4849', 'Toca dos veces para aterrorizar el Acto IV'),
        'UseTerrorTokenAct5Controller': ('7dffa819', 'Toca dos veces para aterrorizar el Acto V'),
        'comparisonTooltipController': ('c84875e2', ''),
        'hireComparisonTooltipController': ('c84875e2', ''),
        'StrYouDiedConsole': ('c2b02f82', 'Has muerto'),
        'ControllerInteractButton': ('54234e14', ''),
        'ControllerTitleScreenPrompt': ('885ef015', 'Toca la pantalla para empezar'),
        'EnterCharacterName': ('be6e6664', ''),
        'ControllerCycleComparisonTooltips': ('1b3ed98b', 'Toca de nuevo para comparar con el otro'),
        'SelectButtonNoteText': ('c037151e', ''),
        'MoviePlaybackHoldToSkip_switch': ('a6b6a532', ''),
    },
    'ui.json': {
        'LevelUpStatsNotificationController': ('cbd9f839', 'Has subido de nivel.'),
        'LevelUpNotificationController': ('4b4b08fe', 'Has subido de nivel.'),
        'QuestLogNotificationController': ('13cf1af4', 'Se ha actualizado la búsqueda.'),
        'MoviePlaybackHoldToSkip': ('a6b6a532', ''),
    },
}

MX = {
    'ui-controller.json': {
        'UseItemController': ('b3c337b6', 'Toca dos veces para usar'),
        'OpenHoradricCubeController': ('316e4f76', 'Toca dos veces para abrir'),
        'ReadScrollController': ('9f9ebb2f', 'Toca dos veces para leer'),
        'UsePotionOfLifeController': ('c7d42535', 'Toca dos veces para agregar 20 a tu vida permanentemente'),
        'UseBookOfSkillController': ('906ec564', 'Toca dos veces para aprender una habilidad a elección'),
        'UseScrollOfResistanceController': ('76a87c82', 'Toca dos veces para activar'),
        'UseTokenOfAbsolutionController': ('5f21cbbb', 'Toca dos veces para restablecer los puntos de atributo/habilidad'),
        'UseTerrorTokenAct1Controller': ('37fbf40e', 'Toca dos veces para aterrorizar el Acto 1'),
        'UseTerrorTokenAct2Controller': ('a23ab72a', 'Toca dos veces para aterrorizar el Acto 2'),
        'UseTerrorTokenAct3Controller': ('65e32c9e', 'Toca dos veces para aterrorizar el Acto 3'),
        'UseTerrorTokenAct4Controller': ('0217c37c', 'Toca dos veces para aterrorizar el Acto 4'),
        'UseTerrorTokenAct5Controller': ('0478c1a6', 'Toca dos veces para aterrorizar el Acto 5'),
        'comparisonTooltipController': ('c84875e2', ''),
        'hireComparisonTooltipController': ('c84875e2', ''),
        'StrYouDiedConsole': ('0cd61240', 'Has muerto'),
        'ControllerInteractButton': ('54234e14', ''),
        'ControllerTitleScreenPrompt': ('3db5d4d5', 'Toca la pantalla para comenzar'),
        'EnterCharacterName': ('53243134', ''),
        'ControllerCycleComparisonTooltips': ('43363ba3', 'Toca de nuevo para comparar con el otro'),
        'SelectButtonNoteText': ('483ba8e7', ''),
        'MoviePlaybackHoldToSkip_switch': ('a6b6a532', ''),
    },
    'ui.json': {
        'LevelUpStatsNotificationController': ('c6faf1bb', 'Subiste de nivel.'),
        'LevelUpNotificationController': ('582b5596', 'Subiste de nivel.'),
        'QuestLogNotificationController': ('b9ebf5ae', 'Se actualizó la gesta.'),
        'MoviePlaybackHoldToSkip': ('a6b6a532', ''),
    },
}
# 俄语列（俄语版）：同一批键的 ruRU 改成触摸说法，口径同英文（空串 = 这句不要）；原版俄文的原文校验各自单独存
# Russian column (Russian version): the same keys' ruRU changed to touch wording, same rules as English (empty string = drop the sentence); the original Russian checksums are stored separately
RU = {
    'ui-controller.json': {
        'UseItemController': ('7474c613', 'Двойной тап — использовать'),
        'OpenHoradricCubeController': ('b702fc29', 'Двойной тап — открыть'),
        'ReadScrollController': ('1aca2594', 'Двойной тап — прочитать'),
        'UsePotionOfLifeController': ('1fc240c4', 'Двойной тап — +20 к запасу здоровья'),
        'UseBookOfSkillController': ('3230fdd2', 'Двойной тап — выучить умение'),
        'UseScrollOfResistanceController': ('a21bc33d', 'Двойной тап — применить'),
        'UseTokenOfAbsolutionController': ('62799712', 'Двойной тап — сбросить очки характеристик и умений'),
        'UseTerrorTokenAct1Controller': ('767eaea7', 'Двойной тап — ввергнуть ужас в 1-й акт'),
        'UseTerrorTokenAct2Controller': ('7e2b5ab4', 'Двойной тап — ввергнуть ужас во 2-й акт'),
        'UseTerrorTokenAct3Controller': ('cbfd42bc', 'Двойной тап — ввергнуть ужас в 3-й акт'),
        'UseTerrorTokenAct4Controller': ('5839a722', 'Двойной тап — ввергнуть ужас в 4-й акт'),
        'UseTerrorTokenAct5Controller': ('f8700dbe', 'Двойной тап — ввергнуть ужас в 5-й акт'),
        'comparisonTooltipController': ('0d40bbad', ''),
        'hireComparisonTooltipController': ('f42afaf2', ''),
        'StrYouDiedConsole': ('2bf9961e', 'Вы погибли'),
        'ControllerInteractButton': ('54234e14', ''),
        'ControllerTitleScreenPrompt': ('1b3b06b3', 'Коснитесь экрана, чтобы начать'),
        'EnterCharacterName': ('74790c9d', ''),
        'ControllerCycleComparisonTooltips': ('3f128d2b', 'Коснитесь снова, чтобы сравнить с другим'),
        'SelectButtonNoteText': ('f2040580', ''),
        'MoviePlaybackHoldToSkip_switch': ('08664e40', ''),
    },
    'ui.json': {
        'LevelUpStatsNotificationController': ('96716426', 'Уровень повышен.'),
        'LevelUpNotificationController': ('96716426', 'Уровень повышен.'),
        'QuestLogNotificationController': ('aad0ef25', 'Задание обновлено.'),
        'MoviePlaybackHoldToSkip': ('8f05e751', ''),
    },
}
EXTRA = (('enUS', EN), ('esES', ES), ('esMX', MX), ('ruRU', RU))   # 中文以外改的列 | Columns changed besides Chinese

# 手柄支持：手柄说法 ↔ 触摸说法按模式切，不再改字符串文件——build_sidecar 出旁路表，宿主触屏模式把串表里这几个键的串换成触摸说法、
# 手柄模式换回原文（dm/host/dm_padmode.c）。只有下面这两条是排版用的（大字号，两种模式都要），照旧改文件
# Controller support: controller wording ↔ touch wording is switched by mode and the string files are no longer modified. build_sidecar emits a sidecar table;
# in touch mode the host swaps the strings of these keys in the string table to the touch wording,
# and in controller mode swaps the original text back (dm/host/dm_padmode.c). Only the two entries below are for typesetting (large font size, needed in
# both modes) and still modify the files as before
FILE_ONLY = {'strchratr', 'strchrrat'}

def build_sidecar(romfs):
    """[(键, 原文校验, 触摸说法)]：FILE_ONLY 以外的全部条目；原文校验同 TABLE（宿主按它认串表里的原文，对不上不换）。 | [(key, source checksum, touch wording)]: all entries other than FILE_ONLY; the source checksum is the same as in TABLE (the host uses it to identify the original text in the string table and does not swap on a mismatch)."""
    out = []
    for name, items in TABLE.items():
        path, text = touch_layout.read_layout(romfs, None, suffix='/lng/strings/' + name)
        for key, (old, new) in items.items():
            if key.startswith('MoviePlaybackHoldToSkip') and os.environ.get('MOVIE_FFMPEG') == '1': continue
            if key in FILE_ONLY: continue
            m = re.search(r'"Key":\s*"%s",.*?"zhCN":\s*("(?:[^"\\]|\\.)*")' % re.escape(key), text, re.S)
            if not m: raise SystemExit('touch_strings：%s 里没有键 %s' % (name, key))
            cur = json.loads(m.group(1))
            if hashlib.sha1(cur.encode('utf-8')).hexdigest()[:8] != old: raise SystemExit('touch_strings：%s 的原文变了（校验 %s，现行「%s」）——原包换了版本，核过再改表' % (key, old, cur))
            out.append((key, old, new))
        # 英文 / 西语列：同一批键按列再各出一行，原文校验是那一列的原文。宿主按「串表里此刻的原文」认校验（dm_padmode.c str_apply），
        # 游戏语言是哪一种，只有那一列的行对得上、才换；不用宿主另跟语言。两列原文相同（如 esES / esMX 同一句）出重复行无妨
        # English / Spanish columns: the same set of keys gets one more row per column, with the original-text check against that column's original. The host recognizes the check by "the original text currently in the string table" (dm_padmode.c str_apply);
        # whichever language the game is in, only that column's rows match and get replaced; the host needn't track language separately. Duplicate rows when two columns share the same original (e.g. same sentence in esES / esMX) are harmless
        for lang, tab in EXTRA:
            for key, (old, new) in tab.get(name, {}).items():
                if key.startswith('MoviePlaybackHoldToSkip') and os.environ.get('MOVIE_FFMPEG') == '1': continue
                m = re.search(r'"Key":\s*"%s",.*?"%s":\s*("(?:[^"\\]|\\.)*")' % (re.escape(key), lang), text, re.S)
                if not m: raise SystemExit('touch_strings：%s 里没有键 %s 的 %s' % (name, key, lang))
                cur = json.loads(m.group(1))
                if hashlib.sha1(cur.encode('utf-8')).hexdigest()[:8] != old: raise SystemExit('touch_strings：%s 的 %s 原文变了（校验 %s，现行「%s」）——原包换了版本，核过再改表' % (key, lang, old, cur))
                if (key, old, new) not in out: out.append((key, old, new))   # esES / esMX 原文相同、新文也相同的只留一行 | Keep only one row when esES / esMX have the same original and the same new text
    return out

def build(romfs, outdir):
    """返回 ({包内逻辑路径(小写): 本地文件}, 改了几条)。 | Returns ({logical path in package (lowercase): local file}, number of entries changed)."""
    repl = {}; total = 0
    for name, items in TABLE.items():
        path, text = touch_layout.read_layout(romfs, None, suffix='/lng/strings/' + name)
        if not any(k in FILE_ONLY for k in items): continue
        for key, (old, new) in items.items():
            if key.startswith('MoviePlaybackHoldToSkip') and os.environ.get('MOVIE_FFMPEG') == '1': continue
            if key not in FILE_ONLY: continue
            m = re.search(r'"Key":\s*"%s",.*?"zhCN":\s*("(?:[^"\\]|\\.)*")' % re.escape(key), text, re.S)
            if not m: raise SystemExit('touch_strings：%s 里没有键 %s' % (name, key))
            cur = json.loads(m.group(1))
            if hashlib.sha1(cur.encode('utf-8')).hexdigest()[:8] != old: raise SystemExit('touch_strings：%s 的原文变了（校验 %s，现行「%s」）——原包换了版本，核过再改表' % (key, old, cur))
            text = text[:m.start(1)] + json.dumps(new, ensure_ascii=False) + text[m.end(1):]; total += 1
        o = os.path.join(outdir, 'str_' + name); open(o, 'w', encoding='utf-8-sig', newline='').write(text); repl[path.lower()] = o
    return repl, total
