#!/usr/bin/env python3
"""手柄 / 触屏模式的登记闸门（宿主单测里跑，秒级）。只读源码，不跑游戏。
三条：
1. dm_cursor.c 每帧那一长行里的每个泵，要么带「if (tm) 」（触屏专属：手柄模式与切换进行中不跑），要么在它上方注释「两种模式都跑：」的单子上；
   文件里定义的每个 *_pump 也必须在这一行或那张单子上出现——新加一个泵、两边都不登记就红。
2. 散判模式（dm_padmode_current()）与替玩家按键 / 发消息（fe_key / ui_auto_key / fe_send / dm_ui_key_pulse）的处数按文件记在下面两张表里，只许减不许加。
   要加：先想清楚它在另一种模式下该停、照常还是换做法，登记进 dm_mode_items.h（要随切换清理的）或上面那张单子，再改这里的数并在提交说明里写原因。
3. dm_mode_items.h 里登记的进 / 出函数都得有定义。
用法：mode_scan.py <dm 目录>
Registration gate for controller / touch modes (run in the host unit tests, takes seconds). Reads source only, does not run the game.
Three rules:
1. Every pump on the long per-frame line in dm_cursor.c either carries "if (tm) " (touch-only: not run in controller mode or while a switch is in progress) or is on the list in the comment above it headed 「两种模式都跑：」(runs in both modes:);
   every *_pump defined in the file must also appear on this line or on that list -- a newly added pump registered in neither fails.
2. The number of places that check the mode ad hoc (dm_padmode_current()) and that press keys / send messages on the player's behalf (fe_key / ui_auto_key / fe_send / dm_ui_key_pulse) is recorded per file in the two tables below, and may only decrease, never increase.
   To add one: first think through whether in the other mode it should stop, carry on as usual, or work differently, register it in dm_mode_items.h (for things to be cleaned up on a switch) or on the list above, then change the number here and state the reason in the commit message.
3. Every enter / leave function registered in dm_mode_items.h must have a definition.
Usage: mode_scan.py <dm directory>
"""
import re, sys, os, glob
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..')
red = []
def src(rel): return open(os.path.join(root, rel), encoding='utf-8').read()
def strip_comments(s): return re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', s, flags=re.S))

# ---- 1 ----
cur = src('host/dm_cursor.c')
m = re.search(r'^.*int tm = ui_touch_mode\(\);.*$', cur, re.M)
if not m: red.append('dm_cursor.c 里找不到每帧那一行（int tm = ui_touch_mode();）')
else:
    line = m.group(0)
    both_txt = re.search(r'两种模式都跑：(.*?)\nstatic int ui_touch_mode', cur, re.S)
    both = set(re.findall(r'\b(\w+_pump|ui_map_center|ui_panel_heal)\b', both_txt.group(1))) if both_txt else set()
    if not both: red.append('dm_cursor.c 里找不到「两种模式都跑：」那张单子')
    on_line = {}
    for mm in re.finditer(r'(if \(tm\) )?\b(\w+_pump|ui_map_center)\(', line): on_line[mm.group(2)] = bool(mm.group(1))
    for name, gated in sorted(on_line.items()):
        if not gated and name not in both: red.append('每帧那一行里的 %s 既没带 if (tm)、也不在「两种模式都跑」单子上' % name)
        if gated and name in both: red.append('%s 带了 if (tm) 又列在「两种模式都跑」单子上，二选一' % name)
    for name in sorted(set(re.findall(r'^static void (\w+_pump)\(', cur, re.M))):
        if name not in on_line and name not in both: red.append('dm_cursor.c 定义了 %s，但每帧那一行和「两种模式都跑」单子上都没有它' % name)

# ---- 2 ----
PADMODE = {'host/dm_cursor.c': 9, 'host/dm_custom.c': 2, 'host/dm_kills.c': 2, 'host/dm_rift_ui.c': 4, 'host/dm_runeword.c': 3, 'host/dm_vault_ui.c': 1, 'host/dm_update.c': 2, 'host/hle_tact.c': 1}
AUTOKEY = {'host/dm_cursor.c': 45, 'host/dm_rift_ui.c': 8, 'host/hle_hid.c': 2, 'ios/Sources/front_ios.m': 4}
def count(pat, skip_defs):
    out = {}
    for f in sorted(glob.glob(os.path.join(root, 'host', '*.c')) + glob.glob(os.path.join(root, 'ios', 'Sources', '*.m')) + glob.glob(os.path.join(root, 'ios', 'Sources', '*.c'))):
        rel = os.path.relpath(f, root); s = strip_comments(open(f, encoding='utf-8').read()); n = 0
        for mm in re.finditer(pat, s):
            ls = s.rfind('\n', 0, mm.start()) + 1; head = s[ls:mm.start()]
            if skip_defs and re.search(r'(^|\s)(static\s+)?(void|int|uint64_t)\s+$', head): continue   # 函数定义 / 声明本身不算调用 | A function definition / declaration itself does not count as a call
            n += 1
        if n: out[rel] = n
    return out
for label, table, got in (('散判模式 dm_padmode_current()', PADMODE, count(r'\bdm_padmode_current\(\)', True)),
                          ('替玩家按键 / 发消息', AUTOKEY, count(r'\b(fe_key|ui_auto_key|fe_send|dm_ui_key_pulse|dm_ui_key_pulse_alone|dm_ui_send|dm_ui_send_queue)\(', True))):
    if label.startswith('散判') and 'host/dm_padmode.c' in got: got.pop('host/dm_padmode.c')
    for rel in sorted(set(got) | set(table)):
        g, t = got.get(rel, 0), table.get(rel, 0)
        if g > t: red.append('%s：%s 现有 %d 处，表里记的是 %d 处——新增的先登记（见本文件开头），再改表' % (label, rel, g, t))
        elif g < t: red.append('%s：%s 现有 %d 处，比表里的 %d 少了——把表里的数改小（只许减）' % (label, rel, g, t))

# ---- 3 ----
items = src('host/dm_mode_items.h'); allsrc = ''.join(open(f, encoding='utf-8').read() for f in glob.glob(os.path.join(root, 'host', '*.c')))
for name, side, enter, leave in re.findall(r'X\("([^"]+)",\s*(\d),\s*(\w+),\s*(\w+)\)', items):
    for fn in (enter, leave):
        if not re.search(r'\bvoid %s\(void\)\s*\{' % fn, allsrc): red.append('dm_mode_items.h 登记项「%s」的函数 %s 没有定义' % (name, fn))

if red:
    print('模式登记闸门 红：%d 条' % len(red)); [print('  ' + r) for r in red]; sys.exit(1)
print('✓ 模式登记闸门：每帧泵 %d 个都已分类，散判模式与替按键的处数没有超出登记，登记项函数齐全' % (len(on_line) if m else 0))
