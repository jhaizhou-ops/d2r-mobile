// 前端（Mac 键盘 / iOS 触屏）→ nn::hid 的输入状态。按钮位与 nn::hid::NpadButton 一致：
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15；摇杆 ±32767。
#pragma once
#include <stdint.h>
// 按钮位（= nn::hid::NpadButton 位序；hle_hid.c 的 SW_KEYS 键名表由这里生成）。前端一律用这些名字组 buttons。
enum {
    SW_BTN_A = 1u << 0, SW_BTN_B = 1u << 1, SW_BTN_X = 1u << 2, SW_BTN_Y = 1u << 3,
    SW_BTN_LS = 1u << 4, SW_BTN_RS = 1u << 5, SW_BTN_L = 1u << 6, SW_BTN_R = 1u << 7,
    SW_BTN_ZL = 1u << 8, SW_BTN_ZR = 1u << 9, SW_BTN_PLUS = 1u << 10, SW_BTN_MINUS = 1u << 11,
    SW_BTN_LEFT = 1u << 12, SW_BTN_UP = 1u << 13, SW_BTN_RIGHT = 1u << 14, SW_BTN_DOWN = 1u << 15,
};
#define SW_STICK_MAX 32767
void sw_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
uint64_t sw_input_samples(void);   // 游戏已取手柄的累计次数
// 实体 / 虚拟手柄（iOS GameController，sw_gamepad.m）：与触屏输入分开存，游戏取手柄时按键按位或、摇杆手柄非零就用手柄的。没连手柄时全 0，取值与原来逐位相同
void sw_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

// 可选诊断SW_HID_RECEIPT_LOG=<可写JSONL路径>：真实GetNpadState返回收样。
// 字段read_start_ns/time_ns同宿主now_ns()；前端publish窗口必须同helper，读取区间跨发布不得强配。
// 状态变化+最多每秒一次活动见证，1024条后limit_reached；未设环境变量不写文件。
// 实际返回含id/style/sampling/buttons/四轴/attributes/script_mask，不把前端输出当收到。
// 无新行不证明空白触点读到，须有效连接/采样活动/原UI与命令包证据；日志有IO开销，不作计时。

// 私有skill-aim-v1：只取已实际GetNpad返回的右轴成对收样，非前端尚未消费值。
void sw_input_sampled_right(int32_t *rx,int32_t *ry);

// 手柄状态注入点：GetNpadState 填完原状态后、伪键覆盖前调用。Mac 宿主触点脚本（touch_mac.c，Mac 专用、按 _mac 约定不进 iOS 包）启动时注册；iOS 不带该文件，保持空
extern void (*sw_pad_hook)(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
