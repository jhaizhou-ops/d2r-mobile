/* Android 侧宿主调用分发（svc 0x80 起）。对照参考实现 dm/host/hle_tact.c 与各门实现：
 *   0x80 TACT BLTE zlib 解码  → libdeflate 完整实现（入口被换，必须实现；等价依据见 hle_tact.c 头注释）
 *   0x86 技能瞄准门          → 移植 dm/host/hle_private_aim.c（拖技能钮任意角度瞄准 + 方向锥选目标）
 *   0x85 技能渲染门          → 移植 hle_tact.c 的判据（触屏模式才藏原版技能格图标；padmode 未接前按手柄模式 = 照画）
 *   其余（0x81-0x84 / 0x87-0x8e）→ "原样代调"：以原参数调用剪口替换掉的原函数并回填返回值 + 审计日志。
 *     功能变换（掉落光柱 / 掉落提示音 / 进局落小站旁 / 秘境转场 / 佣兵装备位）随对应功能阶段再把规则接上，
 *     在此之前行为 = 顶掉这些剪口之前（安全）。
 * 参考：main.cuts.tsv 每条剪口描述里写着 original 目标地址；hle_tact.c 是同构参考。
 * Android dispatcher (svc 0x80+). Reference: dm/host/hle_tact.c and the per-gate implementations. */
#include "../host/hle.h"
#include "../host/dm.h"
#include "../host/dm_input.h"
#include "../third_party/libdeflate/libdeflate.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

// ---- 公共：main 装载基址 / 安全读（process_vm_readv 自读，坏指针不会崩） / 审计日志 ----
// Common: main load base / safe read (process_vm_readv on self; a bad pointer cannot crash us) / audit log
static uint8_t *main_base(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return (uint8_t *)g_mod[i].base;
    return NULL;
}
static int read_live(uint64_t a, void *out, size_t n) {
    if (!a || a > UINT64_MAX - n) return 0;
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}
static void audit(const char *what, int num, uint64_t *regs) {
    static _Atomic unsigned n[0x80];
    int slot = num & 0x7f; unsigned k = slot < 0x80 ? atomic_fetch_add(&n[slot], 1) : 0;
    if (k < 8) LOG("SVC_HOST[%s] %#x 原样代调（第 %u 次）：x0=%#llx x1=%#llx x2=%#llx x3=%#llx", what, num,
                   k + 1, (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2], (unsigned long long)regs[3]);
}

// ---- 0x80：TACT BLTE 块 zlib 解码（移植 hle_tact.c 的 tact_decode_zlib，去掉对拍/预取/计时） ----
// 原函数签名：status DecodeZlib(u8 *out, u32 out_cap, const u8 *in, u32 in_len, u32 flags, u32 stream, Obj *override)
static __thread struct libdeflate_decompressor *t_dec;
static pthread_key_t g_dec_key; static pthread_once_t g_dec_once = PTHREAD_ONCE_INIT;
static void dec_free(void *p) { libdeflate_free_decompressor(p); }
static void dec_key_init(void) { if (pthread_key_create(&g_dec_key, dec_free)) die("libdeflate 解压器键创建失败"); }
static struct libdeflate_decompressor *thread_dec(void) {
    if (t_dec) return t_dec;
    if (!(t_dec = libdeflate_alloc_decompressor())) die("libdeflate 分配解压器失败");
    pthread_once(&g_dec_once, dec_key_init); pthread_setspecific(g_dec_key, t_dec); return t_dec;
}
static uint32_t tact_decode_zlib(uint64_t *r) {
    uint8_t *out = (uint8_t *)r[0]; uint32_t cap = (uint32_t)r[1];
    const uint8_t *in = (const uint8_t *)r[2]; uint32_t n = (uint32_t)r[3], flags = (uint32_t)r[4];
    void **ov = (void **)r[6];
    if (ov) {
        uint32_t (*cb)(void *, void *, uint32_t, const void *, uint32_t, uint32_t) = (*(void ***)ov)[2];
        if (!(cb(ov, out, cap, in, n, 1) & 0xff)) return 0;
    }
    if (flags & 1) {
        if (n < 2 || in[0] != 'Z') { LOG("tact：zlib 块编码字节不是 'Z'（%#x）", n ? in[0] : 0); return 8; }
        in++; n--;
    }
    size_t got = 0;
    enum libdeflate_result rc = libdeflate_zlib_decompress(thread_dec(), in, n, out, cap, &got);
    if (rc == LIBDEFLATE_SUCCESS) return 0;
    LOG("tact：zlib 解码失败 %d（输入 %u，容量 %u）", rc, n, cap);
    return rc == LIBDEFLATE_INSUFFICIENT_SPACE ? 4 : 1;
}

// ---- 0x85：私有渲染门（触屏模式不画原版技能格图标）。弱符号：绑定桥（后续阶段）提供强实现后自动覆盖 ----
// 0x85: private render gate (hide the game's own skill-slot glyphs in touch mode). Weak: the binding bridge (later phase) overrides.
__attribute__((weak)) int dm_touch_game_hide_own_glyph(uint64_t widget) { (void)widget; return 0; }
__attribute__((weak)) int dm_padmode_current(void) { return 1; }   // 1 = 手柄模式（padmode 未接前：图标照画，行为同现在）
static uint64_t svc_private_skill_render(uint64_t *regs) {
    regs[8] = !dm_padmode_current() && dm_touch_game_hide_own_glyph(regs[19]) ? 1 : 0;
    return 1;
}

// ---- 0x86：技能瞄准门（移植 hle_private_aim.c：右摇杆屏幕方向 → 世界格方向 + 方向锥内选目标） ----
// skill-aim-v1-private: direction input adaptation for the original skill ground consumer only.
int dm_private_aim_vector(int32_t rx, int32_t ry, float out[2]) {
#pragma clang fp contract(off)
    if (!rx && !ry) return 0;
    if (rx > 32767) rx = 32767; if (rx < -32767) rx = -32767;
    if (ry > 32767) ry = 32767; if (ry < -32767) ry = -32767;
    float x = (float)rx / 32767.0f, y = (float)ry / 32767.0f;
    float dx = (x - 2.0f * y) * 0.5f, dy = (-x - 2.0f * y) * 0.5f, n = sqrtf(dx * dx + dy * dy);
    if (!(n > 0.0f) || !isfinite(n)) return 0;
    out[0] = dx / n; out[1] = dy / n;
    return 1;
}
#define AIM_CONE_DEG 30.0f
#define AIM_CONE_MAX_DIST 22.0f
static uint64_t g_cone_rec, g_cone_slot_mgr; static uint8_t g_cone_rec_was; static _Atomic int g_cone_set;
static float cone_cos(void) { static float c = -2; if (c < -1.5f) { const char *e = getenv("DM_AIM_CONE"); float d = e ? (float)atof(e) : AIM_CONE_DEG; c = d > 0 ? cosf(d * 3.14159265f / 180.0f) : 2.0f; } return c; }   // 2 = 关掉 | 2 = off
static int cone_unit_xy(uint64_t u, float *x, float *y) { uint64_t pp = 0; uint16_t a = 0, c = 0; if (!read_live(u + 0x38, &pp, 8) || !pp || !read_live(pp + 2, &a, 2) || !read_live(pp + 6, &c, 2)) return 0; *x = (float)a + 0.5f; *y = (float)c + 0.5f; return 1; }
static void cone_restore(void) {
    if (g_cone_rec) { *(uint8_t *)(g_cone_rec + 0x18) = g_cone_rec_was; g_cone_rec = 0; }
    if (g_cone_slot_mgr) { *(uint64_t *)(g_cone_slot_mgr + 0xd10) = 0; g_cone_slot_mgr = 0; }
    atomic_store(&g_cone_set, 0);
}
void dm_aim_cone_pump(void) {   // 取手柄处每帧调（P2 接入 input_backend 后的收尾钩子；现在不调也安全） | per-frame cleanup hook at the controller read (wired in P2; safe when uncalled)
    if (!atomic_load(&g_cone_set)) return; int32_t rx = 0, ry = 0; dm_input_sampled_right(&rx, &ry);
    if (!(getenv("DM_TEST_AIM") || getenv("DM_TEST_AIM_AT")) && !rx && !ry) { cone_restore(); return; }
}
static uint32_t g_cone_dropped[8]; static int g_cone_ndrop;
static uint64_t cone_pick(const module_t *m, uint64_t pl, const float pos[2], const float vec[2], float cmin, int *out_n, float *out_cos) {
    uint64_t best = 0; float bc = -2; int n = 0;
    for (int k = 0; k < 128; k++) for (uint64_t u = *(uint64_t *)(m->base + 0x2d603a0 + 1 * 0x400 + k * 8); u; u = *(uint64_t *)(u + 0x158)) {
        float x, y; uint32_t mode = *(uint32_t *)(u + 0xc); if (mode == 0 || mode == 12 || !cone_unit_xy(u, &x, &y)) continue;
        if (((int (*)(int, uint64_t, uint64_t))(m->base + 0x2f7b7c))(0x165, pl, u) != 1 || !(((int (*)(uint64_t, int, int, int, int))(m->base + 0x464a70))(u, 0, 0, -1, -1) & 1)) continue;
        { int dr = 0; for (int i = 0; i < g_cone_ndrop; i++) if (g_cone_dropped[i] == *(uint32_t *)(u + 8)) dr = 1; if (dr) continue; }
        float dx = x - pos[0], dy = y - pos[1], d = sqrtf(dx * dx + dy * dy); if (d < 0.01f || d > AIM_CONE_MAX_DIST) continue; n++;
        float c = (dx * vec[0] + dy * vec[1]) / d; if (c >= cmin && c > bc) { bc = c; best = u; } }
    *out_n = n; *out_cos = bc; return best;
}
static void svc_private_skill_aim(uint64_t *regs) {
    const module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) die("skill-aim缺main");
    typedef void (*Original)(uint64_t, uint64_t, float *, float *);
    float *position = (float *)regs[2], *direction = (float *)regs[3];
    ((Original)(m->base + 0x2b4ef8))(regs[0], regs[1], position, direction);   // 先保原位置/零aim和所有原回退 | first preserve the original position/zero aim and all original fallbacks
    uint64_t caller = 0; uint32_t type = 99, id = 0, current = 0;
    int scope = regs[29] <= UINT64_MAX - 8 && read_live(regs[29] + 8, &caller, 8) &&
        (caller == (uint64_t)m->base + 0x2b5b80 || caller == (uint64_t)m->base + 0x2b3298) &&
        read_live(regs[1], &type, 4) && !type && read_live(regs[1] + 8, &id, 4) &&
        read_live((uint64_t)m->base + 0x2d60380, &current, 4) && id == current;
    if (!scope) return;
    int32_t rx = 0, ry = 0; dm_input_sampled_right(&rx, &ry);
    { const char *t = getenv("DM_TEST_AIM"); if (t) sscanf(t, "%d,%d", &rx, &ry); }
    float vec[2]; int active = dm_private_aim_vector(rx, ry, vec);
    if (!active && atomic_load(&g_cone_set)) cone_restore();
    if (active) {
        typedef float (*Range)(uint64_t, uint64_t, int); typedef uint64_t (*Mgr)(void);
        typedef uint64_t (*Rec)(uint64_t, int32_t);
        int32_t sk = -1; read_live(regs[23] + (uint64_t)(uint32_t)regs[25] * 0xb8 + 0xd4c, &sk, 4);
        uint64_t mg = ((Mgr)(m->base + 0x2acecc))(), rec = sk >= 0 ? ((Rec)(m->base + 0x2b4bc8))(mg, sk) : 0;
        float dist = 0.0f; uint8_t scaled = 1, ver = 0; uint16_t fn = 0; int far = 0;
        if (rec && read_live(rec + 8, &dist, 4) && read_live(rec + 0x10, &scaled, 1) && dist > 0.0f && dist <= 2.0f && !scaled &&
            read_live(regs[1] + 0x1bd, &ver, 1) && ver >= 1 && ver <= 3) {
            uint64_t tbl = 0, base = 0, cnt = 0;
            if (read_live((uint64_t)m->base + 0x2d64a18 + (uint64_t)ver * 16, &tbl, 8) && read_live(tbl + 0x11e8, &base, 8) && read_live(tbl + 0x11f0, &cnt, 8) &&
                (uint64_t)sk < cnt && read_live(base + (uint64_t)sk * 0x2ec + 0x4e, &fn, 2)) far = !(fn == 27 || fn == 52 || fn == 67 || fn == 76 || fn == 77 || fn == 78);
        }
        float rg = ((Range)(m->base + 0x2b5c70))(mg, regs[1], 0), k = (far && rg > 0.0f && isfinite(rg)) ? 24.0f / rg : 1.0f;
        { static int lg; if (getenv("DM_SKILL_AIM_LOG") && lg++ < 12) LOG("瞄准适配：技能号 %d 手柄落点距离 %.2f 施放函数 %u 射程 %.2f 放大 %.1f", sk, dist, fn, rg, k); }
        direction[0] = vec[0] * k; direction[1] = vec[1] * k;
        if (cone_cos() <= 1.0f) {
            cone_restore(); int nmon = 0; float bc = 0; float p0[2] = {position[0], position[1]};
            uint64_t tgt = cone_pick(m, regs[1], p0, vec, cone_cos(), &nmon, &bc);
            if (tgt) { *(uint64_t *)(mg + 0xd10) = tgt; g_cone_slot_mgr = mg; }
            else if (rec) { g_cone_rec = rec; g_cone_rec_was = *(uint8_t *)(rec + 0x18); *(uint8_t *)(rec + 0x18) = 1; }
            atomic_store(&g_cone_set, 1);
            { static int lg; static uint32_t last = 0xfffffffe; uint32_t now = tgt ? *(uint32_t *)(tgt + 8) : 0xffffffff;
              if (now != last && lg++ < 60) { last = now; uint32_t tc = 0, ti = 0; if (tgt) { tc = *(uint32_t *)(tgt + 4); ti = *(uint32_t *)(tgt + 8); }
                LOG("瞄准选目标：22 格内怪 %d 只；手指方向锥内 %s（类号 %u id %u，夹角 %.1f°）→ %s", nmon, tgt ? "有" : "无", tc, ti,
                    tgt ? acosf(bc > 1 ? 1 : bc) * 57.29578f : 0.0f, tgt ? "锁它" : "对位置放"); } }
        }
    }
}

// ---- 0x81/0x82：收包泵投递（默认原样代调；DM_C2S_REPLAY 测试回放未接——直接代调即原版行为） ----
// 0x81/0x82: receive-pump delivery (called through unchanged; the DM_C2S_REPLAY test path is not ported, so this is the original behaviour)
static uint64_t svc_c2s_deliver(uint64_t *regs) {
    uint8_t *b = main_base(); if (!b) return 0;
    audit("c2s-deliver", SVC_HOST_C2S_DELIVER, regs);
    ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t))(b + 0x632510))(regs[0], regs[1], regs[2], regs[3]);
    return 1;
}
static uint64_t svc_c2s_sys(uint64_t *regs) {
    uint8_t *b = main_base(); if (!b) return 0;
    audit("c2s-sys", SVC_HOST_C2S_SYS, regs);
    ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t))(b + 0x6310b0))(regs[0], regs[1], regs[2], regs[3]);
    return 1;
}

// ---- 0x83/0x84：Granny 段读取逐块解码（原样代调：bl 0x11f4af0(ctx, 块, 长度) 并回填 w0） ----
// 签名照参考实现 hle_granny.c: uint64_t (*g_dec)(uint64_t ctx, uint64_t chunk, uint64_t len)——三个都是 64 位，x0 是指针，
// 之前按 (uint32_t) 传会截掉高 32 位（真机实测：原函数拿 0x76fc1140 访问 [ctx+0x48] 直接 SIGSEGV）。
static uint64_t svc_granny_chunk(uint64_t *regs, int num) {
    uint8_t *b = main_base(); if (!b) return 0;
    audit("granny", num, regs);
    regs[0] = ((uint64_t (*)(uint64_t, uint64_t, uint64_t))(b + 0x11f4af0))(regs[0], regs[1], regs[2]) & 0xffffffff;
    return 1;
}

// ---- 0x87：物品外观路径（原样代调 0xa14dc4；掉落光柱变换等 P8b 接规则） ----
static uint64_t svc_ground_asset(uint64_t *regs) {
    uint8_t *b = main_base(); if (!b) return 0;
    audit("ground-asset", SVC_HOST_GROUND_ASSET, regs);
    {   register uint64_t x0 __asm__("x0") = regs[0], x1 __asm__("x1") = regs[1]; register uint64_t x8 __asm__("x8") = regs[8]; register uint64_t x16 __asm__("x16") = (uint64_t)(uintptr_t)(b + 0xa14dc4);
        __asm__ volatile("blr x16" : "+r"(x0), "+r"(x1), "+r"(x8), "+r"(x16) : : "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x30",
            "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc", "memory"); }
    return 1;
}

// ---- 0x88：掉落音（原样代调 0x470c20(物品, &触发帧) → w0；一档提示音变换等 P8b 接规则） ----
static uint64_t svc_drop_sound(uint64_t *regs) {
    uint8_t *b = main_base(); if (!b) return 0;
    audit("drop-sound", SVC_HOST_DROP_SOUND, regs);
    regs[0] = (uint32_t)((uint32_t (*)(uint64_t, uint64_t))(b + 0x470c20))(regs[0], regs[1]);
    return 1;
}

// ---- 0x89/0x8c/0x8d/0x8e：分支门（x8 = 0 = 原样执行；秘境/佣兵规则等 P8c 接） ----
static uint64_t svc_branch_gate(uint64_t *regs, int num) {
    audit("branch", num, regs);
    regs[8] = 0;
    return 1;
}

// ---- 0x8a/0x8b：进局出生点（原样代调 0x5596c0；"落小站旁"等 P8b 接规则） ----
static uint64_t svc_join_spawn(uint64_t *regs, int num) {
    uint8_t *b = main_base(); if (!b) return 0;
    audit(num == SVC_HOST_JOIN_CORPSE ? "join-corpse" : "join-spawn", num, regs);
    regs[0] = ((uint64_t (*)(int, uint64_t, int, int, int32_t *, int32_t *, int))(b + 0x5596c0))(
        (int)(uint32_t)regs[0], regs[1], (int)(uint32_t)regs[2], (int)(uint32_t)regs[3],
        (int32_t *)(uintptr_t)regs[4], (int32_t *)(uintptr_t)regs[5], (int)(uint32_t)regs[6]);
    return 1;
}

uint64_t svc_host_call(int num, uint64_t *regs) {
    switch (num) {
    case SVC_HOST_TACT_ZLIB: regs[0] = tact_decode_zlib(regs); return 1;
    case SVC_HOST_PRIVATE_SKILL_AIM: svc_private_skill_aim(regs); return 1;
    case SVC_HOST_PRIVATE_SKILL_RENDER: return svc_private_skill_render(regs);
    case SVC_HOST_C2S_DELIVER: return svc_c2s_deliver(regs);
    case SVC_HOST_C2S_SYS: return svc_c2s_sys(regs);
    case SVC_HOST_GRANNY_A: return svc_granny_chunk(regs, num);
    case SVC_HOST_GRANNY_B: return svc_granny_chunk(regs, num);
    case SVC_HOST_GROUND_ASSET: return svc_ground_asset(regs);
    case SVC_HOST_DROP_SOUND: return svc_drop_sound(regs);
    case SVC_HOST_RIFT_EXIT: return svc_branch_gate(regs, num);
    case SVC_HOST_JOIN_SPAWN: return svc_join_spawn(regs, num);
    case SVC_HOST_JOIN_CORPSE: return svc_join_spawn(regs, num);
    case SVC_HOST_SAVE_ACT: return svc_branch_gate(regs, num);
    case SVC_HOST_MERC_SLOT: return svc_branch_gate(regs, num);
    case SVC_HOST_MERC_SLOT_CL: return svc_branch_gate(regs, num);
    }
    die("Android build-specific host call %#x requires verified version mapping", num);
}
