import { hilog } from '@kit.PerformanceAnalysisKit';
import { ScreenAwake, AwakeWindow } from './ScreenAwake';
import { window } from '@kit.ArkUI';

export class WindowAwakeTarget implements AwakeWindow {
  private target: window.Window;
  constructor(target: window.Window) { this.target = target; }
  async setWindowKeepScreenOn(enabled: boolean): Promise<void> {
    await this.target.setWindowKeepScreenOn(enabled);
    const actual = this.target.getWindowProperties().isKeepScreenOn;
    if (actual !== enabled) { throw new Error(`Screen awake readback mismatch requested=${enabled} actual=${actual}`); }
    hilog.info(0, 'XblWindow', 'Screen awake verified requested=%{public}s actual=%{public}s', String(enabled), String(actual));
  }
}

export const gameScreenAwake: ScreenAwake = new ScreenAwake((message: string) => {
  AppStorage.setOrCreate('screenAwakeError', message);
  hilog.warn(0, 'XblWindow', 'Screen awake request failed: %{public}s', message);
}, (enabled: boolean) => {
  AppStorage.setOrCreate('screenAwakeApplied', enabled);
  AppStorage.setOrCreate('screenAwakeError', '');
});
