# 通用 NSO 改写器：Switch aarch64 模块 → 可在苹果系统原生执行的映像。
# 用法：patch_nso.py <NSO 文件> <输出目录> <模块名>
# 改写（苹果内核会改 x18 与 tpidr_el0；Horizon 的线程区、svc、计时器在苹果上没有对应物）：
#   x18 读写            → 线程槽 K18（TSD，经 tpidrro_el0）
#   mrs tpidr_el0       → 读线程槽 KTLS；msr tpidr_el0 → 写 KTLS
#   mrs tpidrro_el0     → 读线程槽 KTLR（Horizon ThreadLocalRegion 指针）
#   svc #n、mrs cntpct_el0 → 宿主门（门地址取自线程槽 KGATE，进 C 后按返回点查 gates.tsv）
#   已登记剪口（CUTS，按 NSO 构建号限定）：改写前逐条断言原字节，不符即中止；结果列在 <名>.cuts.tsv
#   已登记观测门（OBS，按构建号限定）：入口一条指令换成「进宿主观测回调 → 执行原指令 → 跳回」，诊断零运行时改代码；列在 <名>.obs.tsv
# 键页只在数据段末尾保存“实际键号 × 8”，补丁代码通过键页间接取线程槽：装载器可使用任意键号。
# 输出：<名>.patched.bin（键页与补丁区接在映像后 PATCH_OFF）、<名>.fixups.tsv（布局与键页下标表头）、<名>.gates.tsv、<名>.sites.tsv、<名>.cuts.tsv、<名>.obs.tsv
# Generic NSO rewriter: Switch aarch64 module → an image that can execute natively on Apple systems.
# Usage: patch_nso.py <NSO file> <output dir> <module name>
# Rewrites (the Apple kernel modifies x18 and tpidr_el0; Horizon's thread area, svc and timers have no counterpart on Apple):
#   x18 read/write      → thread slot K18 (TSD, via tpidrro_el0)
#   mrs tpidr_el0       → read thread slot KTLS; msr tpidr_el0 → write KTLS
#   mrs tpidrro_el0     → read thread slot KTLR (Horizon ThreadLocalRegion pointer)
#   svc #n, mrs cntpct_el0 → host gate (gate address taken from thread slot KGATE; once in C, gates.tsv is looked up by return point)
#   Registered cuts (CUTS, restricted by NSO build ID): before rewriting, the original bytes are asserted one by one and any mismatch aborts; results are listed in <name>.cuts.tsv
#   Registered observation gates (OBS, restricted by build ID): one instruction at the entry is replaced with "enter the host observation callback → execute the original instruction → jump back", so diagnostics modify zero code at runtime; listed in <name>.obs.tsv
# The key page only stores "actual key number × 8" at the end of the data segment, and patch code fetches thread slots indirectly through the key page: the loader may use any key number.
# Output: <name>.patched.bin (key page and patch area appended after the image at PATCH_OFF), <name>.fixups.tsv (layout and key page index header), <name>.gates.tsv, <name>.sites.tsv, <name>.cuts.tsv, <name>.obs.tsv
import struct, re, sys, os
from itertools import combinations
import lz4.block
from capstone import *

src, outdir, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(src, 'rb').read()
assert d[:4] == b'NSO0'
flags = struct.unpack_from('<I', d, 0xc)[0]
segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
bss = struct.unpack_from('<I', d, 0x3c)[0]
csz = struct.unpack_from('<III', d, 0x60)
IMG = bytearray(segs[2][1] + segs[2][2] + bss)
for i, (fo, mo, sz) in enumerate(segs):
    raw = d[fo:fo + csz[i]]
    if flags >> i & 1: raw = lz4.block.decompress(raw, uncompressed_size=sz)
    IMG[mo:mo + sz] = raw
TEXT_END = segs[0][2]
RO_OFF, DATA_OFF = segs[1][1], segs[2][1]
KEY_PAGE_OFF = (len(IMG) + 0x3FFF) & ~0x3FFF
IMG.extend(bytes(0x4000))
PATCH_OFF = (len(IMG) + 0xFFFF) & ~0xFFFF

# ---- 已登记剪口：{NSO 构建号: [(偏移, 原字节, 新字节, 理由)]} ----
# 联网剪口（排除范围：战网 / 联机）：取自社区离线修复（LFS Mod Offline Fix for v2228224，与官方 94012 main 只差这 7 个字）。
# 实测系统服务层（nifm 无网 / DNS 黑洞 / 账号服务）绕不过：游戏自己直接发战网请求、按「上次在线」时间放行离线。
# ---- Registered cuts: {NSO build ID: [(offset, original bytes, new bytes, reason)]} ----
# Networking cuts (excluded scope: Battle.net / online play): taken from a community offline fix (LFS Mod Offline Fix for v2228224, which differs from the official 94012 main in only these 7 words).
# Measured: the system service layer (nifm offline / DNS black hole / account service) cannot get around it: the game itself sends Battle.net requests directly and allows offline play according to the "last online" time.
CUTS = {'db0507f36b84200ac05f75d76ed5026f': [
    # （放进所有产物共用的基础改写）：游戏显存分配器预算 260 MB → 512 MB
    # (Placed in the base rewrite shared by all build outputs): game video memory allocator budget 260 MB → 512 MB
    (0x8524c4, '0808a2d2', '0800a4d2', '显存分配器初始化配置：mov x8,#0x10400000（260 MB）→ mov x8,#0x20000000（512 MB），随后 movk #0xa0,lsl48 与 str [sp,#0x238] 不变；'
     '原预算下内部分辨率 > 约 345 万像素时（2736x1260 过、2864x1240 崩）渲染目标放不下，main+0x174b018 返回空、main+0x17417a4 读空崩溃；'
     '只是显存预算，不进游戏逻辑：cvd5 逐拍对拍在 1600x900 与 2868x1320 两档均一致'),
    (0x1979bc, 'e8a79f1a', 'e8d79f1a', '离线放行窗口：读存档设置键（0x1d53e80 处字串）的上次在线时刻，'
     '「当前时刻 − 上次在线 < 0x0009356907420000 ns（= 30 天）」置位 [x20+0x2c]；lt→gt'),
    (0x5b14dc, 'e8a79f1a', 'e8d79f1a', '调 0xddab50（缓冲区 [x10+0x138]+x25、剩余长度、回调 0x527d50、表 0x209cdb8、9 项）返回值 <1 置位，'
     '为真时走 0x5b14f8 分支；lt→gt（业务含义未从字串定出）'),
    (0x5f3184, 'e8a79f1a', 'e8d79f1a', '数据表 [0x2d64a18+idx*16] → +0xf90 记录（0x1fc 字节）+0x4c 下标 → +0xfd0 记录（0x128 字节）+0xf1 有符号字节 <0 置位，'
     '与（w26>0x24）相与后作第 4 参传下游；lt→gt（业务含义未从字串定出）'),
    (0x1d53e80, '55736572204c617374204f6e6c696e6500', '566f6963654c616e677561676500000000',
     '只读字串 "User Last Online" → "VoiceLanguage"：第 1 处改读一个必定存在的设置键，「上次在线」取到小数值，差值恒超 30 天，配合 gt 恒放行'),
    # 产品剪口：开场 Logo 与「按 A 开始」页剪掉，启动直达主菜单（装载画面保留）
    # Product cut: the opening Logo and the 「按 A 开始」(Press A to Start) page are cut, and startup goes straight to the main menu (the loading screen is kept)
    (0x8019ec, '280f0034', 'e8feff34', '前端引导首次进入分支：cbz w8,#0x801bd0（LogoMenuPanel → Logo 视频 → 标题页）改为 cbz w8,#0x8019c8'
     '（落到直达路径的 0x23e5e0(1) → 0xb156a0 进主菜单；跳过 0x8019bc 起置联机模式 [0x2d7badc]=1 的两条，联机模式保持启动初值 0 = 离线，'
     '不触发 0x80ec44 的联网检查、断网弹窗与回标题页）；r4 版目标 0x8019bc 会置联机模式 1，实测仍回标题页并弹断网框'),
    # 宿主原生实现剪口（仅宿主：真值模拟器不应用，Ryujinx 不认识 0x80 起的宿主调用号；等价由逐块解码 5511/5511 一致证明）
    # Cuts for host-native implementations (host only: the ground-truth emulator does not apply them, Ryujinx does not know host call numbers from 0x80 up; equivalence is proven by per-block decoding matching 5511/5511)
    (0x1b0ee60, 'fc6fbaa9fa6701a9', '011000d4c0035fd6', 'TACT BLTE 块 zlib 解码入口（DecodeZlib）→ svc #0x80 ; ret：宿主用 libdeflate 实现'
     '（hle_tact.c，调用号登记在 hle.h SVC_HOST_TACT_ZLIB）；改写器先打剪口再扫描，svc #0x80 自动成为宿主门，门块保存恢复 x30，ret 回原调用者', '仅宿主'),
    # 宿主测试开关的落点（仅宿主）：缺省宿主原样转调，行为不变
    # Hook points for host test switches (host only): by default the host forwards as is, behavior unchanged
    (0x5d3a88, 'a27a0194', '211000d4', '收包泵 0x5d3960 投递客户端命令包的调用点 bl 0x632510 → svc #0x81：宿主缺省以原参数原样调用 0x632510（返回值调用方不用）；'
     'DM_C2S_REPLAY 时丢弃客户端自发命令、改由拍入口按文件注入（diag.c，调用号登记在 hle.h SVC_HOST_C2S_DELIVER）', '仅宿主'),
    (0x5d39c8, 'ba750194', '411000d4', '收包泵 0x5d3960 投递系统类消息的调用点 bl 0x6310b0 → svc #0x82：宿主缺省以原参数原样调用 0x6310b0（返回值调用方不用）；'
     'DM_C2S_REPLAY 时，回放文件首拍起丢弃宿主客户端自发的系统类消息（之前的加入消息放行），改由拍入口按文件注入（diag.c，调用号登记在 hle.h SVC_HOST_C2S_SYS）', '仅宿主'),
    # 屏幕比例（仅宿主，配合下方 PARAMS 分辨率参数位）：输出分辨率上限 {u32 宽, u32 高}，只被分辨率选择的三处读写
    # （0xe12e28 / 0xe10d70 读：选中值的宽或高超上限即不换；0xe12f54 初始化选完后写成选中值）；初值 1920x1080 → 4096x2160，让按屏幕比例放宽的宽度能通过
    # Screen aspect (host only, together with the resolution parameter slots in PARAMS below): output resolution cap {u32 width, u32 height}, read or written at only three places in resolution selection
    # (0xe12e28 / 0xe10d70 read: if the selected value's width or height exceeds the cap, no switch is made; 0xe12f54 writes it to the selected value after initialization finishes selecting); initial value 1920x1080 → 4096x2160, so that a width widened to the screen aspect can pass
    (0x211da30, '8007000038040000', '0010000070080000', '输出分辨率上限初值 1920x1080 → 4096x2160（数据段）：宿主按屏幕比例给的宽度（如 iPhone 19.5:9 的 1956x900）'
     '须不超上限才会被选中；Mac 前端仍给原值 1600x900 / 1280x720，行为不变', '仅宿主'),
    # 运行模式通知处理（0xe10d30）给「窗口尺寸变化」事件的宽度：原码 mov w9,#0x740; and x9,x25,x9——编译器知道宽只可能是 1280（0x500）/ 1600（0x640），
    # 把「取低 32 位」优化成了 & (0x500|0x640)。KRES0 / KRES1 参数位换掉了那两个常数，这个推导出来的掩码没跟着改：2868 & 0x740 = 768、2296 & 0x740 = 64
    # （运行中切主机 / 掌机后输出宽 64 / 768，iOS 过热降级走的就是这条）。改成直接取低 32 位：对原版两个宽度结果相同，对任意宽度正确
    # The width that the operation-mode notification handler (0xe10d30) gives to the "window size changed" event: original code mov w9,#0x740; and x9,x25,x9 -- the compiler knew the width could only be 1280 (0x500) / 1600 (0x640),
    # and optimized "take the low 32 bits" into & (0x500|0x640). The KRES0 / KRES1 parameter slots replaced those two constants, but this derived mask was not changed along with them: 2868 & 0x740 = 768, 2296 & 0x740 = 64
    # (after switching docked / handheld while running, the output width was 64 / 768; the iOS overheating downgrade takes exactly this path). Changed to take the low 32 bits directly: same result for the original game's two widths, correct for any width
    (0xe10e48, '09e880522903098a', 'e903192a1f2003d5', '运行模式通知发窗口尺寸事件的宽度：mov w9,#0x740; and x9,x25,x9 → mov w9,w25; nop（取低 32 位，不再用原版两档宽度推出的常数掩码）', '仅宿主'),
    (0xe12e88, '09e880528902098a', 'e903142a1f2003d5', '启动初始化选分辨率（0xe12d70）同一处掩码：mov w9,#0x740; and x9,x20,x9 → mov w9,w20; nop（structck 参数门下游掩码检查查出；启动时事件接收对象通常未建，平时不显形）', '仅宿主'),
    # 产品剪口（复刻排除清单「经典画面切换」）：经典画面切换 0xb15d60 的两条状态写入 → nop。
    # 画面过渡状态在 0x2dc0758 起（+4 起点、+8 目标、+0xc 进度；1.0 = 新画面）；全程序写它的只有 0x852280（启动初始化写 1）、0x85cb20（强制回新画面写 1）、
    # 0x856be0（只写 +0xc）与 0xb15d60（不在局内或局版本 1 / 2 时按当前系数在 0 / 1 间翻转目标）——后者是唯一能把目标推向 0 的地方；
    # 调用方 0x36a5a0 / 0x4035e8 / 0xc25f48 / 0xc2725c 都是切换动作（视频选项 LegacyGraphicsMode、建角页 ToggleLegacy、主机版 ClassicToggle、按键 LegacyToggle 汇入此处）。
    # 剪掉的是两条普通存储、不含调用：被剪部分的同步原语与对外副作用清单为空（剪口规矩）。局内局版本 3 的提示分支（0xb15e2c 起）不动
    # Product cut (reproducing the exclusion list item "classic picture toggle"): the two state writes of the classic picture toggle 0xb15d60 → nop.
    # The picture transition state starts at 0x2dc0758 (+4 start, +8 target, +0xc progress; 1.0 = new picture); in the whole program the only writers are 0x852280 (startup init writes 1), 0x85cb20 (force back to the new picture, writes 1),
    # 0x856be0 (writes only +0xc) and 0xb15d60 (when not in a run, or when the run version is 1 / 2, flips the target between 0 / 1 according to the current coefficient) -- the last is the only place that can push the target toward 0;
    # the callers 0x36a5a0 / 0x4035e8 / 0xc25f48 / 0xc2725c are all toggle actions (the video option LegacyGraphicsMode, the character creation page's ToggleLegacy, the console version's ClassicToggle and the key LegacyToggle all converge here).
    # What is cut is two plain stores, containing no calls: the list of synchronization primitives and external side effects of the cut part is empty (the cut rule). The hint branch for run version 3 in a run (from 0xb15e2c) is untouched
    (0xb15e20, '0281002d', '1f2003d5', '经典画面切换写起点 / 目标（stp s2, s0, [x8, #4]）→ nop：目标恒为 1.0（新画面）'),
    (0xb15e24, '1f0d00b9', '1f2003d5', '经典画面切换清进度（str wzr, [x8, #0xc]）→ nop'),
    # 宽屏（仅宿主，配合 PARAMS 的 KASPECT 宽高比上限）：界面画布宽 = min(屏宽, 4560 = 2160 × 19/9) 的上限放开到 0xffff。
    # 原版 16:9 下屏宽 3840 单位 < 4560，结果不变；iPhone 1952x900 下屏宽约 4685 单位，原先被夹到 4560、两侧各留约 26 像素
    # Widescreen (host only, together with the KASPECT aspect ratio cap in PARAMS): the cap in UI canvas width = min(screen width, 4560 = 2160 × 19/9) is opened up to 0xffff.
    # In the original game at 16:9 the screen width of 3840 units < 4560, so the result is unchanged; on iPhone 1952x900 the screen width is about 4685 units, which used to be clamped to 4560, leaving about 26 pixels on each side
    (0xb1a660, '083a8252', 'e8ff9f52', '界面画布宽上限 mov w8, #4560 → mov w8, #0xffff（画布宽 = 屏宽）', '仅宿主'),
    # （撤销：原 r13 / r15 起的「SetLanguage 0x317298 清空 + 重装字符串表」剪口 0x3172f8 / 0x3173e4（原为排除简中以外的语言而设），
    # 因英文版撤销——原版切语言要靠这次重装。）
    # 产品剪口：字符串表 A（legacy，0x2d70ff0 / 0x2d71010）的装载整段跳过。装载函数 0x7f64f0(w0 = 是否装 legacy)
    # 在 0x7f669c tbz w23 处分支：不装时成功标志置 1（0x7f6800），与 17 个 strings-legacy/*.json 全部装成功的结果相同。
    # 无读者证明：全程序引用表 A 的 128 个函数里 124 个同时读选表标志 [0x2d71098] 并按「标志 = 0 → 表 B（0x2d71030 / 0x2d71050）」选表
    # （如 0x13b7d8 csel），其余 4 个是表自身的静态构造 0x7f8f80、装载 0x7f64f0、插表 0x7f72e0、清空 0x7f5b60。标志唯一写入点 0xb1bcf4
    # 写 !obj[0xb9]，obj[0xb9] 由画面过渡状态 0x2dc0758 与 1.0 比较得出——r14 已剪掉唯一能把目标推离 1.0 的经典画面切换；标志在 .bss
    # （初值 0），r15 cvd5 全程探针 0xb1bcfc 命中 0 次（写入点从未执行）⇒ 全程只读表 B。被剪树 = 17 次 0x7f72e0 单文件装载，同上方
    # 语言重装剪口的补证（同一 0xdf67f0 读事务，成对、无外部等待者）
    # (Reverted: the cuts 0x3172f8 / 0x3173e4 for "SetLanguage 0x317298 clear + reload string tables" in place since r13 / r15 (originally set up to exclude languages other than Simplified Chinese),
    # reverted because of the English version -- switching language in the original game relies on this reload.)
    # Product cut: the loading of string table A (legacy, 0x2d70ff0 / 0x2d71010) is skipped as a whole. The load function 0x7f64f0(w0 = whether to load legacy)
    # branches at the tbz w23 at 0x7f669c: when not loading, the success flag is set to 1 (0x7f6800), the same result as all 17 strings-legacy/*.json loading successfully.
    # No-reader proof: of the 128 functions in the whole program that reference table A, 124 also read the table-select flag [0x2d71098] and select the table by "flag = 0 → table B (0x2d71030 / 0x2d71050)"
    # (e.g. the csel at 0x13b7d8); the other 4 are the table's own static constructor 0x7f8f80, load 0x7f64f0, table insert 0x7f72e0 and clear 0x7f5b60. The flag's only write site 0xb1bcf4
    # writes !obj[0xb9], and obj[0xb9] is obtained by comparing the picture transition state 0x2dc0758 with 1.0 -- r14 already cut the classic picture toggle, the only thing that can push the target away from 1.0; the flag is in .bss
    # (initial value 0), and the probe at 0xb1bcfc over the whole r15 cvd5 run hit 0 times (the write site never executed) ⇒ only table B is read throughout. The cut tree = 17 single-file loads via 0x7f72e0; same as the supplementary proof of
    # the language reload cut above (the same 0xdf67f0 read transaction, paired, no external waiters)
    (0x7f669c, '370b0036', '59000014', '字符串表装载 tbz w23, #0（是否装 legacy 表 A）→ b 0x7f6800：表 A 无读者（选表标志恒 0 → 表 B），整段 17 个 legacy 文件不装'),
    # Granny 段解码（BitKnit2）两条流式段读取路径的逐块解码调用点：宿主缺省原样转调 0x11f4af0（预解码方案 A 的替换落点，hle_granny.c）
    # The per-block decode call sites of the two streaming segment read paths of Granny segment decoding (BitKnit2): by default the host forwards as is to 0x11f4af0 (the replacement hook point of pre-decode plan A, hle_granny.c)
    (0x11f02dc, '05120094', '611000d4', '段读取 0x11f0110 逐块解码 bl 0x11f4af0(ctx, 块, 长度) → svc #0x83：宿主缺省以原参数原样调用并回填 w0（hle.h SVC_HOST_GRANNY_A）', '仅宿主'),
    # 掉落光（仅宿主）：物品取外观定义路径 bl 0xa14dc4(资源管理器, 单位, x8 输出字符串) → svc #0x87：宿主原样代调，
    # 对模式 3 / 5（地上 / 掉落中）的物品按规则把路径改指覆盖层里的带光变体（dm_dropfilter.c svc_ground_asset；hle.h SVC_HOST_GROUND_ASSET）
    # 一档掉落音（仅宿主）：落地取掉落音 bl 0x470c20(物品, &触发帧) → svc #0x88：宿主原样代调，一档物品换成提示音（dm_dropfilter.c svc_drop_sound）
    # Drop light (host only): the item's appearance definition path lookup bl 0xa14dc4(resource manager, unit, x8 output string) → svc #0x87: the host forwards the call as is,
    # and for items in mode 3 / 5 (on the ground / dropping) repoints the path by rule to the lit variant in the overlay (dm_dropfilter.c svc_ground_asset; hle.h SVC_HOST_GROUND_ASSET)
    # Tier-one drop sound (host only): fetching the drop sound on landing bl 0x470c20(item, &trigger frame) → svc #0x88: the host forwards the call as is, and tier-one items get a notification sound instead (dm_dropfilter.c svc_drop_sound)
    (0x468f2c, '3d1f0094', '011100d4', '物品落地取掉落音调用点 bl 0x470c20 → svc #0x88：宿主原样代调后按规则把一档物品的掉落音换成提示音（其余原样）', '仅宿主'),
    (0x8fa590, '0d6a0494', 'e11000d4', '物品外观路径调用点 bl 0xa14dc4 → svc #0x87：宿主原样代调后按规则改指地上物品的带光变体（存储 / 穿戴 / 腰带 / 镶嵌不碰）', '仅宿主'),
    # 进局出生点（进局落在城镇小站旁、尸体与人同处；仅宿主）：玩家进局函数 0x5d7f70 里给玩家、给尸体各取一次出生点，都是 bl 0x5596c0(数据版本, 幕, 关卡, 格类型 0, &x, &y, 单位尺寸)
    # → svc #0x8a（玩家）/ #0x8b（尸体）：宿主原样代调；进局在城镇时改按「小站格 13 + 固定安全距离」取点，人、尸体、佣兵由原版一次放好（不再第 2 拍换关挪人，尸体不再留在原出生点）（dm_townwp.c svc_join_spawn）
    # Run-entry spawn point (entering a run lands next to the town waypoint, with the corpse in the same place as the player; host only): the player join function 0x5d7f70 fetches a spawn point once for the player and once for the corpse, both via bl 0x5596c0(data version, act, level, cell type 0, &x, &y, unit size)
    # → svc #0x8a (player) / #0x8b (corpse): the host forwards the call as is; when the run is entered in town it instead picks the point by "waypoint cell 13 + a fixed safe distance", and the player, corpse and mercenary are placed in one go by the original game (the player is no longer moved by a level change on tick 2, and the corpse no longer stays at the original spawn point) (dm_townwp.c svc_join_spawn)
    (0x5d8040, 'a005fe97', '411100d4', '玩家进局取出生点调用点 bl 0x5596c0 → svc #0x8a：宿主原样代调，进局在城镇时出生点换成小站旁（hle.h SVC_HOST_JOIN_SPAWN）', '仅宿主'),
    (0x5d8400, 'b004fe97', '611100d4', '进局给尸体取落点调用点 bl 0x5596c0 → svc #0x8b：同上，尸体与人取同一个点（hle.h SVC_HOST_JOIN_CORPSE）', '仅宿主'),
    (0x11f0914, '77100094', '811000d4', '段读取 0x11f07d0 逐块解码 bl 0x11f4af0(ctx, 块, 长度) → svc #0x84：同上（hle.h SVC_HOST_GRANNY_B）', '仅宿主'),
]}
# ---- 已登记观测门：{NSO 构建号: [(偏移, 编号, 原指令字, 理由)]}。编号与宿主 hle.h 的 OBS_* 一致；
# 原指令须与位置无关（不得 PC 相对 / 分支 / 涉 x18·tpidr），离线断言；宿主未挂回调时门块进出一次即返回 ----
# ---- Registered observation gates: {NSO build ID: [(offset, number, original instruction word, reason)]}. Numbers match OBS_* in the host's hle.h;
# the original instruction must be position independent (no PC-relative / branch / anything involving x18 or tpidr), asserted offline; when the host has no callback attached the gate block goes in and out once and returns ----
OBS = {'db0507f36b84200ac05f75d76ed5026f': [
    (0xb1dc60, 1, 'd10603ff', 'UI 消息总线虚函数实现入口（x1 = 消息 {u64 类型哈希, u64 动作哈希, char* 面板名, …}）：'
     'DM_UIMSG 消息流与 DM_BOOTPROF 启动分段的唯一来源；原指令 sub sp, sp, #0x180'),
    (0x5d3bc0, 2, 'd10503ff', '服务端拍入口（x0 = 游戏对象 G）：DM_TICKS 逐拍 6 格状态哈希（dm/verify/tick_state_format.md，与真值 swtick.py 同一采集点）；'
     '原指令 sub sp, sp, #0x140'),
    (0x6e5b40, 3, 'd10103ff', '客户端命令分派入口 (G, 单位, 包, 长度)：DM_C2S_LOG 命令包序列（dm/verify/command_packet_format.md，与真值同一采集点）；'
     '原指令 sub sp, sp, #0x40'),
    (0x6310b0, 4, 'a9bb67fc', '系统类客户端消息处理入口 (buf = u32 客户端号 + 包, 包长)：包号 ≥ 0x67（加入 / 心跳 0x6d / 0x6f 等），收包泵先抽干此队列；'
     'DM_C2S_LOG 记为 "q":"sys"；原指令 stp x28, x25, [sp, #-0x50]!'),
    (0x317298, 5, 'd10183ff', 'SetLanguage 入口（x1 → u32 新语言号，语言名表 main+0x20ae478）：配合上方语言重载剪口，宿主断言新语言为 zhCN；'
     '原指令 sub sp, sp, #0x60'),
]}
BUILD_ID = d[0x40:0x50].hex()
# 构建期产品规则：旧原资源/对拍域默认不变；触屏版单独出新版本，不在运行时写客体代码。
# Build-time product rules: the old original-resource / comparison domain is unchanged by default; the touchscreen version is issued separately as a new version, and guest code is not written at runtime.
RULESET = os.environ.get('DM_RULESET', 'native')
assert RULESET in ('native', 'touch-run-v4'), f'未知规则域 {RULESET}'
RULE_BUILD = 'db0507f36b84200ac05f75d76ed5026f'
RULE_RUN = RULESET == 'touch-run-v4' and NAME == 'main'
if RULE_RUN:
    import hashlib
    assert BUILD_ID == RULE_BUILD, f'Run规则构建号不符 {BUILD_ID}'
    RULE_ORIG_SHA = hashlib.sha256(IMG).hexdigest()
cuts = list(CUTS.get(BUILD_ID, []))
# 触屏版规则域默认同时带宿主私有门；规则闸门可显式关闭，保留同一出包入口和规则域。
# The touchscreen rule domain by default also carries the host-private gates; the rule gate check can turn them off explicitly, keeping the same packaging entry point and rule domain.
PRIVATE_GATES = os.environ.get("DM_PRIVATE_GATES", "1") != "0"
PRIVATE_AIM = RULE_RUN and PRIVATE_GATES
if PRIVATE_AIM:
    cuts.append((0x2faf04, 'fde7fe97', 'c11000d4',
      'skill-aim-v1-private: skill ground direction call only; svc86 delegates original2b4ef8 then sampled-right/original rotation+grid; zero/nonplayer/non-skill retain original; busy/town/skill/server untouched', '仅宿主'))
if RULE_RUN:
    # 方向光阴影图边长（画质档位 1，0x168a520 填 +0x48 → 建 2 层数组 0x1697394）：2100 → 1024。授权「视觉无差别即可」；
    # mini 108 张 + 连续 16 帧：营地 / 野外 / 战斗各档到 1024 的 FLIP 与同设置重跑的噪声分不开、影子区相邻帧变化量 0.71 → 0.75；真机两张阴影合计约 0.7–1.4 ms / 帧。
    # 游戏传给着色器的阴影尺寸常量与视口随同一配置值变（转储核过）。native 规则域（Mac 对原版真值的闸门）保持 2100
    # Directional light shadow map edge length (quality level 1, 0x168a520 fills +0x48 → creates the 2-layer array 0x1697394): 2100 → 1024. Authorization: "visually indistinguishable is enough";
    # on the second Mac, 108 images + 16 consecutive frames: for camp / wilderness / combat, at every level down to 1024 the FLIP cannot be told apart from the noise of a rerun with the same settings, and the change between adjacent frames in shadow regions is 0.71 → 0.75; on a real device the two shadow maps total about 0.7–1.4 ms / frame.
    # The shadow size constants and the viewport that the game passes to the shader change with the same config value (verified by dump). The native rule domain (the Mac gate check against the original game's ground truth) stays at 2100
    SHADOW_SIDE = 1024
    movz_w14 = lambda n: struct.pack('<I', 0x52800000 | (n << 5) | 14).hex()   # movz w14,#n——字节由表达式算，不手写（手算成 #32768，阴影图 32768² 分配失败启动即崩） | movz w14,#n -- the bytes are computed by an expression, not written by hand (a hand calculation came out as #32768, and the shadow map allocation of 32768² failed, crashing at startup)
    cuts.append((0x168a580, movz_w14(2100), movz_w14(SHADOW_SIDE), f'阴影图边长 mov w14,#2100 → mov w14,#{SHADOW_SIDE}（档位 1）', '仅宿主'))
if RULE_RUN:
    # 纯提示的系统通知不吞功能键：手柄版通知面板处理函数 0x3d84a0 遇「动作 9（PLUS）按下」就执行通知带的确认动作并吞键（给「升级通知按 PLUS 直达技能树」用）。
    # 系统通知（事件队列第 12 类，0xab5c08；如改难度后的「游戏难度设为 N…」）带的确认动作只是关掉自己，结果是通知停留的 10 秒里第一下红十字没反应（真机 + Mac 复现）。
    # 改法：系统通知不带确认动作（类别哈希存 0）；没有确认动作的通知不处理 PLUS（走原函数的「转给子控件」出口 0x3d880c），到 notificationSeconds 自己关。带确认动作的通知不变
    # Purely informational system notifications do not swallow the function key: the controller version's notification panel handler 0x3d84a0, on "action 9 (PLUS) pressed", executes the confirm action carried by the notification and swallows the key (meant for "press PLUS on a level-up notification to go straight to the skill tree").
    # The confirm action carried by a system notification (event queue class 12, 0xab5c08; e.g. 「游戏难度设为 N…」(game difficulty set to N...) after changing the difficulty) only closes itself, and the result is that during the 10 seconds the notification stays up the first press of the red cross does nothing (reproduced on a real device + Mac).
    # Fix: system notifications carry no confirm action (the category hash is stored as 0); notifications without a confirm action do not handle PLUS (they take the original function's "forward to child widgets" exit 0x3d880c) and close by themselves at notificationSeconds. Notifications with a confirm action are unchanged
    cbz_x8 = lambda at, to: struct.pack('<I', 0xB4000008 | (((to - at) >> 2) & 0x7ffff) << 5).hex()
    str_x19_178 = lambda rt: struct.pack('<I', 0xF9000000 | (0x178 // 8) << 10 | 19 << 5 | rt).hex()
    cuts.append((0xab5c28, str_x19_178(8), str_x19_178(31), '系统通知的确认动作类别 str x8,[x19,#0x178] → str xzr（不带确认动作）', '仅宿主'))
    cuts.append((0x3d853c, cbz_x8(0x3d853c, 0x3d8560), cbz_x8(0x3d853c, 0x3d880c), '通知面板：确认动作类别为空 → 不处理 PLUS', '仅宿主'))
    cuts.append((0x3d8544, cbz_x8(0x3d8544, 0x3d8560), cbz_x8(0x3d8544, 0x3d880c), '通知面板：确认动作哈希为空 → 不处理 PLUS', '仅宿主'))
if RULE_RUN:
    # 技能说明框出在技能树侧边、不盖树：技能树面板每帧更新（0xc356b0）在 0xc35fd4 调说明框入口 0xb16660，位置参数 x1 = 图标绝对坐标（低 32 位 x、高 32 位 y + 3），
    # 手柄版说明框贴着图标右下展开，触屏上正好盖住手指周围的几个技能。0xc35fb0 的 bfxil x1,x8,#0,#32 是把 x1 的低半拷回 x1 的空操作（x8 = x1），
    # 换成 movk x1,#3000：横坐标给到屏幕外，由说明框入口自己的回夹（0xb0d560，按 TooltipsPanel 的 screenEdgePadding）贴到右安全区边；纵坐标不动
    # The skill tooltip appears beside the skill tree and does not cover the tree: the skill tree panel's per-frame update (0xc356b0) calls the tooltip entry 0xb16660 at 0xc35fd4, with position argument x1 = the icon's absolute coordinates (low 32 bits x, high 32 bits y + 3);
    # the controller version's tooltip expands against the icon's lower right, which on a touchscreen covers exactly the few skills around the finger. The bfxil x1,x8,#0,#32 at 0xc35fb0 is a no-op that copies the low half of x1 back into x1 (x8 = x1),
    # and is replaced with movk x1,#3000: the x coordinate is put off screen, and the tooltip entry's own clamp-back (0xb0d560, by TooltipsPanel's screenEdgePadding) snaps it to the right safe-area edge; the y coordinate is untouched
    cuts.append((0xc35fb0, struct.pack('<I', 0xB3407D01).hex(), struct.pack('<I', 0xF2800000 | 3000 << 5 | 1).hex(), '技能说明框横坐标给到屏幕外，由原回夹贴右边（不盖技能树）', '仅宿主'))
if RULE_RUN and PRIVATE_GATES:
    # 只登记精力资格/移动选择/走跑偏好；不改方向、真实幅度、合法mode与零轴分支。
    # 两个成本/选择器通用点的type0防护在下方原生小跳板，非player保原行为。
    # Only stamina eligibility / movement selection / walk-run preference are registered; direction, real magnitude, legal mode and the zero-axis branch are not changed.
    # The type0 guard for the two generic cost / selector points is in the small native trampoline below; non-player keeps the original behavior.
    run_words = [
        (0x2c620c, 0xb90e511f, 0x52800029, '偏好初始化：mov w9,1（x8仍为2d20000）'),
        (0x2c6210, 0xd00152c8, 0xb90e5109, '偏好初始化：str w9,[x8,e50]；替代同页重复adrp，保原x8'),
        (0x369a90, 0xd100c3ff, None, '客户端RunOff入口尾跳原RunOn；保其player0/合法mode/环境门'),
        (0x369980, 0x340006a0, 0xd503201f, '客户端RunOn仅取消既有player0门之后的零精力拒绝'),
        (0x2b2c74, 0x340000a0, 0xd503201f, '控制器d29资格仅取消stat10零值限制；幅度比较与停杆保原'),
        (0x2b2dc0, 0x3900031f, 0xd503201f, '控制器d29仅取消stat10零值清除；幅度归一化保原'),
        (0x2b30fc, 0x0a090101, 0x52800021, '已通过合法动作/零轴门的移动编码选择位固定Run；幅度不改'),
        (0x2b3104, 0x2a1f03e1, 0x52800021, '无用户走跑偏好时亦固定Run编码位；方向/幅度不改'),
        (0x6dc2c4, 0x1a880502, 0x52800062, 'server42合法移动请求固定Run3，原type0/死亡/锁定/幅度/方向门保留'),
        (0x6dce90, 0x340000e0, 0xd503201f, 'server49仅取消既有player0/mode2门之后的零精力拒绝'),
        (0x6dcec0, 0xa9bf7bfd, None, 'server4A入口尾跳原49；长度/type0/mode门保留'),
    ]
    for off, old, new, why in run_words:
        if new is None:
            target = 0x369900 if off == 0x369a90 else 0x6dce50
            new = 0x14000000 | (((target - off) >> 2) & 0x3ffffff)
        cuts.append((off, struct.pack('<I', old).hex(), struct.pack('<I', new).hex(), 'touch-run-v4：' + why))
cuts = [c if len(c) == 5 else (*c, '全') for c in cuts]          # 第 5 项作用域：全 | 仅宿主（仅宿主 = 真值模拟器不应用） | 5th item is the scope: 全 (all) | 仅宿主 (host only) (仅宿主 = not applied by the ground-truth emulator)
for off, ob, nb, why, scope in cuts:
    ob, nb = bytes.fromhex(ob), bytes.fromhex(nb)
    assert len(ob) == len(nb) and IMG[off:off + len(ob)] == ob, f'剪口原字节不符 {off:#x}：{bytes(IMG[off:off + len(ob)]).hex()}'
    IMG[off:off + len(nb)] = nb
# 键页下标固定；键页内容由装载器写入“实际键号 × 8”。
# Key page indices are fixed; the key page content is written by the loader as "actual key number × 8".
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511, 'KRES0': 506, 'KRES1': 507, 'KASPECT': 505, 'KASPECTUI': 504}   # KRES*：分辨率参数位；KASPECT：画面宽高比上限（见 PARAMS） | KRES*: resolution parameter slots; KASPECT: picture aspect ratio cap (see PARAMS)
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE', 'KRES0', 'KRES1', 'KASPECT', 'KASPECTUI'))}

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
patch = bytearray(); fix = []; gates = []; stats = {}
def stat(k): stats[k] = stats.get(k, 0) + 1
def here(): return PATCH_OFF + len(patch)
def emit(w, kind=None):
    if kind: fix.append((len(patch), kind))
    patch.extend(struct.pack('<I', w & 0xFFFFFFFF))

def emit_key_addr(reg, kind):
    fix.append((len(patch), kind)); emit(ADRP(reg, here(), KEY_PAGE_OFF)); emit(LDRX(reg, reg, KEY_INDEX[kind]))

def emit_slot_addr(base, key_reg, kind):
    emit_key_addr(key_reg, kind); emit(ADDR(base, base, key_reg))

# ---- 编码器 ----
# ---- Encoders ----
def B(frm, to):
    off = to - frm; assert -(1 << 27) <= off < (1 << 27) and off % 4 == 0
    return 0x14000000 | ((off >> 2) & 0x3FFFFFF)
def STP(t, t2, n, off): return 0xA9000000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP(t, t2, n, off): return 0xA9400000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STP_PRE(t, t2, n, off): return 0xA9800000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP_POST(t, t2, n, off): return 0xA8C00000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STUR(t, n, off): return 0xF8000000 | ((off & 0x1FF) << 12) | (n << 5) | t   # stur xt,[xn,#off]（off 可负） | stur xt,[xn,#off] (off may be negative)
def LDUR(t, n, off): return 0xF8400000 | ((off & 0x1FF) << 12) | (n << 5) | t   # ldur xt,[xn,#off]
def LDRX(t, n, imm12=0): return 0xF9400000 | (imm12 << 10) | (n << 5) | t
def STRX(t, n, imm12=0): return 0xF9000000 | (imm12 << 10) | (n << 5) | t
def MRS_TPIDRRO(t): return 0xD53BD060 | t
def ADRP(dd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12; assert -(1 << 20) <= imm < (1 << 20)
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | dd
def ADDI(dd, n, imm12, sh=0): return 0x91000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def ADDR(dd, n, m): return 0x8B000000 | (m << 16) | (n << 5) | dd
def SUBI(dd, n, imm12, sh=0): return 0xD1000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def BR(n): return 0xD61F0000 | (n << 5)
def BLR(n): return 0xD63F0000 | (n << 5)
NOP = 0xD503201F

def dis(w, addr):
    l = list(md.disasm(struct.pack('<I', w), addr)); assert len(l) == 1, hex(w)
    return l[0]
def regset(ins):
    r, w = ins.regs_access()
    return {ins.reg_name(x) for x in r}, {ins.reg_name(x) for x in w}
def num(name):
    m = re.fullmatch(r'[xw](\d+)', name or ''); return int(m.group(1)) if m else None
def has18(ins): return re.search(r'\b[xw]18\b', ins.op_str) is not None
def regs_in(ins):
    s = {int(n) for n in re.findall(r'\b[xw](\d+)\b', ins.op_str)}
    try:
        rd, wr = regset(ins); s |= {num(n) for n in rd | wr} - {None}
    except Exception: pass
    return s
def pick(avoid, n=2):
    out = [r for r in range(0, 16) if r not in avoid and r != 18][:n]; assert len(out) == n; return out

def subst(ins, S):
    """x18/w18 → xS/wS：只改 4 个寄存器字段位置，用反汇编文本核对。 | x18/w18 → xS/wS: only the 4 register field positions are changed, verified against the disassembly text."""
    w = struct.unpack('<I', ins.bytes)[0]
    want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{S}', ins.op_str)
    pos = [p for p in (0, 5, 10, 16) if (w >> p) & 31 == 18]
    for k in range(len(pos), 0, -1):
        for c in combinations(pos, k):
            nw = w
            for p in c: nw = (nw & ~(31 << p)) | (S << p)
            try: dd = dis(nw, ins.address)
            except AssertionError: continue
            if dd.mnemonic == ins.mnemonic and dd.op_str == want: return nw
    raise RuntimeError(f'subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}')

# ---- 各类改写块 ----
# ---- Rewrite blocks of each kind ----
def sp_delta(ins):
    """指令对 sp 的写回量（前变址 [sp, #i]! / 后变址 [sp], #i）；不写回 sp 返回 0。 | The amount an instruction writes back to sp (pre-index [sp, #i]! / post-index [sp], #i); returns 0 if sp is not written back."""
    m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-f]+)\]!', ins.op_str)
    if m: return int(m.group(1), 0)
    m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-f]+)$', ins.op_str)
    if m: return int(m.group(1), 0)
    assert not re.match(r'(sp|wsp),', ins.op_str), f'writes sp: {ins.address:x} {ins.op_str}'
    return 0

def site_plain(ins):
    S, T = pick(regs_in(ins))
    dlt = sp_delta(ins)
    save = -16 if dlt == 0 else -112          # 改 sp 的指令会写到 sp 下方，暂存放到保留区深处 | Instructions that modify sp write below sp, so the scratch save is placed deep in the reserved area
    assert dlt == 0 or dlt >= -96, f'sp delta {dlt} at {ins.address:x}'
    start = here()
    emit(STP(S, T, 31, save)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(subst(ins, S))
    emit(STRX(S, T))                 # 一律写回：capstone 会漏报写入（adds w18 实例） | Always write back: capstone can miss reporting a write (the adds w18 instance)
    emit(LDP(S, T, 31, save - dlt)); emit(B(here(), ins.address + 4))
    return start

def site_cond(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    S, T = pick(regs_in(ins))
    tgt = ins.operands[-1].imm
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    m = ins.mnemonic
    taken = here() + 4 * 3
    off = (taken - here()) >> 2
    nw = ((w & 0xFF000000) | ((off & 0x7FFFF) << 5) | S) if m in ('cbz', 'cbnz') else ((w & 0xFFF80000) | ((off & 0x3FFF) << 5) | S)
    pc = here(); emit(nw)
    dd = dis(nw, pc); assert dd.mnemonic == m and dd.operands[-1].imm == taken
    emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
    assert here() == taken
    emit(LDP(S, T, 31, -16)); emit(B(here(), tgt))
    return start

def site_pcrel(ins):
    tgt = ins.operands[1].imm
    S, T = pick(set())
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18')   # S 先作键页偏移暂存，下一条再装目标值 | S first serves as a temporary for the key page offset; the next instruction then loads the target value
    pc = here(); emit(ADRP(S, pc, tgt))
    if ins.mnemonic == 'adr': emit(ADDI(S, S, tgt & 0xFFF))
    emit(STRX(S, T))
    emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
    return start

def site_br(ins, base, count):
    """跳转表 br x18：目标 = base + 4i。x16 暂存红区 → 等距落地垫 → 恢复 x16 → 原目标。 | Jump table br x18: target = base + 4i. Stash x16 in the red zone → equally spaced landing pads → restore x16 → original target."""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # x16 / x17 是函数内可存活的临时寄存器，借用前存红区 | x16 / x17 are temporary registers that can stay live within a function; save them to the red zone before borrowing
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(LDUR(17, 31, -24))
    padbase = start + 4 * 24
    delta = padbase - base
    op = ADDI if delta >= 0 else SUBI; dl = abs(delta)
    ops = []; hi, lo = dl >> 12, dl & 0xFFF
    while hi: h = min(hi, 0xFFF); ops.append(op(16, 16, h, 1)); hi -= h
    if lo: ops.append(op(16, 16, lo, 0))
    assert len(ops) <= 12
    for o in ops: emit(o)
    emit(BR(16))
    while here() < padbase: emit(NOP)
    tails = padbase + 4 * count
    for i in range(count): emit(B(here(), tails + 8 * i))
    for i in range(count):
        emit(0xF85F0000 | (31 << 5) | 16)      # ldur x16,[sp,#-16]
        emit(B(here(), base + 4 * i))
    return start

def site_sysreg_read(ins, kind):
    """mrs xN, tpidr_el0 / tpidrro_el0 → xN = 线程槽值。 | mrs xN, tpidr_el0 / tpidrro_el0 → xN = thread slot value."""
    N = ins.bytes[0] & 31
    tmp = 16 if N != 16 else 17
    start = here()
    emit(STUR(tmp, 31, -16)); emit(MRS_TPIDRRO(N)); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N)); emit(LDUR(tmp, 31, -16))   # tmp 存红区借用 | tmp is saved by borrowing the red zone
    emit(B(here(), ins.address + 4))
    return start

def site_tpidr_write(ins):
    N = struct.unpack('<I', ins.bytes)[0] & 31
    (T,) = pick({N}, 1)
    start = here()
    emit(0xF81F0000 | (31 << 5) | T)           # stur xT,[sp,#-16]
    tmp = next(x for x in (16, 17) if x not in (N, T))
    emit(STUR(tmp, 31, -24)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, tmp, 'KTLS'); emit(STRX(N, T)); emit(LDUR(tmp, 31, -24))
    emit(0xF85F0000 | (31 << 5) | T)           # ldur xT,[sp,#-16]
    emit(B(here(), ins.address + 4))
    return start

def emit_gate_call():
    """门块公共段（紧跟 stp x29,x30,[sp,#-16]! 之后、blr x30 之前）：x30 = 线程槽 KGATE 的门地址。
    键页取槽要借第二个寄存器，借 x29 后立即从栈上恢复：_dm_gate 存下的 x29 必须是客体原值——
    宿主按 [x29+8] 认调用者（技能绑定观测门 6、私有瞄准门 svc86）、崩溃回溯也走 x29 链
    （r24 方案 C 起借了不还，宿主拿到的 x29 是键页里的槽偏移，技能图标退回占位字）。
    Common section of a gate block (right after stp x29,x30,[sp,#-16]! and before blr x30): x30 = the gate address in thread slot KGATE.
        Fetching the slot through the key page needs a second register to be borrowed; x29 is borrowed and then immediately restored from the stack: the x29 saved by _dm_gate must be the guest's original value --
        the host identifies the caller by [x29+8] (skill binding observation gate 6, private aiming gate svc86), and the crash backtrace also walks the x29 chain
        (since r24 plan C it was borrowed and not returned, the x29 the host got was the slot offset from the key page, and skill icons fell back to placeholder text)."""
    emit(MRS_TPIDRRO(30)); emit_slot_addr(30, 29, 'KGATE'); emit(LDRX(30, 30)); emit(LDRX(29, 31))

def site_gate(ins, kind, arg):
    """svc / mrs cntpct_el0 → 宿主门。门地址取自线程槽 KGATE；门按返回点（本块 ldp 的地址）查 gates.tsv。 | svc / mrs cntpct_el0 → host gate. The gate address is taken from thread slot KGATE; the gate looks up gates.tsv by return point (the address of this block's ldp)."""
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(B(here(), ins.address + 4))
    gates.append((ret, kind, arg, ins.address))
    return start

def site_obs(off, oid, word):
    """观测门：入口原指令 → 进宿主门（arg = 观测编号）→ 恢复 x29/x30 → 执行原指令 → 跳回 off+4。
    门块前半与 site_gate 逐字同构；宿主只读寄存器、不改（dm_gate 回写的 x0–x8 即原值）。
    Observation gate: original entry instruction → enter the host gate (arg = observation number) → restore x29/x30 → execute the original instruction → jump back to off+4.
        The first half of the gate block is word-for-word isomorphic with site_gate; the host only reads registers and does not modify them (the x0–x8 written back by dm_gate are the original values)."""
    ins = dis(word, off)
    assert ins.mnemonic not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') and not ins.mnemonic.startswith('b.') \
        and not (ins.mnemonic.startswith('ldr') and '[' not in ins.op_str) and not has18(ins), f'观测门原指令不可搬 {off:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(word)
    emit(B(here(), off + 4))
    gates.append((ret, 'obs', oid, off))
    return start

# ---- 独占窗口 ----
# ---- Exclusive windows ----
EXCL_LD = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
EXCL_ST = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')
def find_windows(insl):
    out = []
    for i, ins in enumerate(insl):
        if ins.mnemonic not in EXCL_LD: continue
        j = i
        while j < len(insl) and insl[j].mnemonic not in EXCL_ST: j += 1
        if j >= len(insl) or j - i >= 16: continue
        if j + 1 < len(insl) and insl[j + 1].mnemonic in ('cbz', 'cbnz'): j += 1
        seg = insl[i:j + 1]
        if any(has18(x) for x in seg): out.append(seg)
    return out

def site_window(seg):
    used = set()
    for x in seg: used |= regs_in(x)
    S, T = pick(used)
    lo, hi = seg[0].address, seg[-1].address + 4
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    copy0 = here(); n = len(seg)
    exits = copy0 + 4 * n
    ex_targets = []; words = []
    for k, x in enumerate(seg):
        w = struct.unpack('<I', x.bytes)[0]
        if has18(x): w = subst(x, S)
        m = x.mnemonic
        if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            tgt = x.operands[-1].imm
            if lo <= tgt < hi: newt = copy0 + (tgt - lo)
            else: ex_targets.append(tgt); newt = exits + 16 * len(ex_targets)   # 出口桩第 0 个留给顺序落出（见下），窗口外的目标从第 1 个起 | Exit stub 0 is reserved for the sequential fall-out (see below); targets outside the window start from stub 1
            pc = copy0 + 4 * k; off = (newt - pc) >> 2
            if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
            else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
            dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
        else:
            assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
        words.append(w)
    for w in words: emit(w)
    # 出口桩的次序：副本最后一条指令执行完是「顺序落出」，落进紧跟在副本后面的第 0 个出口桩——所以第 0 个必须是回 hi 的落出口，窗口外的跳转目标排在其后。
    # r40 及以前落出口排在最后：窗口里只要有跳到窗口外的条件跳转（比较交换的「比较不等」出口），写成功后就落进第一个外部出口 = 当成比较失败处理
    # （一次操作做两遍 / 无锁表被多弹）。暗黑有 6 个这样的窗口（main 0x1848eb8 / 0x1848f28 / 0x1848f84，sdk 0x2ea0fc / 0x2ea4cc / 0x2ea644），
    # 只有跳回窗口内的 cbnz 的窗口不受影响。等价验证原先随机抽不到「比较相等」那一向，所以一直是绿的（equiv.py 已补窗口分支双向强制）。
    # Order of the exit stubs: when the last instruction of the copy has executed, control "falls out sequentially" into exit stub 0, which immediately follows the copy -- so stub 0 must be the fall-out exit back to hi, and jump targets outside the window come after it.
    # Up to r40 the fall-out exit was placed last: whenever a window contained a conditional jump to outside the window (the "compare not equal" exit of a compare-and-swap), a successful write fell into the first external exit = handled as a failed compare
    # (one operation done twice / a lock-free list popped more than once). The game has 6 such windows (main 0x1848eb8 / 0x1848f28 / 0x1848f84, sdk 0x2ea0fc / 0x2ea4cc / 0x2ea644);
    # windows whose only branch is a cbnz back into the window are not affected. The equivalence check used to never draw the "compare equal" direction at random, so it stayed green (equiv.py now forces both directions of window branches).
    for t in [hi] + ex_targets:
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), t)); emit(NOP)
    return start, lo, hi

# ---- 函数边界（eh_frame_hdr）----
# ---- Function boundaries (eh_frame_hdr) ----
m0 = struct.unpack_from('<I', IMG, 4)[0]
assert IMG[m0:m0 + 4] == b'MOD0'
ehs = m0 + struct.unpack_from('<i', IMG, m0 + 16)[0]
def rd(o, enc):
    fmt, app = enc & 0xF, enc & 0x70
    v, sz = {0x3: (struct.unpack_from('<I', IMG, o)[0], 4), 0xB: (struct.unpack_from('<i', IMG, o)[0], 4),
             0x4: (struct.unpack_from('<Q', IMG, o)[0], 8)}[fmt]
    if app == 0x10: v += o
    if app == 0x30: v += ehs
    return v, sz
_, penc, cenc, tenc = IMG[ehs:ehs + 4]
o = ehs + 4
_, s = rd(o, penc); o += s
nfde, s = rd(o, cenc); o += s
starts = set()
for _ in range(nfde):
    a, s = rd(o, tenc); o += s
    _, s = rd(o, tenc); o += s
    starts.add(a)
starts = sorted(x for x in starts if x < TEXT_END)

# ---- 线性扫描找全部改写点（不靠寄存器读写报告）----
# ---- Linear scan to find all rewrite points (not relying on register read/write reports) ----
kinds = {}
for o in range(0, TEXT_END, 4):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w & 0xFFE0001F == 0xD4000001: kinds[o] = ('svc', (w >> 5) & 0xFFFF); continue
    if w & 0xFFFFFFE0 == 0xD53BD040: kinds[o] = ('tpidr_r', None); continue
    if w & 0xFFFFFFE0 == 0xD51BD040: kinds[o] = ('tpidr_w', None); continue
    if w & 0xFFFFFFE0 == 0xD53BD060: kinds[o] = ('tpidrro_r', None); continue
    if w & 0xFFFFFFE0 == 0xD53BE020: kinds[o] = ('cntpct', w & 31); continue
    if w & 0xFFFFFFE0 == 0xD53BE040: kinds[o] = ('cntvct', w & 31); continue
    # x18/w18 只会占这些寄存器字段；无匹配的字无需反汇编。 | x18/w18 can only occupy these register fields; skip words without a match.
    if not any((w >> p) & 31 == 18 for p in (0, 5, 10, 16)):
        continue
    for ins in md.disasm(IMG[o:o + 4], o):
        if has18(ins): kinds[o] = ('x18', None)

# 按函数取指令表，找窗口与跳转表
# Take the instruction table per function, find the windows and jump tables
import bisect
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return (starts[i], starts[i + 1] if i + 1 < len(starts) else TEXT_END) if i >= 0 else (a, a + 4)
done = set(); windows = []; jumps = {}
funcs = sorted({func_of(a) for a, (k, _) in kinds.items() if k == 'x18'})
for fa, fb in funcs:
    insl = list(md.disasm(bytes(IMG[fa:fb]), fa))
    ws = find_windows(insl)
    for wseg in ws:
        windows.append(wseg); done |= {x.address for x in wseg}
    for j, ins in enumerate(insl):
        if ins.mnemonic == 'br' and ins.op_str == 'x18':
            base = width = None
            ctx = insl[max(0, j - 8):j]
            for p in reversed(ctx):
                if p.mnemonic == 'adr' and p.op_str.startswith('x18,'): base = p.operands[1].imm; break
            for p in reversed(ctx):
                if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                    idx = num(p.reg_name(p.operands[2].reg))
                    for q in reversed(ctx):
                        if q.mnemonic in ('ldrb', 'ldrsb') and num(q.reg_name(q.operands[0].reg)) == idx: width = 256
                        if q.mnemonic in ('ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx: width = 65536
                        if width: break
                    break
            assert base and width, f'jump table {ins.address:x}'
            jumps[ins.address] = (base, width)
    # 不许有跳转落进窗口中间
    # No jump may land in the middle of a window
    inwin = {x.address for wseg in ws for x in wseg}; heads = {wseg[0].address for wseg in ws}
    for x in insl:
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            t = x.operands[-1].imm
            assert not (t in inwin and t not in heads), f'branch into window {x.address:x}'

def put_branch(at, to): struct.pack_into('<I', IMG, at, B(at, to))
for wseg in windows:
    st, lo, hi = site_window(wseg); stat('window'); put_branch(lo, st)
for a in sorted(kinds):
    if a in done: continue
    k, arg = kinds[a]
    ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
    if k == 'x18':
        m = ins.mnemonic
        if m == 'br': st = site_br(ins, *jumps[a]); stat('br')
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz'): st = site_cond(ins); stat('cond')
        elif m in ('adr', 'adrp'): st = site_pcrel(ins); stat('pcrel')
        elif m in ('blr', 'ret', 'b', 'bl') or (m == 'ldr' and '[' not in ins.op_str): raise RuntimeError(f'{a:x} {m} {ins.op_str}')
        else: st = site_plain(ins); stat('plain')
    elif k == 'tpidr_r': st = site_sysreg_read(ins, 'KTLS'); stat(k)
    elif k == 'tpidrro_r': st = site_sysreg_read(ins, 'KTLR'); stat(k)
    elif k == 'tpidr_w': st = site_tpidr_write(ins); stat(k)
    elif k == 'svc': st = site_gate(ins, 'svc', arg); stat(k)
    elif k in ('cntpct', 'cntvct'): st = site_gate(ins, k, arg); stat(k)
    put_branch(a, st)

# 私有视觉分支；非OBS，0x85只读查询。只有本实验构建显式opt-in；原Qty/children和Update不变。
# Private visual branch; not OBS, 0x85 is a read-only query. Only this experimental build explicitly opts in; the original Qty/children and Update are unchanged.
if RULE_RUN and PRIVATE_GATES:
    off, word = 0x3e443c, 0x3969c268
    assert struct.unpack_from('<I', IMG, off)[0] == word
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))  # w8查询结果仅用于cbz；两路恢复原x8。 | The w8 query result is used only for cbz; both paths restore the original x8.
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    # cbz w8, nonmatch；不改NZCV。match恢复寄存器后保children。
    # cbz w8, nonmatch; NZCV is not changed. On match, restore the registers and then preserve children.
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), 0x3e4460))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), 0x3e4440))
    gates.append((ret, 'svc', 0x85, off))
    put_branch(off, start); stat('private_skill_render')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off,start)).hex(),
                 'private confirmed six HUD only: own image/text skip ->4460; unknown original ->4440; svc85 full800', '仅宿主'))
elif RULE_RUN:
    patch.extend(bytes(0x80))
    stat('private_gates_reserved')

obs = list(OBS.get(BUILD_ID, []))
if RULE_RUN and PRIVATE_GATES:
    obs.append((0x43ecc8, 6, "b8686953", "private touch-run-v4 PRE-ldr; full800 host ABI"))
    obs.append((0x1237c60, 8, "d10143ff", "ui world anchor: camera world->screen entry (camera, point, out); host swaps x0 to a copy whose projection is shifted by the UI rect y (iPad UI kept 16:9 inside a taller screen), game camera untouched"))
    # 面板开着时冻结世界（宿主 diag.c freeze_tick，缺省关，DM_PANEL_FREEZE=1 开）。四个入口门，都只换入口参数。
    # Freeze the world while a panel is open (host diag.c freeze_tick, off by default, DM_PANEL_FREEZE=1 turns it on). Four entry gates, all of which only swap entry arguments.
    obs.append((0x5eb2c0, 10, "a9bc5ff8", "server world-sim entry (game G): while frozen the host swaps x0 to a stand-in whose event queue is empty, so no unit / timer callback runs this tick; the real queue is left untouched"))
    obs.append((0x4b7230, 11, "d10183ff", "client per-unit tick update entry (unit): while frozen the host swaps x0 to a stand-in unit carrying only the skip flag ([+0x124] bit 23), so the function takes its own early-out to the epilogue; all six call sites are inside the client tick simulation 0x461b80 / 0x461e60"))
    obs.append((0x4b74d0, 12, "f81e0ff3", "client per-unit per-frame update entry (unit): drives model animation / interpolation from the tick fraction; while frozen the host swaps x0 to a stand-in unit (type 9, no render component) so both the callee 0x2a18d0 and this function take their own early-outs"))
    obs.append((0x2c3c90, 13, "f81c0ff7", "render per-frame entry, called right after the main loop writes the world frame step [0x20cebb8] and world time [0x3114208]: while frozen the host rewrites the step to 0 and the world time to its value at freeze, the same thing the game's own pause path does at 0x2c1574"))
    obs.append((0xadc434, 14, "d10143ff", "TextBoxWidget text-changed entry (widget): reads the raw text at +0x88 and rebuilds the display; host rewrites +0x88 for strings our item-notes generator annotated with colour codes (TextBoxWidget draws U+E07E as '*'), exact-match table only, everything else untouched"))
    obs.append((0x442bc0, 15, "a9ba6ffc", "client event-message entry (x0 = packet; byte 1 = event subtype, jump table 0x1da6752): subtype 0x17 = offline difficulty scale changed -> 0x20f91c chat line + system notification; host rewrites byte 1 past the table (default branch, nothing shown) only for 0x17 and only in a rift game (dm_rift.c); otherwise untouched. orig stp x28, x27, [sp, #-0x60]!"))
    obs.append((0x634000, 16, "d10143ff", "server level change by level number (game, player, level, kind), used by waypoints / portals / quest moves: in a rift game, when the player leaves a rift map for a level that is neither town nor a rift map, the host swaps w2 to the rift's next map so the destination is replaced inside the single native transition"))
    obs.append((0x711040, 17, "d101c3ff", "server quest event 'monster killed' entry (game, monster, attacker), the only entry (callers 0x7d8550 kill, 0x7ab8dc); in a rift game, for a monster that died on a rift map, the host swaps x1 to a stand-in whose +0xc0 is null so the function takes its own early return (0x711064): bosses in the rift are combat only, no quest state / quest notifications"))
    obs.append((0x3d84a0, 18, "d10143ff", "NotificationPanel message handler entry (panel, message): on InputMessage / ControllerActionBegin with action id 9 (the menu key) the native handler re-sends the notice callback and marks the message bus consumed, so the first menu press only dismisses the notice. In touch mode the host swaps x1 to a copy of the message whose action id is not 9: the notice stays until it times out and the menu key opens the menu; controller mode keeps the native behaviour"))
    obs.append((0x463f40, 19, "f81c0ff7", "client per-frame position-error smoothing entry (s0 = frame length in ticks), called by the main loop right after all units were extrapolated in place and before the per-unit error is added back: the native error is only computed on start / stop (0x4639c0, which runs before the tick loop), so a sharp turn inside a tick snaps the extrapolated position back in one frame; the host adds that frame's jump to the local player's error fields (path +0x2a8 / +0x2ac, same clamp as native) and the native code decays it"))
    obs.append((0x9da5b0, 20, "d104c3ff", "model yaw component per-frame update entry (x1 = argument block: +8 component, +0x20 -> +8 unit): the body turn speed is instance data on the component (+0x10..+0x2c min / max degrees per second for controller / mouse, +0x34 spring parameter); the host only reads them for the local player when a test switch is set"))
    obs.append((0x2f7664, 7, "6dbb23e9", "touch target rank: candidate compare entry (closure, unit, kind, s0=score); host raises Boss/elite score, native eligibility unchanged"))
    obs.append((0xb0d560, 21, "fc1a0fe8", "TooltipsPanel add-tooltip entry (panel, text, target position x2 / x3, style, text2): tooltips are immediate-mode (re-submitted by the hovered widget every frame and cleared after drawing, so the host cannot reach the record at its own time); on touch, in panel state, for an item the player just tapped, the host swaps the target position so the game lays the tooltip out beside the panel instead of over the item grid; everything else passes through unchanged"))
    obs.append((0x7ce800, 22, "d100c3ff", "server place-one-preset-unit entry (game, room, record), only called by the room-activation preset loop 0x7ce930 (8 sites): in a rift game at tier 90+ on a rift map, for a monster record that is not a superunique and not scenery / NPC, the host swaps x2 to a copy whose class is -1 so 0x6910d0 takes its own early return (tbnz w2, #31) and nothing is spawned; the real record is untouched; rift guardian / invader / superunique spawns call 0x7ce630 / 0x69b310 directly and never pass here"))
for off, oid, ow, why in obs:
    w = struct.unpack_from('<I', IMG, off)[0]
    assert f'{w:08x}' == ow, f'观测门原指令不符 {off:#x}：{w:08x}'
    # 只许函数入口：dm_gate 只回写 x0–x8 / x30，宿主 C 代码可能改 x9–x17；入口处这些按 AAPCS64 不承载值（参数只在 x0–x7 / x8）
    # Function entries only: dm_gate writes back only x0–x8 / x30, and host C code may modify x9–x17; at entry these carry no values under AAPCS64 (arguments are only in x0–x7 / x8)
    assert off in set(starts) or (oid == 6 and off == 0x43ecc8), f'private obs point not approved {off:#x}'
    assert off < TEXT_END and off not in kinds and not any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'观测门与改写点冲突 {off:#x}'
    put_branch(off, site_obs(off, oid, w)); stat('obs')

# 秘境出口改道（r36 起）：分支门 0x89，挂在「按出口砖的连接换关」main+0x7cc1e0(游戏, 玩家, 砖) 的入口。它的四个调用者（点出口砖 0x6dbea0、
# 入口物件 0x6aeaa0 / 0x6b0500、任务 0x7314c0）都在各自的判定通过之后才调它；目的地不是关卡号而是砖的对端房间（同一幕已生成的地图里），换不成别的幕。
# 宿主在秘境局里把 w2 / w3 换成（秘境下一张图, 11）并让 x8≠0 → 门块直接跳去「按关卡号换关」0x634000，同一个调用者、只发生一次换关；其余情形执行原指令回 0x7cc1e4。
# 门块与 0x85 分支门逐字同构（structck ⑪ 按 hle.h 登记表核）；与观测门 15 / 16 一样只在带宿主私有门的产物里（touch）
# Rift exit rerouting (since r36): branch gate 0x89, hooked at the entry of "change level by the exit tile's connection" main+0x7cc1e0(game, player, tile). Its four callers (clicking an exit tile 0x6dbea0,
# entrance objects 0x6aeaa0 / 0x6b0500, quest 0x7314c0) all call it only after their own checks have passed; the destination is not a level id but the tile's peer room (in the already generated map of the same act), so it cannot be switched to another act.
# In a rift run the host replaces w2 / w3 with (the rift's next map, 11) and makes x8≠0 → the gate block jumps straight to "change level by level id" 0x634000, same caller, exactly one level change; in all other cases the original instruction is executed and control returns to 0x7cc1e4.
# The gate block is word-for-word isomorphic with the 0x85 branch gate (structck ⑪ checks it against the hle.h registry); like observation gates 15 / 16 it exists only in build outputs with host-private gates (touch)
if RULE_RUN and PRIVATE_GATES:
    off, word, tgt = 0x7cc1e0, 0xd10183ff, 0x634000
    assert struct.unpack_from('<I', IMG, off)[0] == word and off in set(starts) and tgt in set(starts)
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), tgt))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), off + 4))
    gates.append((ret, 'svc', 0x89, off))
    put_branch(off, start); stat('rift_exit')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off, start)).hex(),
                 'rift exit redirect: tile-link level change entry -> host swaps (level, kind) and x8!=0 takes the branch to level-number change 0x634000; otherwise original ->0x7cc1e4; svc89', '仅宿主'))

# 秘境里存档时的当前幕（r38 起）：分支门 0x8c，挂在原版写存档头函数 0x6fa3e0 里「取难度」那条 main+0x6fa9d0（ldrb w10, [x21, #0x104]）。
# 它的上一条 0x6fa9cc 刚把「玩家当前在第几幕」读进 w9（ldrb w9, [x24, #0x2e8]），下面 0x6fa9e4 / 0x6fa9e8 写成难度字节 0x80 | 幕号（.d2s 0x98–0x9a），
# 下次进局落哪个城由它定。宿主在人处于秘境图里时把 w9 换成 4（第五幕 → 重进落哈洛加斯），只动写进存档的这一个值，运行中的幕号不碰。
# 用分支门而不是「替换一条指令」：structck 只认「bl 调用点」「入口」「分支门」三种登记形态。宿主永远让 x8 = 0（不走跳转那一路），
# 门块恢复 x8、执行原指令、回 0x6fa9d4；跳转目标填回点本身只为满足模板（走不到）。门块与 0x85 / 0x89 逐字同构
# Current act written to the save while in a Rift (since r38): branch gate 0x8c, hooked at the "read difficulty" instruction main+0x6fa9d0 (ldrb w10,
# [x21, #0x104]) inside the original save-header writer 0x6fa3e0.
# The preceding instruction 0x6fa9cc has just loaded "which act the player is currently in" into w9 (ldrb w9, [x24, #0x2e8]); below, 0x6fa9e4 / 0x6fa9e8
# write it as the difficulty byte 0x80 | act number (.d2s 0x98–0x9a),
# which decides which town the next session starts in. While the player is in a Rift map the host replaces w9 with 4 (Act 5 → re-enter in Harrogath);
# only this one value written to the save is changed, the running act number is untouched.
# A branch gate is used rather than "replace one instruction": structck only recognizes three registered forms, "bl call site", "entry" and "branch
# gate". The host always sets x8 = 0 (the jump path is never taken);
# the gate block restores x8, executes the original instruction and returns to 0x6fa9d4; the jump target is set to the return point itself only to
# satisfy the template (never reached). The gate block is word-for-word identical in structure to 0x85 / 0x89
if RULE_RUN and PRIVATE_GATES:
    off, word, tgt = 0x6fa9d0, 0x394412aa, 0x6fa9d4   # 函数中间的一条指令（同 0x85 那扇门），不要求是函数入口；原指令不依赖 pc | An instruction in the middle of a function (same as gate 0x85); it need not be a function entry; the original instruction does not depend on pc
    assert struct.unpack_from('<I', IMG, off)[0] == word
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), tgt))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), off + 4))
    gates.append((ret, 'svc', 0x8c, off))
    put_branch(off, start); stat('rift_save_act')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off, start)).hex(),
                 'rift save act: save-header writer, right after it loads the current act into w9 -> host swaps w9 to 4 (act V) while the player is in a rift map; x8 stays 0, original insn runs ->0x6fa9d4; svc8c', '仅宿主'))
# 佣兵加装备位（r39 起）：分支门 0x8d，挂在原版通用放置校验 main+0x6493a8 的怪物主人分支里 main+0x64a34c（mov w9, w25）。
# 这一段（0x64a314–0x64a354）对怪物类主人只放行两类物品：0x574bc0(物品, 3 护甲)、0x574bc0(物品, 0x25 头盔)，都不是就走武器那条（按 monstats 的左右手类型），再不中就拒。
# 门挂在「是不是头盔」返回之后、cbnz w0 之前：宿主在「主人是玩家的佣兵 + 秘境最高通关 ≥ 10 层 + 物品是腰带 / 鞋 / 手套 / 戒指 / 项链」时把 w0 改成 1，
# 走头盔同一条放行路径（后面的部位格子、部位空不空等通用检查照常）。宿主永远让 x8 = 0；门块与 0x85 / 0x89 / 0x8c 逐字同构
# Extra mercenary equipment slots (since r39): branch gate 0x8d, hooked in the monster-owner branch of the original game's generic placement check main+0x6493a8 at main+0x64a34c (mov w9, w25).
# This section (0x64a314–0x64a354) lets through only two kinds of items for monster-type owners: 0x574bc0(item, 3 armor) and 0x574bc0(item, 0x25 helm); if neither, it takes the weapon path (by the left / right hand types in monstats), and if that fails too it rejects.
# The gate is hooked after the "is it a helm" call returns and before cbnz w0: when "the owner is the player's mercenary + highest rift level cleared ≥ 10 + the item is a belt / boots / gloves / ring / amulet", the host changes w0 to 1,
# taking the same pass path as the helm (the generic checks that follow, such as the body location cell and whether the location is empty, run as usual). The host always sets x8 = 0; the gate block is word-for-word isomorphic with 0x85 / 0x89 / 0x8c
if RULE_RUN and PRIVATE_GATES:
    off, word, tgt = 0x64a34c, 0x2a1903e9, 0x64a350   # 原指令不依赖 pc | The original instruction does not depend on pc
    assert struct.unpack_from('<I', IMG, off)[0] == word
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), tgt))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), off + 4))
    gates.append((ret, 'svc', 0x8d, off))
    put_branch(off, start); stat('merc_slot')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off, start)).hex(),
                 'merc slots: placement check, monster-owner branch right after the helm type test -> host sets w0=1 for ring/amulet/boots/gloves/belt on a hireling (rift best >= 10 for new equips; always on load); x8 stays 0, original insn runs ->0x64a350; svc8d', '仅宿主'))
# 佣兵加装备位的客户端那一半（r39 同代）：分支门 0x8e，挂在客户端「佣兵能不能穿这件」函数 main+0x3bea44(佣兵, 物品) 里 main+0x3beb58（mov x0, x19）。
# 这个函数和服务端那段同一个规则：0x574bc0(物品, 3 护甲)、0x574bc0(物品, 0x25 头盔) 中一个就返回 1，都不是走武器那条。物品格控件拿它的结果画「不可放置」的红色、
# 决定松手时放不放——不改它，已解锁的新格拖着对的物品悬停也是红的、原版放下不成（原先靠触摸层兜底直接调 0x3bed60）。
# 门挂在「mov w1, #0x25」之后、这次类型判断的 bl 之前：宿主在「秘境最高通关 ≥ 10 层 + 物品是腰带 / 鞋 / 手套 / 戒指 / 项链」时把 w1 换成该物品自己的类型号，
# 类型判断自然成立、走头盔同一条返回 1 的路。宿主永远让 x8 = 0；门块与 0x8d 逐字同构
# Client half of the extra mercenary equipment slots (same generation as r39): branch gate 0x8e, hooked inside the client's "can the mercenary wear this" function main+0x3bea44(mercenary, item) at main+0x3beb58 (mov x0, x19).
# This function follows the same rule as the server-side section: if either 0x574bc0(item, 3 armor) or 0x574bc0(item, 0x25 helm) matches it returns 1, and if neither does it takes the weapon path. The item cell widget uses its result to draw the "cannot place" red and
# to decide whether to place on release -- without changing it, an unlocked new cell still shows red when the right item is dragged over it, and the original game's drop fails (previously the touch layer's fallback called 0x3bed60 directly).
# The gate is hooked after "mov w1, #0x25" and before the bl of this type check: when "highest rift level cleared ≥ 10 + the item is a belt / boots / gloves / ring / amulet", the host replaces w1 with the item's own type number,
# so the type check passes naturally and takes the same return-1 path as the helm. The host always sets x8 = 0; the gate block is word-for-word isomorphic with 0x8d
if RULE_RUN and PRIVATE_GATES:
    off, word, tgt = 0x3beb58, 0xaa1303e0, 0x3beb5c   # 原指令不依赖 pc | The original instruction does not depend on pc
    assert struct.unpack_from('<I', IMG, off)[0] == word
    assert off < TEXT_END and off not in kinds and not any(seg[0].address <= off < seg[-1].address + 4 for seg in windows)
    start = here()
    emit(STP_PRE(8, 31, 31, -16))
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call(); emit(BLR(30))
    ret = here(); emit(LDP_POST(29, 30, 31, 16))
    emit(0x34000068)
    emit(LDP_POST(8, 31, 31, 16)); emit(B(here(), tgt))
    emit(LDP_POST(8, 31, 31, 16)); emit(word); emit(B(here(), off + 4))
    gates.append((ret, 'svc', 0x8e, off))
    put_branch(off, start); stat('merc_slot_client')
    cuts.append((off, struct.pack('<I', word).hex(), struct.pack('<I', B(off, start)).hex(),
                 'merc slots (client): hireling can-equip check, between "mov w1,#0x25" and the type test -> host swaps w1 to the item\'s own type for ring/amulet/boots/gloves/belt (rift best >= 10); x8 stays 0, original insn runs ->0x3beb5c; svc8e', '仅宿主'))

# ---- 参数位（仅宿主）：{NSO 构建号: [(偏移 A, 原字 A, 偏移 B, 原字 B, 寄存器, 键, 理由)]}。
# 原处两条造常数指令改成第一条跳补丁区参数跳板、第二条 nop；跳板间接取键页值后跳回第一条之后（A+4）：A、B 不相邻时中间的原指令照常执行
# （曾跳回 B+4，运行模式通知处理 0xe10d5c/0xe10d60 越过 adrp x19.. 与另一跳板，切掌机即崩；structck ⑩ 核跳板出口）。
# A、B 之间不得读写 xR（离线断言：中间指令不含 xR）。宿主按前端给值，Mac 给原常数 → 行为不变 ----
# ---- Parameter slots (host only): {NSO build ID: [(offset A, original word A, offset B, original word B, register, key, reason)]}.
# The two constant-building instructions at the original site are changed so that the first jumps to a parameter trampoline in the patch area and the second is a nop; the trampoline fetches the key page value indirectly and jumps back to just after the first (A+4): when A and B are not adjacent, the original instructions in between execute as usual
# (it used to jump back to B+4; in the operation-mode notification handler 0xe10d5c/0xe10d60 that skipped adrp x19.. and another trampoline, and switching to handheld crashed immediately; structck ⑩ checks the trampoline exit).
# xR must not be read or written between A and B (offline assertion: the instructions in between do not contain xR). The host supplies values according to the front end, Mac supplies the original constants → behavior unchanged ----
PARAMS = {'db0507f36b84200ac05f75d76ed5026f': [
    (0xe12d8c, 'd280c813', 0xe12d90, 'f2c07093', 19, 'KRES1', '启动初始化：主机模式输出分辨率 1600x900（{u32 宽, u32 高} 打包）'),
    (0xe12e1c, 'd280a008', 0xe12e20, 'f2c05a08', 8, 'KRES0', '启动初始化：掌机模式输出分辨率 1280x720'),
    (0xe10d60, 'd280c818', 0xe10d80, 'f2c07098', 24, 'KRES1', '运行模式变化通知（0x1f）处理：主机模式 1600x900'),
    (0xe10d5c, 'd280a016', 0xe10d74, 'f2c05a16', 22, 'KRES0', '运行模式变化通知（0x1f）处理：掌机模式 1280x720'),
    # 宽高比上限 19:9（float 0x40071c72）：宽于 19:9 的屏幕两侧留黑柱。宿主给 max(19/9, 输出宽 / 高) 的浮点位模式（低 32 位）；
    # 原版输出 16:9（1600x900 / 1280x720）⇒ 仍为 19/9，行为不变；iPhone 1952x900（2.17:1）⇒ 2.169，黑柱宽 0
    # Aspect ratio cap 19:9 (float 0x40071c72): screens wider than 19:9 get black bars on both sides. The host supplies the float bit pattern (low 32 bits) of max(19/9, output width / height);
    # the original game outputs 16:9 (1600x900 / 1280x720) ⇒ still 19/9, behavior unchanged; iPhone 1952x900 (2.17:1) ⇒ 2.169, black bar width 0
    (0x1663ee8, '52838e49', 0x1663eec, '72a800e9', 9, 'KASPECTUI', '界面常量缓冲 0x15 号槽 [屏宽, 屏高, 宽高比上限]：界面精灵着色器按它在上限以外画两侧黑柱（带约 20–30 像素向内的柔边，宿主另加余量把柔边推出屏幕）'),
    (0xb152bc, '52838e48', 0xb152c0, '72a800e8', 8, 'KASPECT', '新画面模式视口：边距 = (屏宽 − 屏高 × 上限) / 2，经 0x85bc90 设居中视口'),
    (0x23e888, '52838e48', 0x23e88c, '72a800e8', 8, 'KASPECT', '新画面模式坐标钳制（0x23e760）：x 夹进 [边距, 边距 + 屏高 × 上限]，宽屏两侧的指针 / 触摸不再被夹回中间'),
]}
params = PARAMS.get(BUILD_ID, [])
for oa, wa, ob_, wb, r, key, why in params:
    for o, w0 in ((oa, wa), (ob_, wb)):
        w = struct.unpack_from('<I', IMG, o)[0]
        assert f'{w:08x}' == w0, f'参数位原指令不符 {o:#x}：{w:08x}'
        assert o not in kinds and not any(lo <= o < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'参数位与改写点冲突 {o:#x}'
    for o in range(oa + 4, ob_, 4):
        ins = dis(struct.unpack_from('<I', IMG, o)[0], o)
        assert r not in regs_in(ins), f'参数位中间用到 x{r}：{o:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    tmp = 16 if r != 16 else 17
    emit(STUR(tmp, 31, -16)); emit(MRS_TPIDRRO(r)); emit_slot_addr(r, tmp, key); emit(LDRX(r, r)); emit(LDUR(tmp, 31, -16)); emit(B(here(), oa + 4))
    struct.pack_into('<I', IMG, oa, B(oa, start)); struct.pack_into('<I', IMG, ob_, NOP); stat('param')

# 既有三块原生规则跳板均只使用后继覆盖的死临时w8，不改w0/stat返回、sp或NZCV。
# 6c6290位于原mode3门后；前方Blaze/引用计数与数据失效6c62dc分支、后方状态更新不动。
# The three existing native rule trampolines all use only the dead temporary w8 that is overwritten afterwards, and do not change the w0/stat return, sp or NZCV.
# 6c6290 is after the original mode3 gate; the Blaze / reference count and data invalidation 6c62dc branches before it and the state update after it are untouched.
rule_blocks = []
if RULE_RUN:
    def run_branch(off, old, body, why):
        assert struct.unpack_from('<I', IMG, off)[0] == old, f'Run跳板原字不符 {off:#x}'
        start = here()
        for word in body(start): emit(word)
        new = B(off, start)
        struct.pack_into('<I', IMG, off, new)
        cuts.append((off, struct.pack('<I', old).hex(), struct.pack('<I', new).hex(), why, '全'))
        rule_blocks.append((start, bytes(patch[start - PATCH_OFF:]).hex(), why))
    def cbz(at, target, r, nonzero=False):
        delta = target-at; assert delta%4==0 and -(1<<20)<=delta<(1<<20)
        return (0x35000000 if nonzero else 0x34000000) | (((delta>>2)&0x7ffff)<<5) | r
    run_branch(0x6c6648, 0x34000060, lambda a: [
        0xb9400268, cbz(a+4,a+12,8,True), B(a+8,0x6c664c),
        cbz(a+12,a+20,0), B(a+16,0x6c664c), B(a+20,0x6c6654)],
        'touch-run-v4：Run选择仅player0忽略精力门，非player按原stat返回；w8后继覆盖')
    run_branch(0x6c6290, 0x710006df, lambda a: [
        0xb9400268, cbz(a+4,a+12,8,True), B(a+8,0x6c62ec), 0x710006df, B(a+16,0x6c6294)],
        'touch-run-v4：原合法Run成本段仅player0跳stat10扣除/耗尽降走；非player保持原cmp')


    # client4abc60原mode3/当前受控unit门之后；实际GetStat已返回，保W0，只type0取消STA自动Stop。
    # After the original mode3 / currently controlled unit gate at client4abc60; the actual GetStat has already returned, W0 is preserved, and only for type0 is the STA auto Stop cancelled.
    run_branch(0x4ac368, 0x35000460, lambda a: [
        0xb9400268, cbz(a+4,a+12,8), B(a+8,a+16), B(a+12,0x4ac3f4),
        cbz(a+16,a+24,0), B(a+20,0x4ac3f4), B(a+24,0x4ac36c)],
        'touch-run-v4：client当前受控Run3仅player0跳STA零值自动Stop；原getter值与真实松杆保留，非player保原门')


    # 原unit null/type5门之后、w8仍为原unitType；只规范player请求2，原其它动作分派/资格完整执行。
    # After the original unit null/type5 gate, with w8 still the original unitType; only player request 2 is normalized, and the other original action dispatch / eligibility checks run in full.
    run_branch(0x6c6580, 0x51000849, lambda a: [
        cbz(a,a+16,8,True), 0x51000849, cbz(a+8,a+16,9,True),
        0x52800062, 0x51000849, B(a+20,0x6c6584)],
        'touch-run-v4：原通用合法移动仅player0请求Walk2规范为Run3，非player与其它请求保原，w8/原资格不改')

os.makedirs(outdir, exist_ok=True)
P = lambda ext: os.path.join(outdir, f'{NAME}.{ext}')
open(P('patched.bin'), 'wb').write(IMG + bytearray(PATCH_OFF - len(IMG)) + patch)
import hashlib
with open(os.path.join(outdir, 'build_src.sha256'), 'w') as f:
    f.write(hashlib.sha256(open(os.path.abspath(__file__), 'rb').read()).hexdigest() + '  patch_nso.py\n')
if PRIVATE_AIM:
    with open(P('aim.tsv'), 'w') as f:
        f.write('# RULESET\tskill-aim-v1-private\n# ORIGINAL_CALL\t0x2faf04\t97fee7fd\n# ORIGINAL_CALLEE\t0x2b4ef8\n# SKILL_CALLER\t0x2b5b80\n# SVC\t0x86\n# INPUT\tactual GetNpad sampled right; move axis unchanged\n')
        f.write('# IMAGE_SHA256\t'+hashlib.sha256(open(P('patched.bin'),'rb').read()).hexdigest()+'\n')
if RULE_RUN:
    with open(P('rules.tsv'), 'w') as f:
        f.write(f'# RULESET\t{RULESET}\n# BUILD_ID\t{BUILD_ID}\n# ORIGINAL_SHA256\t{RULE_ORIG_SHA}\n# IMAGE_SHA256\t{hashlib.sha256(open(P("patched.bin"), "rb").read()).hexdigest()}\n')
        for off, raw, why in rule_blocks: f.write(f'{off:#x}\t{raw}\t{hashlib.sha256(bytes.fromhex(raw)).hexdigest()}\t{why}\n')
elif os.path.exists(P('rules.tsv')):
    os.unlink(P('rules.tsv'))  # 重出native时清本模块旧规则登记，不让旧manifest混入原资源域 | When rebuilding native, clear this module's old rule registration so the old manifest does not leak into the original-resource domain
with open(P('fixups.tsv'), 'w') as f:
    f.write(f'# PATCH_OFF\t{PATCH_OFF:#x}\n# KEY_PAGE_OFF\t{KEY_PAGE_OFF:#x}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{TEXT_END:#x}\n# RO_OFF\t{RO_OFF:#x}\n# DATA_OFF\t{DATA_OFF:#x}\n')
    for k in KEY_INDEX: f.write(f'# KEY_{k}_IDX\t{KEY_INDEX[k]}\n')
with open(P('gates.tsv'), 'w') as f:
    for ret, k, arg, orig in gates: f.write(f'{ret:#x}\t{k}\t{arg}\t{orig:#x}\n')
with open(P('sites.tsv'), 'w') as f:
    for a in sorted(kinds):
        if a not in done: f.write(f'{a:#x}\t{kinds[a][0]}\n')
    for wseg in windows: f.write(f'{wseg[0].address:#x}\tW\t{wseg[-1].address + 4:#x}\n')
    for a, (b, c) in jumps.items(): f.write(f'{a:#x}\tJ\t{b:#x}\t{c}\n')
with open(P('cuts.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, ob, nb, why, scope in cuts: f.write(f'{off:#x}\t{ob}\t{nb}\t{scope}\t{why}\n')   # 列：偏移 原字节 新字节 作用域 理由 | Columns: offset, original bytes, new bytes, scope, reason
with open(P('params.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for oa, wa, ob_, wb, r, key, why in params: f.write(f'{oa:#x}\t{ob_:#x}\tx{r}\t{key}\t{why}\n')
with open(P('obs.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, oid, ow, why in obs: f.write(f'{off:#x}\t{oid}\t{ow}\t{why}\n')
print(NAME, stats, 'cuts', len(cuts), 'obs', len(obs), 'patch', len(patch), 'PATCH_OFF', hex(PATCH_OFF), 'slot_refs', len(fix), 'gates', len(gates))
# 启动块清单随 main 出：取仓库版本化数据 dm/data/bootlist_<main 构建号>.tsv 原样拷进产物目录并写 sha256（清单入库、不每版重录；
# 记录本身有竞态——同产物三次记录块集合 5292 / 5114 / 5281、发起线程亦有不同——重录只在构建号变化或主动改进时做，提交附 mini A/B）
# The boot block list ships with main: take the repository's versioned data dm/data/bootlist_<main build ID>.tsv, copy it as is into the build output directory and write its sha256 (the list is checked in, not re-recorded for every version;
# the recording itself is racy -- three recordings of the same build output gave block sets of 5292 / 5114 / 5281, and the initiating threads also differed -- re-record only when the build ID changes or for a deliberate improvement, and attach an A/B from the second Mac to the commit)
if NAME == 'main':
    import hashlib, shutil
    BL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', f'bootlist_{BUILD_ID}.tsv')
    if not os.path.isfile(BL):   # 公开仓库不带这份清单（它每行带原版资源块的开头字节）：没有就不预取，启动时少一步提前解码，别的不受影响 | the public repository does not ship this list (each line carries the first bytes of an original resource block): without it there is no prefetch, so launch skips one ahead-of-time decode step and nothing else is affected
        print('bootlist 没有启动块清单，跳过（不预取）。要用的话自己录一份：dm/tools/tact_bootlist.py')
        for n in ('bootlist.tsv', 'bootlist.sha256'):   # 产物目录里以前留下的也去掉：留着会被当成这一次的产物带进安装包和壳的清单 | Leftovers from earlier runs in the build output directory are removed too: if kept they would be taken as outputs of this run and carried into the install package and the shell manifest
            if os.path.exists(os.path.join(outdir, n)): os.remove(os.path.join(outdir, n))
    else:
        hd = open(BL, encoding='utf-8').readline().split()
        if hd[-1:] != [BUILD_ID]: sys.exit(f'【红】{BL} 头行构建号 {hd[-1:]} 与 main {BUILD_ID} 不符')
        shutil.copyfile(BL, os.path.join(outdir, 'bootlist.tsv'))
        sha = hashlib.sha256(open(BL, 'rb').read()).hexdigest()
        open(os.path.join(outdir, 'bootlist.sha256'), 'w').write(f'{sha}  bootlist.tsv\n')
        print('bootlist', os.path.relpath(BL), sha[:12])
