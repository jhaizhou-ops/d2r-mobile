#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* UI线程仅排队；step必须挂在dm_pad_hook游戏采样线程。 | UI only queues; step must run on the guest sampling thread through dm_pad_hook. */
/* 窗口尺寸（dp，整屏）：安全区也按整屏给；控制器与自绘 HUD 摆在「游戏视图矩形」里，见 dm_android_touch_view。
   Window size (dp, the whole screen); the safe area is given for the whole screen too. Controls and the custom HUD are laid out
   inside the game view rectangle, see dm_android_touch_view. */
void dm_android_touch_resize(float width_dp, float height_dp, float left_dp, float right_dp, float bottom_dp);
/* 游戏视图矩形（dp，窗口坐标系；front_android_view_rect 除以 density）：界面、自绘 HUD / 控件、界面触点都在它里面。
   真全屏局内 = 贴底的 16:9，局外 = 居中的 16:9，没开真全屏 = 整屏。窗口尺寸变化时先 resize 再 view。
   Game view rectangle (dp, window coordinates; front_android_view_rect divided by the density): the UI, the custom-drawn HUD /
   controls and UI touches live inside it. True fullscreen in a run = the bottom-anchored 16:9, out of a run = the centered 16:9,
   without true fullscreen = the whole screen. On a window size change call resize before view. */
void dm_android_touch_view(float x_dp, float y_dp, float width_dp, float height_dp);
void dm_android_touch_tablet(int tablet);
/* phase: 0 down, 1 move, 2 up, 3 cancel; slot: stable 0..15; coordinates in dp. | 阶段：按下/移动/抬起/取消；槽固定0..15；坐标dp。 */
void dm_android_touch_event(int phase, int slot, float x, float y);
void dm_android_touch_focus(int active);
void dm_android_touch_uimsg(const char *cat, const char *act, const char *arg);
void dm_android_touch_step(uint64_t guest_now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
/* 绘图快照：header[width,height,navigation,skill_set]，23个[x,y,r,enabled,pressed]；12键、摇杆、换组、难度及8选择格。 | Drawing snapshot: header followed by 23 records (12 keys, stick, set switch, difficulty, 8 choices). */
/* 后接六个技能图标记录（状态、图集、裁切矩形、副手/充能、次数）和18个HUD数值（16个底栏数值，再加画摇杆头用的摇杆推量 x、y）。
   Append six icon records (status, atlas, crop, secondary/charge flags, charges) and 18 HUD values (16 bottom-bar values, then the stick deflection x, y
   the thumb is drawn from). */
enum { DM_ANDROID_TOUCH_GLYPHS = 4 + 23 * 5, DM_ANDROID_TOUCH_HUD = DM_ANDROID_TOUCH_GLYPHS + 6 * 8,
       DM_ANDROID_TOUCH_DRAW_FLOATS = DM_ANDROID_TOUCH_HUD + 18 };
void dm_android_touch_layout(float out[DM_ANDROID_TOUCH_DRAW_FLOATS]);
#ifdef __cplusplus
}
#endif
