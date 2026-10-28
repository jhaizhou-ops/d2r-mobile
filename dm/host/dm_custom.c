// 「自定义」设置：我们自己加的功能的启停开关。接口见 dm_custom.h。
// 存储：存档提交目录旁的 sw_custom.txt（iOS 上在 Documents 里，升级包不丢），一行一个「键=0/1」；没有这个文件 / 没写的键 = 各键缺省（省电模式关，其余开）；不认识的行（旧版的 drop_filter、ver）读时跳过。
// 掉落筛选照红叶 mod 一件都不藏，整块拆掉，drop_filter 键与它的 ver=2 迁移一起删了
// 不写进游戏自己的设置存档：不依赖它的格式与写盘时机，应用 / 放弃 / 默认由我们自己按原版选项页的语义做。
// 界面：选项面板「自定义」页（借原版视频页 VideoOptionsPanel），每行一个原版勾选框，控件名 TouchCustom_<键>（布局 touch_layout.py 的 _custom_page）。勾选只记成待定值，
// 「应用」（OptionsPanelMessage:AcceptChanges）才生效并存盘，「放弃」（CancelChanges）丢掉待定值，「默认」（ResetToDefaults）待定值回各键缺省（省电模式关、其余开；还要点应用）
// 「自定义」(Custom) settings: on/off switches for the features we added ourselves. For the interface see dm_custom.h.
// Storage: sw_custom.txt next to the save commit directory (on iOS it is in Documents and survives app upgrades), one 'key=0/1' per line; without this file / for keys not written = each key's default (power-saving mode off, the rest on); unrecognized lines (drop_filter and ver from old versions) are skipped when reading.
// Drop filtering follows the Hongye community mod and hides nothing, so the whole block was removed, and the drop_filter key was deleted together with its ver=2 migration
// Not written into the game's own settings save: no dependence on its format or on when it writes to disk; apply / discard / default are done by ourselves following the semantics of the original game's options page.
// UI: the 「自定义」(Custom) page of the options panel (borrowing the original game's video page VideoOptionsPanel), one original-game checkbox per row, widget name TouchCustom_<key> (_custom_page in the layout touch_layout.py). Checking only records a pending value;
// only 「应用」(Apply) (OptionsPanelMessage:AcceptChanges) makes it take effect and saves to disk, 「放弃」(Discard) (CancelChanges) throws away the pending values, and 「默认」(Default) (ResetToDefaults) sets the pending values back to each key's default (power-saving mode off, the rest on; Apply still has to be tapped)
#include "dm_custom.h"
#include "dm_widget.h"
#include "dm_cursor.h"
#include "hle.h"
#include "dm_input.h"
#include "dm_i18n.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 「配音」单选（voice：0 中文配音 / 1 English voice，公开）只在编进过场解码（DM_MOVIE_FFMPEG，Makefile 的 MOVIE_FFMPEG=1）的包里有：
// 布局侧 touch_layout.py 的 _CUSTOM_KEYS 按同一个开关（环境变量 MOVIE_FFMPEG=1）加这一行。缺省包键表、行序、存盘内容与原来一字不差
#ifdef DM_MOVIE_FFMPEG
#define VOICE_ROW 1
#else
#define VOICE_ROW 0
#endif
static const char *const k_keys[] = { "lang",
#if VOICE_ROW
    "voice",
#endif
    "auto_mana", "merc_pot", "drop_light", "map_mark", "item_notes", "icloud", "pot_pick", "drop_haptic", "gem_pick", "store_btn", "eco_fps", "q3d", "loot_filter", "auto_buff", "touch_swap_above", "cam" };   // 顺序 = 自定义页从上到下的行序（touch_layout.py _CUSTOM_ROWS）：手柄在这页按这个顺序上下选行 | Order = row order of the Custom page from top to bottom (touch_layout.py _CUSTOM_ROWS): the controller selects rows up/down on this page in this order
#define NK (int)(sizeof k_keys / sizeof *k_keys)
static const int k_def[NK] = { 0,
#if VOICE_ROW
    1,
#endif
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0 };   // 缺省值：省电模式（30 帧）缺省关、3D 画质缺省流畅优先（q3d=0），换组默认在传送门左侧，其余缺省开（voice 的缺省不用：读盘时按当前语言定、「默认」不管它）
// q3d（3D 画质）：0 = 流畅优先（掉帧就自动降 3D 比例）、1 = 画质优先（不自动降）。生效 = 调 hle_oe.c 的 dm_3d_quality_mode_set，读盘与点「应用」时当场调，下一帧控制器就按新档走。
// 页上是上下两项单选（同语言行）：TouchCustom_q3d 是「画质优先」那格（勾上 = 1），TouchCustom_q3d_smooth 是它的反面
// q3d (3D quality): 0 = smoothness first (automatically lowers the 3D scale on frame drops), 1 = quality first (no automatic lowering). Applied by calling hle_oe.c's dm_3d_quality_mode_set, immediately on load and on tapping "Apply", so the controller follows the new setting from the next frame.
// On the page it is a stacked two-option radio (like the language row): TouchCustom_q3d is the "Quality first" cell (checked = 1), TouchCustom_q3d_smooth is its inverse
#define K_VOICE (VOICE_ROW ? 1 : -2)   // 缺省包没有这一行：-2 不等于任何行号（也不等于 key_index 认不出时的 -1）
#define K_MERC (2 + VOICE_ROW)
#define K_Q3D (12 + VOICE_ROW)
#define K_TOUCH_SWAP (15 + VOICE_ROW)
// cam（镜头视角，实验）：三项单选 0 原版 / 1 越肩 / 2 第一人称，勾选框 TouchCustom_cam / _cam_ots / _cam_fp 互斥（同语言行），缺省 0；
// 读盘与点「应用」时当场交给 dm_cam.c（dm_cam_set_mode），不用重启
// cam (camera view, experimental): three-option radio 0 original / 1 over-the-shoulder / 2 first person, checkboxes TouchCustom_cam / _cam_ots / _cam_fp
// mutually exclusive (like the language row), default 0; handed to dm_cam.c (dm_cam_set_mode) on load and on Apply, no restart needed
#define K_CAM (16 + VOICE_ROW)
#define CAM_N 3
static const char *const k_cam_box[CAM_N] = { "TouchCustom_cam", "TouchCustom_cam_ots", "TouchCustom_cam_fp" };
static const char *const k_cam_name[CAM_N] = { "off", "over-the-shoulder", "first-person" };   // 只进日志 | Log only
void dm_cam_set_mode(int m);
void dm_3d_quality_mode_set(int mode);
static const char *rev_box(int i) { return i == K_VOICE ? "TouchCustom_voice_zh" : i == K_Q3D ? "TouchCustom_q3d_smooth" : i == K_TOUCH_SWAP ? "TouchCustom_touch_swap_above_near" : NULL; }   // 单选行「反面那一项」的勾选框 | Checkbox of the radio row's "inverse option"
static _Atomic int g_val[NK];        // 已生效：1 开 / 0 关 | In effect: 1 on / 0 off
static _Atomic int g_pend[NK];       // 选项页上改了还没应用：-1 = 没改 | Changed on the options page but not applied yet: -1 = not changed
static _Atomic int g_loaded;         // 0 没读盘 / 1 已读 | 0 not read from disk / 1 read
// lang（语言开关，自定义页第一行）：五项单选 0 中文 / 1 English / 2 Español / 3 Русский / 4 한국어（勾选框 TouchCustom_lang_zh / _en / _es / _ru / _ko 互斥）。它不是功能开关，生效方式不同——
// 下次启动挂存档时由 hle_fs.c 按它改写原版设置文件的 Language / VoiceLanguage（游戏运行中自己会把内存里的语言写回设置文件，所以只能在游戏读设置之前改；
// 见 dm_custom_lang_explicit）。文件里没有这个键（还没点过应用；旧版本写的布尔键 lang_en 不再读——原版设置文件里本来就记着上次选的语言）
// = 不动原版设置，勾选框按当前实际语言显示；「默认」不改它（按一下默认把语言切了不合适）
#define LANG_N 5
static const char *const k_lang_box[LANG_N] = { "TouchCustom_lang_zh", "TouchCustom_lang_en", "TouchCustom_lang_es", "TouchCustom_lang_ru", "TouchCustom_lang_ko" };
static const char *const k_lang_hint[LANG_N] = { "TouchCustomLangHintZh", "TouchCustomLangHintEn", "TouchCustomLangHintEs", "TouchCustomLangHintRu", "TouchCustomLangHintKo" };
static const char *const k_lang_name[LANG_N] = { "zh", "en", "es", "ru", "ko" };   // 只进日志 | Log only
static _Atomic int g_lang_explicit = -1;   // 文件里写着的 lang：-1 没写 / 0 中文 / 1 English / 2 Español / 3 Русский / 4 한국어
static int g_lang_test;   // 测试开关 DM_TEST_LANG=<0–4>：当作玩家选了这一项并应用（走 hle_fs.c 挂存档时的真实改写），但不写回 sw_custom.txt（容器各局共用）
static int lang_running(void) { int g = dm_lang_group(); return g == DM_LG_ZH ? 0 : g == DM_LG_ES || g == DM_LG_MX ? 2 : g == DM_LG_RU ? 3 : g == DM_LG_KO ? 4 : 1; }   // 此刻实际在跑的语言落在哪一项 | Which option the currently running language falls into
static _Atomic int g_voice_explicit = -1;   // 文件里写着的 voice：-1 没写 / 0 中文配音 / 1 英文配音
static int g_voice_test;   // 测试开关 DM_TEST_VOICE=0|1；只用于真机回归，不写回文件
#if VOICE_ROW
static int voice_running(void) { return lang_running() == 0 ? 0 : 1; }   // 没单独选过时，按原来的「中文配中文、其余英文」显示
#endif
// merc_pot（佣兵自动喂药）原先缺省关、现在缺省开。旧文件每次存盘都把全部键写一遍，里面的 merc_pot=0 分不出「没动过」还是「自己关的」，
// 所以另记一行 merc_pot_v2：只有在缺省改成开之后玩家自己在页上改过这一项才写。读盘时有 merc_pot_v2 以它为准；没有 = 没在新缺省下动过 → 开
// （旧文件里的 merc_pot=1 也是开，结果一样）。merc_pot 那一行照旧写当前值，旧版本的包读它不出错
// merc_pot (mercenary auto-feed potions) used to be off by default and is now on by default. Old files write all keys on every save, and the merc_pot=0 in them cannot tell "never touched" from "turned off by the player",
// so a separate line merc_pot_v2 is recorded: it is written only when the player has changed this item on the page after the default was changed to on. On load, if merc_pot_v2 exists it is authoritative; absent = never touched under the new default → on
// (merc_pot=1 in an old file is also on, the same result). The merc_pot line is still written with the current value, so packages of older versions read it without error
static _Atomic int g_merc_explicit = -1;   // 文件里写着的 merc_pot_v2：-1 没写 / 0 / 1 | merc_pot_v2 as written in the file: -1 not written / 0 / 1
static _Atomic int g_repaint;        // 默认 / 放弃之后：页上的勾选框按待定值 / 生效值重摆一遍 | After default / discard: lay out the checkboxes on the page again according to pending values / effective values

const char *dm_save_commitdir(void);
static int key_index(const char *k) { if (k) for (int i = 0; i < NK; i++) if (!strcmp(k, k_keys[i])) return i; return -1; }
static char g_dir[1024];   // iOS 外壳启动时给的 Documents（main.m）：启动期（存档挂上之前）就要能读，覆盖层选择用 | Documents given by the iOS shell at startup (main.m): must be readable already during startup (before the save is mounted), used for overlay selection
void dm_custom_init(const char *dir) { if (dir) strlcpy(g_dir, dir, sizeof g_dir); }
static int file_path(char *p, size_t n) {
    if (g_dir[0]) { snprintf(p, n, "%s/sw_custom.txt", g_dir); return 1; }
    const char *c = dm_save_commitdir(); if (!c) return 0; snprintf(p, n, "%s/../sw_custom.txt", c); return 1; }

static void load_once(void) {
    if (atomic_load(&g_loaded)) return;
    char p[1100]; if (!file_path(p, sizeof p)) return;   // 存档还没挂上：先按缺省开答，挂上后再读 | The save is not mounted yet: answer with the default (on) for now, and read again after it is mounted
    int v[NK]; for (int i = 0; i < NK; i++) v[i] = k_def[i];
    FILE *f = fopen(p, "r"); char ln[128];
    int lang = -1, voice = -1, merc = -1;
    while (f && fgets(ln, sizeof ln, f)) { char *eq = strchr(ln, '='); if (!eq) continue; *eq = 0; int i = key_index(ln); if (i > 0) v[i] = eq[1] != '0';
        if (i == 0) { int l = atoi(eq + 1); if (l >= 0 && l < LANG_N) lang = v[0] = l; }
        if (i == K_CAM) { int c = atoi(eq + 1); v[K_CAM] = c >= 0 && c < CAM_N ? c : 0; }
#if VOICE_ROW
        if (i == K_VOICE) { int x = atoi(eq + 1); if (x == 0 || x == 1) voice = v[K_VOICE] = x; }
#endif
        if (!strcmp(ln, "merc_pot_v2")) merc = eq[1] != '0'; }
    if (f) fclose(f);
    { const char *tl = getenv("DM_TEST_LANG"); if (tl && *tl >= '0' && *tl < '0' + LANG_N) { lang = v[0] = *tl - '0'; g_lang_test = 1; LOG("自定义设置：测试开关 DM_TEST_LANG=%d（当作语言行选了 %s 并应用，不写回文件）", lang, k_lang_name[lang]); } }
#if VOICE_ROW
    { const char *tv = getenv("DM_TEST_VOICE"); if (tv && *tv >= '0' && *tv <= '1') { voice = v[K_VOICE] = *tv - '0'; g_voice_test = 1; LOG("自定义设置：测试开关 DM_TEST_VOICE=%d（当作配音行选了 %s 并应用，不写回文件）", voice, voice ? "English" : "中文"); } }
#endif
    if (lang < 0) v[0] = lang_running();   // 没写过：按当前实际语言显示 | Never written: display according to the current actual language
#if VOICE_ROW
    if (voice < 0) v[K_VOICE] = voice_running();
    char vlog[32]; snprintf(vlog, sizeof vlog, " voice=%d%s", v[K_VOICE], voice < 0 ? "(auto)" : "");   // (auto) = 文件里没写、按当前语言显示
#else
    const char *vlog = "";
#endif
    atomic_store(&g_lang_explicit, lang);
    atomic_store(&g_voice_explicit, voice);
    v[K_MERC] = merc < 0 ? k_def[K_MERC] : merc; atomic_store(&g_merc_explicit, merc);
    for (int i = 0; i < NK; i++) { atomic_store(&g_val[i], v[i]); atomic_store(&g_pend[i], -1); }
    dm_cam_set_mode(v[K_CAM]);
    dm_3d_quality_mode_set(v[K_Q3D]); LOG("自定义设置：3D 画质读盘为%s（q3d=%d，已交给 3D 比例控制）", v[K_Q3D] ? "画质优先" : "流畅优先", v[K_Q3D]);
    if (!atomic_exchange(&g_loaded, 1)) LOG("自定义设置：读盘 %s%s lang=%d%s%s auto_mana=%d merc_pot=%d drop_light=%d map_mark=%d item_notes=%d icloud=%d pot_pick=%d drop_haptic=%d gem_pick=%d store_btn=%d eco_fps=%d", p, f ? "" : "（没有文件，全按缺省）", v[0], lang < 0 ? "（没写，按当前语言）" : "", vlog, v[1 + VOICE_ROW], v[2 + VOICE_ROW], v[3 + VOICE_ROW], v[4 + VOICE_ROW], v[5 + VOICE_ROW], v[6 + VOICE_ROW], v[7 + VOICE_ROW], v[8 + VOICE_ROW], v[9 + VOICE_ROW], v[10 + VOICE_ROW], v[11 + VOICE_ROW]);
}
int dm_custom_get(const char *key) {
    load_once(); int i = key_index(key); if (i < 0) return 1;
    return atomic_load(&g_loaded) ? atomic_load(&g_val[i]) : k_def[i];
}
int dm_custom_lang_explicit(void) { load_once(); return atomic_load(&g_loaded) ? atomic_load(&g_lang_explicit) : -1; }
int dm_custom_voice_explicit(void) { load_once(); return atomic_load(&g_loaded) ? atomic_load(&g_voice_explicit) : -1; }
int dm_fps_setting(void) { return dm_custom_get("eco_fps") ? 30 : 60; }   // 图形层 nvn_mtl.m mtl_fps_target 每帧读（弱符号的强定义） | Read every frame by mtl_fps_target in the graphics layer nvn_mtl.m (strong definition of a weak symbol)
static void save(void) {
    char p[1100], t[1120]; if (!file_path(p, sizeof p)) { LOG("【红】自定义设置：存档目录没挂上，没存盘"); return; }
    snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); int ok = f != NULL;
    for (int i = 0; ok && i < NK; i++) { if (i == 0 && (atomic_load(&g_lang_explicit) < 0 || g_lang_test)) continue; if (i == K_VOICE && (atomic_load(&g_voice_explicit) < 0 || g_voice_test)) continue; ok = fprintf(f, "%s=%d\n", k_keys[i], atomic_load(&g_val[i])) > 0; }   // lang / voice 只在玩家真改过之后才写进文件
    if (ok && atomic_load(&g_merc_explicit) >= 0) ok = fprintf(f, "merc_pot_v2=%d\n", atomic_load(&g_merc_explicit)) > 0;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(t, p) == 0;   // 先写临时文件再换名：写到一半被杀也不会留下半个文件 | Write a temporary file first and then rename: being killed halfway through the write does not leave half a file behind
    LOG("自定义设置：存盘%s", ok ? "" : "【红】失败");
}
void dm_custom_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)arg; if (!cat || !act || strcmp(cat, "OptionsPanelMessage")) return;
    load_once();
    if (!strcmp(act, "AcceptChanges")) { int ch = 0;
        for (int i = 0; i < NK; i++) { int pd = atomic_exchange(&g_pend[i], -1); if (pd >= 0 && pd != atomic_load(&g_val[i])) { atomic_store(&g_val[i], pd); ch = 1; if (i == K_CAM) dm_cam_set_mode(pd); if (i == K_Q3D) { int dm_3d_quality_mode_get(void); float dm_3d_scale_cur(void); dm_3d_quality_mode_set(pd); LOG("自定义设置：3D 画质 → %s（已交给 3D 比例控制，读回模式 %d，此刻 3D 比例 %.2f）", pd ? "画质优先" : "流畅优先", dm_3d_quality_mode_get(), dm_3d_scale_cur()); } if (i == 0) atomic_store(&g_lang_explicit, pd); if (i == K_VOICE) atomic_store(&g_voice_explicit, pd); if (i == K_MERC) atomic_store(&g_merc_explicit, pd); if (i == 0) LOG("自定义设置：语言 → %s（应用；下次启动生效）", k_lang_name[pd]); else if (i == K_VOICE) LOG("自定义设置：配音 → %s（应用；下次启动生效）", pd ? "English" : "中文"); else LOG("自定义设置：%s → %s（应用）", k_keys[i], pd ? "开" : "关"); } }
        if (ch) save(); }
    else if (!strcmp(act, "CancelChanges")) { for (int i = 0; i < NK; i++) atomic_store(&g_pend[i], -1); atomic_store(&g_repaint, 1); }
    else if (!strcmp(act, "ResetToDefaults")) { for (int i = 1 + VOICE_ROW; i < NK; i++) atomic_store(&g_pend[i], k_def[i]); atomic_store(&g_repaint, 1); }   // 从 1 起（有配音行时从 2 起）：语言与单独配音那一格不归「默认」管
}

// ---- 选项页上的勾选框
// ---- Checkboxes on the options page
static uint8_t *main_base(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static uint64_t find_w(uint64_t w, const char *name, int dep) {   // 按名字找控件：名字 +8、子控件数组 +0x58、个数 +0x60 | find a widget by name: name +8, child widget array +0x58, count +0x60
    if (!w || dep > 14) return 0; const char *nm = (const char *)*(uint64_t *)(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = find_w(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
// 勾选框的状态（本机逐字节差分，点一下只变这几处）：+0xa88 是它自己的「勾上」标志（点击按它翻转）；
// +0x6e8..+0x6f4 是「常态 / 按下 / 悬停 / 禁用」四个帧号，勾上 = 4 5 6 -1、没勾 = 0 1 3 2（= 布局里 toggled* / untoggled* 那几项）；+0x610 是此刻画的帧
// 读写这几处之前核控件是勾选框那个类（dm_widget.h）：不是就当没勾、不写，并记红
// Checkbox state (byte-by-byte diff on this machine; one tap changes only these places): +0xa88 is its own "checked" flag (a click toggles it);
// +0x6e8..+0x6f4 are the four frame ids "normal / pressed / hover / disabled", checked = 4 5 6 -1, unchecked = 0 1 3 2 (= the toggled* / untoggled* entries in the layout); +0x610 is the frame drawn right now
// Before reading or writing these, check the widget is of the checkbox class (dm_widget.h): if not, treat it as unchecked, do not write, and log a failure
DM_WIDGET_RED_DEF
#define TOGGLED(w) (dm_widget_is(main_base(), (w), DM_VT_TOGGLE) && *(uint8_t *)((w) + 0xa88) != 0)
static void set_toggle(uint64_t w, int on) {
    if (!DM_WIDGET_OK(main_base(), w, DM_VT_TOGGLE, "自定义页")) return;
    static const int32_t fr[2][4] = { { 0, 1, 3, 2 }, { 4, 5, 6, -1 } };
    *(uint8_t *)(w + 0xa88) = on != 0; memcpy((void *)(w + 0x6e8), fr[on != 0], sizeof fr[0]); *(int32_t *)(w + 0x610) = fr[on != 0][0];
}
// 多项单选行（语言五项、镜头视角三项）：几个勾选框互斥、存一个键 i（值 = 选中第几项）。页刚出现（布局里一律没勾）先摆成该显示的值、这一帧不当玩家点了；
// 之后点没选中的那一项 = 选它，点已选中的那一项不变（摆回勾上）。改了只记待定值，点「应用」才生效
// Multi-option radio rows (language: five, camera view: three): the checkboxes are mutually exclusive and store one key i (value = which option is selected).
// When the page has just appeared (everything unchecked in the layout), first set what should show and do not treat this frame as a tap; after that,
// tapping an unselected option = select it, tapping the selected one changes nothing (it is re-checked). A change only records the pending value; Apply makes it take effect
static void radio_pump(uint64_t sp, int i, const char *const *box, const char *const *name, int n, dm_wref *seen) {
    uint64_t bx[8]; int ok = n <= 8; for (int k = 0; ok && k < n; k++) { bx[k] = find_w(sp, box[k], 0); ok &= bx[k] && *(uint8_t *)(bx[k] + 0x50); }
    if (!ok) return;
    int pd0 = atomic_load(&g_pend[i]), shown = pd0 >= 0 ? pd0 : atomic_load(&g_val[i]), to = shown;
    if (dm_wref_get(seen) != bx[0]) dm_wref_set(seen, bx[0]);
    else for (int k = 0; k < n; k++) if (k != shown && TOGGLED(bx[k])) { to = k; break; }
    if (to != shown) { atomic_store(&g_pend[i], to); LOG("自定义设置：%s → %s（点「应用」生效）", k_keys[i], name[to]); }
    for (int k = 0; k < n; k++) if (TOGGLED(bx[k]) != (k == to)) set_toggle(bx[k], k == to);
}
void dm_custom_ui_pump(unsigned long long root) {
    if (!root || !main_base()) return;
    static dm_wref seen[NK];   // 每行最近见到的勾选框（带界面代号：重建后取到 0 = 页刚出现） | Most recently seen checkbox of each row (with UI generation: 0 after a rebuild = page just appeared)
    uint64_t sp = dm_ui_top(root, "VideoOptionsPanel"); if (!sp || !*(uint8_t *)(sp + 0x50)) {   // 「自定义」页 = 借来的视频页（界面根的直接子，与设置页并列）。页不在屏上：没点「应用」的勾选作废（同原版不应用就离开），下次出现重摆 | The 「自定义」(Custom) page = the borrowed video page (a direct child of the UI root, alongside the settings page). Page not on screen: checkbox changes without tapping 「应用」(Apply) are discarded (same as leaving without applying in the original game), and it is laid out again the next time it appears
        int was = 0; for (int i = 0; i < NK; i++) was |= seen[i].w != 0;
        if (was) for (int i = 0; i < NK; i++) atomic_store(&g_pend[i], -1);
        memset(seen, 0, sizeof seen); return; }
    load_once(); if (atomic_exchange(&g_repaint, 0)) memset(seen, 0, sizeof seen);
    if (atomic_load(&g_lang_explicit) < 0) atomic_store(&g_val[0], lang_running());   // 语言那一格没点过应用：生效值 = 此刻实际在跑的语言（读盘时游戏还没设语言，那时取到的不准） | Apply never tapped for the language cell: effective value = the language actually running right now (when read from disk the game has not set the language yet, so the value fetched then is not accurate)
#if VOICE_ROW
    if (atomic_load(&g_voice_explicit) < 0) atomic_store(&g_val[K_VOICE], voice_running());
#endif
    // 手柄模式：原版选项页的焦点只认表格里它自己的行，够不着我们的勾选框——十字键上下选行（原版手柄选中框 TouchCustomSel 框住这一行）、A 翻勾选（下面照「玩家点了」处理），其余键照旧归原版
    // Controller mode: the original options page's focus only recognizes its own rows in the table and cannot reach our checkboxes. D-pad up/down selects a row
    // (the original controller selection frame TouchCustomSel frames that row), A toggles the checkbox (handled below as "the player tapped it"), and all other
    // buttons go to the original as before
    { int dm_padmode_current(void); uint64_t dm_padmode_page_keys(uint64_t); static int sel; static dm_wref sel_sp; int pad = dm_padmode_current();
      { uint64_t cm = find_w(root, "ConfirmationModal", 1); if (cm && *(uint8_t *)(cm + 0x50)) pad = 0; }   // 原版「设定已更改，应用 / 放弃」确认框开着：键全还给它（实测：不让的话 A 落在我们这一行上、点不到「应用」） | The original "Settings changed, Apply / Discard" confirmation box is open: hand all keys back to it (measured: otherwise A lands on our row and "Apply" cannot be reached)
      if (dm_wref_get(&sel_sp) != sp) { dm_wref_set(&sel_sp, sp); sel = 0; }
      uint64_t dm = find_w(sp, "TouchCustomSel", 0), tw = 0, cw = 0; char nm[64];
      if (pad) { uint64_t ev = dm_padmode_page_keys(DM_BTN_UP | DM_BTN_DOWN | DM_BTN_A);
          if (ev & DM_BTN_DOWN) sel = sel + 1 < NK ? sel + 1 : sel; else if (ev & DM_BTN_UP) sel = sel > 0 ? sel - 1 : 0;
          snprintf(nm, sizeof nm, "TouchCustom_%s", k_keys[sel]); cw = find_w(sp, sel == 0 ? k_lang_box[LANG_N - 1] : nm, 0); snprintf(nm, sizeof nm, "TouchCustom_%s_Text", k_keys[sel]); tw = find_w(sp, nm, 0);   // 语言行：框住标签到最右那一格 | Language row: box from the label to the rightmost cell
          if ((ev & DM_BTN_A) && cw && dm_wref_get(&seen[sel]) == cw) {
              if (sel == K_VOICE || sel == K_Q3D || sel == K_TOUCH_SWAP) { int p0 = atomic_load(&g_pend[sel]), sh = p0 >= 0 ? p0 : atomic_load(&g_val[sel]); atomic_store(&g_pend[sel], !sh); set_toggle(cw, !sh); { uint64_t z = find_w(sp, rev_box(sel), 0); if (z) set_toggle(z, sh); }
                  LOG("自定义设置：%s → %d（手柄 A 在两项间切换；点「应用」生效）",k_keys[sel],!sh); }
              else if (sel == K_CAM) { int p0 = atomic_load(&g_pend[K_CAM]), sh = p0 >= 0 ? p0 : atomic_load(&g_val[K_CAM]), to = (sh + 1) % CAM_N; atomic_store(&g_pend[K_CAM], to);   // 镜头视角三项：A 依次轮换 | camera view: A cycles through the three
                  for (int k = 0; k < CAM_N; k++) { uint64_t z = find_w(sp, k_cam_box[k], 0); if (z) set_toggle(z, k == to); }
                  LOG("自定义设置：镜头视角 → %s（手柄 A 在三项间轮换；点「应用」生效）", k_cam_name[to]); }
              else if (sel == 0) { int p0 = atomic_load(&g_pend[0]), sh = p0 >= 0 ? p0 : atomic_load(&g_val[0]), to = (sh + 1) % LANG_N; atomic_store(&g_pend[0], to);   // 语言行是单选的五项：A 依次轮换，四个框当场摆好
                  for (int k = 0; k < LANG_N; k++) { uint64_t z = find_w(sp, k_lang_box[k], 0); if (z) set_toggle(z, k == to); }
                  LOG("自定义设置：语言 → %s（手柄 A 在五项间轮换；点「应用」生效）", k_lang_name[to]); }
              else set_toggle(cw, !TOGGLED(cw)); } }
      if (dm) { int32_t *r = (int32_t *)(dm + 0x70), want[4] = { 99999, 0, r[2], r[3] };   /* 不用时只把 x 挪出屏：它在滚动区里，y 放远了会撑大可滚范围 | when unused only x is moved off screen: it is inside the scroll area, and a far-away y would enlarge the scrollable range */
          if (dm_padmode_current() && tw && cw) { int32_t *t = (int32_t *)(tw + 0x70), *c = (int32_t *)(cw + 0x70); int top = t[1] < c[1] ? t[1] : c[1], bot = t[1] + t[3] > c[1] + c[3] ? t[1] + t[3] : c[1] + c[3];   // 框住整行：文字与勾选框两者的上下沿取并，再各放 10 单位（原先只按文字框的高度算，文字框比字矮，框成了横穿文字中线的一条） | Frame the whole row: take the union of the top and bottom edges of the text and the checkbox, then pad 10 units on each side (previously computed from the text box height only; the text box is shorter than the glyphs, so the frame became a strip cutting across the middle of the text)
              want[0] = t[0] - 30; want[1] = top - 10; want[2] = c[0] + c[2] + 30 - want[0]; want[3] = bot - top + 20;
              { static int lg; if (lg++ < 2) LOG("自定义设置：手柄选中框 文字框 %d,%d %dx%d 勾选框 %d,%d %dx%d → 框 %d,%d %dx%d", t[0], t[1], t[2], t[3], c[0], c[1], c[2], c[3], want[0], want[1], want[2], want[3]); } }
          if (memcmp(r, want, sizeof want)) { memcpy(r, want, sizeof want); ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)dm + 0x10))(dm); } }
      // 这页可以滚（各行在原版滚动区里，touch_layout.py _custom_page）：手柄选到列表框外的行时，让原版滚动区自己滚到位——发滚轮，每 4 帧一格，直到这一行整行在框内。
      // 已滚的量 = 内层视图（行容器的父控件）矩形 y 的相反数；框高 = 滚动区（再上一级）的高。只读这两个数，不写原版控件
      // This page can scroll (the rows are in the original scroll area, touch_layout.py _custom_page): when the controller selects a row outside the list box, let the original scroll area scroll into place by itself -- send wheel events, one notch every 4 frames, until the whole row is inside the box.
      // Amount scrolled = the negative of the rect y of the inner view (the parent widget of the row container); box height = the height of the scroll area (one level further up). Only these two numbers are read; the original game's widgets are not written
      if (pad && tw && cw) { uint64_t rw = find_w(sp, "TouchCustomRows", 0), view = rw ? *(uint64_t *)(rw + 0x30) : 0, sv = view ? *(uint64_t *)(view + 0x30) : 0; static uint64_t lf; uint64_t dm_guest_frame(void); uint64_t f = dm_guest_frame();
          if (sv && f - lf >= 4) { int32_t *t = (int32_t *)(tw + 0x70), *c = (int32_t *)(cw + 0x70); int top = (t[1] < c[1] ? t[1] : c[1]) - 10, bot = (t[1] + t[3] > c[1] + c[3] ? t[1] + t[3] : c[1] + c[3]) + 10;
              int off = -*(int32_t *)(view + 0x74), vh = *(int32_t *)(sv + 0x7c); if (vh <= 0) vh = 1260; void dm_ui_wheel_on(uint64_t, int); if (sel == 0) top = 0;   /* 选回第一行：一直滚到最顶，把标题行露出来 | selection back on the first row: scroll all the way to the top so that the header row is revealed */
              if (top < off) { lf = f; dm_ui_wheel_on(sv, 1); } else if (bot > off + vh) { lf = f; dm_ui_wheel_on(sv, -1); } } } }
    radio_pump(sp, 0, k_lang_box, k_lang_name, LANG_N, &seen[0]);   // 语言行：五项单选，存一个键 lang | language row: five-option radio, one key lang
    radio_pump(sp, K_CAM, k_cam_box, k_cam_name, CAM_N, &seen[K_CAM]);   // 镜头视角：三项单选，存一个键 cam | camera view: three-option radio, one key cam
    for (int i = 1; i < NK; i++) { if (i == K_CAM) continue; char nm[48]; snprintf(nm, sizeof nm, "TouchCustom_%s", k_keys[i]);   // 镜头行是三项单选，在 radio_pump 里 | the camera row is a three-option radio, handled in radio_pump
        uint64_t w = find_w(sp, nm, 0); if (!w || !*(uint8_t *)(w + 0x50)) continue;
        int pd0 = atomic_load(&g_pend[i]), want = pd0 >= 0 ? pd0 : atomic_load(&g_val[i]);
        uint64_t z = rev_box(i) ? find_w(sp, rev_box(i), 0) : 0;   // 两项单选行（3D 画质：上下的「流畅优先」「画质优先」；换组位置）：存的仍是一个键，反面那一项是它的反面 | Two-option radio rows (3D quality: "Smoothness first" / "Quality first" stacked; swap-group position): still stores one key, the other option is its negation
        if (dm_wref_get(&seen[i]) != w) { dm_wref_set(&seen[i], w); set_toggle(w, want); if (z) set_toggle(z, !want); continue; }   // 页刚出现（布局里一律是没勾）：先摆成该显示的值，这一帧不当玩家点了 | The page has just appeared (in the layout everything is unchecked): first set it to the value it should show, and do not treat this frame as a player tap
        int on = TOGGLED(w), pd = atomic_load(&g_pend[i]), shown = pd >= 0 ? pd : atomic_load(&g_val[i]);
        if (z) {   // 点没选中的那一项 = 选它；点已选中的那一项不变（摆回勾上） | Tapping the item that is not selected = select it; tapping the one already selected changes nothing (it is put back to checked)
            int zon = TOGGLED(z), to = on != shown ? (on ? 1 : shown) : zon != !shown ? (zon ? 0 : shown) : shown;
            if (to != shown) { atomic_store(&g_pend[i], to); LOG("自定义设置：%s → %d（点「应用」生效）",k_keys[i],to); }
            if (TOGGLED(w) != to) set_toggle(w, to); if (TOGGLED(z) != !to) set_toggle(z, !to); continue; }
        if (on != shown) { atomic_store(&g_pend[i], on); LOG("自定义设置：%s 勾选框 → %s（点「应用」生效）", k_keys[i], on ? "勾上" : "取消"); } }
    // 语言那一行下面的「点应用后把 App 关掉重开」提示（TouchCustomLangHint，布局里先放在屏外）：勾选框此刻显示的值与正在跑的语言不一致时挪到标签正下方（横坐标取标签的）
    // The "tap Apply, then close and reopen the app" hint under the language row (TouchCustomLangHint, placed off screen in the layout): moved right under the label (taking the label's x) when the value the checkboxes currently show differs from the language now running
    { uint64_t lw = find_w(sp, "TouchCustom_lang_Text", 0); int pd = atomic_load(&g_pend[0]), shown = pd >= 0 ? pd : atomic_load(&g_val[0]), diff = shown != lang_running();
      for (int k = 0; k < LANG_N && lw; k++) { uint64_t hw = find_w(sp, k_lang_hint[k], 0); if (!hw) continue;   // 提示用「选中的那种语言」写：选了哪种语言就显示哪种语言的那句 | Write the hint in "the selected language": whichever language is selected, show that language's sentence
          int x = diff && shown == k ? ((int32_t *)(lw + 0x70))[0] : 99999; int32_t *r = (int32_t *)(hw + 0x70);
          if (r[0] != x) { r[0] = x; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)hw + 0x10))(hw); } } }
    // 「应用 / 放弃」亮不亮（自定义页改了勾选，「应用」仍是灰的）：原版各选项子页的每帧更新 main+0xae5f28 把自己 +0x1a0「有未应用的改动」
    // 交给 main+0xaed9e4(设置页, 亮否)，按钮 +0x57a 是灰的标志；原版的勾选框一改就由面板置 +0x1a0，我们的勾选框原版面板不认识，这一位一直是 0。
    // 按待定值与生效值比（改回原样 = 没改动 = 灭）写这一位，走原版自己的亮灭与「有改动离开要确认」。只在面板虚表里确有 0xae5f28 时写（确认 +0x1a0 是这个意思）
    // Whether 「应用 / 放弃」(Apply / Discard) light up (a checkbox on the custom page was changed, yet 「应用」(Apply) is still gray): the per-frame update main+0xae5f28 of each original-game option subpage passes its own +0x1a0 'has unapplied changes'
    // to main+0xaed9e4(settings page, lit or not), and button +0x57a is the gray flag; an original-game checkbox has +0x1a0 set by the panel as soon as it changes, but the original panel does not know our checkboxes, so this bit stays 0.
    // Write this bit by comparing pending values with effective values (changed back to the original = no change = off), using the original game's own lit / unlit behavior and its 'confirm when leaving with changes'. Written only when the panel vtable really contains 0xae5f28 (confirming that +0x1a0 has this meaning)
    static uint64_t vt_ok_for; static int vt_ok; if (vt_ok_for != *(uint64_t *)sp) { vt_ok_for = *(uint64_t *)sp; vt_ok = 0;
        for (int k = 0; k < 96 && !vt_ok; k++) vt_ok = *(uint64_t *)(vt_ok_for + 8 * k) == (uint64_t)main_base() + 0xae5f28;
        if (!vt_ok) LOG("【红】自定义设置：借来的视频页虚表里没有 main+0xae5f28，不写「有改动」位（原版版本变了？）"); }
    if (vt_ok) { int dirty = 0; for (int i = 0; i < NK; i++) { int pd = atomic_load(&g_pend[i]); dirty |= pd >= 0 && pd != atomic_load(&g_val[i]); }
        if (*(uint8_t *)(sp + 0x1a0) != dirty) { *(uint8_t *)(sp + 0x1a0) = (uint8_t)dirty; LOG("自定义设置：有未应用的改动 → %d（「应用 / 放弃」%s）", dirty, dirty ? "点亮" : "变灰"); } }
}
