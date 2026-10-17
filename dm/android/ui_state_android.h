/* UI 状态机（Android）：前端模式 / 面板模式（移植 front_ios.m 的 touch_ui_message 一族）。 */
#ifndef DM_ANDROID_UI_STATE_H
#define DM_ANDROID_UI_STATE_H

/* 挂钩观测门 OBS_UIMSG（在游戏线程初始化时调一次；门回调 → 本地总线 → 状态机）。 */
void dm_android_ui_state_install(void);
/* 局外（前端页面：主菜单 / 选角 / 加载）＝1；进局＝0。对齐 iOS 的 g_touch_frontend。 */
int dm_android_ui_frontend(void);
/* 面板模式：0 游戏 / 1 面板 / 2 死亡弹窗 / 3 只开对话菜单（对齐 dm_tc_panel_mode）。 */
int dm_android_ui_panel_mode(void);
/* 状态变化计数（触摸层按它判断要不要重新应用掩码/导航态）。 */
unsigned dm_android_ui_rev(void);

#endif
