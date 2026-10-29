#!/usr/bin/env python3
# 安卓版的导入包：用你自己的游戏文件，在电脑上做出发布页那个安卓 App（不含任何游戏内容）缺的全部东西，放进一个文件夹，拷进手机，App 打开时自己导入。
#   做出来的是 <输出目录>/android/：out/（改写后的游戏程序与配套表）、shaders/（Vulkan 着色器包）、overlay/（触屏覆盖层与界面素材）、
#   derived.tsv（几张从游戏文件生成的数据表）、pack_info.txt（做给哪一版 App）。游戏文件只在本机读，不上传；这个文件夹里有游戏内容，请只自己用，不要分发。
# 用法：d2rm_android.py <游戏文件目录> <输出目录>
#   游戏文件目录 = Switch 版 3.3.94012 的 Update 目录，里面有 ExeFS/{main,sdk} 与 RomFS/data/…（怎么从自己的游戏导出）
#   工作目录（缓存与中间产物）：环境变量 DM_ROOT，缺省 ~/dm-work；每一步做过就跳过
# 之后：把 android 文件夹整个拷到手机的 Download/S2X/D2R Mobile/ 下（和放游戏资源的 data 文件夹并排），打开 App 就会导入；导入完可以删掉这份。
# 需要：Python 3.9+ 与 pillow numpy lz4 capstone unicorn；.NET SDK（编着色器翻译工具）；Vulkan SDK 里的 spirv-opt、spirv-val、glslc
#   （Mac：brew install spirv-tools shaderc；Windows / Linux：LunarG Vulkan SDK；glslc 也可以用安卓 NDK 里的）。缺什么本工具开头会逐项报。
# 它做的事与 d2rm_pack.py（iOS 壳）同一套：改写游戏程序换成安卓目标，着色器翻成 Vulkan 用的包（dm/gfx/tools/dmsh_vk.py）而不是 Metal 库。
# 鸿蒙：d2rm_android.py --target ohos <游戏文件目录> <输出目录>，写出 <输出目录>/harmony/dm/：改写目标换成鸿蒙（out/ 与安卓不能共用），着色器包与安卓同一份，
#   另加 ui-art/（技能键标签 lbl_* 与技能剪影 sil_*，各 9 张，取自底栏素材生成器的输出）；把 dm 文件夹拷进手机上和游戏资源 data 并排的「D2R Mobile」文件夹，应用导入时选它。
# The Android import pack: from your own game files, make on a computer everything the Android App on the release page (which contains no game content) lacks, put it in one folder, copy it to the phone, and the App imports it when opened.
#   The result is <output directory>/android/: out/ (the rewritten game program and its tables), shaders/ (Vulkan shader bundles), overlay/ (touch overlay and UI art),
#   derived.tsv (a few data tables generated from the game files), pack_info.txt (which App release it is for). Game files are only read on this machine, never uploaded; this folder contains game content, keep it to yourself and do not distribute it.
# Usage: d2rm_android.py <game files directory> <output directory>
#   game files directory = the Update directory of the Switch version 3.3.94012, with ExeFS/{main,sdk} and RomFS/data/... (how to export them from your own game: docs/GAME_FILES)
#   working directory (caches and intermediate products): environment variable DM_ROOT, default ~/dm-work; each step that is done is skipped
# Then: copy the whole android folder to Download/S2X/D2R Mobile/ on the phone (next to the data folder with the game resources); opening the App imports it, and afterwards this copy can be deleted.
# Needs: Python 3.9+ with pillow numpy lz4 capstone unicorn; the .NET SDK (to build the shader translation tool); spirv-opt, spirv-val and glslc from the Vulkan SDK
#   (Mac: brew install spirv-tools shaderc; Windows / Linux: the LunarG Vulkan SDK; glslc can also come from the Android NDK). The tool lists anything missing at the start.
# It does the same as d2rm_pack.py (the iOS shell): the game program is rewritten for the Android target, and the shaders become the bundles Vulkan uses (dm/gfx/tools/dmsh_vk.py) instead of a Metal library.
# HarmonyOS: d2rm_android.py --target ohos <game files directory> <output directory> writes <output directory>/harmony/dm/: the game program is rewritten for HarmonyOS (out/ cannot be shared with Android), the shader bundles are the same as Android's,
#   plus ui-art/ (9 skill-button labels lbl_* and 9 skill silhouettes sil_*, taken from the bottom-bar art generator's output); copy the dm folder into a "D2R Mobile" folder on the phone next to the data folder with the game resources, and pick that folder when the app asks to import.
import hashlib, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__)); DM = os.path.dirname(HERE); IOS = os.path.join(DM, 'ios'); GT = os.path.join(DM, 'gfx', 'tools')
sys.path.insert(0, HERE); from d2rm_pack import ROOT, PY, MODS, say, die, run, step   # 共用 iOS 壳工具的执行与跳步框架（含 Windows 换行处理） | the run-and-skip framework shared with the iOS shell tool (including the Windows line-ending handling)


def check_env(game):
    bad = []
    for m in ('PIL', 'numpy', 'lz4', 'capstone', 'unicorn'):
        try: __import__(m)
        except Exception: bad.append('Python 包 %s：%s -m pip install pillow numpy lz4 capstone unicorn' % (m, PY))
    dotnet = shutil.which('dotnet') or (os.path.expanduser('~/.dotnet/dotnet') if os.path.exists(os.path.expanduser('~/.dotnet/dotnet')) else None)
    if not dotnet: bad.append('.NET SDK（dotnet）：https://dotnet.microsoft.com/download')
    for t in ('spirv-opt', 'spirv-val'):
        if not shutil.which(t): bad.append('%s：Vulkan SDK（https://vulkan.lunarg.com/），Mac 上 brew install spirv-tools' % t)
    if shutil.which('spirv-opt') and b'--trim-capabilities' not in subprocess.run(['spirv-opt', '--help'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout:
        bad.append('spirv-opt 版本太旧（%s，没有 --trim-capabilities）：换新版 Vulkan SDK 的（spirv-opt --help 里要有这一项）；安卓 NDK 自带的那份也太旧，别让它排在 PATH 前面' % shutil.which('spirv-opt'))
    sys.path.insert(0, GT); import pack_builtin_queries
    try: pack_builtin_queries.find_glslc(None)
    except SystemExit: bad.append('glslc：Vulkan SDK 或安卓 NDK 里都有（Mac 上 brew install shaderc），也可以用环境变量 GLSLC 指过去')
    for f in ('ExeFS/main', 'ExeFS/sdk', 'RomFS/data/build_manifest.psv'):
        if not os.path.isfile(os.path.join(game, f)): bad.append('游戏文件 %s（在 %s 下没找到）' % (f, game))
    if bad: die('还缺 %d 项：\n  - %s' % (len(bad), '\n  - '.join(bad)))
    if not os.environ.get('DOTNET_ROOT'):   # 翻译工具启动时自己找 .NET 运行时，理由见 d2rm_pack.check_env | the translation tool finds the .NET runtime itself at startup; see d2rm_pack.check_env for why
        import re
        r = subprocess.run([dotnet, '--list-runtimes'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        m = re.search(r'^Microsoft\.NETCore\.App \S+ \[(.*)[/\\]shared[/\\]Microsoft\.NETCore\.App\]', r.stdout.decode('utf-8', 'replace'), re.M)
        if m: os.environ['DOTNET_ROOT'] = m.group(1)
    return dotnet


def main():
    args = sys.argv[1:]; T = 'android'
    if args[:1] == ['--target'] and len(args) >= 2: T = args[1]; args = args[2:]
    if len(args) != 2 or T not in ('android', 'ohos'): sys.exit('用法：d2rm_android.py [--target ohos] <游戏文件目录> <输出目录>')
    game, dst = (os.path.abspath(x) for x in args)
    for st in (sys.stdout, sys.stderr):
        try: st.reconfigure(errors='replace')
        except Exception: pass
    if not os.path.isdir(game): die('没有这个游戏文件目录：%s' % game)
    dotnet = check_env(game)
    P = open(os.path.join(DM, 'PRODUCT')).readline().strip()
    romfs = os.path.join(game, 'RomFS'); stage = os.path.join(ROOT, 'build', 'pack', T); shutil.rmtree(stage, ignore_errors=True); os.makedirs(stage)
    final = os.path.join(dst, 'android') if T == 'android' else os.path.join(dst, 'harmony', 'dm')
    say('产物代号 %s；做好的文件夹放在 %s' % (P, final))

    # 一、改写游戏程序（安卓 / 鸿蒙目标）
    # 1. Rewrite the game program (Android / HarmonyOS target)
    out = os.path.join(ROOT, 'build', 'cache', 'dm', 'out', P + '-' + T); os.makedirs(out, exist_ok=True)
    def patch():
        for m in MODS: run([PY, os.path.join(HERE, 'patch_nso.py'), os.path.join(game, 'ExeFS', m), out, m], env={'DM_TARGET': T, 'DM_RULESET': 'touch-run-v4'},
                           log=os.path.join(out, m + '.patch.log'), what='改写 ' + m)
    # 做过的标准：产物齐，且产物记下的改写器指纹（build_src.sha256）就是这份源码的 patch_nso.py——改写器更新过（产物代号可能没变）就重做
    # Done means: the products are complete and the rewriter fingerprint they record (build_src.sha256) is this source's patch_nso.py -- redo it when the rewriter was updated (the product code may not have changed)
    src = hashlib.sha256(open(os.path.join(HERE, 'patch_nso.py'), 'rb').read()).hexdigest()
    def patched():
        try: rec = open(os.path.join(out, 'build_src.sha256')).read().split()[0]
        except (OSError, IndexError): return False
        return rec == src and all(os.path.isfile(os.path.join(out, m + '.patched.bin')) and os.path.isfile(os.path.join(out, m + '.gates.tsv')) for m in MODS)
    step('改写游戏程序', patched, patch)
    os.makedirs(os.path.join(stage, 'out'))
    for f in sorted(os.listdir(out)):
        if not f.endswith('.log'): shutil.copyfile(os.path.join(out, f), os.path.join(stage, 'out', f))

    # 二、界面素材、技能图标索引与几张数据表
    # 2. UI art, the skill icon index and a few data tables
    say('从游戏文件生成界面素材（输入没变会自己跳过）')
    run([PY, os.path.join(IOS, 'tools', 'gen_assets.py'), romfs], cwd=IOS, env={'DM_ROMFS_SRC': romfs}, log=os.path.join(stage, 'gen_assets.log'), what='生成界面素材')
    gen = os.path.join(stage, 'gen'); os.makedirs(gen)
    run([PY, os.path.join(IOS, 'tools', 'native_art', 'generate.py'), romfs, '--output-dir', gen], what='生成技能图标')
    for name, gen_py in (('dm_vault_tab.h', 'gen_vault_table.py'), ('dm_hud_exp.h', 'gen_hud_exp.py')):
        with open(os.path.join(gen, name), 'w', encoding='utf-8') as f: f.write(run([PY, os.path.join(IOS, 'tools', gen_py), romfs], what='生成 ' + name))
    sys.path.insert(0, os.path.join(IOS, 'tools')); import shell_stubs
    say('数据表：经验 %d、部位 %d、仓库条目 %d、套装 %d、技能索引 %d' % shell_stubs.data(None, os.path.join(stage, 'derived.tsv'), gen))

    # 三、覆盖层（触屏布局、界面底图）+ 技能图集与 HUD 小图，最后写清单（App 启动时逐文件核对）
    # 3. Overlay (touch layout, UI base art) + skill atlases and small HUD images, then write the manifest (the App checks it file by file at startup)
    say('生成覆盖层')
    run([PY, os.path.join(IOS, 'tools', 'gen_romfs_overlay.py'), romfs, os.path.join(stage, 'overlay')], cwd=IOS, env={'DM_ROMFS_SRC': romfs}, log=os.path.join(stage, 'gen_overlay.log'), what='生成覆盖层')
    sys.path.insert(0, os.path.join(DM, 'android', 'tools')); import stage_touch_assets
    hud = os.path.join(IOS, 'Gen', 'assets', 'NativeHUD', 'v4', 'hud_v4_ui')
    stage_touch_assets.stage(os.path.join(gen, 'TouchArt'), hud, os.path.join(stage, 'overlay'))
    if T == 'ohos':   # 鸿蒙前端从 ui-art/ 取技能键标签与技能剪影（剪影是彩色原图，银灰在绘制时做，同 iOS） | the HarmonyOS front end takes the skill-button labels and skill silhouettes from ui-art/ (the silhouettes are the colour originals; the silver is applied when drawing, as on iOS)
        os.makedirs(os.path.join(stage, 'ui-art'))
        for n in ['lbl_main.png'] + ['lbl_f%d.png' % i for i in range(1, 9)] + ['sil_dm_native_skill_%d.png' % i for i in range(9)]:
            if not os.path.isfile(os.path.join(hud, n)): die('底栏素材里缺 %s（%s）' % (n, hud))
            shutil.copyfile(os.path.join(hud, n), os.path.join(stage, 'ui-art', n))

    # 四、着色器：取包 → 翻译工具 → 翻成 Vulkan 用的包；再加 4 个查询核（项目手写，现编）
    # 4. Shaders: get the packages → translation tool → translate into the bundles Vulkan uses; then add the 4 query kernels (written by the project, compiled here)
    packs = os.path.join(ROOT, 'build', 'cache', 'dm', 'dmsh')
    def get_packs():
        os.makedirs(packs, exist_ok=True); e = {'DMX_ROOT': os.path.join(romfs, 'data')}
        for line in run([PY, os.path.join(HERE, 'dmx.py'), 'list', 'data:data/hd/shaders/*.bin'], env=e, what='列着色器包').splitlines():
            p = line.split('\t')[0]
            if p: run([PY, os.path.join(HERE, 'dmx.py'), 'get', p, os.path.join(packs, os.path.basename(p))], env=e, what='取着色器包 ' + p)
    step('取着色器包（162 个）', lambda: os.path.isdir(packs) and len(os.listdir(packs)) == 162, get_packs)
    xl = os.path.join(ROOT, 'build', 'gfx', 'dmsh_xlate'); exe = os.path.join(xl, 'dmsh_xlate.exe' if os.name == 'nt' else 'dmsh_xlate')
    step('编译着色器翻译工具', lambda: os.path.isfile(exe),
         lambda: run([dotnet, 'publish', os.path.join(GT, 'dmsh_xlate'), '-c', 'Release', '-o', xl, '-v', 'quiet'], what='编译着色器翻译工具'))
    sh = os.path.join(stage, 'shaders'); t0 = time.time(); say('翻译全部着色器（约 2 分钟）')
    run([PY, os.path.join(GT, 'dmsh_vk.py'), packs, sh], env={'XLATE': exe, 'MAIN': os.path.join(game, 'ExeFS', 'main'), 'WORK': stage}, log=os.path.join(stage, 'shaders.log'), what='翻译着色器')
    q = os.path.join(stage, 'queries'); run([PY, os.path.join(GT, 'pack_builtin_queries.py'), '--output', q], what='编查询核')
    for f in os.listdir(q):
        if f.endswith('.xgs'): shutil.move(os.path.join(q, f), os.path.join(sh, f))
    n = len([f for f in os.listdir(sh) if f.endswith('.xgs')]); say('着色器 %d 个（%d 秒）' % (n, time.time() - t0))

    # 五、放到输出目录：先写在旁边，齐了再换上（中途失败不留半份）
    # 5. Put it in the output directory: write it alongside first and swap it in once complete (a failure halfway leaves no half copy)
    with open(os.path.join(stage, 'pack_info.txt'), 'w', encoding='utf-8', newline='\n') as f: f.write('PRODUCT=%s\nTARGET=%s\nMADE=%s\n' % (P, T, time.strftime('%Y-%m-%dT%H:%M:%S')))
    part = final + '.part'
    shutil.rmtree(part, ignore_errors=True); os.makedirs(part)
    for x in ('out', 'shaders', 'overlay') + (('ui-art',) if T == 'ohos' else ()): shutil.copytree(os.path.join(stage, x), os.path.join(part, x))
    for x in ('derived.tsv', 'pack_info.txt'): shutil.copyfile(os.path.join(stage, x), os.path.join(part, x))
    shutil.rmtree(final, ignore_errors=True); os.replace(part, final)
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(final) for f in fs)
    if T == 'ohos': say('好了：%s（%.0f MB）。在手机上建一个「D2R Mobile」文件夹，把这个 dm 文件夹和游戏资源（%s 整个拷过去，叫 data）并排放进去，'
                        '打开应用按提示选「D2R Mobile」导入。这里面有游戏内容，请只自己用' % (final, size / 1048576, os.path.join(romfs, 'data')))
    else: say('好了：%s（%.0f MB）。把这个 android 文件夹整个拷到手机的 Download/S2X/D2R Mobile/ 下（游戏资源放在同一处的 data 文件夹：%s 整个拷过去），'
              '打开 App 会自己导入。这里面有游戏内容，请只自己用' % (final, size / 1048576, os.path.join(romfs, 'data')))


if __name__ == '__main__': main()
