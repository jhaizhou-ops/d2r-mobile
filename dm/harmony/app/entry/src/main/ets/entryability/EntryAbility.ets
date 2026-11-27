// Full-screen landscape window, live safe areas and foreground-only screen wake lock.
import { AbilityConstant, UIAbility, Want, bundleManager } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hiAppEvent, hilog } from '@kit.PerformanceAnalysisKit';
import { deviceInfo } from '@kit.BasicServicesKit';
import { fileIo } from '@kit.CoreFileKit';
import { buffer } from '@kit.ArkTS';
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
    this.watchCrashes();
  }
  // 闪退报告：系统在下次启动时把上次的崩溃 / 卡死事件（含故障日志文件）交给本应用；存成 files/dm/last-crash.txt，游戏页给「分享」入口。
  // 只在本机，不上传；玩家自己选发到哪。
  // Crash report: on the next launch the system hands this app its previous crash / freeze event (with the fault log files); saved as
  // files/dm/last-crash.txt and offered for sharing on the game page. Local only, nothing is uploaded; the player picks where it goes.
  private watchCrashes(): void {
    const dir = this.context.filesDir + '/dm';
    try {
      hiAppEvent.addWatcher({
        name: 'dmCrash',
        appEventFilters: [{ domain: hiAppEvent.domain.OS, names: [hiAppEvent.event.APP_CRASH, hiAppEvent.event.APP_FREEZE] }],
        onReceive: (domain: string, groups: Array<hiAppEvent.AppEventGroup>) => {
          let text = '';
          for (const g of groups) {
            for (const ev of g.appEventInfos) {
              text += `== ${ev.name} (${domain})\n${JSON.stringify(ev.params)}\n`;
              const logs = (ev.params as Record<string, Object>)['external_log'] as string[] | undefined;
              for (const path of logs ?? []) {
                try {
                  const size = fileIo.statSync(path).size, keep = Math.min(size, 512 * 1024), buf = new ArrayBuffer(keep);
                  const f = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY);
                  fileIo.readSync(f.fd, buf, { offset: size - keep, length: keep }); fileIo.closeSync(f);
                  text += `-- ${path} (${size} bytes)\n${buffer.from(buf).toString('utf-8')}\n`;
                } catch (e) { text += `-- ${path}: ${JSON.stringify(e)}\n`; }
              }
            }
          }
          if (!text) { return; }
          try {
            fileIo.mkdirSync(dir, true);
            const out = fileIo.openSync(dir + '/last-crash.txt', fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
            fileIo.writeSync(out.fd, text); fileIo.closeSync(out);
            AppStorage.setOrCreate('crashReport', dir + '/last-crash.txt');
            hilog.warn(0, 'dm', 'previous crash report saved (%{public}d chars)', text.length);
          } catch (e) { hilog.warn(0, 'dm', 'crash report save failed %{public}s', JSON.stringify(e)); }
        }
      });
    } catch (e) { hilog.warn(0, 'dm', 'crash watcher: %{public}s', JSON.stringify(e)); }
  }
  onWindowStageCreate(windowStage: window.WindowStage): void {
    this.stage = windowStage;
    windowStage.getMainWindow().then(async (win: window.Window) => {
      if (this.stage !== windowStage) { return; }
      this.mainWindow = win;
      win.on('avoidAreaChange', this.avoidChanged);
      win.on('windowSizeChange', this.sizeChanged);
      // 每一步单独兜底：电脑（2in1）不支持转向 / 系统栏这几项，一项失败不能让后面的（常亮、安全区）也跳过。
      // 电脑：自由窗口最大化成沉浸全屏（游戏开机后出图尺寸不再变大，要一开始就是整屏）。
      // Each step has its own fallback: a PC (2in1) does not support orientation / system bars, and one failure must not skip the rest
      // (wake lock, safe area). PC: maximize the free window into immersive full screen (the game's output size cannot grow after boot).
      const step = async (what: string, f: () => Promise<void>): Promise<void> => {
        try { await f(); } catch (e) { hilog.warn(0, 'dm', 'window %{public}s: %{public}s', what, JSON.stringify(e)); }
      };
      if (deviceInfo.deviceType === '2in1') {
        await step('maximize', () => win.maximize(window.MaximizePresentation.ENTER_IMMERSIVE));
      } else {
        await step('full screen', () => win.setWindowLayoutFullScreen(true));
        await step('system bars', () => win.setWindowSystemBarEnable([]));
        await step('orientation', () => win.setPreferredOrientation(window.Orientation.AUTO_ROTATION_LANDSCAPE));
      }
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
