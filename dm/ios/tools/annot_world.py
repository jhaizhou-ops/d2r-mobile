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
        changed += s.set('levels.json', key, lambda text, levels=levels: '%s[%d/%d/%d]' % (text, *levels))
    if skipped:
        print('ann_levels：同键多值，跳过：%s' % '、'.join(sorted(skipped)))
    return changed


def _duration(row):
    frames = _number(row, 'Duration in frames')
    return '%d 秒' % (frames // 25) if frames else ''


def _reset(row):
    value = row.get('reset time in minutes', '')
    return '刷新时间 %s 分钟' % value if value else '不会刷新'


def _shrine_effect(row):
    code = row['Code']
    arg0, arg1 = row.get('Arg0', ''), row.get('Arg1', '')
    duration, reset = _duration(row), _reset(row)
    if code == '1':
        effect = '生命和法力全满'
    elif code == '2':
        effect = '生命全满'
    elif code == '3':
        effect = '法力全满'
    elif code == '4':
        effect = '用当前生命的 %s%% 恢复法力，最多 %s' % (arg0, arg1)
    elif code == '5':
        effect = '用当前法力的 %s%% 恢复生命，最多 %s' % (arg0, arg1)
    elif code == '6':
        effect = '+%s%% 防御' % arg0
    elif code == '7':
        effect = '+%s%% 准确率，+%s%% 伤害' % (arg0, arg1)
    elif code in ('8', '9', '10', '11'):
        effect = '+%s%% %s' % (arg0, {'8': '抗火', '9': '抗冰', '10': '抗电', '11': '抗毒'}[code])
    elif code == '12':
        effect = '+%s 所有技能' % arg0
    elif code == '13':
        effect = '+%s%% 法力重生' % arg0
    elif code == '14':
        effect = '无限耐力'
    elif code == '15':
        effect = '经验+%s%%' % arg0
    elif code == '16':
        effect = '颠倒角色名称'
    elif code == '17':
        effect = '生成一个回家的传送门'
    elif code == '18':
        effect = '随机把背包中的一颗宝石提升一级；没有宝石则生成一颗最低级宝石'
    elif code == '19':
        effect = '爆发火焰，使周围所有单位失去 %s%% 生命' % arg0
    elif code == '20':
        effect = '使最近的怪物升级为精英或冠军怪'
    elif code == '21':
        effect = '获得 %s-%s 个爆炸药剂，并使周围产生爆炸' % (arg0, arg1)
    elif code == '22':
        effect = '获得 %s-%s 个窒息气体药剂，并在周围产生毒雾' % (arg0, arg1)
    else:
        effect = row['*Effect']
    details = [effect]
    if reset:
        details.append(reset)
    if duration:
        details.append('持续 %s' % duration)
    return '，'.join(details)



def _spanish_shrine_effect(row):
    effects = {
        '1': 'Restaura toda la vida y el maná',
        '2': 'Restaura toda la vida',
        '3': 'Restaura todo el maná',
        '4': 'Convierte el {a}% de vida en maná, hasta {b}',
        '5': 'Convierte el {a}% de maná en vida, hasta {b}',
        '6': '+{a}% de defensa',
        '7': '+{a}% de ataque y +{b}% de daño',
        '8': '+{a}% de resistencia al fuego',
        '9': '+{a}% de resistencia al frío',
        '10': '+{a}% de resistencia a los rayos',
        '11': '+{a}% de resistencia al veneno',
        '12': '+{a} a todas las habilidades',
        '13': '+{a}% de regeneración de maná',
        '14': 'Resistencia ilimitada',
        '15': '+{a}% de experiencia',
        '16': 'Invierte el nombre del personaje',
        '17': 'Crea un portal al pueblo',
        '18': 'Mejora una gema del inventario al azar. Si no hay gemas, crea una fragmentada',
        '19': 'Fuego: las unidades cercanas pierden el {a}% de vida',
        '20': 'Convierte al monstruo más cercano en único o campeón',
        '21': 'Crea {a}-{b} pociones explosivas y explosiones cercanas',
        '22': 'Crea {a}-{b} pociones de gas asfixiante y nubes de veneno',
    }
    code = row['Code']
    if code not in effects:
        raise SystemExit('annot_world: falta efecto español del santuario ' + code)
    details = [effects[code].format(a=row.get('Arg0', ''), b=row.get('Arg1', ''))]
    reset = row.get('reset time in minutes', '')
    details.append('reaparece en %s min' % reset if reset else 'no reaparece')
    frames = _number(row, 'Duration in frames')
    if frames:
        details.append('duración %d s' % (frames // 25))
    return ', '.join(details)

def ann_shrines(s, romfs):
    from annot_strings import excel

    changed = 0
    for row in excel(romfs, 'shrines.txt'):
        key = row.get('StringName')
        if not key or key == 'ShrId0' or '(Not Used)' in row.get('*Effect', ''):   # 表里标了不用的（生命 / 法力交换、颠倒名字）游戏不生成，不加 | Those marked unused in the table (life / mana exchange, reversed names) are not generated by the game; not added
            continue
        effect = _spanish_shrine_effect(row) if s.spanish else _shrine_effect(row)
        if s.spanish:
            from annot_strings import annotation_lines
            effect = annotation_lines(effect)
        changed += s.set('shrines.json', key, lambda text, effect=effect: 'ÿc5%s\nÿc0%s' % (effect, text))
    return changed
