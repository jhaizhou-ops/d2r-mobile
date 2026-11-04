#import <UIKit/UIKit.h>
#include "dm_touch_controls.h"
#include "dm_touch_game_state.h"
#include "../../host/dm_hud.h"

// 绘制层不吃触点；真实事件由DmMetalView统一收集并保留dm_touch_set通路。
// The drawing layer does not consume touch points; real events are collected in one place by DmMetalView, keeping the dm_touch_set path.
@interface DmTouchOverlay : UIView
@property(nonatomic, assign) const dm_tc_state *controls;
@property(nonatomic, assign) BOOL navigationMode;
@property(nonatomic, assign) dm_tg_snapshot nativeMapping;
@property(nonatomic, assign) dm_hud_info hud;
@property(nonatomic, assign) BOOL dialogOnly;   // 只开着 NPC 对话菜单：底栏照常画 | only the NPC dialog menu is open: the bottom bar is drawn as usual
@property(nonatomic, assign) BOOL hudBlocked;   // 面板栈里有非常驻面板：UIKit 底栏让开（front_ios.m hud_stack_message）   // 宿主每拍发布的 HUD 数字（底部 UI v4：经验条、角标、难度） | the panel stack has a non-persistent panel: the UIKit bottom bar gets out of the way (front_ios.m hud_stack_message)   // HUD numbers published by the host every tick (bottom UI v4: experience bar, badges, difficulty)
- (void)controlsChanged;   // 触控状态变了（front_ios pushFrom）：挪摇杆层，按钮显示变了才重画 | touch state changed (front_ios pushFrom): move the joystick layers, redraw only when the button visuals change
- (NSDictionary *)controlMetadata;
@end
