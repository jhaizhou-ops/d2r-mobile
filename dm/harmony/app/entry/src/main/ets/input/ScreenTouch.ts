// One content-local finger, original UI touch phases. No button substitution.
export class ScreenTouch {
  private finger: number = -1;
  private send: (phase: number, x: number, y: number) => number;
  constructor(send: (phase: number, x: number, y: number) => number) { this.send = send; }
  cancel(): void {
    if (this.finger >= 0) { this.finger = -1; this.send(3, 0, 0); }
  }
  event(phase: number, id: number, x: number, y: number, width: number, height: number): number {
    if (phase === 3) { this.cancel(); return 0; }
    if (!(width > 0 && height > 0 && x >= 0 && x <= width && y >= 0 && y <= height)) {
      if (id === this.finger) { this.cancel(); }
      return 40;
    }
    if (phase === 0) {
      if (this.finger >= 0) { return 47; }
      const result = this.send(0, x / width, y / height);
      if (result === 0) { this.finger = id; }
      return result;
    }
    if (id !== this.finger) { return 47; }
    const result = this.send(phase, x / width, y / height);
    if (result !== 0) { this.cancel(); }
    if (phase === 2) { this.finger = -1; }
    return result;
  }
}
