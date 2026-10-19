# 从 dm/host/Makefile 实际展开的编译命令（make -n -B dmhost）取宿主的源文件 / 头文件路径 / 框架，生成 iOS 工程的构建输入，
# 不手抄列表：Makefile 一改，iOS 下次构建自动跟上。
#   Gen/src/<扁平名>   源文件符号链接（排除 main.c 与 front_mac.m；main.c 另链到 Gen/main/main.c，以 -Dmain=dm_main 编）
#   stdout            xcconfig 行：HEADER_SEARCH_PATHS（-I 目录 + 每个源文件所在目录，保证相对 include 照常解析）、
#                     OTHER_CFLAGS（-D）、框架（Cocoa 换成 UIKit 侧已有，跳过）
# 用法：gen_build.py <dm/host 目录> <Gen 目录>
# Take the host's source files / header paths / frameworks from the actually expanded compile commands of dm/host/Makefile (make -n -B dmhost) and generate the iOS project's build inputs,
# without hand-copying lists: when the Makefile changes, the next iOS build follows automatically.
#   Gen/src/<flat name>   symlinks to source files (excluding main.c and front_mac.m; main.c is linked separately to Gen/main/main.c and compiled with -Dmain=dm_main)
#   stdout            xcconfig lines: HEADER_SEARCH_PATHS (-I directories + the directory of every source file, so relative includes resolve as usual),
#                     OTHER_CFLAGS (-D), frameworks (Cocoa is replaced by what the UIKit side already has, skipped)
# Usage: gen_build.py <dm/host dir> <Gen dir>
import os, shlex, subprocess, sys
host, gen = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
cmd = subprocess.run(['make', '-s', '-n', '-B', '-C', host, 'dmhost'], capture_output=True, text=True, check=True).stdout
toks = shlex.split(next(l for l in cmd.splitlines() if l.lstrip().startswith('clang')))
srcs, incs, defs, fws = [], [], [], []
i = 0
while i < len(toks):
    t = toks[i]
    if t == '-framework': fws.append(toks[i + 1]); i += 2; continue
    if t.startswith('-I'): incs.append(os.path.normpath(os.path.join(host, t[2:])))
    elif t.startswith('-D'): defs.append(t)
    elif t.endswith(('.c', '.m', '.S')): srcs.append(os.path.normpath(os.path.join(host, t)))
    i += 1
assert srcs, '编译命令里没有源文件'
dup = sorted({s for s in srcs if srcs.count(s) > 1})
if dup: sys.exit('编译命令里源文件重复（Makefile 显式列与通配重叠？）：' + ' '.join(dup))
os.makedirs(f'{gen}/src'); os.makedirs(f'{gen}/main')
dirs = []
for s in srcs:
    d = os.path.dirname(s)
    if d not in dirs: dirs.append(d)
    b = os.path.basename(s)
    if os.path.splitext(b)[0].endswith('_mac'): continue   # Mac 专用源按 _mac 命名约定不进 iOS（front_mac.m 由 front_ios.m 实现 front.h；touch_mac.c 是 Mac 触点脚本） | Mac-only sources are kept out of iOS by the _mac naming convention (for front_mac.m, front_ios.m implements front.h; touch_mac.c is the Mac touch point script)
    if os.path.dirname(s) == os.path.normpath(os.path.join(host, '../ios/Sources')): continue   # 宿主借用的 iOS 源（如触屏模块）iOS 工程本身已编，再收一份会重复链接 | iOS sources borrowed by the host (e.g. the touch module) are already compiled by the iOS project itself; collecting another copy would link them twice
    if s == os.path.join(host, 'main.c'): os.symlink(s, f'{gen}/main/main.c'); continue
    flat = os.path.relpath(s, os.path.dirname(host)).replace('/', '__')   # 不同目录同名文件不冲突 | Same-named files in different directories do not collide
    os.symlink(s, f'{gen}/src/{flat}')
print('HEADER_SEARCH_PATHS = $(inherited) ' + ' '.join(f'"{d}"' for d in incs + dirs))
print('OTHER_CFLAGS = $(inherited) ' + ' '.join(defs))
print('DM_FRAMEWORKS = ' + ' '.join(f'-framework {f}' for f in fws if f not in ('Cocoa', 'Carbon')))
