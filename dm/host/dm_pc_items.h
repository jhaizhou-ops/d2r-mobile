// PC 物品/NPC 接口；依据原版 3.3.94012 的 ItemSlot、FocusManager 和 NpcDialog。
// PC item/NPC boundary, based on the original 3.3.94012 ItemSlot, FocusManager and NpcDialog.
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t root;
    uint32_t generation, epoch;
    int32_t x, y, width, height;
    int32_t classic_width, classic_height;
    uint32_t modifiers;
    int valid, event_scope, ingame, loading, busy, hd;
} dm_pc_items_snapshot;

// 仅游戏线程发布/分发；坐标为 HD 输入像素，SD 绘制由原版转换。取消可从任意线程调用。
// Publish/dispatch only on the game thread; coordinates are HD input pixels, converted by the original for SD drawing. Cancel is thread-safe.
// event_scope：0=持续指针，1=当前物理事件，2=无效；取消后必须发布更新的 epoch。
// event_scope: 0=persistent pointer, 1=current physical event, 2=invalid; cancellation requires a newer epoch before rearming.
// 消费 LUP/RUP 时，发布者必须清除其按键追踪。
// When LUP/RUP is consumed, the publisher must clear its own button tracking.
int dm_pc_items_enabled(void);
void dm_pc_items_publish(const dm_pc_items_snapshot *snapshot);
void dm_pc_items_cancel(void);
int dm_pc_items_pointer(int phase, int x, int y);
void dm_pc_items_pointer_after(int phase, int x, int y);
void dm_pc_items_pump(void);
void dm_pc_items_message(const char *category, const char *action, const char *argument);
uint32_t dm_pc_items_vault_drop(void);

// 这些入口仅由新改写门调用；缺少 PC profile 时保留原版，物理操作另需 snapshot。
// Called only by the new rewrite gates; no PC profile preserves the original, and physical operations additionally require a snapshot.
void dm_ui_pc_item_left(uint64_t *regs);
void dm_ui_pc_item_right(uint64_t *regs);
void dm_ui_pc_item_hold(uint64_t *regs);
void dm_ui_pc_npc_click(uint64_t *regs);
void dm_ui_pc_held_draw(uint64_t *regs);
void dm_ui_pc_item_tooltip(uint64_t *regs);
void dm_ui_pc_grid_hover(uint64_t *regs);
