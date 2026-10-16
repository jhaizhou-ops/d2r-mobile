// 触屏专属 / 手柄专属行为的登记表：模式切换事务（dm_padmode.c）在改布局之前调旧一侧各项的「出」，重建完成之后调新一侧各项的「进」。
// 出 = 这一项自己收手：作废在途的替按键 / 替发消息请求、把挪过的原版控件写回、清掉缓存的控件地址（重建后旧地址全是悬空的）。
// 侧：0 = 只在触屏模式做事，1 = 只在手柄模式做事，2 = 两边都在、但握着会随重建失效的东西（每次切换都出一次、进一次）。
// 新加按模式分的行为在这里登记一行，不要在别处各自判模式、也不要让切换代码点名调某个模块。
//   X(名字, 侧, 进, 出)   进 / 出：void (*)(void)，没有写 dm_mode_none（空函数）
// Registry of touch-only / controller-only behaviors: the mode switch transaction (dm_padmode.c) calls the "leave" of each item on the old side before changing the layout, and the "enter" of each item on the new side after the rebuild completes.
// Leave = the item stands down by itself: void in-flight key-press / message requests made on the player's behalf, write moved original widgets back, clear cached widget addresses (after a rebuild the old addresses are all dangling).
// Side: 0 = acts only in touch mode, 1 = acts only in controller mode, 2 = present on both sides but holding things that become invalid with a rebuild (leaves once and enters once on every switch).
// Register a new mode-specific behavior with one row here; do not check the mode separately elsewhere, and do not have the switch code call some module by name.
//   X(name, side, enter, leave)   enter / leave: void (*)(void); write dm_mode_none (an empty function) when there is none
#define DM_MODE_ITEMS(X) \
    X("符文之语清单", 2, dm_mode_none, dm_runeword_ui_rebuild) \
    X("前端路径理顺与替按键", 0, dm_mode_none, dm_ui_auto_mode_leave) \
    X("界面触摸缓存与挪位表", 2, dm_mode_none, dm_ui_cache_mode_leave) \
    X("主菜单构建号与新版本提示", 2, dm_update_mode_enter, dm_mode_none) \
    X("死亡弹窗按当前模式重开", 2, dm_dead_modal_mode_enter, dm_mode_none) \
    X("小地图跨幕恢复", 0, dm_mode_none, dm_minimap_mode_leave) \
    X("魔盒并排", 0, dm_mode_none, dm_cube_side_mode_leave)
