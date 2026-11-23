# 构建后检查：安卓库里不许有 ARMv8.0 之后才有的指令（#293）。
# 用法：check_armv80.py <llvm-objdump> <libdm.so>
# 骁龙 680 / 665 / 662 / 460 这类 ARMv8.0 核（Cortex-A53 / A73）上，这些指令就是非法指令（SIGILL），而我们两台测试机和苹果芯片都有它们，
# 测试时永远发现不了。查的是：LSE 原子（cas / casp / swp / ld<op> / st<op>）、RCpc 读写（ldapr / ldapur / stlur）、点积（sdot / udot）。
# 放行运行时先查 CPU 再选用的函数（名单 RUNTIME_SELECTED，逐个核过选法才加）：compiler-rt 的 __aarch64_* 外联原子函数（查 LSE，
# 没有就走 ldxr / stxr）；libdeflate 的 adler32_arm_neon_dotprod（arm/adler32_impl.h：只在 CPU 报点积 HWCAP_ASIMDDP 时选它）。
# 发现一处就列出所在函数和指令、构建失败；改法见 asm_elf.py（汇编里的 stadd）或去掉相应的 -march / -mcpu 选项。
# Post-build check: the Android library must contain no instruction newer than ARMv8.0 (#293).
# Usage: check_armv80.py <llvm-objdump> <libdm.so>
# On ARMv8.0 cores such as Snapdragon 680 / 665 / 662 / 460 (Cortex-A53 / A73) these are illegal instructions (SIGILL), while both our
# test phones and every Apple chip have them, so testing never catches them. Checked: LSE atomics (cas / casp / swp / ld<op> / st<op>),
# RCpc loads and stores (ldapr / ldapur / stlur) and dot product (sdot / udot).
# Functions that check the CPU at run time before they are used are allowed (RUNTIME_SELECTED; add one only after checking how it is
# picked): compiler-rt's __aarch64_* outline-atomic functions (they check for LSE and use ldxr / stxr without it), and libdeflate's
# adler32_arm_neon_dotprod (arm/adler32_impl.h: chosen only when the CPU reports dot product, HWCAP_ASIMDDP).
# Any hit is listed with its function and instruction and fails the build; the fix is in asm_elf.py (stadd in assembly) or in dropping
# the -march / -mcpu option that introduced it.
import re, subprocess, sys

objdump, lib = sys.argv[1], sys.argv[2]
newer = re.compile(r'(cas(p)?(a|al|l)?(b|h)?|swp(a|al|l)?(b|h)?|ld(add|clr|eor|set|smax|smin|umax|umin)(a|al|l)?(b|h)?'
                   r'|st(add|clr|eor|set|smax|smin|umax|umin)(l)?(b|h)?|ldapr(b|h)?|ldapur(b|h|sb|sh|sw)?|stlur(b|h)?|sdot|udot)')
RUNTIME_SELECTED = ('adler32_arm_neon_dotprod',)
func, hits, count = '?', [], 0
proc = subprocess.Popen([objdump, '-d', '--no-show-raw-insn', lib], stdout=subprocess.PIPE, text=True, errors='replace')
for line in proc.stdout:
    m = re.match(r'[0-9a-f]+ <(.+)>:$', line)
    if m: func = m.group(1); continue
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)', line)
    if m and newer.fullmatch(m.group(2)) and not func.startswith('__aarch64_') and func not in RUNTIME_SELECTED:
        count += 1
        if len(hits) < 20: hits.append('  %s  %s  %s' % (m.group(1), func, line.split(':', 1)[1].strip()))
if proc.wait() != 0: sys.exit('check_armv80.py: %s failed on %s' % (objdump, lib))
if count:
    sys.exit('check_armv80.py: %s has %d instruction(s) newer than ARMv8.0, which crash Snapdragon 680-class phones (#293):\n%s'
             % (lib, count, '\n'.join(hits)))
print('check_armv80.py: %s has no instruction newer than ARMv8.0' % lib)
