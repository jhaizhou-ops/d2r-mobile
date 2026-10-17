export interface PadKey { label: string; mask: number; x: number; y: number; radius: number; }
export interface PadFinger { id: number; x: number; y: number; role: number; }
export interface PadFrame { buttons: number; lx: number; ly: number; }
export const PAD_KEYS: PadKey[] = [
  { label: 'X', mask: 4, x: 324, y: 72, radius: 23 },
  { label: 'Y', mask: 8, x: 276, y: 120, radius: 23 },
  { label: 'A', mask: 1, x: 372, y: 120, radius: 23 },
  { label: 'B', mask: 2, x: 324, y: 168, radius: 23 },
  { label: 'L', mask: 64, x: 32, y: 24, radius: 19 },
  { label: 'ZL', mask: 256, x: 78, y: 24, radius: 19 },
  { label: 'ZR', mask: 512, x: 322, y: 24, radius: 19 },
  { label: 'R', mask: 128, x: 368, y: 24, radius: 19 },
  { label: '−', mask: 2048, x: 177, y: 24, radius: 17 },
  { label: '+', mask: 1024, x: 223, y: 24, radius: 17 }
];

export class PadModel {
  private fingers: Map<number, PadFinger> = new Map<number, PadFinger>();
  down(id: number, x: number, y: number): void {
    if (this.fingers.has(id)) { return; }
    let role: number = 0;
    const stickBusy = Array.from(this.fingers.values()).some((finger: PadFinger) => finger.role === -1);
    if (!stickBusy && Math.hypot(x - 82, y - 120) <= 64) { role = -1; }
    else {
      const key = PAD_KEYS.find((key: PadKey) => Math.hypot(x - key.x, y - key.y) <= key.radius);
      if (key) { role = key.mask; }
    }
    // A touch beginning in empty space never turns into movement or a button.
    this.fingers.set(id, { id: id, x: x, y: y, role: role });
  }
  move(id: number, x: number, y: number): void {
    const finger = this.fingers.get(id);
    if (finger) { finger.x = x; finger.y = y; }
  }
  up(id: number): void { this.fingers.delete(id); }
  reset(): void { this.fingers.clear(); }
  frame(): PadFrame {
    let buttons = 0;
    let lx = 0;
    let ly = 0;
    this.fingers.forEach((finger: PadFinger) => {
      if (finger.role === -1) {
        const dx = (finger.x - 82) / 48;
        const dy = (120 - finger.y) / 48;
        const length = Math.hypot(dx, dy);
        if (length > 0.15) {
          const magnitude = (Math.min(1, length) - 0.15) / 0.85;
          lx = Math.round(dx / length * magnitude * 32767);
          ly = Math.round(dy / length * magnitude * 32767);
        }
      } else if (finger.role > 0) {
        const key = PAD_KEYS.find((key: PadKey) => key.mask === finger.role);
        if (key && Math.hypot(finger.x - key.x, finger.y - key.y) <= key.radius) { buttons |= key.mask; }
      }
    });
    return { buttons: buttons, lx: lx, ly: ly };
  }
}
