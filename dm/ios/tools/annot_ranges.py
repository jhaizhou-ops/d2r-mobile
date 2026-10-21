"""给暗金、套装和符文之语名称加关键可变属性范围。 | Add key variable-property ranges to unique, set and runeword names."""
ITEM_FILES = ('item-names.json', 'chinese-overlay.json')


def _excel(romfs, name):
    from annot_strings import excel
    return excel(romfs, name)


STAT_NAMES = {
    'abs-cold': '吸收冰伤', 'abs-cold%': '吸冰伤', 'abs-fire': '吸收火伤', 'abs-fire%': '吸火伤',
    'abs-ltng': '吸电伤', 'abs-ltng%': '吸电伤', 'abs-mag': '吸收魔伤', 'ac': '防御', 'ac%': '强防',
    'ac-miss': '防御', 'addxp': '经验', 'all-stats': '全属性', 'allskills': '全技能', 'ama': '亚马逊',
    'att': '命中', 'att%': '命中', 'att-demon': '对恶魔命中', 'att-skill': '命中技能', 'att-undead': '对亡灵命中',
    'aura': '光环', 'balance1': '打击恢复', 'balance2': '打击恢复', 'balance3': '打击恢复', 'block': '格挡',
    'bloody': '撕裂', 'cast1': '施法速度', 'cast2': '施法速度', 'cast3': '施法速度', 'cheap': '商店折扣',
    'cold-len': '冰冻时长', 'cold-max': '冰伤上限', 'cold-min': '冰伤下限', 'crush': '压碎打击',
    'death-skill': '死亡技能', 'demon-heal': '对恶魔回复', 'dex': '敏捷', 'dmg': '伤害', 'dmg%': '强伤',
    'dmg-cold': '冰伤', 'dmg-demon': '对恶魔伤', 'dmg-elem': '元素伤', 'dmg-fire': '火伤', 'dmg-ltng': '电伤',
    'dmg-mag': '魔伤', 'dmg-max': '伤害上限', 'dmg-min': '伤害下限', 'dmg-norm': '伤害', 'dmg-undead': '对亡灵伤',
    'dmg-pois': '毒伤', 'dru': '德鲁伊', 'dur': '耐久', 'enr': '精力', 'extra-cold': '冰伤加成',
    'extra-fire': '火伤加成', 'extra-ltng': '电伤加成', 'extra-mag': '魔伤加成', 'extra-pois': '毒伤加成',
    'fire-max': '火伤上限', 'fire-min': '火伤下限', 'freeze': '冻结', 'gold%': '金币', 'gethit-skill': '受击技能',
    'heal-kill': '击杀回复', 'hit-skill': '击中技能', 'hp': '生命', 'hp%': '生命', 'kill-skill': '击杀技能',
    'levelup-skill': '升级技能', 'lifesteal': '吸血', 'light': '照亮范围', 'light-thorns': '荆棘', 'ltng-max': '电伤上限',
    'mag%': '寻宝', 'mana': '法力', 'mana%': '法力', 'mana-kill': '击杀回蓝', 'manasteal': '吸蓝',
    'move1': '跑速', 'move2': '跑速', 'move3': '跑速', 'nec': '死灵法师', 'oskill': '技能',
    'pal': '圣骑士', 'pierce-cold': '冰抗穿透', 'pierce-fire': '火抗穿透', 'pierce-ltng': '电抗穿透',
    'pierce-mag': '魔抗穿透', 'pierce-pois': '毒抗穿透', 'randclassskill': '随机技能', 'red-dmg': '减伤',
    'red-dmg%': '减伤', 'red-mag': '魔法减伤', 'regen': '生命回复', 'regen-mana': '法力回复', 'res-all': '全抗',
    'res-cold': '冰抗', 'res-fire': '火抗', 'res-ltng': '电抗', 'res-mag': '魔抗', 'res-pois': '毒抗',
    'skill': '技能', 'skill-rand': '随机技能', 'skilltab': '技能页', 'sock': '孔', 'sor': '女巫', 'stam': '耐力',
    'str': '力量', 'swing1': '攻速', 'swing2': '攻速', 'swing3': '攻速', 'thorns': '荆棘', 'vit': '体力',
    'charged': '技能', '*charged': '技能',
}

# 西语列的属性简称（esES / esMX 共用；用词照原版 item-modifiers 的西语说法缩写）
# Stat abbreviations for the Spanish columns (shared by esES / esMX; abbreviated from the original Spanish wording in item-modifiers)
STAT_NAMES_ES = {
    'abs-cold': 'Abs. frío', 'abs-cold%': 'Abs. frío', 'abs-fire': 'Abs. fuego', 'abs-fire%': 'Abs. fuego',
    'abs-ltng': 'Abs. rayo', 'abs-ltng%': 'Abs. rayo', 'abs-mag': 'Abs. magia', 'ac': 'Def', 'ac%': 'Def. mejorada',
    'ac-miss': 'Def', 'addxp': 'Exp', 'all-stats': 'Atributos', 'allskills': 'Habilidades', 'ama': 'Amazona',
    'att': 'Ataque', 'att%': 'Ataque', 'att-demon': 'Ataque vs. demonios', 'att-skill': 'Hab. al atacar', 'att-undead': 'Ataque vs. no muertos',
    'aura': 'Aura', 'balance1': 'Recup. golpe', 'balance2': 'Recup. golpe', 'balance3': 'Recup. golpe', 'block': 'Bloqueo',
    'bloody': 'Heridas abiertas', 'cast1': 'Vel. lanz.', 'cast2': 'Vel. lanz.', 'cast3': 'Vel. lanz.', 'cheap': 'Descuento',
    'cold-len': 'Dur. frío', 'cold-max': 'Frío máx.', 'cold-min': 'Frío mín.', 'crush': 'Golpe aplastante',
    'death-skill': 'Hab. al morir', 'demon-heal': 'Vida vs. demonios', 'dex': 'Destreza', 'dmg': 'Daño', 'dmg%': 'Daño mejorado',
    'dmg-cold': 'Daño de frío', 'dmg-demon': 'Daño vs. demonios', 'dmg-elem': 'Daño elem.', 'dmg-fire': 'Daño de fuego', 'dmg-ltng': 'Daño de rayo',
    'dmg-mag': 'Daño mág.', 'dmg-max': 'Daño máx.', 'dmg-min': 'Daño mín.', 'dmg-norm': 'Daño', 'dmg-undead': 'Daño vs. no muertos',
    'dmg-pois': 'Daño de veneno', 'dru': 'Druida', 'dur': 'Durabilidad', 'enr': 'Energía', 'extra-cold': 'Hab. de frío',
    'extra-fire': 'Hab. de fuego', 'extra-ltng': 'Hab. de rayo', 'extra-mag': 'Hab. mágicas', 'extra-pois': 'Hab. de veneno',
    'fire-max': 'Fuego máx.', 'fire-min': 'Fuego mín.', 'freeze': 'Congela', 'gold%': 'Oro', 'gethit-skill': 'Hab. al recibir golpe',
    'heal-kill': 'Vida al matar', 'hit-skill': 'Hab. al golpear', 'hp': 'Vida', 'hp%': 'Vida', 'kill-skill': 'Hab. al matar',
    'levelup-skill': 'Hab. al subir de nivel', 'lifesteal': 'Robo de vida', 'light': 'Radio de luz', 'light-thorns': 'Espinas', 'ltng-max': 'Rayo máx.',
    'mag%': 'Objetos mágicos', 'mana': 'Maná', 'mana%': 'Maná', 'mana-kill': 'Maná al matar', 'manasteal': 'Robo de maná',
    'move1': 'Vel. carrera', 'move2': 'Vel. carrera', 'move3': 'Vel. carrera', 'nec': 'Nigromante', 'oskill': 'Habilidad',
    'pal': 'Paladín', 'pierce-cold': 'Pen. frío', 'pierce-fire': 'Pen. fuego', 'pierce-ltng': 'Pen. rayo',
    'pierce-mag': 'Pen. magia', 'pierce-pois': 'Pen. veneno', 'randclassskill': 'Hab. aleatoria', 'red-dmg': 'Red. daño',
    'red-dmg%': 'Red. daño', 'red-mag': 'Red. daño mág.', 'regen': 'Regen. vida', 'regen-mana': 'Regen. maná', 'res-all': 'Res. todas',
    'res-cold': 'Res. frío', 'res-fire': 'Res. fuego', 'res-ltng': 'Res. rayo', 'res-mag': 'Res. magia', 'res-pois': 'Res. veneno',
    'skill': 'Habilidad', 'skill-rand': 'Hab. aleatoria', 'skilltab': 'Árbol de hab.', 'sock': 'Engarces', 'sor': 'Hechicera', 'stam': 'Aguante',
    'str': 'Fuerza', 'swing1': 'Vel. ataque', 'swing2': 'Vel. ataque', 'swing3': 'Vel. ataque', 'thorns': 'Espinas', 'vit': 'Vitalidad',
    'charged': 'Habilidad', '*charged': 'Habilidad',
}
# 按语言列：(属性简称表, 技能名最长几个字符才直接写名字, 写不下时的通称, 简称与数字之间的分隔)
# Per language column: (stat abbreviation table, max characters of a skill name to write it directly, generic term when it doesn't fit, separator between abbreviation and number)
RANGE_WORDS = {'zhCN': (STAT_NAMES, 4, '技能', ''), 'esES': (STAT_NAMES_ES, 16, 'Habilidad', ' '), 'esMX': (STAT_NAMES_ES, 16, 'Habilidad', ' ')}

# 只有「min..max 里随机取一个值」的属性才是可变范围：看 properties.txt 的 func1。
# 1 / 2 / 5 / 6 / 7 / 8 / 10 / 14 / 22 = 随机取值（数值、伤害下限 / 上限 / 百分比、速度、技能页、孔数、单个技能等级）；
# 15 = min 给下限属性、max 给上限属性（如「冰伤 15-45」是固定的伤害区间）；11 / 19 = 触发 / 充能（几率或次数 + 技能等级）；17 / 18 = 按等级 / 按时间。这些都不写。
# Only properties that "pick a random value within min..max" are variable ranges: look at func1 in properties.txt.
# 1 / 2 / 5 / 6 / 7 / 8 / 10 / 14 / 22 = random value (number, damage min / max / percent, speed, skill tab, socket count, single skill level);
# 15 = min goes to the minimum property and max to the maximum property (e.g. 「冰伤 15-45」(cold damage 15-45) is a fixed damage range); 11 / 19 = proc / charges (chance or count + skill level); 17 / 18 = per level / per time. None of these are written.
RANDOM_FUNCS = {'1', '2', '5', '6', '7', '8', '10', '14', '22'}
SKILL_CODES = {'skill', 'oskill', 'aura', 'skilltab', 'charged', '*charged', 'hit-skill', 'gethit-skill',
               'kill-skill', 'death-skill', 'levelup-skill', 'att-skill', 'skill-rand'}


def _skill_names(s, romfs, maxlen, generic):
    names = {}
    for row in _excel(romfs, 'skills.txt'):
        skill_id = row.get('*Id', '').strip()
        skill = row.get('skill', '').strip()
        if skill_id and skill:
            value = s.get('skills.json', 'skillname' + skill_id)
            names[skill] = value if value and len(value) <= maxlen else generic
    return names


def _label(code, param, skills, stat_names=STAT_NAMES, generic='技能'):
    label = stat_names.get(code, stat_names.get(code.lstrip('*')))
    if code in SKILL_CODES or code.lstrip('*') in SKILL_CODES:
        if param:
            return skills.get(param, generic) + '+'
        return generic
    return label or code


def _range_line(rows, code_prefix, min_prefix, max_prefix, slots, skills, props, stat_names=STAT_NAMES, generic='技能', sep=''):
    parts = []
    for i in slots:
        code = rows.get(code_prefix.format(i), '').strip()
        if not code:
            continue
        minimum = rows.get(min_prefix.format(i), '').strip()
        maximum = rows.get(max_prefix.format(i), '').strip()
        if props.get(code.lstrip('*'), {}).get('func1') not in RANDOM_FUNCS or minimum == maximum or not minimum or not maximum:
            continue
        label = _label(code, rows.get(('par' + str(i)) if code_prefix.startswith('prop') else ('T1Param' + str(i)), '').strip(), skills, stat_names, generic)
        suffix = '%' if '%' in code else ''
        prefix = '+' if code == 'ac' else ''
        parts.append('%s%s%s%s-%s%s' % (label, '' if label.endswith('+') else sep, prefix, minimum, maximum, suffix))
        if len(parts) == 3:
            break
    return (sep + '/' + sep).join(parts)


def _annotate(s, key, line, color, files=ITEM_FILES):
    """名字下方加灰色范围行。串里的颜色码从写下处一直生效到被下一个码改掉（只在前面加 ÿc5 时名字也变灰），
    所以名字行前补回它本来的颜色：暗金 / 符文之语 ÿc4、套装 ÿc2。
    Add a gray range line below the name. A color code in the string stays in effect from where it is written until the next code changes it (adding only ÿc5 in front would turn the name gray too),
    so the name line gets its original color restored in front: unique / runeword ÿc4, set ÿc2."""
    if not line:
        return 0
    def add(text):
        return text if text.startswith('\u00ffc5[') else '\u00ffc5[%s]\n%s%s' % (line, color, text)
    n = 0
    for filename in files:
        n += s.set(filename, key, add)
    return n


def ann_ranges(s, romfs):
    """按表中原顺序给可变属性加范围，返回改写条数。 | Add ranges to variable properties in the table's original order; returns the number of rewritten entries."""
    stat_names, maxlen, generic, sep = RANGE_WORDS[s.col]
    skills = _skill_names(s, romfs, maxlen, generic)
    props = {r['code']: r for r in _excel(romfs, 'properties.txt') if r.get('code')}
    changed = 0
    for row in _excel(romfs, 'uniqueitems.txt'):
        if row.get('disabled') == '1' or row.get('spawnable') != '1':
            continue
        changed += _annotate(s, row.get('index', ''), _range_line(row, 'prop{}', 'min{}', 'max{}', range(1, 13), skills, props, stat_names, generic, sep), '\u00ffc4')
    for row in _excel(romfs, 'setitems.txt'):
        changed += _annotate(s, row.get('index', ''), _range_line(row, 'prop{}', 'min{}', 'max{}', range(1, 9), skills, props, stat_names, generic, sep), '\u00ffc2')
    for row in _excel(romfs, 'runes.txt'):
        if row.get('complete') != '1':
            continue
        changed += _annotate(s, row.get('Name', ''), _range_line(row, 'T1Code{}', 'T1Min{}', 'T1Max{}', range(1, 8), skills, props, stat_names, generic, sep), '\u00ffc4', ('item-runes.json', 'chinese-overlay.json'))
    return changed
