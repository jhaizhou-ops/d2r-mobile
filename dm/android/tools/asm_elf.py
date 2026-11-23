# 苹果（Mach-O）汇编 → 安卓（ELF）汇编：宿主的 .S 只写一份苹果写法（含生成的 nvn_thunks.S），安卓构建时过一遍本脚本再汇编。
# 用法：asm_elf.py <输入.S> <输出.S>
# 除下面最后一条外只做语法层的一一对应，不改任何指令：
#   C 符号的下划线前缀：_name → name（Mach-O 给 C 符号加 _，ELF 不加）
#   adrp / add 的页寻址：sym@PAGE → sym，sym@PAGEOFF → :lo12:sym
#   局部标号：本文件定义的 Lxxx → .Lxxx（ELF 的局部标号前缀）
#   .cstring → 只读字符串段；.private_extern → .hidden；文件末尾补不可执行栈声明
#   唯一改指令的一条：LSE 原子 stadd Xs, [Xn] → ldxr / add / stxr / cbnz 重试循环（#293）。苹果芯片都有 LSE，安卓的
#   ARMv8.0 核（Cortex-A53 / A73 一类：骁龙 680 / 665 / 662 / 460 等）没有，执行 stadd 就是非法指令（SIGILL），而每个 NVN
#   调用都经过 nvn_thunks.S 里带 stadd 的计数跳板，游戏一调图形接口就崩。循环在所有 ARMv8 上都能跑，计数照样是原子的。
#   临时寄存器用 x14 / w15：调用约定里是调用方保存的，跳板转去的 C 实现本来就会改它们
# Apple (Mach-O) assembly → Android (ELF) assembly: the host's .S files are written once in Apple syntax (including the generated nvn_thunks.S), and the Android build runs them through this script before assembling.
# Usage: asm_elf.py <input.S> <output.S>
# Apart from the last item below, only one-to-one syntax mapping, no instruction is changed:
#   underscore prefix of C symbols: _name → name (Mach-O prefixes C symbols with _, ELF does not)
#   page addressing for adrp / add: sym@PAGE → sym, sym@PAGEOFF → :lo12:sym
#   local labels: Lxxx defined in this file → .Lxxx (the ELF local label prefix)
#   .cstring → read-only string section; .private_extern → .hidden; a non-executable stack note is appended at the end of the file
#   The one instruction change: the LSE atomic stadd Xs, [Xn] → an ldxr / add / stxr / cbnz retry loop (#293). Every Apple chip has
#   LSE, but Android's ARMv8.0 cores (Cortex-A53 / A73 class: Snapdragon 680 / 665 / 662 / 460 and others) do not, so stadd is an
#   illegal instruction (SIGILL) there, and every NVN call goes through the counting thunks in nvn_thunks.S that use it: the game
#   crashed on its first graphics call. The loop runs on every ARMv8 CPU and the count stays atomic. Scratch registers are x14 / w15:
#   caller-saved in the calling convention, and the C implementations the thunks jump to clobber them anyway
import re, sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding='utf-8').read()
locals_ = set(re.findall(r'^\s*(L\w+):', text, re.M))
out = []
for line in text.split('\n'):
    code, sep, comment = line.partition('//')
    code = re.sub(r'(\w+)@PAGEOFF\b', r':lo12:\1', code)
    code = re.sub(r'(\w+)@PAGE\b', r'\1', code)
    code = re.sub(r'(?<![\w.$])_(\w+)', r'\1', code)
    if locals_: code = re.sub(r'(?<![\w.])(L\w+)\b', lambda m: '.' + m.group(1) if m.group(1) in locals_ else m.group(1), code)
    code = re.sub(r'^(\s*)\.cstring\b', r'\1.section .rodata.str1.1,"aMS",@progbits,1', code)
    code = re.sub(r'^(\s*)\.private_extern\b', r'\1.hidden', code)
    if re.search(r'\bstadd\b', code):
        m = re.fullmatch(r'(\s*)stadd\s+(x\d+)\s*,\s*\[\s*(x\d+)\s*\]\s*', code)
        # 只认 64 位、无偏移的写法（生成的跳板与 nvn_entry.S 都是这样）；别的写法或撞上临时寄存器就停下，不悄悄放过
        # Only the 64-bit form without an offset is handled (what the generated thunks and nvn_entry.S use); any other form,
        # or one that uses the scratch registers, stops the build instead of slipping through
        if not m or {m.group(2), m.group(3)} & {'x14', 'x15'}:
            sys.exit('asm_elf.py: %s: cannot rewrite "%s" for ARMv8.0' % (src, line.strip()))
        ind, xs, xn = m.groups()
        out += ['%s1: ldxr x14, [%s]%s%s' % (ind, xn, sep, comment), '%sadd x14, x14, %s' % (ind, xs),
                '%sstxr w15, x14, [%s]' % (ind, xn), '%scbnz w15, 1b' % ind]
        continue
    out.append(code + sep + comment)
out.append('    .section .note.GNU-stack,"",@progbits')
open(dst, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
