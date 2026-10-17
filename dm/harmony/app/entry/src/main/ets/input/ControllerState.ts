export interface ControllerKey { label: string; mask: number; }
export const SHOULDER_KEYS: ControllerKey[] = [
  {label:'L',mask:64},{label:'ZL',mask:256},{label:'−',mask:2048},
  {label:'+',mask:1024},{label:'ZR',mask:512},{label:'R',mask:128}
];
export const FACE_KEYS: ControllerKey[] = [
  {label:'X',mask:4},{label:'Y',mask:8},{label:'A',mask:1},{label:'B',mask:2}
];
export const NAV_KEYS: ControllerKey[] = [
  {label:'←',mask:4096},{label:'↑',mask:8192},{label:'↓',mask:32768},
  {label:'→',mask:16384},{label:'L3',mask:16},{label:'R3',mask:32}
];
export interface ControllerFrame { buttons: number; lx: number; ly: number; rx: number; ry: number; }
interface StickContact { source: string; side: number; x: number; y: number; }
export class ControllerState {
  private buttons: Map<string, number> = new Map<string, number>();
  private sticks: Map<number, StickContact> = new Map<number, StickContact>();
  touch(source: string, phase: string, mask: number, side: number, x: number, y: number): void {
    if (phase === 'cancel') { this.reset(); return; }
    const size = side < 0 ? 40 : 72;
    if (phase === 'up' || !Number.isFinite(x) || !Number.isFinite(y) || x < 0 || x > size || y < 0 || y > size) {
      this.release(source); return;
    }
    if (phase === 'down') {
      if (side < 0) { this.press(source, mask); }
      else { this.startStick(source, side, (x - 36) / 28, (36 - y) / 28); }
    } else if (phase === 'move' && side >= 0) { this.moveStick(source, (x - 36) / 28, (36 - y) / 28); }
  }
  press(source: string, mask: number): void { this.buttons.set(source, mask); }
  release(source: string): void {
    this.buttons.delete(source);
    this.sticks.forEach((contact: StickContact, side: number) => {
      if (contact.source === source) { this.sticks.delete(side); }
    });
  }
  startStick(source: string, side: number, x: number, y: number): void {
    if ((side === 0 || side === 1) && !this.sticks.has(side)) {
      this.sticks.set(side, { source: source, side: side, x: x, y: y });
    }
  }
  moveStick(source: string, x: number, y: number): void {
    this.sticks.forEach((contact: StickContact) => {
      if (contact.source === source) { contact.x = x; contact.y = y; }
    });
  }
  reset(): void { this.buttons.clear(); this.sticks.clear(); }
  frame(): ControllerFrame {
    const frame: ControllerFrame = {buttons:0,lx:0,ly:0,rx:0,ry:0};
    this.buttons.forEach((mask: number) => { frame.buttons |= mask; });
    this.sticks.forEach((contact: StickContact, side: number) => {
      const length = Math.hypot(contact.x, contact.y);
      if (!Number.isFinite(length) || length <= 0.15) { return; }
      const magnitude = (Math.min(length, 1) - 0.15) / 0.85;
      const x = Math.round(contact.x / length * magnitude * 32767);
      const y = Math.round(contact.y / length * magnitude * 32767);
      if (side === 0) { frame.lx = x; frame.ly = y; }
      else { frame.rx = x; frame.ry = y; }
    });
    return frame;
  }
}
