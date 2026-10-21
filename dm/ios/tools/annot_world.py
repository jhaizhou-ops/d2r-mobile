#!/usr/bin/env python3
"""区域、怪物、神殿名称注释。 | Annotations for area, monster and shrine names."""
from collections import defaultdict

def _number(row, key):
    value = row.get(key, '')
    return int(value or 0)


def _groups(rows, key, fields):
    groups = defaultdict(list)
    for row in rows:
        if row.get(key):
            groups[row[key]].append(tuple(row.get(field, '') for field in fields))
    return groups


def ann_levels(s, romfs):
    from annot_strings import excel

    fields = ('MonLvlEx', 'MonLvlEx(N)', 'MonLvlEx(H)')
    groups = _groups(excel(romfs, 'levels.txt'), 'LevelName', fields)
    skipped = []
    changed = 0
    for key, values in groups.items():
        unique = set(values)
        if len(unique) > 1:
            skipped.append(key)
            continue
        levels = tuple(_number(dict(zip(fields, values[0])), field) for field in fields)
        if levels == (0, 0, 0):
            continue
        fmt = '%s[%d/%d/%d]' if s.col == 'zhCN' else '%s [%d/%d/%d]'   # 西语名字与括号之间空一格 | One space between a Spanish name and the parenthesis
        changed += s.set('levels.json', key, lambda text, levels=levels, fmt=fmt: fmt % (text, *levels))
    if skipped and s.col == 'zhCN':   # 每个语言列都会跳过同一批键，只在中文列那遍报一次 | Every language column skips the same set of keys; report only once, on the Chinese column pass
        print('ann_levels：同键多值，跳过：%s' % '、'.join(sorted(skipped)))
    return changed


# 神殿效果的措辞（按语言列）。效果号 = shrines.txt 的 Code；%s 依次填 Arg0、Arg1。西语按原版 esES / esMX 各自的用词（rayos / rayo、aguante / vigor、ciudad / poblado）
# Shrine effect wording (per language column). Effect number = Code in shrines.txt; %s filled with Arg0, Arg1 in order. Spanish follows the original esES / esMX wording respectively (rayos / rayo, aguante / vigor, ciudad / poblado)
_ES_SHRINE = {
    '1': 'Vida y maná al máximo', '2': 'Vida al máximo', '3': 'Maná al máximo',
    '4': 'Convierte el %s%% de la vida actual en maná, hasta %s', '5': 'Convierte el %s%% del maná actual en vida, hasta %s',
    '6': '+%s%% de defensa', '7': '+%s%% de índice de ataque, +%s%% de daño',
    '8': '+%s%% de resistencia al fuego', '9': '+%s%% de resistencia al frío', '10': '+%s%% de resistencia a los rayos', '11': '+%s%% de resistencia al veneno',
    '12': '+%s a todas las habilidades', '13': '+%s%% de regeneración de maná', '14': 'Aguante infinito', '15': '+%s%% de experiencia',
    '16': 'Invierte el nombre del personaje', '17': 'Abre un portal a la ciudad',
    '18': 'Mejora una gema al azar del inventario; sin gemas, crea una de la calidad más baja',
    '19': 'Estalla en llamas: todos los que están cerca pierden el %s%% de su vida', '20': 'Convierte al monstruo más cercano en campeón o élite',
    '21': 'Recibes %s-%s pociones explosivas y estallan explosiones alrededor', '22': 'Recibes %s-%s pociones de gas asfixiante y se forma una nube venenosa'}
SHRINE_WORDS = {
    'zhCN': {'effect': {
        '1': '生命和法力全满', '2': '生命全满', '3': '法力全满',
        '4': '用当前生命的 %s%% 恢复法力，最多 %s', '5': '用当前法力的 %s%% 恢复生命，最多 %s',
        '6': '+%s%% 防御', '7': '+%s%% 准确率，+%s%% 伤害',
        '8': '+%s%% 抗火', '9': '+%s%% 抗冰', '10': '+%s%% 抗电', '11': '+%s%% 抗毒',
        '12': '+%s 所有技能', '13': '+%s%% 法力重生', '14': '无限耐力', '15': '经验+%s%%',
        '16': '颠倒角色名称', '17': '生成一个回家的传送门',
        '18': '随机把背包中的一颗宝石提升一级；没有宝石则生成一颗最低级宝石',
        '19': '爆发火焰，使周围所有单位失去 %s%% 生命', '20': '使最近的怪物升级为精英或冠军怪',
        '21': '获得 %s-%s 个爆炸药剂，并使周围产生爆炸', '22': '获得 %s-%s 个窒息气体药剂，并在周围产生毒雾'},
        'sec': '%d 秒', 'reset': '刷新时间 %s 分钟', 'noreset': '不会刷新', 'dur': '持续 %s', 'sep': '，'},
    'esES': {'effect': _ES_SHRINE, 'sec': '%d s', 'reset': 'se recarga en %s min', 'noreset': 'no se recarga', 'dur': 'dura %s', 'sep': ', '},
    'esMX': {'effect': dict(_ES_SHRINE, **{'10': '+%s%% de resistencia al rayo', '14': 'Vigor infinito', '17': 'Abre un portal al poblado'}),
             'sec': '%d s', 'reset': 'se recarga en %s min', 'noreset': 'no se recarga', 'dur': 'dura %s', 'sep': ', '}}


def _duration(row, w):
    frames = _number(row, 'Duration in frames')
    return w['sec'] % (frames // 25) if frames else ''


def _reset(row, w):
    value = row.get('reset time in minutes', '')
    return w['reset'] % value if value else w['noreset']


def _shrine_effect(row, w):
    code = row['Code']
    args = tuple(a for a in (row.get('Arg0', ''), row.get('Arg1', '')))
    duration, reset = _duration(row, w), _reset(row, w)
    fmt = w['effect'].get(code)
    effect = fmt % args[:fmt.count('%s')] if fmt else row['*Effect']
    details = [effect]
    if reset:
        details.append(reset)
    if duration:
        details.append(w['dur'] % duration)
    return w['sep'].join(details)


def ann_shrines(s, romfs):
    from annot_strings import excel

    changed = 0
    for row in excel(romfs, 'shrines.txt'):
        key = row.get('StringName')
        if not key or key == 'ShrId0' or '(Not Used)' in row.get('*Effect', ''):   # 表里标了不用的（生命 / 法力交换、颠倒名字）游戏不生成，不加 | Those marked unused in the table (life / mana exchange, reversed names) are not generated by the game; not added
            continue
        effect = _shrine_effect(row, SHRINE_WORDS[s.col])
        changed += s.set('shrines.json', key, lambda text, effect=effect: 'ÿc5%s\nÿc0%s' % (effect, text))
    return changed
