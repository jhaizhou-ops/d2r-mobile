#!/usr/bin/env python3
# 安卓 Vulkan 着色器包：Switch 原版着色器 → SPIR-V（同 dmsh_build.py 的 dmsh_xlate，Ryujinx 翻译器，Vulkan 目标）→ 导出 <键>.vk.json + <键>.spv
# → pack_game_shaders.py 打成 XGS1 包 <键>.xgs（Vulkan 引擎运行时从 files/dm/shaders/ 读）。iOS 那条链在同一份 SPIR-V 之后转 MSL，这里转成引擎要的包。
# 取着色器、组作业与 dmsh_build.py 逐字相同（键 = 控制段前 0x880 字节 SHA-256 前 16 位，即 shader_map.tsv 第 4 列，引擎 xbg_shader_key.c 同一算法）；
# dmsh_build.py 本身算进 iOS 着色器链版本，不在里面加开关，这里另起一份。
# 导出时 SPIR-V 改三处（与作者鸿蒙移植那套 6931 个包逐字节核过，见 --compare）：
#   ① 绑定号：引擎每条管线只有一个描述符集，各阶段从 0 起的绑定号会撞——改成 原号 × 6 + 阶段号（顶点 0、外壳 1、域 2、几何 3、像素 4、计算 5）；
#   ② 裁剪（③）之后，补「未定格式」存储图像读写的能力（Ryujinx 不声明，见 add_format_caps）；
#   ③ spirv-opt --trim-capabilities：Ryujinx 无条件声明 Float64 / GroupNonUniform* / DrawParameters 等，引擎只放行白名单、见到扩展即报错。其余不优化
#     （quad / warp 改写器按 Ryujinx 原始写法匹配）。最后 spirv-val --target-env vulkan1.1。
#   不在这里内联：Adreno 750 要的「辅助函数内联」由引擎在手机上载入时做（gfx-inline，见 xbg_adreno_policy.h），导入包对所有机型一样。
# 绑定来源（贴图 / 采样器 / 存储缓冲槽、c0 / c2 驱动段、c1）与 dmsh_build.py 的 reflect() / decode_tex() 同一套解码。
# 用法：dmsh_vk.py <Switch 包目录> <输出目录> [键…]     环境：XLATE（翻译器）、MAIN（main NSO）、J（并发）、SPIRV_OPT / SPIRV_VAL
#       dmsh_vk.py --compare <参考 .xgs 目录> <本次输出目录>    逐个比字节：一致 / 不一致（不一致的再分 SPIR-V / 绑定表 / 其他）
# Android Vulkan shader bundles: original Switch shaders → SPIR-V (the same dmsh_xlate as dmsh_build.py, the Ryujinx translator, Vulkan target) → export <key>.vk.json + <key>.spv
# → pack_game_shaders.py packs the XGS1 bundle <key>.xgs (the Vulkan engine reads it from files/dm/shaders/ at run time). The iOS chain turns the same SPIR-V into MSL; this one into the bundles the engine needs.
# Getting shaders and building jobs are word for word as dmsh_build.py (key = first 16 hex digits of the SHA-256 of the first 0x880 bytes of the control section, i.e. column 4 of shader_map.tsv, the same algorithm as the engine's xbg_shader_key.c);
# dmsh_build.py itself counts towards the iOS shader chain version, so no switch is added there and this is a separate script.
# The SPIR-V gets three changes on export (checked byte for byte against the 6931 bundles of the authors' HarmonyOS port, see --compare):
#   ① binding numbers: the engine has one descriptor set per pipeline, and each stage's bindings from 0 would collide -- they become original × 6 + stage number (vertex 0, hull 1, domain 2, geometry 3, pixel 4, compute 5);
#   ② after trimming (③), add the capabilities for reading and writing storage images of unknown format (Ryujinx does not declare them, see add_format_caps);
#   ③ spirv-opt --trim-capabilities: Ryujinx always declares Float64 / GroupNonUniform* / DrawParameters and others, while the engine allows only a whitelist and fails on any extension. No other optimization
#     (the quad / warp rewriters match Ryujinx's original form). Finally spirv-val --target-env vulkan1.1.
#   No inlining here: the helper inlining the Adreno 750 needs is done by the engine on the phone at load time (gfx-inline, see xbg_adreno_policy.h), so the import pack is the same for every phone.
# Binding sources (texture / sampler / storage buffer slots, the c0 / c2 driver banks, c1) use the same decoding as reflect() / decode_tex() in dmsh_build.py.
# Usage: dmsh_vk.py <Switch package directory> <output directory> [keys...]     environment: XLATE (translator), MAIN (main NSO), J (parallelism), SPIRV_OPT / SPIRV_VAL
#        dmsh_vk.py --compare <reference .xgs directory> <output directory>    compare byte by byte: identical / different (the different ones split into SPIR-V / binding table / other)
import os, re, sys, json, struct, hashlib, shutil, subprocess, tempfile, collections, concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')
OPT, VAL = (os.environ.get(k, d) for k, d in (('SPIRV_OPT', 'spirv-opt'), ('SPIRV_VAL', 'spirv-val')))
STAGE_NO = {'vs': 0, 'hs': 1, 'ds': 2, 'gs': 3, 'ps': 4, 'cs': 5}

TEX_BASE, SAMP_BASE, IMG_BASE = 0x168, 0x568, 0x120   # 同 dmsh_build.py：c[2] 贴图句柄表、采样器句柄表、图像句柄表 | as dmsh_build.py: the texture handle, sampler handle and image handle tables in c[2]


def compare(ref, got):
    """逐个比 .xgs：一致 / 不一致（SPIR-V 部分同不同、绑定表同不同）/ 参考里有本次没有 | Compare .xgs one by one: identical / different (whether the SPIR-V part and the binding table differ) / in the reference but not this time"""
    st = collections.Counter(); bad = []
    for f in sorted(os.listdir(ref)):
        if not f.endswith('.xgs'): continue
        p = os.path.join(got, f)
        if not os.path.exists(p): st['本次没有'] += 1; bad.append((f, '本次没有')); continue
        a, b = open(os.path.join(ref, f), 'rb').read(), open(p, 'rb').read()
        if a == b: st['一致'] += 1; continue
        def parts(d):
            m, v, s, nb, ni, nc, ns, _ = struct.unpack_from('<8I', d, 0); o = 32 + 32 * nb + 4 * ni + nc
            return d[:o], d[o:o + ns]
        (ha, sa), (hb, sb) = parts(a), parts(b)
        why = ('绑定表 / 头不同' if ha != hb else '') + ('，' if ha != hb and sa != sb else '') + ('SPIR-V 不同' if sa != sb else '')
        st['不一致'] += 1; bad.append((f, why))
    print(f'参考 {sum(st.values())} 个：' + '，'.join(f'{k} {v}' for k, v in st.items()))
    for why, n in collections.Counter(w for _, w in bad).most_common(): print(f'  {why}：{n}（例：{next(f for f, w in bad if w == why)}）')
    return st['一致'] == sum(st.values())


if len(sys.argv) > 1 and sys.argv[1] == '--compare':
    sys.exit(0 if compare(sys.argv[2], sys.argv[3]) else 1)

dm_dir, out = sys.argv[1:3]; want = set(sys.argv[3:])
sys.path.insert(0, HERE); import dmsh_facts, pack_game_shaders
XLATE = os.environ.get('XLATE', f'{DM_ROOT}/build/gfx/dmsh_xlate/dmsh_xlate')
MAIN = os.environ.get('MAIN', f'{DM_ROOT}/data/switch/3.3.94012/Update/ExeFS/main')
MAP = os.path.join(HERE, '../nvn/shader_map.tsv')
FACTS = dmsh_facts.load()
if not FACTS: sys.exit('【红】没有作答表 nvn/shader_facts.tsv')

# ---- 取着色器、组作业（与 dmsh_build.py 同） ----
# ---- Getting shaders and building jobs (as dmsh_build.py) ----
def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]

loc, stage_of = {}, {}
for ln in open(MAP):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): loc.setdefault(c[3], (c[0], int(c[1])))
    if len(c) > 4 and c[0].startswith('main.rodata@'):
        loc.setdefault(c[3], (c[0], int(c[0].split('@')[1], 16))); stage_of[c[3]] = {'顶点': 'vs', '像素': 'ps'}[c[2]]

def main_rec(mi, o):
    g = mi.rfind(bytes.fromhex('78563412'), o - 0x1000, o)
    if g < 0 or mi[o:o + 4] != bytes.fromhex('34127698'): sys.exit(f'【红】main 映像 @{o:#x} 不是内嵌着色器控制段（MAIN={MAIN}）')
    return bytes(0x38) + mi[g:o + 0x880]

def job_for(key, name, rec, w):
    ci = rec.find(bytes.fromhex('34127698')); code = rec[0x38:ci]; stage = stage_of.get(key, name[:2])
    c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)
    c1 = rec[0x38 + c1o:0x38 + c1t]
    if stage != 'cs': code = code[0x30:]
    cp = os.path.join(w, key + '.bin'); open(cp, 'wb').write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(),
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}
    if key in FACTS: j.update(FACTS[key])
    return j, c1

# ---- SPIR-V：读写指令 ----
# ---- SPIR-V: reading and writing instructions ----
def insts(words):
    i = 5
    while i < len(words):
        n = words[i] >> 16
        yield i, words[i] & 0xffff, words[i + 1:i + n]
        i += n

def rewrite(spv, stage):
    """绑定号 × 6 + 阶段号 | binding number × 6 + stage number"""
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv))
    for i, op, a in insts(w):
        if op == 71 and a[1] == 33: w[i + 3] = a[2] * 6 + STAGE_NO[stage]                       # OpDecorate … Binding
    return struct.pack('<%dI' % len(w), *w)

def add_format_caps(spv):
    """Ryujinx 对「未定格式」（Format = Unknown）的存储图像读写不声明 StorageImageRead/WriteWithoutFormat，spirv-val 不放行。
    用到了就补在能力段最前面（作者那套包的写法：只补缺的，其余能力次序不动）
    '''Ryujinx does not declare StorageImageRead/WriteWithoutFormat for reads and writes of storage images with Format = Unknown, and spirv-val rejects that.
        When used, add them at the front of the capability section (as the authors' bundles do: only what is missing, the order of the rest unchanged)'''"""
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv))
    fmt, ty, have, need = {}, {}, set(), []
    for i, op, a in insts(w):
        if op == 17: have.add(a[0])
        elif op == 25: fmt[a[0]] = a[7]                                          # OpTypeImage：结果号 → 格式 | OpTypeImage: result id → format
        elif op == 61: ty[a[1]] = a[0]                                           # OpLoad：结果号 → 类型 | OpLoad: result id → type
    for i, op, a in insts(w):
        if op in (98, 99):                                                       # OpImageRead（结果类型、结果、图像…）/ OpImageWrite（图像…） | OpImageRead (result type, result, image...) / OpImageWrite (image...)
            img = a[2] if op == 98 else a[0]
            cap = 55 if op == 98 else 56
            if fmt.get(ty.get(img)) == 0 and cap not in have and cap not in need: need.append(cap)
    for cap in need: w[5:5] = [(2 << 16) | 17, cap]
    return struct.pack('<%dI' % len(w), *w)

def spirv_info(spv):
    """能力、扩展、各变量的绑定 / 存储类 / 指向类型 / 输入位置 / 图像属性（导出 vk.json 用） | Capabilities, extensions, and each variable's binding / storage class / pointee type / input location / image properties (for exporting vk.json)"""
    w = struct.unpack('<%dI' % (len(spv) // 4), spv)
    caps, exts, deco, types, ptr, var, names, consts = [], [], collections.defaultdict(dict), {}, {}, {}, {}, {}
    for i, op, a in insts(w):
        if op == 17: caps.append(a[0])
        elif op == 43 and len(a) >= 3: consts[a[1]] = a[2]                  # OpConstant 结果号 → 值（数组长度用） | OpConstant result id → value (for array lengths)
        elif op == 10: exts.append(bytes(struct.pack('<%dI' % len(a), *a)).split(b'\0')[0].decode())
        elif op == 5: names[a[0]] = bytes(struct.pack('<%dI' % (len(a) - 1), *a[1:])).split(b'\0')[0].decode()
        elif op == 71: deco[a[0]][a[1]] = a[2] if len(a) > 2 else 1
        elif op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30): types[a[0]] = (op, a[1:])
        elif op == 32: ptr[a[0]] = (a[1], a[2])
        elif op == 59: var[a[1]] = (a[0], a[2])
    return caps, exts, deco, types, ptr, var, names, consts

def image_desc(types, tid):
    op, a = types[tid]
    while op in (27, 28, 29): tid = a[0]; op, a = types[tid]                # 采样图像 / 数组 → 图像 | sampled image / array → image
    st, dim, depth, arrayed, ms, sampled, fmt = a[0], a[1], a[2], a[3], a[4], a[5], a[6]
    sop, sa = types[st]
    scalar = {'kind': 'float'} if sop == 22 else {'kind': 'int', 'signed': bool(sa[1])}
    return {'format': fmt, 'dim': dim, 'arrayed': arrayed, 'multisampled': ms, 'depth': depth, 'scalar': scalar}

def decode_tex(slots, h, image):
    tb = (slots & 0xffff) if slots >= 0 else 2; sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    def slot(word, base, cb): return (word * 4 - base) // 8 if cb == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0 else None
    words = [o0] + ([o1] if ty else [])
    if image: return next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < TEX_BASE and slot(x, IMG_BASE, tb) is not None), None), None, ty
    t = next((slot(x, TEX_BASE, tb) for x in words if slot(x, TEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None)
    return t, s, ty

def export(key, stage, c1, spv, res):
    """改写后的 SPIR-V + 翻译器的 res.json → vk.json（字段见 pack_game_shaders.py） | Rewritten SPIR-V + the translator's res.json → vk.json (fields: see pack_game_shaders.py)"""
    caps, exts, deco, types, ptr, var, names, consts = spirv_info(spv)
    if exts: raise ValueError('残留扩展 ' + ','.join(exts))
    by_bind = {}
    for vid, (pt, sc) in var.items():
        if 33 in deco[vid] and sc in (0, 2, 12): by_bind[deco[vid][33]] = (vid, ptr[pt][1], sc)   # UniformConstant / Uniform / StorageBuffer
    base_no = STAGE_NO[stage]; descs = []
    def vbind(b): return b * 6 + base_no
    def block_bytes(tid):
        op, a = types[tid]                                                      # 结构体首成员是 vec4 定长数组（常量缓冲）→ 长度 × 16；运行时数组（存储缓冲）→ 0 | first struct member is a fixed vec4 array (constant buffer) → length × 16; a runtime array (storage buffer) → 0
        if op == 30 and a and types[a[0]][0] == 28: return 16 * consts.get(types[a[0]][1][1], 0)
        return 0
    sb_sorted = sorted(res['sbufs'], key=lambda b: b['binding'])
    # 支持缓冲：Ryujinx 绑定 0、名字 support_buffer，声明了就列（作者那套 6927 个游戏着色器全都带，不管用没用到）
    # Support buffer: Ryujinx binding 0, named support_buffer; listed whenever declared (all 6927 game shaders in the authors' set have it, used or not)
    sup = by_bind.get(vbind(0))
    if sup and names.get(sup[0], '') == 'support_buffer':
        descs.append({'name': 'support_buffer', 'binding': vbind(0), 'type': 'uniform_buffer', 'count': 1, 'source': {'category': 'support_buffer'}})
    for b in res['cbufs']:
        vid, tid, sc = by_bind[vbind(b['binding'])]
        n = b['slot']
        nvn = {'kind': 'uniform', 'slot': n - 3} if 3 <= n <= 16 else ({'kind': 'c1', 'slot': 0} if n == 1 else {'kind': 'driver', 'bank': n, 'slot': 0})
        descs.append({'name': names.get(vid, ''), 'binding': vbind(b['binding']), 'type': 'uniform_buffer', 'count': 1,
                      'source': {'category': 'nvn', 'nvn': nvn}, 'block': {'bytes': block_bytes(tid)}})
    sbase = {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}.get(stage)
    for b in sb_sorted:
        vid, tid, sc = by_bind[vbind(b['binding'])]
        off = b['cb_offset'] * 4 if b['cb_slot'] == 0 else None
        if off is None: raise ValueError(f'存储缓冲不在 c0：{b}')
        if stage == 'cs' and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0: nvn = {'kind': 'uniform_pointer', 'slot': (off - 0x210) // 16}
        elif sbase is not None and 0 <= off - sbase < 0x100 and (off - sbase) % 16 == 0: nvn = {'kind': 'storage_pointer', 'slot': (off - sbase) // 16}
        else: raise ValueError(f'存储缓冲槽解不出：{stage} off={off:#x}')
        descs.append({'name': names.get(vid, ''), 'binding': vbind(b['binding']), 'type': 'storage_buffer', 'count': 1,
                      'source': {'category': 'nvn', 'nvn': nvn}, 'block': {'bytes': block_bytes(tid)}})
    for kind, lst in (('texture', res['textures']), ('image', res['images'])):
        for t in lst:
            vid, tid, sc = by_bind[vbind(t['binding'])]
            img = image_desc(types, tid)
            slot, samp, ty = decode_tex(t['cbuf_slot'], t['handle'], kind == 'image')
            if slot is None: raise ValueError(f'{kind} 槽解不出：{t}')
            if kind == 'texture':
                typ = 'uniform_texel_buffer' if img['dim'] == 5 else 'combined_image_sampler'
                nvn = {'kind': 'texture', 'slot': slot, 'sampler_slot': samp if ty else None}
            else:
                typ = 'storage_texel_buffer' if img['dim'] == 5 else 'storage_image'
                nvn = {'kind': 'image', 'slot': slot}
            descs.append({'name': names.get(vid, ''), 'binding': vbind(t['binding']), 'type': typ, 'count': 1,
                          'source': {'category': 'nvn', 'nvn': nvn, 'handle_decode': {'type': ty}}, 'image': img})
    descs.sort(key=lambda d: d['binding'])
    inputs = []
    if stage == 'vs':
        for vid, (pt, sc) in var.items():
            if sc == 1 and 30 in deco[vid] and 11 not in deco[vid]: inputs.append({'storage': 'input', 'location': deco[vid][30]})   # Input、Location、非 BuiltIn | Input, Location, not BuiltIn
    return {'identity': {'key': key}, 'sha256': hashlib.sha256(spv).hexdigest(), 'stage': stage, 'capabilities': sorted(set(caps)),
            'descriptors': descs, 'interfaces': inputs, 'frozen_job': {'c1': c1.hex()}}

# ---- 主流程 ----
# ---- Main flow ----
keys = sorted(want) if want else sorted(loc)
NJ = int(os.environ.get('J', '8'))
os.makedirs(out, exist_ok=True)
work = tempfile.mkdtemp(prefix='dmsh_vk_', dir=os.environ.get('WORK'))
bundle = os.path.join(work, 'bundle'); os.makedirs(bundle)
packs, jobs, c1s = {}, {}, {}
for key in keys:
    name, k = loc[key]
    if name.startswith('main.rodata@'):
        if 'main' not in packs:
            sys.path.insert(0, os.path.join(HERE, '../../verify')); import nsoimg
            packs['main'] = nsoimg.load(MAIN)
        rec = main_rec(packs['main'], k)
    else:
        if name not in packs: packs[name] = records(open(os.path.join(dm_dir, name), 'rb').read())
        rec = packs[name][k]
    jobs[key], c1s[key] = job_for(key, name, rec, work)
chunks = [keys[i::NJ] for i in range(NJ)]
def xl(ch):
    jp = os.path.join(work, f'job_{abs(hash(ch[0]))}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
    return subprocess.run([XLATE, jp, work], capture_output=True, text=True)
with cf.ThreadPoolExecutor(NJ) as ex: xr = list(ex.map(xl, [c for c in chunks if c]))
xerr = {}
for r in xr:
    for ln in r.stderr.splitlines():
        m = re.match(r'【红】(\w+)：(.*)', ln)
        if m: xerr[m.group(1)] = m.group(2)[:160]

def one(key):
    if key in xerr or not os.path.exists(os.path.join(work, key + '.spv')): return key, '翻译失败', xerr.get(key, '无 spv')
    stage = jobs[key]['stage']
    spv = rewrite(open(os.path.join(work, key + '.spv'), 'rb').read(), stage)
    f1, f2 = os.path.join(work, key + '.r.spv'), os.path.join(work, key + '.t.spv')
    open(f1, 'wb').write(spv)
    r = subprocess.run([OPT, '--trim-capabilities', f1, '-o', f2], capture_output=True, text=True)
    if r.returncode: return key, 'spirv-opt 失败', r.stderr.strip()[:160]
    spv = add_format_caps(open(f2, 'rb').read()); open(f2, 'wb').write(spv)
    r = subprocess.run([VAL, '--target-env', 'vulkan1.1', f2], capture_output=True, text=True)
    if r.returncode: return key, 'spirv-val 失败', r.stderr.strip().splitlines()[0][:160] if r.stderr.strip() else '?'
    spv = open(f2, 'rb').read()
    try: vk = export(key, stage, c1s[key], spv, json.load(open(os.path.join(work, key + '.res.json'))))
    except (ValueError, KeyError) as e: return key, '导出失败', str(e)[:160]
    open(os.path.join(bundle, key + '.spv'), 'wb').write(spv)
    json.dump(vk, open(os.path.join(bundle, key + '.vk.json'), 'w'), ensure_ascii=False)
    try: k2, data = pack_game_shaders.pack(__import__('pathlib').Path(os.path.join(bundle, key + '.vk.json')))
    except (ValueError, KeyError) as e: return key, '打包失败', str(e)[:160]
    open(os.path.join(out, k2 + '.xgs'), 'wb').write(data)
    return key, 'ok', ''

with cf.ThreadPoolExecutor(NJ) as ex: res = list(ex.map(one, keys))
if os.environ.get('DMSH_VK_KEEP'): print('中间产物留在', work)
else: shutil.rmtree(work, ignore_errors=True)
st = collections.Counter(r[1] for r in res)
print(f'着色器 {len(res)}：成功 {st["ok"]}，' + '，'.join(f'{k} {v}' for k, v in st.items() if k != 'ok'))
for lvl in [k for k in st if k != 'ok']:
    for e, n in collections.Counter(r[2] for r in res if r[1] == lvl).most_common(8): print(f'  [{lvl}] {n:5d}  {e}')
sys.exit(0 if st['ok'] == len(res) else 1)
