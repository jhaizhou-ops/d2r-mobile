// Full-screen landscape window, live safe areas and foreground-only screen wake lock.
import { AbilityConstant, UIAbility, Want, bundleManager } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import dm from 'libdm.so';
import { refreshForms } from '../entryformability/EntryFormAbility';

export default class EntryAbility extends UIAbility {
  private mainWindow: window.Window | undefined = undefined;
  private stage: window.WindowStage | undefined = undefined;
  private foreground: boolean = false;
  private awakeQueue: Promise<void> = Promise.resolve();
  private avoidChanged = (options: window.AvoidAreaOptions): void => { this.refreshSafeArea(); };
  private sizeChanged = (size: window.Size): void => { this.refreshSafeArea(); };
  private stageChanged = (ev: window.WindowStageEventType): void => {
    const map = new Map<window.WindowStageEventType, number>([
      [window.WindowStageEventType.SHOWN, 2], [window.WindowStageEventType.ACTIVE, 3],
      [window.WindowStageEventType.INACTIVE, 0], [window.WindowStageEventType.HIDDEN, 1]]);
    const v = map.get(ev);
    if (v !== undefined) {
      this.foreground = v === 2 || v === 3; this.syncAwake(); dm.focus(v);
      if (this.foreground) { this.refreshSafeArea(); }
    }
  };
  private syncAwake(): void {
    const win = this.mainWindow;
    if (!win) { return; }
    this.awakeQueue = this.awakeQueue.then(() => win.setWindowKeepScreenOn(this.mainWindow === win && this.foreground))
      .catch((e: Error) => { hilog.warn(0, 'dm', 'screen wake: %{public}s', e.message); });
  }
  private refreshSafeArea(): void {
    const win = this.mainWindow;
    if (!win) { return; }
    try {
      const cutout = win.getWindowAvoidArea(window.AvoidAreaType.TYPE_CUTOUT);
      const navigation = win.getWindowAvoidArea(window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR);
      AppStorage.setOrCreate('safeLeftPx', Math.max(cutout.leftRect.width, navigation.leftRect.width));
      AppStorage.setOrCreate('safeRightPx', Math.max(cutout.rightRect.width, navigation.rightRect.width));
      AppStorage.setOrCreate('safeBottomPx', Math.max(cutout.bottomRect.height, navigation.bottomRect.height));
    } catch (e) { hilog.warn(0, 'dm', 'safe area update failed'); }
  }
  onForeground(): void { this.foreground = true; this.syncAwake(); this.refreshSafeArea(); }
  onBackground(): void { this.foreground = false; this.syncAwake(); refreshForms(this.context.filesDir); }
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    const bundle = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT);
    dm.appIdentity(bundle.name, bundle.versionName);
    AppStorage.setOrCreate('filesDir', this.context.filesDir);
    AppStorage.setOrCreate('lang', this.context.config.language ?? 'zh');
  }
  onWindowStageCreate(windowStage: window.WindowStage): void {
    this.stage = windowStage;
    windowStage.getMainWindow().then(async (win: window.Window) => {
      if (this.stage !== windowStage) { return; }
      this.mainWindow = win;
      win.on('avoidAreaChange', this.avoidChanged);
      win.on('windowSizeChange', this.sizeChanged);
      await win.setWindowLayoutFullScreen(true);
      await win.setWindowSystemBarEnable([]);
      await win.setPreferredOrientation(window.Orientation.AUTO_ROTATION_LANDSCAPE);
      this.syncAwake(); this.refreshSafeArea();
    }).catch((e: Error) => hilog.warn(0, 'dm', 'window setup: %{public}s', e.message));
    windowStage.on('windowStageEvent', this.stageChanged);
    windowStage.loadContent('pages/Index');
  }
  onWindowStageDestroy(): void {
    this.foreground = false; this.syncAwake();
    if (this.mainWindow) {
      this.mainWindow.off('avoidAreaChange', this.avoidChanged);
      this.mainWindow.off('windowSizeChange', this.sizeChanged);
    }
    this.stage?.off('windowStageEvent', this.stageChanged);
    this.mainWindow = undefined; this.stage = undefined;
  }
}
