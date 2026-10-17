import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import host from 'libxblhost.so';
import { gameWindow } from '../lifecycle/GameWindow';
import { SaveSync } from '../data/SaveSync';
import { gameScreenAwake, WindowAwakeTarget } from '../lifecycle/GameScreenAwake';

export default class EntryAbility extends UIAbility {
  onForeground(): void { gameScreenAwake.setForeground(true);host.setGameFocus(1); }
  onBackground(): void {
    gameScreenAwake.setForeground(false); gameWindow.request(false);
    host.setGameTouch(false,0,0);host.setGameFocus(3);
    AppStorage.setOrCreate('padReset', (AppStorage.get<number>('padReset') ?? 0) + 1);
    SaveSync.run(this.context, false);   // upload committed saves (never changes local saves while the game may run)
  }
  onWindowStageCreate(windowStage: window.WindowStage): void {
    PersistentStorage.persistProp('testKeepScreenOn', false);
    gameScreenAwake.setTestKeepScreenOn(AppStorage.get<boolean>('testKeepScreenOn') ?? false);
    try { gameScreenAwake.bind(new WindowAwakeTarget(windowStage.getMainWindowSync())); gameWindow.bind(windowStage.getMainWindowSync()); }
    catch (error) { hilog.warn(0, 'XblWindow', 'Main window unavailable: %{public}s', String(error)); }
    windowStage.loadContent('pages/Index', (error) => {
      if (error.code) {
        hilog.error(0x0000, 'XblBootstrap', 'loadContent failed: %{public}s', JSON.stringify(error));
      }
    });
  }
  onWindowStageDestroy(): void { gameWindow.unbind(); gameScreenAwake.unbind(); }
  onDestroy(): void {
    gameWindow.unbind(); host.cancelGamePresentation();
    gameScreenAwake.setGameActive(false);
    gameScreenAwake.setForeground(false); gameWindow.request(false);
    gameScreenAwake.unbind();
  }
}
