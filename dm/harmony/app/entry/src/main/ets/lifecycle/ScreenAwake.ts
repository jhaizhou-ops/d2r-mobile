export interface AwakeWindow {
  setWindowKeepScreenOn(enabled: boolean): Promise<void>;
}

// Serialize window requests so a late foreground completion cannot override exit.
export class ScreenAwake {
  private target: AwakeWindow | undefined = undefined;
  private foreground: boolean = false;
  private gameActive: boolean = false;
  private testKeepScreenOn: boolean = false;
  private pending: Promise<void> = Promise.resolve();
  private report: (message: string) => void;
  private applied: (enabled: boolean) => void;

  constructor(report: (message: string) => void, applied: (enabled: boolean) => void = () => {}) { this.report = report; this.applied = applied; }
  bind(target: AwakeWindow): void {
    if (this.target && this.target !== target) { this.schedule(this.target, true); }
    this.target = target;
    this.schedule(target, false);
  }
  unbind(): void {
    const previous = this.target;
    this.target = undefined;
    if (previous) { this.schedule(previous, true); }
  }
  setForeground(active: boolean): void {
    this.foreground = active;
    if (this.target) { this.schedule(this.target, false); }
  }
  setGameActive(active: boolean): void {
    this.gameActive = active;
    if (this.target) { this.schedule(this.target, false); }
  }
  setTestKeepScreenOn(enabled: boolean): void {
    this.testKeepScreenOn = enabled;
    if (this.target) { this.schedule(this.target, false); }
  }
  whenIdle(): Promise<void> { return this.pending; }
  private schedule(target: AwakeWindow, release: boolean): void {
    this.pending = this.pending.then(async () => {
      const enabled = !release && this.target === target && this.foreground && this.testKeepScreenOn;
      await target.setWindowKeepScreenOn(enabled);
      if (this.target === target) { this.applied(enabled); }
    }).catch((error: Error) => { this.report(String(error)); });
  }
}
