#!/usr/bin/env python3
# 老接口版的工程描述：project_m3.py <project.yml> <输出.yml> [free]   （run.sh 在 DM_IOS_M3=1 时调）
# 第三个参数 free = 输入已经过了免费档（tools/project_free.py，DM_FREE_ACCOUNT=1 同时设着）：资源下载扩展那五处免费档已去掉，这里要求一处都不剩
# 只改主程序三处：部署目标 26.0 → 18.0；编译参数加 -DDM_NVN_M3=1（图形后端不用 Metal 4，见 gfx/nvn/nvn_mtl.m）；
# 处理器基线 a18 → a14（iOS 18 能装的机器里，内存够跑这个游戏的最老是 A14；宿主汇编要的 LSE 原子指令 A11 起就有）。
# 资源下载扩展（RomfsDownloader，只服务苹果托管资源包，部署目标 iOS 26.0）不带：扩展的最低系统高于主程序的包在老系统上装不装得上没人验过，
# 老接口版的资源本来也不走托管资源包（内测直装走自己的下载地址 / 手动拷，公开版用户自备）。去掉的三样与免费账号档相同（tools/project_free.py）：
# 扩展目标、主程序对它的依赖、Info.plist 里托管资源包的三个键；主程序里见不到这三个键就不碰托管资源包。
# 每处都要求恰好命中一次，工程描述改了格式就在这里失败，不悄悄出一个没改到的工程。
# Project description for the older-API build: project_m3.py <project.yml> <output.yml> [free]   (run.sh calls it when DM_IOS_M3=1)
# The third argument free = the input has already been through the free tier (tools/project_free.py, with DM_FREE_ACCOUNT=1 set at the same time): the five places of the resource-download extension are already removed by the free tier, and here none may remain
# Only three things of the main program change: deployment target 26.0 -> 18.0; compile flags gain -DDM_NVN_M3=1 (the graphics backend does not use Metal 4, see gfx/nvn/nvn_mtl.m);
# processor baseline a18 -> a14 (among the devices that can install iOS 18, the oldest with enough memory for this game is the A14; the LSE atomic instructions the host assembly needs exist from the A11).
# The resource-download extension (RomfsDownloader, serving only Apple-hosted asset packs, deployment target iOS 26.0) is left out: whether a package whose extension has a higher minimum system than the main program installs on an older system has not been verified,
# and the older-API build does not use hosted asset packs for resources anyway (direct test installs use their own download address / manual copy, public users bring their own). The three things removed are the same as for the free-account tier (tools/project_free.py):
# the extension target, the main program's dependency on it, and the three hosted-asset-pack keys in Info.plist; without these three keys the main program does not touch hosted asset packs.
# Each place must match exactly once, so that a change in the format of the project description fails here instead of quietly producing a project that was not changed.
import sys
src, dst = sys.argv[1:3]; free = sys.argv[3:4] == ['free']; s = open(src).read()
def one(old, new):
    global s
    if s.count(old) != 1: sys.exit(f"🔴 project_m3：工程描述里「{old[:40]}」应恰好出现 1 次，实际 {s.count(old)} 次")
    s = s.replace(old, new)
one('    iOS: "26.0"', '    iOS: "18.0"')
one('OTHER_CFLAGS: $(inherited) -mcpu=apple-a18', 'OTHER_CFLAGS: $(inherited) -mcpu=apple-a14 -DDM_NVN_M3=1')
import re
L = s.split('\n'); out, n_ba, n_dep, n_tgt, i = [], 0, 0, 0, 0
while i < len(L):
    l = L[i]
    if re.match(r'^ *(BAAppGroupID|BAHasManagedAssetPacks|BAUsesAppleHosting):', l): n_ba += 1; i += 1; continue
    if re.match(r'^ *- target: RomfsDownloader\b', l): n_dep += 1; i += 1; continue
    if re.match(r'^  RomfsDownloader:\s*$', l):   # 整个目标块：到下一个同级键为止 | the whole target block: up to the next key at the same level
        n_tgt += 1; i += 1
        while i < len(L) and not re.match(r'^  \S', L[i]) and not re.match(r'^\S', L[i]): i += 1
        continue
    out.append(l); i += 1
want = (0, 0, 0) if free else (3, 1, 1)
if (n_ba, n_dep, n_tgt) != want: sys.exit(f"🔴 project_m3：资源下载扩展没去干净（托管资源包键 {n_ba} / {want[0]}、依赖 {n_dep} / {want[1]}、扩展目标 {n_tgt} / {want[2]}{'；输入应已过免费档' if free else ''}）")
if any('RomfsDownloader' in l and not l.lstrip().startswith('#') for l in out): sys.exit("🔴 project_m3：去掉之后工程描述里还有 RomfsDownloader")
open(dst, 'w').write('\n'.join(out))
