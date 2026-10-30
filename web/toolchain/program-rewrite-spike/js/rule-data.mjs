/**
 * 来自 dm/tools/patch_nso.py 的字面规则数据，不可脱离固定源码独立修改。
 * Literal rule data from dm/tools/patch_nso.py. Do not alter independently of the pinned source. */

export const SOURCE_SHA256 = "f23802979ceeeab8267b724b2d7f1a568b900777eb8718a0471a41317dc744ee";

export const RULE_BUILD = "db0507f36b84200ac05f75d76ed5026f";

export const CUTS = {
  db0507f36b84200ac05f75d76ed5026f: [
    [
      8725700,
      "0808a2d2",
      "0800a4d2",
      "显存分配器初始化配置：mov x8,#0x10400000（260 MB）→ mov x8,#0x20000000（512 MB），随后 movk #0xa0,lsl48 与 str [sp,#0x238] 不变；原预算下内部分辨率 > 约 345 万像素时（2736x1260 过、2864x1240 崩）渲染目标放不下，main+0x174b018 返回空、main+0x17417a4 读空崩溃；只是显存预算，不进游戏逻辑：cvd5 逐拍对拍在 1600x900 与 2868x1320 两档均一致",
    ],
    [
      1669564,
      "e8a79f1a",
      "e8d79f1a",
      "离线放行窗口：读存档设置键（0x1d53e80 处字串）的上次在线时刻，「当前时刻 − 上次在线 < 0x0009356907420000 ns（= 30 天）」置位 [x20+0x2c]；lt→gt",
    ],
    [
      5969116,
      "e8a79f1a",
      "e8d79f1a",
      "调 0xddab50（缓冲区 [x10+0x138]+x25、剩余长度、回调 0x527d50、表 0x209cdb8、9 项）返回值 <1 置位，为真时走 0x5b14f8 分支；lt→gt（业务含义未从字串定出）",
    ],
    [
      6238596,
      "e8a79f1a",
      "e8d79f1a",
      "数据表 [0x2d64a18+idx*16] → +0xf90 记录（0x1fc 字节）+0x4c 下标 → +0xfd0 记录（0x128 字节）+0xf1 有符号字节 <0 置位，与（w26>0x24）相与后作第 4 参传下游；lt→gt（业务含义未从字串定出）",
    ],
    [
      30752384,
      "55736572204c617374204f6e6c696e6500",
      "566f6963654c616e677561676500000000",
      '只读字串 "User Last Online" → "VoiceLanguage"：第 1 处改读一个必定存在的设置键，「上次在线」取到小数值，差值恒超 30 天，配合 gt 恒放行',
    ],
    [
      8395244,
      "280f0034",
      "e8feff34",
      "前端引导首次进入分支：cbz w8,#0x801bd0（LogoMenuPanel → Logo 视频 → 标题页）改为 cbz w8,#0x8019c8（落到直达路径的 0x23e5e0(1) → 0xb156a0 进主菜单；跳过 0x8019bc 起置联机模式 [0x2d7badc]=1 的两条，联机模式保持启动初值 0 = 离线，不触发 0x80ec44 的联网检查、断网弹窗与回标题页）；r4 版目标 0x8019bc 会置联机模式 1，实测仍回标题页并弹断网框",
    ],
    [
      28372576,
      "fc6fbaa9fa6701a9",
      "011000d4c0035fd6",
      "TACT BLTE 块 zlib 解码入口（DecodeZlib）→ svc #0x80 ; ret：宿主用 libdeflate 实现（hle_tact.c，调用号登记在 hle.h SVC_HOST_TACT_ZLIB）；改写器先打剪口再扫描，svc #0x80 自动成为宿主门，门块保存恢复 x30，ret 回原调用者",
      "仅宿主",
    ],
    [
      6109832,
      "a27a0194",
      "211000d4",
      "收包泵 0x5d3960 投递客户端命令包的调用点 bl 0x632510 → svc #0x81：宿主缺省以原参数原样调用 0x632510（返回值调用方不用）；DM_C2S_REPLAY 时丢弃客户端自发命令、改由拍入口按文件注入（diag.c，调用号登记在 hle.h SVC_HOST_C2S_DELIVER）",
      "仅宿主",
    ],
    [
      6109640,
      "ba750194",
      "411000d4",
      "收包泵 0x5d3960 投递系统类消息的调用点 bl 0x6310b0 → svc #0x82：宿主缺省以原参数原样调用 0x6310b0（返回值调用方不用）；DM_C2S_REPLAY 时，回放文件首拍起丢弃宿主客户端自发的系统类消息（之前的加入消息放行），改由拍入口按文件注入（diag.c，调用号登记在 hle.h SVC_HOST_C2S_SYS）",
      "仅宿主",
    ],
    [
      34724400,
      "8007000038040000",
      "0010000070080000",
      "输出分辨率上限初值 1920x1080 → 4096x2160（数据段）：宿主按屏幕比例给的宽度（如 iPhone 19.5:9 的 1956x900）须不超上限才会被选中；Mac 前端仍给原值 1600x900 / 1280x720，行为不变",
      "仅宿主",
    ],
    [
      14749256,
      "09e880522903098a",
      "e903192a1f2003d5",
      "运行模式通知发窗口尺寸事件的宽度：mov w9,#0x740; and x9,x25,x9 → mov w9,w25; nop（取低 32 位，不再用原版两档宽度推出的常数掩码）",
      "仅宿主",
    ],
    [
      14757512,
      "09e880528902098a",
      "e903142a1f2003d5",
      "启动初始化选分辨率（0xe12d70）同一处掩码：mov w9,#0x740; and x9,x20,x9 → mov w9,w20; nop（structck 参数门下游掩码检查查出；启动时事件接收对象通常未建，平时不显形）",
      "仅宿主",
    ],
    [
      11623968,
      "0281002d",
      "1f2003d5",
      "经典画面切换写起点 / 目标（stp s2, s0, [x8, #4]）→ nop：目标恒为 1.0（新画面）",
    ],
    [11623972, "1f0d00b9", "1f2003d5", "经典画面切换清进度（str wzr, [x8, #0xc]）→ nop"],
    [
      11642464,
      "083a8252",
      "e8ff9f52",
      "界面画布宽上限 mov w8, #4560 → mov w8, #0xffff（画布宽 = 屏宽）",
      "仅宿主",
    ],
    [
      8349340,
      "370b0036",
      "59000014",
      "字符串表装载 tbz w23, #0（是否装 legacy 表 A）→ b 0x7f6800：表 A 无读者（选表标志恒 0 → 表 B），整段 17 个 legacy 文件不装",
    ],
    [
      18809564,
      "05120094",
      "611000d4",
      "段读取 0x11f0110 逐块解码 bl 0x11f4af0(ctx, 块, 长度) → svc #0x83：宿主缺省以原参数原样调用并回填 w0（hle.h SVC_HOST_GRANNY_A）",
      "仅宿主",
    ],
    [
      4624172,
      "3d1f0094",
      "011100d4",
      "物品落地取掉落音调用点 bl 0x470c20 → svc #0x88：宿主原样代调后按规则把一档物品的掉落音换成提示音（其余原样）",
      "仅宿主",
    ],
    [
      9414032,
      "0d6a0494",
      "e11000d4",
      "物品外观路径调用点 bl 0xa14dc4 → svc #0x87：宿主原样代调后按规则改指地上物品的带光变体（存储 / 穿戴 / 腰带 / 镶嵌不碰）",
      "仅宿主",
    ],
    [
      6127680,
      "a005fe97",
      "411100d4",
      "玩家进局取出生点调用点 bl 0x5596c0 → svc #0x8a：宿主原样代调，进局在城镇时出生点换成小站旁（hle.h SVC_HOST_JOIN_SPAWN）",
      "仅宿主",
    ],
    [
      6128640,
      "b004fe97",
      "611100d4",
      "进局给尸体取落点调用点 bl 0x5596c0 → svc #0x8b：同上，尸体与人取同一个点（hle.h SVC_HOST_JOIN_CORPSE）",
      "仅宿主",
    ],
    [
      18811156,
      "77100094",
      "811000d4",
      "段读取 0x11f07d0 逐块解码 bl 0x11f4af0(ctx, 块, 长度) → svc #0x84：同上（hle.h SVC_HOST_GRANNY_B）",
      "仅宿主",
    ],
  ],
};

export const OBS = {
  db0507f36b84200ac05f75d76ed5026f: [
    [
      11656288,
      1,
      "d10603ff",
      "UI 消息总线虚函数实现入口（x1 = 消息 {u64 类型哈希, u64 动作哈希, char* 面板名, …}）：DM_UIMSG 消息流与 DM_BOOTPROF 启动分段的唯一来源；原指令 sub sp, sp, #0x180",
    ],
    [
      6110144,
      2,
      "d10503ff",
      "服务端拍入口（x0 = 游戏对象 G）：DM_TICKS 逐拍 6 格状态哈希（dm/verify/tick_state_format.md，与真值 swtick.py 同一采集点）；原指令 sub sp, sp, #0x140",
    ],
    [
      7232320,
      3,
      "d10103ff",
      "客户端命令分派入口 (G, 单位, 包, 长度)：DM_C2S_LOG 命令包序列（dm/verify/command_packet_format.md，与真值同一采集点）；原指令 sub sp, sp, #0x40",
    ],
    [
      6492336,
      4,
      "a9bb67fc",
      '系统类客户端消息处理入口 (buf = u32 客户端号 + 包, 包长)：包号 ≥ 0x67（加入 / 心跳 0x6d / 0x6f 等），收包泵先抽干此队列；DM_C2S_LOG 记为 "q":"sys"；原指令 stp x28, x25, [sp, #-0x50]!',
    ],
    [
      3240600,
      5,
      "d10183ff",
      "SetLanguage 入口（x1 → u32 新语言号，语言名表 main+0x20ae478）：配合上方语言重载剪口，宿主断言新语言为 zhCN；原指令 sub sp, sp, #0x60",
    ],
  ],
};

export const PARAMS = {
  db0507f36b84200ac05f75d76ed5026f: [
    [
      14757260,
      "d280c813",
      14757264,
      "f2c07093",
      19,
      "KRES1",
      "启动初始化：主机模式输出分辨率 1600x900（{u32 宽, u32 高} 打包）",
    ],
    [
      14757404,
      "d280a008",
      14757408,
      "f2c05a08",
      8,
      "KRES0",
      "启动初始化：掌机模式输出分辨率 1280x720",
    ],
    [
      14749024,
      "d280c818",
      14749056,
      "f2c07098",
      24,
      "KRES1",
      "运行模式变化通知（0x1f）处理：主机模式 1600x900",
    ],
    [
      14749020,
      "d280a016",
      14749044,
      "f2c05a16",
      22,
      "KRES0",
      "运行模式变化通知（0x1f）处理：掌机模式 1280x720",
    ],
    [
      23477992,
      "52838e49",
      23477996,
      "72a800e9",
      9,
      "KASPECTUI",
      "界面常量缓冲 0x15 号槽 [屏宽, 屏高, 宽高比上限]：界面精灵着色器按它在上限以外画两侧黑柱（带约 20–30 像素向内的柔边，宿主另加余量把柔边推出屏幕）",
    ],
    [
      11621052,
      "52838e48",
      11621056,
      "72a800e8",
      8,
      "KASPECT",
      "新画面模式视口：边距 = (屏宽 − 屏高 × 上限) / 2，经 0x85bc90 设居中视口",
    ],
    [
      2353288,
      "52838e48",
      2353292,
      "72a800e8",
      8,
      "KASPECT",
      "新画面模式坐标钳制（0x23e760）：x 夹进 [边距, 边距 + 屏高 × 上限]，宽屏两侧的指针 / 触摸不再被夹回中间",
    ],
  ],
};

export const RUN_WORDS = [
  [2908684, 3104723231, 1384120361, "偏好初始化：mov w9,1（x8仍为2d20000）"],
  [2908688, 3489747656, 3104723209, "偏好初始化：str w9,[x8,e50]；替代同页重复adrp，保原x8"],
  [3578512, 3506488319, null, "客户端RunOff入口尾跳原RunOn；保其player0/合法mode/环境门"],
  [3578240, 872416928, 3573751839, "客户端RunOn仅取消既有player0门之后的零精力拒绝"],
  [2829428, 872415392, 3573751839, "控制器d29资格仅取消stat10零值限制；幅度比较与停杆保原"],
  [2829760, 956302111, 3573751839, "控制器d29仅取消stat10零值清除；幅度归一化保原"],
  [2830588, 168362241, 1384120353, "已通过合法动作/零轴门的移动编码选择位固定Run；幅度不改"],
  [2830596, 706675681, 1384120353, "无用户走跑偏好时亦固定Run编码位；方向/幅度不改"],
  [
    7193284,
    445121794,
    1384120418,
    "server42合法移动请求固定Run3，原type0/死亡/锁定/幅度/方向门保留",
  ],
  [7196304, 872415456, 3573751839, "server49仅取消既有player0/mode2门之后的零精力拒绝"],
  [7196352, 2847898621, null, "server4A入口尾跳原49；长度/type0/mode门保留"],
];

export const PRIVATE_OBS = [
  [4451528, 6, "b8686953", "private touch-run-v4 PRE-ldr; full800 host ABI"],
  [
    19102816,
    8,
    "d10143ff",
    "ui world anchor: camera world->screen entry (camera, point, out); host swaps x0 to a copy whose projection is shifted by the UI rect y (iPad UI kept 16:9 inside a taller screen), game camera untouched",
  ],
  [
    6206144,
    10,
    "a9bc5ff8",
    "server world-sim entry (game G): while frozen the host swaps x0 to a stand-in whose event queue is empty, so no unit / timer callback runs this tick; the real queue is left untouched",
  ],
  [
    4944432,
    11,
    "d10183ff",
    "client per-unit tick update entry (unit): while frozen the host swaps x0 to a stand-in unit carrying only the skip flag ([+0x124] bit 23), so the function takes its own early-out to the epilogue; all six call sites are inside the client tick simulation 0x461b80 / 0x461e60",
  ],
  [
    4945104,
    12,
    "f81e0ff3",
    "client per-unit per-frame update entry (unit): drives model animation / interpolation from the tick fraction; while frozen the host swaps x0 to a stand-in unit (type 9, no render component) so both the callee 0x2a18d0 and this function take their own early-outs",
  ],
  [
    2899088,
    13,
    "f81c0ff7",
    "render per-frame entry, called right after the main loop writes the world frame step [0x20cebb8] and world time [0x3114208]: while frozen the host rewrites the step to 0 and the world time to its value at freeze, the same thing the game's own pause path does at 0x2c1574",
  ],
  [
    11387956,
    14,
    "d10143ff",
    "TextBoxWidget text-changed entry (widget): reads the raw text at +0x88 and rebuilds the display; host rewrites +0x88 for strings our item-notes generator annotated with colour codes (TextBoxWidget draws U+E07E as '*'), exact-match table only, everything else untouched",
  ],
  [
    4467648,
    15,
    "a9ba6ffc",
    "client event-message entry (x0 = packet; byte 1 = event subtype, jump table 0x1da6752): subtype 0x17 = offline difficulty scale changed -> 0x20f91c chat line + system notification; host rewrites byte 1 past the table (default branch, nothing shown) only for 0x17 and only in a rift game (dm_rift.c); otherwise untouched. orig stp x28, x27, [sp, #-0x60]!",
  ],
  [
    6504448,
    16,
    "d10143ff",
    "server level change by level number (game, player, level, kind), used by waypoints / portals / quest moves: in a rift game, when the player leaves a rift map for a level that is neither town nor a rift map, the host swaps w2 to the rift's next map so the destination is replaced inside the single native transition",
  ],
  [
    7409728,
    17,
    "d101c3ff",
    "server quest event 'monster killed' entry (game, monster, attacker), the only entry (callers 0x7d8550 kill, 0x7ab8dc); in a rift game, for a monster that died on a rift map, the host swaps x1 to a stand-in whose +0xc0 is null so the function takes its own early return (0x711064): bosses in the rift are combat only, no quest state / quest notifications",
  ],
  [
    4031648,
    18,
    "d10143ff",
    "NotificationPanel message handler entry (panel, message): on InputMessage / ControllerActionBegin with action id 9 (the menu key) the native handler re-sends the notice callback and marks the message bus consumed, so the first menu press only dismisses the notice. In touch mode the host swaps x1 to a copy of the message whose action id is not 9: the notice stays until it times out and the menu key opens the menu; controller mode keeps the native behaviour",
  ],
  [
    4603712,
    19,
    "f81c0ff7",
    "client per-frame position-error smoothing entry (s0 = frame length in ticks), called by the main loop right after all units were extrapolated in place and before the per-unit error is added back: the native error is only computed on start / stop (0x4639c0, which runs before the tick loop), so a sharp turn inside a tick snaps the extrapolated position back in one frame; the host adds that frame's jump to the local player's error fields (path +0x2a8 / +0x2ac, same clamp as native) and the native code decays it",
  ],
  [
    10331568,
    20,
    "d104c3ff",
    "model yaw component per-frame update entry (x1 = argument block: +8 component, +0x20 -> +8 unit): the body turn speed is instance data on the component (+0x10..+0x2c min / max degrees per second for controller / mouse, +0x34 spring parameter); the host only reads them for the local player when a test switch is set",
  ],
  [
    3110500,
    7,
    "6dbb23e9",
    "touch target rank: candidate compare entry (closure, unit, kind, s0=score); host raises Boss/elite score, native eligibility unchanged",
  ],
  [
    11588960,
    21,
    "fc1a0fe8",
    "TooltipsPanel add-tooltip entry (panel, text, target position x2 / x3, style, text2): tooltips are immediate-mode (re-submitted by the hovered widget every frame and cleared after drawing, so the host cannot reach the record at its own time); on touch, in panel state, for an item the player just tapped, the host swaps the target position so the game lays the tooltip out beside the panel instead of over the item grid; everything else passes through unchanged",
  ],
  [
    8185856,
    22,
    "d100c3ff",
    "server place-one-preset-unit entry (game, room, record), only called by the room-activation preset loop 0x7ce930 (8 sites): in a rift game at tier 90+ on a rift map, for a monster record that is not a superunique and not scenery / NPC, the host swaps x2 to a copy whose class is -1 so 0x6910d0 takes its own early return (tbnz w2, #31) and nothing is spawned; the real record is untouched; rift guardian / invader / superunique spawns call 0x7ce630 / 0x69b310 directly and never pass here",
  ],
];

export const RUN_REASONS = [
  "touch-run-v4：Run选择仅player0忽略精力门，非player按原stat返回；w8后继覆盖",
  "touch-run-v4：原合法Run成本段仅player0跳stat10扣除/耗尽降走；非player保持原cmp",
  "touch-run-v4：client当前受控Run3仅player0跳STA零值自动Stop；原getter值与真实松杆保留，非player保原门",
  "touch-run-v4：原通用合法移动仅player0请求Walk2规范为Run3，非player与其它请求保原，w8/原资格不改",
];

export const PRIVATE_BRANCHES = [
  [
    4080700,
    963232360,
    4080736,
    133,
    "private_skill_render",
    "private confirmed six HUD only: own image/text skip ->4460; unknown original ->4440; svc85 full800",
  ],
  [
    8176096,
    3506537471,
    6504448,
    137,
    "rift_exit",
    "rift exit redirect: tile-link level change entry -> host swaps (level, kind) and x8!=0 takes the branch to level-number change 0x634000; otherwise original ->0x7cc1e4; svc89",
  ],
  [
    7317968,
    960762538,
    7317972,
    140,
    "rift_save_act",
    "rift save act: save-header writer, right after it loads the current act into w9 -> host swaps w9 to 4 (act V) while the player is in a rift map; x8 stays 0, original insn runs ->0x6fa9d4; svc8c",
  ],
  [
    6595404,
    706282473,
    6595408,
    141,
    "merc_slot",
    "merc slots: placement check, monster-owner branch right after the helm type test -> host sets w0=1 for ring/amulet/boots/gloves/belt on a hireling (rift best >= 10 for new equips; always on load); x8 stays 0, original insn runs ->0x64a350; svc8d",
  ],
  [
    3926872,
    2853372896,
    3926876,
    142,
    "merc_slot_client",
    'merc slots (client): hireling can-equip check, between "mov w1,#0x25" and the type test -> host swaps w1 to the item\'s own type for ring/amulet/boots/gloves/belt (rift best >= 10); x8 stays 0, original insn runs ->0x3beb5c; svc8e',
  ],
];
