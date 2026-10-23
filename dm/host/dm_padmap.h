// 实体手柄四个面键 → 游戏键：系统（GameController）给的 buttonA/B/X/Y 直接对应游戏的 A/B/X/Y，不认手柄类型、不做位置翻译。纯函数，前端 dm_gamepad.m 用，宿主单测 tests/t_padmap.c 核。
//   系统对印字母的手柄按键帽字母给：任天堂手柄（Switch Pro、Joy-Con 一对）印 A 的右键走 buttonA、印 B 的下键走 buttonB——与 Switch 原样一致，右 A 确认；
//   Xbox 手柄字母与位置本来一致（下 A、右 B、左 X、上 Y）。键帽没字母的（PS 等）按系统的菱形位置给：下方键 = buttonA = 确认。
//   依据：公开日志（iPadOS 27 + Switch Pro：已认出任天堂、按位置再翻译一次后 A/B、X/Y 全反）；SDL 的苹果手柄实现把任天堂手柄的 buttonA 当右键。
//   （先前按 GCExtendedGamepad.h 的菱形约定当 buttonA = 下方键，任天堂手柄再换成右 A 下 B，等于换了两次。头文件的菱形对任天堂手柄不成立。）
// Physical controller face buttons → game buttons: the system (GameController) buttonA/B/X/Y map directly to the game's A/B/X/Y, regardless of controller type, with no positional translation. Pure function, used by the front end dm_gamepad.m, checked by the host unit test tests/t_padmap.c.
//   For controllers with printed letters the system reports by keycap letter: on Nintendo controllers (Switch Pro, a Joy-Con pair) the right button printed A is buttonA and the bottom button printed B is buttonB — identical to the Switch, right A confirms;
//   on Xbox controllers letters and positions already agree (bottom A, right B, left X, top Y). For controllers without letters (PS etc.) the system reports by diamond position: bottom button = buttonA = confirm.
//   Evidence: a public log (iPadOS 27 + Switch Pro: recognized as Nintendo, translated by position once more, and A/B, X/Y were all swapped); SDL's Apple controller code treats a Nintendo controller's buttonA as the right button.
//   (Previously buttonA was taken as the bottom button per the diamond convention in GCExtendedGamepad.h and Nintendo controllers were swapped again to right A / bottom B, i.e. swapped twice. The header's diamond does not hold for Nintendo controllers.)
#pragma once
#include <stdint.h>
#include <string.h>
#include "dm_input.h"
static inline uint64_t dm_padmap_face(int a, int b, int x, int y) {   // 四个参数 = 系统 buttonA / B / X / Y 按着没有 | The four parameters = whether the system's buttonA / B / X / Y are pressed
    return (a ? DM_BTN_A : 0) | (b ? DM_BTN_B : 0) | (x ? DM_BTN_X : 0) | (y ? DM_BTN_Y : 0);
}
// GameController's product category does not guarantee Nintendo's letter-based face-button order.
// GameSir X5 Lite reports "Switch Pro Controller" but its buttonA/B follow physical Xbox positions.
static inline int dm_padmap_nintendo_labels(const char *vendor, const char *category) {
    if (vendor && strstr(vendor, "GameSir-X5 Lite")) return 0;
    return category && (strstr(category, "Switch") || strstr(category, "Joy-Con"));
}
// Xbox 键位（dm_gamepad.m 认出 Xbox 手柄时自动用）：按物理位置排，与 PC / Xbox 版同位置同功能——下 = A 确认、右 = B 返回、上 = X（放进堆叠仓库）、左 = Y。
//   按键按 Nintendo 字母上报的手柄：右 = buttonA、下 = buttonB、上 = buttonX、左 = buttonY → 只换 A/B。不能仅凭 Switch Pro 类别判断（GameSir X5 Lite 就是例外，见 dm_padmap_nintendo_labels）。
//   其余手柄系统按菱形位置给：下 = buttonA、右 = buttonB、左 = buttonX、上 = buttonY → 只换 X/Y。
// Xbox layout (used automatically when dm_gamepad.m recognizes an Xbox controller): by physical position, same position = same function as the PC / Xbox version -- bottom = A confirm, right = B back, top = X (into the stack stash), left = Y.
//   Controllers that report Nintendo keycap letters: right = buttonA, bottom = buttonB, top = buttonX, left = buttonY -> only A/B swap. A Switch Pro category alone is insufficient (GameSir X5 Lite is an exception; see dm_padmap_nintendo_labels).
//   Every other controller is reported by diamond position: bottom = buttonA, right = buttonB, left = buttonX, top = buttonY -> only X/Y swap.
static inline uint64_t dm_padmap_face_xbox(int a, int b, int x, int y, int nintendo) {
    return nintendo ? dm_padmap_face(b, a, x, y) : dm_padmap_face(a, b, y, x);
}
// Apply the per-user layout choice while accounting for Nintendo's letter-based reporting.
static inline uint64_t dm_padmap_face_for_layout(int a, int b, int x, int y, int nintendo, int xbox_layout) {
    return xbox_layout ? dm_padmap_face_xbox(a, b, x, y, nintendo) : dm_padmap_face(a, b, x, y);
}
