export interface Insets { left: number; top: number; right: number; bottom: number; }
export interface GameViewport { width: number; height: number; }

// All inputs are vp. The Surface keeps the source aspect ratio even in a floating window.
export function fitGameViewport(width: number, height: number): GameViewport {
  if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0) {
    return { width: 0, height: 0 };
  }
  const fitted = Math.min(width, height * 16 / 9);
  return { width: fitted, height: fitted * 9 / 16 };
}
export function controlScale(width: number, intrinsic: number): number {
  return Number.isFinite(width) && width > 0 && intrinsic > 0 ? Math.min(1, width / intrinsic) : 0;
}

export interface ImmersiveTarget { apply(enabled: boolean): Promise<void>; }
export async function restoreWindowProperties(layout: () => Promise<void>, orientation: () => Promise<void>): Promise<void> {
  let failure = '';
  try { await layout(); } catch (error) { failure = String(error); }
  // A failed layout restore must not prevent restoring the original orientation.
  try { await orientation(); } catch (error) { failure += ` ${String(error)}`; }
  if (failure) { throw new Error(failure); }
}
export class ImmersiveState {
  private target: ImmersiveTarget | undefined = undefined;
  private requested: boolean = false;
  private pending: Promise<void> = Promise.resolve();
  private changed: (enabled: boolean) => void;
  private failed: (message: string) => void;
  constructor(changed: (enabled: boolean) => void, failed: (message: string) => void) {
    this.changed = changed; this.failed = failed;
  }
  bind(target: ImmersiveTarget): void {
    this.unbind(); this.target = target;
  }
  unbind(): void {
    const old = this.target;
    this.target = undefined; this.requested = false; this.changed(false);
    if (old) { this.enqueue(old, true); }
  }
  request(enabled: boolean): void {
    this.requested = enabled;
    if (!enabled) { this.changed(false); }
    if (this.target) { this.enqueue(this.target, false); }
  }
  whenIdle(): Promise<void> { return this.pending; }
  private enqueue(target: ImmersiveTarget, release: boolean): void {
    this.pending = this.pending.then(async () => {
      const enabled = !release && this.target === target && this.requested;
      try {
        await target.apply(enabled);
        if (this.target === target && this.requested === enabled) { this.changed(enabled); }
      } catch (error) {
        this.failed(String(error));
        if (this.target === target) { this.requested = false; this.changed(false); }
        // An enter can fail after changing orientation/layout. Always attempt restoration.
        try { await target.apply(false); } catch (restoreError) { this.failed(String(restoreError)); }
      }
    });
  }
}
