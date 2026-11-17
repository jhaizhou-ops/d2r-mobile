// 鸿蒙触控（iOS front_ios.m DmMetalView 的逐段搬运，见 touch_ohos.c）。全部入口在 ArkTS UI 线程调，界面消息除外（游戏线程）。
// HarmonyOS touch (a section-by-section port of iOS front_ios.m DmMetalView, see touch_ohos.c). All entries are called on the ArkTS UI thread, except UI messages (game thread).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void dm_ohos_touch_uimsg(const char *cat, const char *act, const char *arg);   // 游戏线程 | game thread
void dm_ohos_touch_screen(float w, float h, float safe_l, float safe_r, float safe_b);   // 整个窗口（vp）与安全区 | whole window (vp) and safe area
void dm_ohos_touch_event(int phase, int id, float x, float y);   // 0 按下 1 移动 2 抬起 3 取消；窗口坐标（vp） | 0 down 1 move 2 up 3 cancel; window coordinates (vp)
void dm_ohos_touch_focus(int active);
void dm_ohos_touch_pad_mode(int active);
// 每帧调（同 iOS CADisplayLink tick）：跑手势层计时、刷新绑定快照；out 写绘图快照，返回 1 = 要重画
// Called every frame (as the iOS CADisplayLink tick): runs gesture timers and refreshes the binding snapshot; writes the drawing snapshot to out, returns 1 = redraw
int dm_ohos_touch_tick(float *out, int cap);
void front_ohos_view_rect(float fw, float fh, float out[4]);
int front_ohos_full(void);

// 绘图快照布局（ArkTS IosOverlay.ets 同一份） | Drawing snapshot layout (same in ArkTS IosOverlay.ets)
enum {
    DM_OS_VISIBLE, DM_OS_VX, DM_OS_VY, DM_OS_VW, DM_OS_VH, DM_OS_NAV, DM_OS_SNAV, DM_OS_DIALOG, DM_OS_BLOCKED, DM_OS_SET,
    DM_OS_MAPCANCEL, DM_OS_DIFFOPEN, DM_OS_PLUSHELD, DM_OS_BARVIS, DM_OS_DEAD, DM_OS_JOYACT, DM_OS_JOYX, DM_OS_JOYY, DM_OS_JOYR,
    DM_OS_LX, DM_OS_LY, DM_OS_SWX, DM_OS_SWY, DM_OS_SWR, DM_OS_SWHELD, DM_OS_COVER, DM_OS_HEADER = 32,
    DM_OS_TARGETS = DM_OS_HEADER, DM_OS_TSTRIDE = 6,                 // 12 × [x, y, r, enabled, held, button 低 16 位] | 12 × [x, y, r, enabled, held, low 16 bits of button]
    DM_OS_GLYPHS = DM_OS_TARGETS + 12 * DM_OS_TSTRIDE, DM_OS_GSTRIDE = 8,   // 6 × [状态, 图集, 帧 x, y, w, h, 副手组标记, 次数]（0–4 技能钮，5 回城） | 6 × [status, atlas, frame x, y, w, h, swap-skill kind, charges] (0–4 skills, 5 portal)
    DM_OS_HUD = DM_OS_GLYPHS + 6 * DM_OS_GSTRIDE,                    // [valid, hp, hp_max, mp, mp_max, level, exp‰, points, quest_unread, belt×4, players, dead]
    DM_OS_FLOATS = DM_OS_HUD + 16
};
#ifdef __cplusplus
}
#endif
