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
#include "dm_input.h"
static inline uint64_t dm_padmap_face(int a, int b, int x, int y) {   // 四个参数 = 系统 buttonA / B / X / Y 按着没有 | The four parameters = whether the system's buttonA / B / X / Y are pressed
    return (a ? DM_BTN_A : 0) | (b ? DM_BTN_B : 0) | (x ? DM_BTN_X : 0) | (y ? DM_BTN_Y : 0);
}
