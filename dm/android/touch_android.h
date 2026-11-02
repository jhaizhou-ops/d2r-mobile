#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* UI线程仅排队；step必须挂在dm_pad_hook游戏采样线程。 | UI only queues; step must run on the guest sampling thread through dm_pad_hook. */
void dm_android_touch_resize(float width_dp, float height_dp, float left_dp, float right_dp, float bottom_dp);
void dm_android_touch_tablet(int tablet);
/* phase: 0 down, 1 move, 2 up, 3 cancel; slot: stable 0..15; coordinates in dp. | 阶段：按下/移动/抬起/取消；槽固定0..15；坐标dp。 */
void dm_android_touch_event(int phase, int slot, float x, float y);
void dm_android_touch_focus(int active);
void dm_android_touch_uimsg(const char *cat, const char *act, const char *arg);
void dm_android_touch_step(uint64_t guest_now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
/* 绘图快照：header[width,height,navigation,skill_set]，23个[x,y,r,enabled,pressed]；12键、摇杆、换组、难度及8选择格。 | Drawing snapshot: header followed by 23 records (12 keys, stick, set switch, difficulty, 8 choices). */
/* 后接六个技能图标记录（状态、图集、裁切矩形、副手/充能、次数）和16个HUD数值。
   Append six icon records (status, atlas, crop, secondary/charge flags, charges) and 16 HUD values. */
enum { DM_ANDROID_TOUCH_GLYPHS = 4 + 23 * 5, DM_ANDROID_TOUCH_HUD = DM_ANDROID_TOUCH_GLYPHS + 6 * 8,
       DM_ANDROID_TOUCH_DRAW_FLOATS = DM_ANDROID_TOUCH_HUD + 18 };
void dm_android_touch_layout(float out[DM_ANDROID_TOUCH_DRAW_FLOATS]);
/* 手柄（gamepad_android.c）与触屏层的交界：模式回调（藏 / 露控件）、面板 / 读取 / 前端页状态（dm_front_navigation 读）、测试开关的泵。
   The interface between the controller side (gamepad_android.c) and the touch layer: the mode callback (hide / reveal controls), the panel / loading /
   front-end state (read by dm_front_navigation), and the test switch's pump. */
void dm_android_touch_pad_mode(int pad);
int dm_android_touch_navigation(void);
void dm_android_gamepad_test_pump(void);
#ifdef __cplusplus
}
#endif
