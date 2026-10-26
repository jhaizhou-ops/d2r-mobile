// 画面设置：Mac 选项页顶上「显示」「图形」两节（布局 touch_layout.py _VIDEO），存在自定义设置旁的 sw_video.txt。
// 三个值：mode（0 窗口 / 1 全屏）、res（出图宽档 0 = 1600 / 1 = 1920 / 2 = 2560）、up（0 标准拉伸 / 1 MetalFX），另存一份「推荐」rec_*。
// 首次启动（文件里没有 rec_*）按这台 Mac 定推荐值并当作当前值（前端 front_video_firstboot：屏幕像素宽、显卡核数、MetalFX 支持；旧版偏好 StartFullScreen /
// RenderScale / MetalFXUpscale 设过的接着用）。「图形预设」是推导出来的：三项都等于推荐 = 推荐；分辨率 + 放大方式等于性能档 / 品质档 = 那一档；都不是 = 自定义。
// 页上照原版选项页的语义：切换框改的是待定值，「应用」才生效并存盘（显示模式当场切、放大方式下一帧起、分辨率下次启动——行下提示「重启游戏后生效」），
// 「放弃」丢掉待定值，「默认」待定值回推荐值。「有改动」位与自定义页合在一起（dm_custom.c 按 dm_video_dirty 一并点亮「应用」）。
// 切换框（CarouselWidget，宿主实读 3.3.94012）：选项个数 +0x7e8、当前项 +0x7ec、每项可选与否 +0x7d8 起每项一字节（0 = 跳过）；
// 设当前项走原版 main+0xab0c84(控件, 序号)（按个数钳、跳过不可选的项、按当前语言换上这一项的字）。读写前核虚表与选项个数。
// Picture settings: the "Display" and "Graphics" sections at the top of the Mac options page (layout touch_layout.py _VIDEO), stored in sw_video.txt next to the custom settings.
// Three values: mode (0 windowed / 1 full screen), res (render width tier 0 = 1600 / 1 = 1920 / 2 = 2560), up (0 standard stretch / 1 MetalFX), plus a stored "recommended" copy rec_*.
// On first boot (no rec_* in the file) the recommendation is worked out for this Mac and becomes the current value (front end front_video_firstboot: screen pixel width, GPU cores, MetalFX support; values from the
// older preferences StartFullScreen / RenderScale / MetalFXUpscale carry over when set). "Graphic Presets" is derived: all three equal to the recommendation = Recommended; resolution + upscaling equal to the
// Performance / Quality tier = that tier; otherwise = Custom.
// On the page it follows the original options page: a selector changes a pending value, only "Apply" makes it take effect and saves (display mode switches at once, upscaling from the next frame, resolution at the next
// launch -- with "Takes effect after restarting the game" under the row), "Discard" drops pending values, "Default" sets pending values back to the recommendation. The "has changes" bit is shared with the custom page
// (dm_custom.c lights "Apply" from dm_video_dirty too).
// Selector (CarouselWidget, read on the host, 3.3.94012): option count +0x7e8, current option +0x7ec, one byte per option from +0x7d8 saying whether it can be chosen (0 = skipped);
// the current option is set through the game's main+0xab0c84(widget, index) (clamps to the count, skips options that cannot be chosen, puts that option's text in the current language). The vtable and option count are checked before reading or writing.
// 手机（DM_PHONE，iOS 覆盖层 DM_OVERLAY_IOS）：页上只有「图形」一节——res 是 3D 清晰度（0.75 / 0.85 / 1.0，交给 hle_oe.c 的 3D 比例，点应用当场换），
// up 是游戏 3D 放大趟用不用 MetalFX（nvn_mtl.m mfx3d_draw），没有显示模式那一行；预设：推荐 = 首次启动按机型定，性能 = 75% 标准（最省电），品质 = 100% + MetalFX。
// 页上有哪几行就管哪几行（dm_video_rows 按实际找到的切换框排）
// Phones (DM_PHONE, iOS overlay DM_OVERLAY_IOS): the page only has the "Graphics" section -- res is the 3D resolution (0.75 / 0.85 / 1.0, handed to the 3D scale in hle_oe.c, switching as soon as Apply is pressed),
// up is whether the game's 3D upscale pass uses MetalFX (nvn_mtl.m mfx3d_draw), and there is no display mode row; presets: Recommended = chosen per device on first boot, Performance = 75% Standard (lightest on the battery), Quality = 100% + MetalFX.
// Only the rows the page actually has are handled (dm_video_rows lists the selectors it finds)
#include "hle.h"
#include "dm.h"
#include "dm_cursor.h"
#include "dm_widget.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t dm_custom_find_w(uint64_t w, const char *name);
int dm_custom_path(const char *name, char *p, size_t n);
enum { K_MODE, K_RES, K_UP, NV };
static const char *const k_keys[NV] = { "mode", "res", "up" };
static const int k_n[NV] = { 2, 3, 2 };
static const char *const k_rows[4] = { "mode", "res", "preset", "upscale" };   // 页上的行序 | row order on the page
static const int k_row_n[4] = { 2, 3, 4, 2 };
static const int k_row_key[4] = { K_MODE, K_RES, -1, K_UP };   // -1 = 图形预设（推导） | -1 = graphic presets (derived)
enum { P_REC, P_PERF, P_QUAL, P_CUSTOM };
static _Atomic int g_val[NV], g_pend[NV] = { -1, -1, -1 }, g_rec[NV], g_loaded, g_repaint;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // 读盘 / 存盘：前端主线程（启动、全屏切换）与游戏线程（应用）都会写 | load / save: written by both the front end's main thread (launch, full-screen changes) and the game thread (Apply)

// 前端提供（Mac：front_mac.m）；别的平台没有这一节，用这里的弱定义 | Provided by the front end (Mac: front_mac.m); other platforms have no such section and use the weak definitions here
__attribute__((weak)) int front_video_firstboot(int rec[3], int cur[3]) { (void)rec; (void)cur; return 0; }
__attribute__((weak)) void front_video_fullscreen(int on) { (void)on; }
__attribute__((weak)) int front_video_running_res(void) { return -1; }
__attribute__((weak)) int front_video_mfx_ok(void) { return 0; }

static void save_locked(void) {
    char p[1100], t[1120]; if (!dm_custom_path("sw_video.txt", p, sizeof p)) { LOG("【红】画面设置：设置目录还不知道，没存盘"); return; }
    snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); int ok = f != NULL;
    for (int i = 0; ok && i < NV; i++) ok = fprintf(f, "%s=%d\nrec_%s=%d\n", k_keys[i], atomic_load(&g_val[i]), k_keys[i], atomic_load(&g_rec[i])) > 0;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(t, p) == 0;   // 先写临时文件再换名（同 sw_custom.txt） | write a temporary file, then rename (as sw_custom.txt)
    LOG("画面设置：存盘%s mode=%d res=%d up=%d（推荐 %d/%d/%d）", ok ? "" : "【红】失败", g_val[K_MODE], g_val[K_RES], g_val[K_UP], g_rec[K_MODE], g_rec[K_RES], g_rec[K_UP]);
}
static void load_once(void) {
#ifndef __APPLE__
    return;   // 安卓 / 鸿蒙的覆盖层没有这几行：不读不写 sw_video.txt，取值全为 0，3D 比例照旧按各自的缺省 | the Android / HarmonyOS overlays have none of these rows: sw_video.txt is never read or written, every value reads 0, and the 3D scale keeps each platform's own default
#endif
    if (atomic_load(&g_loaded)) return;
    pthread_mutex_lock(&g_mu);
    char p[1100];
    if (!atomic_load(&g_loaded) && dm_custom_path("sw_video.txt", p, sizeof p)) {
        int v[NV] = { 0, 0, 0 }, r[NV] = { -1, -1, -1 }; FILE *f = fopen(p, "r"); char ln[64];
        while (f && fgets(ln, sizeof ln, f)) { char *eq = strchr(ln, '='); if (!eq) continue; *eq = 0; int x = atoi(eq + 1), rec = !strncmp(ln, "rec_", 4);
            for (int i = 0; i < NV; i++) if (!strcmp(rec ? ln + 4 : ln, k_keys[i]) && x >= 0 && x < k_n[i]) { if (rec) r[i] = x; else v[i] = x; } }
        if (f) fclose(f);
        int first = r[0] < 0 || r[1] < 0 || r[2] < 0;
        if (first) {   // 首次启动：定推荐值（前端没有 = 原版：窗口、1600、标准） | first boot: work out the recommendation (no front end = the original: windowed, 1600, standard)
            int rec[3] = { 0, 0, 0 }, cur[3] = { 0, 0, 0 };
            if (!front_video_firstboot(rec, cur)) memcpy(cur, rec, sizeof cur);
            for (int i = 0; i < NV; i++) { r[i] = rec[i] >= 0 && rec[i] < k_n[i] ? rec[i] : 0; v[i] = cur[i] >= 0 && cur[i] < k_n[i] ? cur[i] : r[i]; }
        }
        for (int i = 0; i < NV; i++) { atomic_store(&g_val[i], v[i]); atomic_store(&g_rec[i], r[i]); }
        atomic_store(&g_loaded, 1);
        LOG("画面设置：%s %s mode=%d res=%d up=%d，推荐 %d/%d/%d", first ? "首次启动定推荐值" : "读盘", p, v[0], v[1], v[2], r[0], r[1], r[2]);
        if (first) save_locked();
    }
    pthread_mutex_unlock(&g_mu);
}
int dm_video_get(const char *key) {   // 生效值；设置目录还不知道时答原版值 | the value in effect; the original value while the settings directory is unknown
    load_once(); for (int i = 0; i < NV; i++) if (key && !strcmp(key, k_keys[i])) return atomic_load(&g_loaded) ? atomic_load(&g_val[i]) : 0;
    return 0;
}
int dm_video_upscale(void) { return dm_video_get("up"); }
float dm_video_scale3d(void) {   // 手机：3D 清晰度（hle_oe.c s3_cfg 每拍读；> 0 才算数）；别的平台 0 = 不管 | phone: the 3D resolution (read every tick by s3_cfg in hle_oe.c; only > 0 counts); other platforms 0 = not ours
    static const float k[3] = { 0.75f, 0.85f, 1.0f };
    if (!DM_PHONE) return 0; load_once(); if (!atomic_load(&g_loaded)) return 0;
    int t = atomic_load(&g_val[K_RES]); return t >= 0 && t < 3 ? k[t] : 0;
}   // 出图每帧读（nvn_mtl.m mfx_on） | read by the present every frame (nvn_mtl.m mfx_on)
void dm_video_fullscreen_note(int on) {   // 玩家用菜单 / ⌃⌘F 切了全屏：设置跟着窗口走（同电脑版「显示模式」） | the player switched full screen from the menu / ⌃⌘F: the setting follows the window (as the PC "Display Mode")
    load_once(); if (!atomic_load(&g_loaded) || atomic_load(&g_val[K_MODE]) == (on != 0)) return;
    pthread_mutex_lock(&g_mu); atomic_store(&g_val[K_MODE], on != 0); save_locked(); pthread_mutex_unlock(&g_mu);
    atomic_store(&g_repaint, 1);
}
static int shown(int k) { int p = atomic_load(&g_pend[k]); return p >= 0 ? p : atomic_load(&g_val[k]); }
static int perf_up(void) { return DM_PHONE ? 0 : front_video_mfx_ok() ? 1 : 0; }   // 性能档的放大方式：手机 = 标准（省电），Mac = MetalFX（它很便宜） | Performance's upscaling: phone = Standard (saves power), Mac = MetalFX (cheap there)
static int preset_of(void) {
    int m = shown(K_MODE), r = shown(K_RES), u = shown(K_UP), pu = front_video_mfx_ok() ? 1 : 0;
    if (m == g_rec[K_MODE] && r == g_rec[K_RES] && u == g_rec[K_UP]) return P_REC;
    if (r == 0 && u == perf_up()) return P_PERF;
    if (r == 2 && u == pu) return P_QUAL;
    return P_CUSTOM;
}
static void apply_preset(int p) {   // 选了某档：改待定值（推荐 = 三项全回推荐；性能 / 品质只管分辨率与放大方式） | a preset was chosen: change pending values (Recommended = all three back to the recommendation; Performance / Quality set only resolution and upscaling)
    int pu = front_video_mfx_ok() ? 1 : 0;
    if (p == P_REC) for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], atomic_load(&g_rec[i]));
    else if (p == P_PERF) { atomic_store(&g_pend[K_RES], 0); atomic_store(&g_pend[K_UP], perf_up()); }
    else if (p == P_QUAL) { atomic_store(&g_pend[K_RES], 2); atomic_store(&g_pend[K_UP], pu); }
}
int dm_video_dirty(void) { int d = 0; for (int i = 0; i < NV; i++) { int p = atomic_load(&g_pend[i]); d |= p >= 0 && p != atomic_load(&g_val[i]); } return d; }
void dm_video_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)arg; if (!cat || !act || strcmp(cat, "OptionsPanelMessage")) return;
    load_once(); if (!atomic_load(&g_loaded)) return;
    if (!strcmp(act, "AcceptChanges")) { int ch = 0, mode = -1;
        for (int i = 0; i < NV; i++) { int p = atomic_exchange(&g_pend[i], -1); if (p >= 0 && p != atomic_load(&g_val[i])) { atomic_store(&g_val[i], p); ch = 1; if (i == K_MODE) mode = p; } }
        if (ch) { pthread_mutex_lock(&g_mu); save_locked(); pthread_mutex_unlock(&g_mu); }
        if (mode >= 0) front_video_fullscreen(mode);   // 显示模式当场切 | display mode switches at once
        atomic_store(&g_repaint, 1); }
    else if (!strcmp(act, "CancelChanges")) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], -1); atomic_store(&g_repaint, 1); }
    else if (!strcmp(act, "ResetToDefaults")) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], atomic_load(&g_rec[i])); atomic_store(&g_repaint, 1); LOG("画面设置：默认 → 推荐值（点「应用」生效）"); }
}

// ---- 页上的切换框
// ---- Selectors on the page
static uint8_t *main_base(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
DM_WIDGET_RED_DEF
static uint64_t row_w(uint64_t sp, int i) { char nm[48]; snprintf(nm, sizeof nm, "TouchVideo_%s", k_rows[i]); return dm_custom_find_w(sp, nm); }
static int car_ok(uint64_t w, int n) {
    if (!w || !*(uint8_t *)(w + 0x50) || !DM_WIDGET_OK(main_base(), w, DM_VT_CAROUSEL, "画面设置")) return 0;
    if (*(int32_t *)(w + 0x7e8) != n) { static int k; if (k++ < 3) LOG("【红】画面设置：切换框「%s」选项个数 %d，应为 %d，不读写", *(const char **)(w + 8), *(int32_t *)(w + 0x7e8), n); return 0; }
    return 1;
}
static int car_get(uint64_t w) { return *(int32_t *)(w + 0x7ec); }
static void car_set(uint64_t w, int i) { if (car_get(w) != i) ((void (*)(uint64_t, int))(main_base() + 0xab0c84))(w, i); }
static void preset_enable(uint64_t w, int custom) { *(uint8_t *)(w + 0x7d8 + P_CUSTOM) = custom != 0; }   // 「自定义」只在不对应任何一档时可选（箭头跳过它） | "Custom" can only be chosen when no preset matches (the arrows skip it)
static int present_rows(uint64_t sp, int idx[4]) {   // 页上实际有的行（按页上的顺序）：Mac 四行，手机没有显示模式 | the rows the page really has (in page order): four on the Mac, no display mode on phones
    int n = 0; for (int i = 0; i < 4 && sp; i++) { uint64_t w = row_w(sp, i); if (w && *(uint8_t *)(w + 0x50)) idx[n++] = i; } return n;
}
int dm_video_rows(uint64_t sp) { int idx[4]; return present_rows(sp, idx); }
void dm_video_row(uint64_t sp, int i, uint64_t *tw, uint64_t *cw) {   // 手柄选中框：第 i 个画面设置行的标签与切换框 | controller selection frame: label and selector of the i-th picture row
    int idx[4], n = present_rows(sp, idx); *tw = *cw = 0; if (i < 0 || i >= n) return;
    char nm[48]; snprintf(nm, sizeof nm, "TouchVideo_%s_Text", k_rows[idx[i]]); *tw = dm_custom_find_w(sp, nm); *cw = row_w(sp, idx[i]);
}
void dm_video_pad(uint64_t sp, int r, int dir) {   // 手柄：左右 = 上一项 / 下一项，A = 下一项（到头回第一项）；之后 dm_video_pump 当玩家改了处理 | controller: left / right = previous / next option, A = next (wrapping to the first); dm_video_pump then handles it as a player change
    int idx[4], nr = present_rows(sp, idx); if (r < 0 || r >= nr) return; int i = idx[r];
    uint64_t w = row_w(sp, i); if (!car_ok(w, k_row_n[i])) return;
    int n = k_row_n[i], cur = car_get(w), to = cur;
    for (int k = 0; k < n; k++) { to = dir == 2 ? (to + 1) % n : to + dir; if (to < 0 || to >= n) { to = cur; break; } if (*(uint8_t *)(w + 0x7d8 + to)) break; }
    car_set(w, to);
}
static void hint(uint64_t sp) {   // 分辨率改了还没重启：「重启游戏后生效」挪到标签下面（横坐标取标签的；同语言行的提示） | resolution changed but not restarted yet: move "Takes effect after restarting the game" under the label (x from the label; as the language row's hint)
    uint64_t hw = dm_custom_find_w(sp, "TouchVideoResHint"), lw = dm_custom_find_w(sp, "TouchVideo_res_Text"); if (!hw || !lw) return;
    int run = front_video_running_res(), x = run >= 0 && shown(K_RES) != run ? ((int32_t *)(lw + 0x70))[0] : 99999; int32_t *r = (int32_t *)(hw + 0x70);
    if (r[0] != x) { r[0] = x; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)hw + 0x10))(hw); }
}
void dm_video_pump(uint64_t sp) {   // 自定义页在屏上时每帧调（dm_custom.c）；sp = 0：页不在屏上 | called every frame while the custom page is on screen (dm_custom.c); sp = 0: the page is not on screen
    static dm_wref seen[4];
    uint64_t w[4] = { 0, 0, 0, 0 }; int ok = sp != 0 && dm_video_rows(sp) > 0;
    for (int i = 0; ok && i < 4; i++) { w[i] = row_w(sp, i); if (w[i] && !*(uint8_t *)(w[i] + 0x50)) w[i] = 0; }   // 页上没有的行 = 0，下面跳过 | rows the page lacks = 0, skipped below
    if (!ok) { int was = 0; for (int i = 0; i < 4; i++) was |= seen[i].w != 0;   // 页没了：没点「应用」的改动作废（同自定义页） | the page is gone: changes without "Apply" are dropped (as the custom page)
        if (was) for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], -1);
        memset(seen, 0, sizeof seen); return; }
    for (int i = 0; i < 4; i++) if (w[i] && !car_ok(w[i], k_row_n[i])) return;
    load_once(); if (!atomic_load(&g_loaded)) return;
    if (atomic_exchange(&g_repaint, 0)) memset(seen, 0, sizeof seen);
    int want[4], ch = -1;   // 先按改之前的值算各行该显示什么，再找玩家动了哪一行（一帧只认一行）：原先边改边比，预设刚改的待定值被后面几行当成玩家又改回去了
                            // first work out what each row should show from the values before any change, then find the row the player moved (one per frame): comparing while changing used to let the later rows read a preset's fresh pending values as the player changing them back
    for (int i = 0; i < 4; i++) want[i] = k_row_key[i] < 0 ? preset_of() : shown(k_row_key[i]);
    for (int i = 0; i < 4; i++) {
        if (!w[i]) continue;
        if (dm_wref_get(&seen[i]) != w[i]) { dm_wref_set(&seen[i], w[i]); if (k_row_key[i] < 0) preset_enable(w[i], want[i] == P_CUSTOM); car_set(w[i], want[i]); continue; }   // 页刚出现：摆成该显示的值，这一帧不当玩家改了 | the page just appeared: show the value it should, and do not treat this frame as a player change
        if (ch < 0 && car_get(w[i]) != want[i]) ch = i;
    }
    if (ch >= 0) {
        int k = k_row_key[ch], cur = car_get(w[ch]);
        if (k < 0) { if (cur != P_CUSTOM) apply_preset(cur); LOG("画面设置：图形预设 → %d（点「应用」生效）", cur); }
        else { atomic_store(&g_pend[k], cur); LOG("画面设置：%s → %d（点「应用」生效）", k_keys[k], cur); }
        for (int i = 0; i < 4; i++) { if (!w[i]) continue; int kk = k_row_key[i], v = kk < 0 ? preset_of() : shown(kk); if (kk < 0) preset_enable(w[i], v == P_CUSTOM); car_set(w[i], v); }   // 预设带动其余几项、其余几项带动预设 | presets move the other rows, and the other rows move the preset
    }
    hint(sp);
}
