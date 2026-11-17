import { inputDevice, KeyCode } from '@kit.InputKit';
import dm from 'libdm.so';

// 位序与原生 dm_input.h 一致。
const BUTTONS: ReadonlyMap<number, number> = new Map<number, number>([
  [KeyCode.KEYCODE_BUTTON_A, 1 << 0], [KeyCode.KEYCODE_BUTTON_B, 1 << 1],
  [KeyCode.KEYCODE_BUTTON_X, 1 << 2], [KeyCode.KEYCODE_BUTTON_Y, 1 << 3],
  [KeyCode.KEYCODE_BUTTON_THUMBL, 1 << 4], [KeyCode.KEYCODE_BUTTON_THUMBR, 1 << 5],
  [KeyCode.KEYCODE_BUTTON_L1, 1 << 6], [KeyCode.KEYCODE_BUTTON_R1, 1 << 7],
  [KeyCode.KEYCODE_BUTTON_L2, 1 << 8], [KeyCode.KEYCODE_BUTTON_R2, 1 << 9],
  [KeyCode.KEYCODE_BUTTON_START, 1 << 10], [KeyCode.KEYCODE_BUTTON_SELECT, 1 << 11]
]);
const LEFT = 1 << 12, UP = 1 << 13, RIGHT = 1 << 14, DOWN = 1 << 15;
const DIRECTIONS: ReadonlyMap<number, number> = new Map<number, number>([
  [KeyCode.KEYCODE_DPAD_LEFT, LEFT], [KeyCode.KEYCODE_DPAD_UP, UP],
  [KeyCode.KEYCODE_DPAD_RIGHT, RIGHT], [KeyCode.KEYCODE_DPAD_DOWN, DOWN],
  [KeyCode.KEYCODE_DPAD_CENTER, 1 << 0]
]);

class PadState {
  keys: number = 0;
  axisButtons: number = 0;
  lx: number = 0;
  ly: number = 0;
  rx: number = 0;
  ry: number = 0;
}

function stick(value: number): number {
  const magnitude = Math.min(1, Math.abs(value));
  return Math.round(Math.sign(value) * Math.max(0, magnitude - 0.15) * 32767 / 0.85);
}

export class PadInput {
  private states: Map<number, PadState> = new Map<number, PadState>();
  private deviceChange: (event: inputDevice.DeviceListener) => void = (event: inputDevice.DeviceListener) => {
    if (event.type === 'remove' && this.states.delete(event.deviceId)) { this.publish(); }
  };

  listen(): void { inputDevice.on('change', this.deviceChange); }

  stop(): void {
    inputDevice.off('change', this.deviceChange);
    this.reset();
  }

  reset(): void {
    if (!this.states.size) { return; }
    this.states.clear();
    this.publish();
  }

  key(event: KeyEvent): boolean {
    const bit = BUTTONS.get(event.keyCode) ?? (event.keySource === KeySource.JOYSTICK ? DIRECTIONS.get(event.keyCode) : 0);
    if (!bit) { return false; }
    const pad = this.state(event.deviceId);
    if (event.type === KeyType.CANCEL) { this.states.delete(event.deviceId); }
    else if (event.type === KeyType.Down) { pad.keys |= bit; }
    else { pad.keys &= ~bit; }
    this.publish();
    return true;
  }

  axes(event: FocusAxisEvent): void {
    if (event.source !== SourceType.JOYSTICK) { return; }
    const axis = event.axisMap;
    const pad = this.state(event.deviceId ?? 0);
    pad.lx = stick(axis.get(AxisModel.ABS_X) ?? 0);
    pad.ly = -stick(axis.get(AxisModel.ABS_Y) ?? 0);
    pad.rx = stick(axis.get(AxisModel.ABS_RX) ?? axis.get(AxisModel.ABS_Z) ?? 0);
    pad.ry = -stick(axis.get(AxisModel.ABS_RY) ?? axis.get(AxisModel.ABS_RZ) ?? 0);
    const x = axis.get(AxisModel.ABS_HAT0X) ?? 0;
    const y = axis.get(AxisModel.ABS_HAT0Y) ?? 0;
    pad.axisButtons = (x < -0.5 ? LEFT : x > 0.5 ? RIGHT : 0) | (y < -0.5 ? UP : y > 0.5 ? DOWN : 0);
    if ((axis.get(AxisModel.ABS_BRAKE) ?? 0) > 0.35) { pad.axisButtons |= 1 << 8; }
    if ((axis.get(AxisModel.ABS_GAS) ?? 0) > 0.35) { pad.axisButtons |= 1 << 9; }
    this.publish();
  }

  private state(id: number): PadState {
    let pad = this.states.get(id);
    if (!pad) {
      pad = new PadState();
      this.states.set(id, pad);
    }
    return pad;
  }

  private publish(): void {
    let buttons = 0, lx = 0, ly = 0, rx = 0, ry = 0;
    this.states.forEach((pad: PadState) => {
      buttons |= pad.keys | pad.axisButtons;
      if (pad.lx * pad.lx + pad.ly * pad.ly > lx * lx + ly * ly) { lx = pad.lx; ly = pad.ly; }
      if (pad.rx * pad.rx + pad.ry * pad.ry > rx * rx + ry * ry) { rx = pad.rx; ry = pad.ry; }
    });
    dm.pad(buttons, lx, ly, rx, ry);
  }
}
