// 高价值掉落震动：只在高价值物品掉落时震，档位与过滤器分享站的高价值物品挂钩；暴击和喝药不震。自定义页开关 drop_haptic，缺省开。
// 时机：物品落地那一刻——掉落音剪口 svc #0x88（dm_dropfilter.c，原版落地调用点 0x468f2c，物品模式 5）每落一件调 dm_drop_haptic_land。
//   怪物 / 宝箱掉出来的都算掉落；捡起、交易、魔盒不落地，不会走到这里；玩家从手上扔到地上（包 0x17）也不经这个调用点（Mac 上的 iOS 包实测：扔 Ber 没有触发）。
// 前端（front_ios.m）每帧 dm_drop_haptic_take() 取走待震的最高一档：1 重 / 2 中 / 3 轻 → UIImpactFeedbackGenerator Heavy / Medium / Light。
// 安卓（#289）：touch_android.c dm_android_touch_step 每帧取档，经 text_input_jni.c 的工作线程交给 DmActivity.dropHaptic → 系统预设的重击 / 点击 / 轻触。
//
// 名单（k_rules，按档从重到轻，命中第一条即定档；改名单只改这张表）。来源：
//   · 社群过滤器：GitHub n0130dykn0ws/n0130dy-d2r-loot-filter（v1.5，D2R 3.x 原版过滤器 JSON）——它的「常显」类：全部符文与钥匙 / 精华、暗金、套装、护身符、珠宝、
//     稀有戒指项链、精英带孔底材；它只分显示 / 隐藏，不分值钱程度
//   · 分档：符文价值按 maxroll「Rune Value」（Ber / Jah / Sur / Lo 很高，Ohm 高）与 Blizzard 论坛社群换算表（以 Ist 计：Vex 3、Ohm 6–7、Lo 12、Sur 16–18、Ber 32、Jah ≈ Ber、Cham 5、Zod 9）
//   · 其余档位是按上面两者合并：地上一律看不出是哪一件暗金（未鉴定），暗金整体放中档；「热门暗金」细分要读暗金表序号，留作以后加
// 物品读法同 dm_dropfilter.c：码 main+0x570260、类型（含父类型）main+0x574bc0、品质 = 数据块 +0、属性 main+0x5b9290（194 = 孔数）。
// High-value drop haptics: vibrate only when a high-value item drops, with tiers tied to the high-value items of the filter sharing sites; critical hits and drinking potions do not vibrate. Custom page switch drop_haptic, on by default.
// Timing: the moment the item lands -- the drop sound cut svc #0x88 (dm_dropfilter.c, the original game's landing call site 0x468f2c, item mode 5) calls dm_drop_haptic_land for every item that lands.
//   Anything dropped by monsters / chests counts as a drop; picking up, trading and the Horadric Cube do not land and never get here; the player throwing an item from hand onto the ground (packet 0x17) does not go through this call site either (measured with the iOS app on Mac: throwing Ber did not trigger it).
// The front end (front_ios.m) calls dm_drop_haptic_take() every frame to take the highest pending tier: 1 heavy / 2 medium / 3 light -> UIImpactFeedbackGenerator Heavy / Medium / Light.
// Android (#289): touch_android.c dm_android_touch_step takes the tier every frame and the worker thread of text_input_jni.c hands it to DmActivity.dropHaptic -> the system's predefined heavy click / click / tick.
// List (k_rules, ordered by tier from heavy to light, the first matching rule sets the tier; to change the list change only this table). Sources:
//   - Community filter: GitHub n0130dykn0ws/n0130dy-d2r-loot-filter (v1.5, original-game filter JSON for D2R 3.x) -- its 'always show' classes: all runes and keys / essences, uniques, sets, charms, jewels,
//     rare rings and amulets, elite socketed bases; it only distinguishes show / hide, not how valuable
//   - Tiering: rune value follows maxroll 'Rune Value' (Ber / Jah / Sur / Lo very high, Ohm high) and the community conversion table on the Blizzard forums (in Ist: Vex 3, Ohm 6–7, Lo 12, Sur 16–18, Ber 32, Jah ≈ Ber, Cham 5, Zod 9)
//   - The remaining tiers are a merge of the two above: on the ground there is never any way to tell which unique it is (unidentified), so uniques as a whole go into the medium tier; a 'popular uniques' subdivision would need to read the unique table index and is left to be added later
// Items are read the same way as in dm_dropfilter.c: code main+0x570260, type (including parent types) main+0x574bc0, quality = data block +0, stat main+0x5b9290 (194 = socket count).
#include "dm_custom.h"
#include "hle.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { K_CODE, K_QUAL, K_TYPE_QMIN, K_SOCK };   // 码 4 字符 / 品质 = n / 类型 t 且品质 ≥ q / 普通或超强品质、孔数 ≥ n 的武器或护甲 | 4-character code / quality = n / type t and quality ≥ q / weapon or armor of normal or superior quality with socket count ≥ n
typedef struct { uint8_t tier, kind; uint16_t a, b; const char *code; const char *note; } dh_rule;
enum { T_RING = 10, T_AMUL = 12, T_WEAP = 45, T_ARMO = 50, T_JEWL = 58, T_SCHA = 83, T_MCHA = 84, T_LCHA = 85 };   // itemtypes.txt 行序（同 dm_dropfilter.c） | Row order of itemtypes.txt (same as dm_dropfilter.c)
static const dh_rule k_rules[] = {
    { 1, K_CODE, 0, 0, "r27 ", "Ohm" }, { 1, K_CODE, 0, 0, "r28 ", "Lo" }, { 1, K_CODE, 0, 0, "r29 ", "Sur" }, { 1, K_CODE, 0, 0, "r30 ", "Ber" }, { 1, K_CODE, 0, 0, "r31 ", "Jah" },
    { 2, K_CODE, 0, 0, "r23 ", "Mal" }, { 2, K_CODE, 0, 0, "r24 ", "Ist" }, { 2, K_CODE, 0, 0, "r25 ", "Gul" }, { 2, K_CODE, 0, 0, "r26 ", "Vex" }, { 2, K_CODE, 0, 0, "r32 ", "Cham" }, { 2, K_CODE, 0, 0, "r33 ", "Zod" },
    { 2, K_QUAL, 7, 0, NULL, "暗金（含暗金护身符 / 珠宝）" },
    { 2, K_CODE, 0, 0, "pk1 ", "恐惧之钥" }, { 2, K_CODE, 0, 0, "pk2 ", "憎恨之钥" }, { 2, K_CODE, 0, 0, "pk3 ", "毁灭之钥" },
    { 2, K_CODE, 0, 0, "tes ", "痛苦精华" }, { 2, K_CODE, 0, 0, "ceh ", "憎恨精华" }, { 2, K_CODE, 0, 0, "bet ", "恐惧精华" }, { 2, K_CODE, 0, 0, "fed ", "毁灭精华" }, { 2, K_CODE, 0, 0, "toa ", "赦免之证" },
    { 3, K_CODE, 0, 0, "r21 ", "Pul" }, { 3, K_CODE, 0, 0, "r22 ", "Um" },
    { 3, K_QUAL, 5, 0, NULL, "套装" },
    { 3, K_TYPE_QMIN, T_SCHA, 4, NULL, "魔法及以上小护身符" }, { 3, K_TYPE_QMIN, T_MCHA, 4, NULL, "魔法及以上中护身符" }, { 3, K_TYPE_QMIN, T_LCHA, 4, NULL, "魔法及以上大护身符" },
    { 3, K_TYPE_QMIN, T_JEWL, 4, NULL, "魔法及以上珠宝" }, { 3, K_TYPE_QMIN, T_RING, 6, NULL, "稀有戒指" }, { 3, K_TYPE_QMIN, T_AMUL, 6, NULL, "稀有项链" },
    { 3, K_SOCK, 4, 0, NULL, "4 孔及以上的白 / 灰底材（符文之语底材）" },
};

static _Atomic int g_pending;   // 待震的最高档（1 最重）；0 = 没有 | Highest tier pending vibration (1 is the heaviest); 0 = none

static int tier_of(uint8_t *b, uint64_t u, const char **why) {
    int (*is)(uint64_t, int) = (int (*)(uint64_t, int))(b + 0x574bc0);
    uint32_t code = ((uint32_t (*)(uint64_t))(b + 0x570260))(u);
    const uint8_t *d = (const uint8_t *)(uintptr_t)*(uint64_t *)(u + 0x10); uint32_t q = d ? *(const uint32_t *)d : 0;
    for (unsigned i = 0; i < sizeof k_rules / sizeof *k_rules; i++) { const dh_rule *r = &k_rules[i]; int hit = 0;
        switch (r->kind) {
        case K_CODE: hit = !memcmp(&code, r->code, 4); break;
        case K_QUAL: hit = q == r->a; break;
        case K_TYPE_QMIN: hit = q >= r->b && is(u, r->a); break;
        case K_SOCK: hit = q <= 3 && (is(u, T_WEAP) || is(u, T_ARMO)) && ((int (*)(uint64_t, int, int))(b + 0x5b9290))(u, 194, 0) >= r->a; break; }
        if (hit) { *why = r->note; return r->tier; } }
    return 0;
}

// 掉落音剪口里调（物品刚落地，模式 5）
// Called in the drop sound cut (the item has just landed, mode 5)
void dm_drop_haptic_land(uint8_t *b, uint64_t u) {
    static int env = -2; if (env == -2) { const char *e = getenv("DM_DROP_HAPTIC"); env = e ? e[0] != '0' : -1; }   // 测试强制：0 关 / 其余开 | Test override: 0 off / anything else on
    if (!b || !u || *(uint32_t *)u != 4 || !(env >= 0 ? env : dm_custom_get("drop_haptic"))) return;
    uint32_t id = *(uint32_t *)(u + 8);
    const char *why = NULL; int t = tier_of(b, u, &why); if (!t) return;
    int cur = atomic_load(&g_pending); while ((!cur || t < cur) && !atomic_compare_exchange_weak(&g_pending, &cur, t)) {}
    LOG("掉落震动：物品 id %u 第 %d 档（%s）", id, t, why);
}

int dm_drop_haptic_take(void) { return atomic_exchange(&g_pending, 0); }
