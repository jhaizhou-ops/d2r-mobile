#!/usr/bin/env python3
# B 着色器库的翻译链版本（构建结果须只由源码决定）。
# 链版本 V = 下列仓内文件内容 + 翻译选项的 sha256 前 12 位：本脚本、dmsh_build.py、dmsh_fetch.py（可读 tile 版本）、dmsh_xlate 源（Program.cs、csproj）、
# 映射表（含冲突表）、翻译查询的作答表（dmsh_facts.py 与 nvn/shader_facts.tsv）、iOS 编译口径 ios_metal.py、工具锁 dmsh_chain.lock（Ryujinx 着色器 dll / spirv-cross / metal 编译器的实测指纹）。
# 出包（dmsh_build.py）写进 <缓存目录>/b-<V>/，反射 json 记 chain = V；宿主编译时取 nvn/dmsh_chain.h 里的 V，只从 b-<V>/ 加载并逐个核 json。
# 用法：dmsh_chain.py                 打印 V（缺省选项）
#       dmsh_chain.py --header        重写 nvn/dmsh_chain.h（改了上列任一文件后要跑，tests/t_chain.sh 核对）
#       dmsh_chain.py --check-tools   核对实际工具与锁文件一致，不一致报红退出 1
#       dmsh_chain.py --update-lock   按实际工具重写锁文件（换 Ryujinx / spirv-cross / Xcode 后跑，随提交入仓）
# 环境：XLATE_DEBUG / XLATE_REDUCED（与 dmsh_build.py 同义，缺省 1 / 1；非缺省选项得到不同的 V，宿主不会加载）
# Translation chain version of the B shader library (the build result must be determined by source alone).
# Chain version V = first 12 digits of the sha256 of the contents of the following in-repo files + the translation options: this script, dmsh_build.py, dmsh_fetch.py (readable-tile version), the dmsh_xlate sources (Program.cs, csproj),
# the mapping table (including the conflict table), the answer table for translation queries (dmsh_facts.py and nvn/shader_facts.tsv), the iOS compile convention ios_metal.py, the tool lock dmsh_chain.lock (measured fingerprints of the Ryujinx shader dll / spirv-cross / metal compiler).
# The build (dmsh_build.py) writes into <cache dir>/b-<V>/ and the reflection json records chain = V; at compile time the host takes V from nvn/dmsh_chain.h, loads only from b-<V>/ and verifies each json.
# Usage: dmsh_chain.py                 print V (default options)
#       dmsh_chain.py --header        rewrite nvn/dmsh_chain.h (run after changing any file listed above; tests/t_chain.sh verifies it)
#       dmsh_chain.py --check-tools   verify that the actual tools match the lock file; on mismatch report a failure and exit 1
#       dmsh_chain.py --update-lock   rewrite the lock file from the actual tools (run after changing Ryujinx / spirv-cross / Xcode; committed to the repo)
# Environment: XLATE_DEBUG / XLATE_REDUCED (same meaning as in dmsh_build.py, default 1 / 1; non-default options yield a different V, which the host will not load)
import os, sys, hashlib, subprocess
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # 工作区根目录（游戏文件、产物、运行现场都在它下面） | Workspace root (game files, build outputs and run artifacts are all under it)
HERE = os.path.dirname(os.path.abspath(__file__)); GFX = os.path.dirname(HERE); HOME = os.path.expanduser('~')
FILES = ['tools/dmsh_chain.py', 'tools/dmsh_build.py', 'tools/dmsh_fetch.py', 'tools/dmsh_facts.py', 'nvn/shader_facts.tsv', 'tools/dmsh_xlate/Program.cs', 'tools/dmsh_xlate/dmsh_xlate.csproj',
         'nvn/shader_map.tsv', 'nvn/shader_map.tsv.conflicts.tsv', '../ios/tools/ios_metal.py', 'tools/dmsh_chain.lock']
LOCK = os.path.join(HERE, 'dmsh_chain.lock'); HEADER = os.path.join(GFX, 'nvn/dmsh_chain.h')
XLATE_DIR = os.path.dirname(os.environ.get('XLATE', f'{DM_ROOT}/build/gfx/dmsh_xlate/dmsh_xlate'))

def options():
    return 'debug=%s reduced=%s' % (os.environ.get('XLATE_DEBUG', '1') != '0', os.environ.get('XLATE_REDUCED', '1') == '1')

def version():
    h = hashlib.sha256()
    for f in FILES:
        h.update(f.encode() + b'\0' + open(os.path.join(GFX, f), 'rb').read() + b'\0')
    h.update(options().encode())
    return h.hexdigest()[:12]

def sha16(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()[:16]

def tools_now():
    """实际工具指纹：翻译器 dll（含 Ryujinx 着色器库）、spirv-cross、metal 编译器（macOS / iOS） | Actual tool fingerprints: translator dll (including the Ryujinx shader library), spirv-cross, metal compiler (macOS / iOS)"""
    t = {}
    for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll'): t[dll] = sha16(os.path.join(XLATE_DIR, dll))
    # 翻译器跑不起来（例如旧的框架依赖版找不到 .NET，#299）时记成以 ? 开头的原因，报错里与「不是用仓内源编的」分开
    # When the translator does not run (for example an old framework-dependent build that cannot find .NET, #299), record a reason
    # starting with ?, so the report keeps it apart from "not built from the repository's source"
    try:
        r = subprocess.run([os.path.join(XLATE_DIR, 'dmsh_xlate'), '--source-sha'], capture_output=True, text=True)
        t['xlate_source'] = r.stdout.strip() or '?退出码 %d：%s' % (r.returncode, (r.stderr.strip().splitlines() or ['没有输出'])[0][:160])
    except OSError as e:
        t['xlate_source'] = '?起不来：%s' % e
    sc = subprocess.run(['which', 'spirv-cross'], capture_output=True, text=True).stdout.strip()
    t['spirv-cross'] = sha16(sc) if sc else 'missing'
    for sdk in ('macosx', 'iphoneos'):
        t['metal_' + sdk] = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '--version'], capture_output=True, text=True).stdout.splitlines()[0:1] or ['missing']
        t['metal_' + sdk] = t['metal_' + sdk][0]
    return t

def xlate_source_sha():   # 与 dmsh_xlate --source-sha 同口径：嵌入的 Program.cs 与 csproj 依次拼接 | same convention as dmsh_xlate --source-sha: the embedded Program.cs and csproj concatenated in that order
    return hashlib.sha256(b''.join(open(os.path.join(HERE, 'dmsh_xlate', f), 'rb').read() for f in ('Program.cs', 'dmsh_xlate.csproj'))).hexdigest()[:16]

def xlate_problem(src):
    """翻译器嵌入源对不上时的说法：没跑起来与不是用仓内源编的是两回事，方向不同（#299：前者曾被报成后者）
    | What to say when the translator's embedded source does not match: not running and not built from the repository's source are
    different problems with different fixes (#299: the first used to be reported as the second)"""
    if src.startswith('?'):
        return f'翻译器没跑起来（{src[1:]}）：多半是旧壳留下的框架依赖版找不到 .NET；删掉 {XLATE_DIR} 再做包（会换成壳里预编的），或重编 dmsh_xlate'
    return f'翻译器不是用仓内源编的（嵌入 {src} / 仓内 {xlate_source_sha()}）：先重编 dmsh_xlate'

def check_tools():
    want = dict(l.rstrip('\n').split('\t', 1) for l in open(LOCK) if '\t' in l and not l.startswith('#'))
    now = tools_now(); bad = [f'{k}：锁 {want.get(k)} / 实际 {v}' for k, v in now.items() if want.get(k) != v]
    if now['xlate_source'] != xlate_source_sha(): bad.append(xlate_problem(now['xlate_source']))
    return bad

if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['--update-lock']:
        t = tools_now()
        if t['xlate_source'] != xlate_source_sha(): sys.exit('【红】' + xlate_problem(t['xlate_source']))
        open(LOCK, 'w').write('# 翻译链工具指纹（dmsh_chain.py --update-lock 生成；换工具后重生成并随提交入仓）\n' + ''.join(f'{k}\t{v}\n' for k, v in t.items()))
        print('已写', LOCK)
    elif a == ['--check-tools']:
        bad = check_tools()
        for b in bad: print('【红】翻译链工具与锁文件不符：' + b)
        sys.exit(1 if bad else 0)
    elif a == ['--header']:
        open(HEADER, 'w').write('// 生成文件（dm/gfx/tools/dmsh_chain.py --header）：本源码树期望的 B 着色器库翻译链版本（缺省选项）；宿主只加载 <库目录>/b-<V>/\n'
                                f'#define DMSH_CHAIN "{version()}"\n')
        print(version())
    elif not a: print(version())
    else: sys.exit(__doc__ if __doc__ else '用法见文件头')
