// 实体手柄 → 宿主手柄状态（dm_pad_set），并对齐 iOS 的「谁接管」逻辑（dm/ios/Sources/dm_gamepad.m）：
//   · 手柄「明确输入」= 按下任何一颗键，或把摇杆从中位推出来 → 切手柄布局（宿主 dm_padmode_request(1)）。
//   · 碰屏幕 → 切回触屏布局（0），摇杆要先回中、再推出来才算重新接管（漂移大的手柄不会来回切）。
//   · 触屏模式下手柄摇杆不交给游戏；手柄模式下没推出来的摇杆给 0。
//   · 摇杆迟滞：推过 25% 算推出，回到 15% 以内算回中。
//   · 键帽样式按手柄厂商报（Xbox / PlayStation / 其余原版 Switch 键帽）。
// 按键位 = nn::hid::NpadButton（host/dm_input.h 的 DM_BTN_*），摇杆 ±32767（上为正）；多个手柄按键取或、每根摇杆取推得最远的。
// Physical controller -> host controller state (dm_pad_set), mirroring iOS's "who takes over" logic (dm/ios/Sources/dm_gamepad.m):
//   clear input (any new button, or a stick pushed out) switches to the controller layout (host dm_padmode_request(1));
//   touching the screen switches back to touch (0) and a deflected stick must return to center before taking over again;
//   sticks are not delivered in touch mode and give 0 in controller mode while not pushed out; hysteresis 25% out / 15% in.
package com.xiaoboluo.d2r;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;

final class PadInput {
    // host/dm_input.h 的 DM_BTN_*（= nn::hid::NpadButton 位序） | DM_BTN_* from host/dm_input.h (= nn::hid::NpadButton order)
    private static final int BTN_A = 1, BTN_B = 1 << 1, BTN_X = 1 << 2, BTN_Y = 1 << 3,
            BTN_LS = 1 << 4, BTN_RS = 1 << 5, BTN_L = 1 << 6, BTN_R = 1 << 7,
            BTN_ZL = 1 << 8, BTN_ZR = 1 << 9, BTN_PLUS = 1 << 10, BTN_MINUS = 1 << 11,
            BTN_LEFT = 1 << 12, BTN_UP = 1 << 13, BTN_RIGHT = 1 << 14, BTN_DOWN = 1 << 15;

    // 键帽样式（host/dm_input.h 的 DM_GLYPH_*） | Glyph styles (DM_GLYPH_* in host/dm_input.h)
    private static final int GLYPH_PAD = 0, GLYPH_XBOX = 2, GLYPH_PS = 3;

    private static final double OUT = 0.25, IN = 0.15;   // 迟滞：推过 25% 算推出，回到 15% 算回中 | Hysteresis: 25% out, 15% in
    private static final float TRIGGER = 0.5f;

    private static final class State {
        long buttons;
        float lx, ly, rx, ry;
        boolean lt, rt;       // 扳机轴（部分手柄 L2/R2 只报轴不报键） | trigger axes (some pads report L2/R2 as axes only)
        boolean hatL, hatR, hatU, hatD;   // 十字键走 AXIS_HAT 的手柄 | pads that report the d-pad through AXIS_HAT
        int glyph = GLYPH_PAD;
    }

    private static final Map<Integer, State> pads = new HashMap<>();
    private static boolean padMode;       // 当前是不是手柄布局 | current layout: controller or touch
    private static boolean needCenter;    // 碰过屏：摇杆要先回中 | screen was touched: sticks must return to center first
    private static boolean onL, onR;      // 摇杆是否推出（带迟滞） | sticks pushed out (with hysteresis)
    private static long prevButtons;
    private static long lastButtons = -1;
    private static int lastLx, lastLy, lastRx, lastRy;

    private PadInput() {}

    static boolean isPadMode() { return padMode; }

    static boolean onKey(KeyEvent e) {
        if (!isPadSource(e.getSource())) return false;
        State s = state(e.getDeviceId());
        long bit = keyBit(e.getKeyCode());
        if (bit == 0) return false;
        s.glyph = glyphOf(e.getDevice());
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            s.buttons |= bit;
        } else if (e.getAction() == KeyEvent.ACTION_UP) {
            s.buttons &= ~bit;
        } else {
            return true;   // 重复按键不改变状态，但要吃掉事件 | repeats keep the state, still consumed
        }
        publish();
        return true;
    }

    static boolean onMotion(MotionEvent e) {
        if (!isPadSource(e.getSource())) return false;
        State s = state(e.getDeviceId());
        s.glyph = glyphOf(e.getDevice());
        s.lx = axis(e.getAxisValue(MotionEvent.AXIS_X));
        s.ly = -axis(e.getAxisValue(MotionEvent.AXIS_Y));       // Android 摇杆向下为正，宿主向上为正 | Android sticks are down-positive, the host is up-positive
        s.rx = axis(e.getAxisValue(MotionEvent.AXIS_Z));
        s.ry = -axis(e.getAxisValue(MotionEvent.AXIS_RZ));
        float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
        float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
        s.lt = lt > TRIGGER;
        s.rt = rt > TRIGGER;

        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        boolean hl = hx < -TRIGGER, hr = hx > TRIGGER, hu = hy < -TRIGGER, hd = hy > TRIGGER;
        if (hl != s.hatL) { s.hatL = hl; toggle(s, BTN_LEFT, hl); }
        if (hr != s.hatR) { s.hatR = hr; toggle(s, BTN_RIGHT, hr); }
        if (hu != s.hatU) { s.hatU = hu; toggle(s, BTN_UP, hu); }
        if (hd != s.hatD) { s.hatD = hd; toggle(s, BTN_DOWN, hd); }
        publish();
        return true;
    }

    // 玩家碰了屏幕：手柄布局 → 触屏布局，并等摇杆回中（iOS 的 dm_gamepad_touched） | The player touched the screen (iOS dm_gamepad_touched)
    static void onTouch() {
        if (!padMode) return;
        needCenter = true;
        setMode(false);
        publish();
    }

    // 失活 / 进后台：手柄的键和摇杆清零（iOS 的 dm_gamepad_reset） | Resigned active / backgrounded (iOS dm_gamepad_reset)
    static void reset() {
        boolean any = false;
        for (State s : pads.values()) {
            any |= s.buttons != 0 || s.lx != 0 || s.ly != 0 || s.rx != 0 || s.ry != 0;
            s.buttons = 0; s.lx = s.ly = s.rx = s.ry = 0; s.lt = s.rt = false;
        }
        if (any) publish();
    }

    // 手柄断开后状态别留着（按键卡住会一直发给游戏）；一个手柄都不剩就回触屏布局
    // Drop a controller's state once it is gone (a stuck button would keep being sent); back to touch layout when none is left
    static void sweep() {
        if (pads.isEmpty()) return;
        boolean removed = false;
        for (Iterator<Map.Entry<Integer, State>> it = pads.entrySet().iterator(); it.hasNext(); ) {
            Map.Entry<Integer, State> e = it.next();
            if (InputDevice.getDevice(e.getKey()) == null) { it.remove(); removed = true; }
        }
        if (!removed) return;
        if (pads.isEmpty()) { setMode(false); }
        publish();
    }

    private static void setMode(boolean pad) {
        if (padMode == pad) return;
        padMode = pad;
        DmActivity.nativePadMode(pad ? 1 : 0);
        DmActivity.onPadMode(pad);
        android.util.Log.i("dm", pad ? "手柄：有明确输入 → 手柄布局" : "手柄：碰屏 / 断开 → 触屏布局");
    }

    private static void toggle(State s, long bit, boolean on) {
        if (on) s.buttons |= bit; else s.buttons &= ~bit;
    }

    private static State state(int id) {
        State s = pads.get(id);
        if (s == null) { s = new State(); pads.put(id, s); }
        return s;
    }

    private static boolean isPadSource(int source) {
        return (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    // 系统手柄的面键按菱形报告（下方 = BUTTON_A），与宿主"下 = A 确认"一致；肩键 / 扳机 / 摇杆按 / 菜单 / 十字键逐个对位。
    // The system reports face buttons by diamond position (bottom = BUTTON_A), matching the host's "bottom = A confirm".
    private static long keyBit(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: return BTN_A;
            case KeyEvent.KEYCODE_BUTTON_B: return BTN_B;
            case KeyEvent.KEYCODE_BUTTON_X: return BTN_X;
            case KeyEvent.KEYCODE_BUTTON_Y: return BTN_Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return BTN_L;
            case KeyEvent.KEYCODE_BUTTON_R1: return BTN_R;
            case KeyEvent.KEYCODE_BUTTON_L2: return BTN_ZL;
            case KeyEvent.KEYCODE_BUTTON_R2: return BTN_ZR;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return BTN_LS;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return BTN_RS;
            case KeyEvent.KEYCODE_BUTTON_START: return BTN_PLUS;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_BACK: return BTN_MINUS;
            case KeyEvent.KEYCODE_DPAD_UP: return BTN_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return BTN_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return BTN_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return BTN_RIGHT;
            default: return 0;
        }
    }

    // 厂商 → 键帽样式（iOS 按 GCProductCategory 认，Android 只能按厂商号） | Vendor -> glyph style (Android only has the vendor id)
    private static int glyphOf(InputDevice dev) {
        if (dev == null) return GLYPH_PAD;
        switch (dev.getVendorId()) {
            case 0x045e: return GLYPH_XBOX;   // Microsoft
            case 0x054c: return GLYPH_PS;     // Sony
            default: return GLYPH_PAD;
        }
    }

    private static float axis(float v) {
        if (v > 1f) v = 1f;
        if (v < -1f) v = -1f;
        return v * 32767f;
    }

    private static boolean stickOn(boolean was, float x, float y) {
        double m = Math.hypot(x, y) / 32767.0;
        return was ? m >= IN : m >= OUT;
    }

    private static void publish() {
        sweep();
        long bits = 0;
        float lx = 0, ly = 0, rx = 0, ry = 0;
        double bestL = -1, bestR = -1;
        for (State s : pads.values()) {
            bits |= s.buttons | (s.lt ? BTN_ZL : 0) | (s.rt ? BTN_ZR : 0);
            double ml = Math.hypot(s.lx, s.ly), mr = Math.hypot(s.rx, s.ry);
            if (ml > bestL) { bestL = ml; lx = s.lx; ly = s.ly; }
            if (mr > bestR) { bestR = mr; rx = s.rx; ry = s.ry; }
        }
        onL = stickOn(onL, lx, ly);
        onR = stickOn(onR, rx, ry);
        if (!onL && !onR) needCenter = false;   // 摇杆回中了：之后再推出来算数 | The stick is back to center: pushing it out counts again from now on

        if ((bits & ~prevButtons) != 0 || ((onL || onR) && !needCenter)) {
            // 先报键帽样式，同一次切换里就换成这只手柄的键帽（iOS 顺序相同） | Report the glyph style first, so the same switch already uses this pad's glyphs
            int g = GLYPH_PAD;
            for (State s : pads.values()) g = s.glyph;
            DmActivity.nativePadGlyph(g);
            setMode(true);
        }
        prevButtons = bits;

        // 触屏布局不交摇杆；手柄布局下没推出来的给 0 | No sticks in touch layout; a stick that is not pushed out gives 0 in controller layout
        if (!padMode || !onL) { lx = 0; ly = 0; }
        if (!padMode || !onR) { rx = 0; ry = 0; }

        int ilx = (int) lx, ily = (int) ly, irx = (int) rx, iry = (int) ry;
        if (bits == lastButtons && ilx == lastLx && ily == lastLy && irx == lastRx && iry == lastRy) return;
        lastButtons = bits; lastLx = ilx; lastLy = ily; lastRx = irx; lastRy = iry;
        DmActivity.nativePad(bits, ilx, ily, irx, iry);
    }
}
