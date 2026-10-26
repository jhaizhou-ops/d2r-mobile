// 前端（Mac 键盘 / iOS 触屏）→ nn::hid 的输入状态。按钮位与 nn::hid::NpadButton 一致：
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15；摇杆 ±32767。
// Front end (Mac keyboard / iOS touchscreen) -> nn::hid input state. Button bits match nn::hid::NpadButton:
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15; sticks ±32767.
#pragma once
#include <stdint.h>
// 按钮位（= nn::hid::NpadButton 位序；hle_hid.c 的 DM_KEYS 键名表由这里生成）。前端一律用这些名字组 buttons。
// Button bits (= nn::hid::NpadButton bit order; the DM_KEYS key-name table in hle_hid.c is generated from here). Frontends always compose buttons from these names.
enum {
    DM_BTN_A = 1u << 0, DM_BTN_B = 1u << 1, DM_BTN_X = 1u << 2, DM_BTN_Y = 1u << 3,
    DM_BTN_LS = 1u << 4, DM_BTN_RS = 1u << 5, DM_BTN_L = 1u << 6, DM_BTN_R = 1u << 7,
    DM_BTN_ZL = 1u << 8, DM_BTN_ZR = 1u << 9, DM_BTN_PLUS = 1u << 10, DM_BTN_MINUS = 1u << 11,
    DM_BTN_LEFT = 1u << 12, DM_BTN_UP = 1u << 13, DM_BTN_RIGHT = 1u << 14, DM_BTN_DOWN = 1u << 15,
};
#define DM_STICK_MAX 32767
// 手柄版键位图标（键帽）样式：前端按最近一次输入的来源报（dm_padmode.c 在没开面板时换图；覆盖层没有那套就用原版 Switch 键帽）
// Button-icon (glyph) style of the controller layout: the front end reports the source of the latest input (dm_padmode.c swaps the images while no panel is open; without that set in the overlay the original Switch glyphs are used)
enum { DM_GLYPH_PAD, DM_GLYPH_KBD, DM_GLYPH_XBOX, DM_GLYPH_PS, DM_GLYPH_N };   // 原版 Switch 键帽 / 键盘键帽（桌面 App）/ Xbox 键帽 / PlayStation 键帽
void dm_padmode_request_glyph(int glyph);
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
uint64_t dm_input_samples(void);   // 游戏已取手柄的累计次数 | Cumulative number of times the game has sampled the controller
// 实体 / 虚拟手柄（iOS GameController，dm_gamepad.m）：与触屏输入分开存，游戏取手柄时按键按位或、摇杆手柄非零就用手柄的。没连手柄时全 0，取值与原来逐位相同
// Physical / virtual controller (iOS GameController, dm_gamepad.m): stored separately from touch input; when the game reads the controller, buttons are
// ORed bitwise and a stick uses the controller's value if it is non-zero. All 0 when no controller is connected, so values are bit-for-bit the same as before
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

// 可选诊断DM_HID_RECEIPT_LOG=<可写JSONL路径>：真实GetNpadState返回收样。
// 字段read_start_ns/time_ns同宿主now_ns()；前端publish窗口必须同helper，读取区间跨发布不得强配。
// 状态变化+最多每秒一次活动见证，1024条后limit_reached；未设环境变量不写文件。
// 实际返回含id/style/sampling/buttons/四轴/attributes/script_mask，不把前端输出当收到。
// 无新行不证明空白触点读到，须有效连接/采样活动/原UI与命令包证据；日志有IO开销，不作计时。
// Optional diagnostic DM_HID_RECEIPT_LOG=<writable JSONL path>: received samples of real GetNpadState returns.
// Fields read_start_ns/time_ns use the same host now_ns(); the front-end publish window must use the same helper, and a read interval that spans a publish must not be force-matched.
// State changes + at most one activity witness per second, limit_reached after 1024 entries; no file is written when the environment variable is not set.
// The actual return includes id/style/sampling/buttons/four axes/attributes/script_mask; front-end output is not treated as received.
// No new line does not prove that a blank touch point was read; a valid connection / sampling activity / evidence from the original UI and command packets is required; the log has IO overhead and is not used for timing.

// 私有skill-aim-v1：只取已实际GetNpad返回的右轴成对收样，非前端尚未消费值。
// Private skill-aim-v1: takes only paired received samples of the right axis that GetNpad has actually returned, not values the front end has not yet had consumed.
void dm_input_sampled_right(int32_t *rx,int32_t *ry);

// 手柄状态注入点：GetNpadState 填完原状态后、伪键覆盖前调用。桌面宿主的鼠标 / 触点脚本（touch_mac.c，Windows 显式纳入）注册；iOS 不带该文件，保持空。
// Controller state injection point: after GetNpadState fills the original state and before pseudo-key overrides. Desktop mouse / touch scripts register it (touch_mac.c, explicitly included on Windows); iOS omits the file and leaves it empty.
extern void (*dm_pad_hook)(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
