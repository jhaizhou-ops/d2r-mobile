import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ImmersiveState, ImmersiveTarget, restoreWindowProperties } from './GameLayout';

class WindowTarget implements ImmersiveTarget {
  private target: window.Window;
  private saved: boolean = false;
  private orientation: window.Orientation = window.Orientation.UNSPECIFIED;
  private layout: boolean = false;
  constructor(target: window.Window) { this.target = target; }
  async apply(enabled: boolean): Promise<void> {
    if (enabled) {
      if (!this.saved) {
        this.orientation = this.target.getPreferredOrientation();
        this.layout = this.target.getWindowProperties().isLayoutFullScreen;
        this.saved = true;
      }
      await this.target.setPreferredOrientation(window.Orientation.AUTO_ROTATION_LANDSCAPE);
      await this.target.setWindowLayoutFullScreen(true);
    } else if (this.saved) {
      // We retain system bar visibility; only restore properties actually changed by this feature.
      await restoreWindowProperties(() => this.target.setWindowLayoutFullScreen(this.layout),
        () => this.target.setPreferredOrientation(window.Orientation.AUTO_ROTATION_LANDSCAPE));   // release: the app stays landscape (no portrait page on return)
      this.saved = false;
    }
  }
}

class GameWindow {
  private target: window.Window | undefined = undefined;
  private state: ImmersiveState = new ImmersiveState((enabled: boolean) => {
    AppStorage.setOrCreate('gameFullscreen', enabled);
    AppStorage.setOrCreate('gameFullscreenRequested', enabled);
  }, (message: string) => {
    AppStorage.setOrCreate('gameWindowError', `窗口调整失败：${message}`);
    hilog.warn(0, 'XblWindow', '%{public}s', message);
  });
  private sizeChanged = (size: window.Size): void => { this.refresh(); };
  private areaChanged = (options: window.AvoidAreaOptions): void => { this.refresh(); };
  bind(target: window.Window): void {
    this.unbind(); this.target = target; this.state.bind(new WindowTarget(target));
    target.on('windowSizeChange', this.sizeChanged);
    target.on('avoidAreaChange', this.areaChanged);
    this.refresh();
  }
  unbind(): void {
    if (this.target) {
      try { this.target.off('windowSizeChange', this.sizeChanged); } catch (error) { this.log(error); }
      try { this.target.off('avoidAreaChange', this.areaChanged); } catch (error) { this.log(error); }
    }
    this.target = undefined; this.state.unbind();
  }
  request(enabled: boolean): void {
    AppStorage.setOrCreate('gameFullscreenRequested', enabled);
    AppStorage.setOrCreate('gameWindowError', ''); this.state.request(enabled);
  }
  private log(error: Object): void { hilog.warn(0, 'XblWindow', '%{public}s', String(error)); }
  private refresh(): void {
    if (!this.target) { return; }
    let left=0, top=0, right=0, bottom=0;
    const types: window.AvoidAreaType[] = [window.AvoidAreaType.TYPE_SYSTEM,
      window.AvoidAreaType.TYPE_CUTOUT, window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR];
    types.forEach((type: window.AvoidAreaType) => {
      try {
        const area = this.target!.getWindowAvoidArea(type);
        if (area.visible) {
          left=Math.max(left,area.leftRect.width); top=Math.max(top,area.topRect.height);
          right=Math.max(right,area.rightRect.width); bottom=Math.max(bottom,area.bottomRect.height);
        }
      } catch (error) { this.log(error); }
    });
    AppStorage.setOrCreate('gameInsetLeftPx',left); AppStorage.setOrCreate('gameInsetTopPx',top);
    AppStorage.setOrCreate('gameInsetRightPx',right); AppStorage.setOrCreate('gameInsetBottomPx',bottom);
    AppStorage.setOrCreate('padReset', (AppStorage.get<number>('padReset') ?? 0) + 1);
  }
}
export const gameWindow = new GameWindow();
