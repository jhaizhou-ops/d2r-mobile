// 游戏页：XComponent（原生 Vulkan 画面，libdm.so）+ 透明触控层 + 起名输入框 + 启动错误提示。
// 数据目录沿用老前端在同一应用沙箱里已导入的游戏数据（files/game-data）；新核心的存档单独放 files/save，不碰老存档 files/saves。
// Game page: XComponent (native Vulkan output from libdm.so) + transparent touch overlay + naming dialog + boot error text.
// Game data reuses what the old front end already imported into this app's sandbox (files/game-data); the new core keeps its saves in
// files/save, separate from the old saves in files/saves.
import dm from 'libdm.so';
import { fileIo } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { display } from '@kit.ArkUI';
import { IosOverlay } from '../components/IosOverlay';
import { importResources } from '../data/ResourceImport';
import { exportSaves, importSaves, selectFolder } from '../data/SaveTransfer';
import { PadInput } from '../data/PadInput';
import { ShellText, shellText } from '../data/ShellText';

interface InputRequest { id: number; title: string; sub: string; guide: string; initial: string; maxChars: number; nameRule: number; numericMax: number }

@Entry
@Component
struct Index {
  @State started: boolean = false;
  @State error: string = '';
  @State request: InputRequest | null = null;
  @State text: string = '';
  @State inputError: string = '';
  @State fps: string = '';
  @State ready: boolean = false;
  @State busy: boolean = false;
  @State needsRestart: boolean = false;
  @State showFps: boolean = false;
  @State importing: string = '';
  @State notice: string = '';
  private pad: PadInput = new PadInput();
  private fpsTimer: number = -1;
  private lastPresents: number = 0;
  private lastFpsAt: number = 0;
  private callbacksReady: boolean = false;
  @StorageProp('safeLeftPx') safeLeftPx: number = 0;
  @StorageProp('safeRightPx') safeRightPx: number = 0;
  @StorageProp('safeBottomPx') safeBottomPx: number = 0;
  private files: string = AppStorage.get<string>('filesDir') ?? '';
  private lang: string = AppStorage.get<string>('lang') ?? 'zh';

  private t: ShellText = shellText(this.lang);
  private dataDir(): string {
    const base = this.files + '/game-data';
    try { if (fileIo.accessSync(base + '/data/.build.config') || fileIo.accessSync(base + '/data/build.config')) { return base + '/data'; } } catch (e) { }
    return base;
  }
  aboutToAppear(): void {
    this.showFps = fileIo.accessSync(this.files + '/show-fps');
    try { this.pad.listen(); } catch (error) { hilog.warn(0xD2E0, 'dm', '手柄热插拔监听不可用'); }
  }
  aboutToDisappear(): void {
    this.stopFps();
    try { this.pad.stop(); } catch (error) { this.pad.reset(); }
  }
  private focusGame(): void {
    if (this.started && this.request === null) { this.getUIContext().getFocusController().requestFocus('dm-game'); }
  }
  private stopFps(): void {
    if (this.fpsTimer >= 0) { clearInterval(this.fpsTimer); this.fpsTimer = -1; }
    this.fps = '';
  }
  private toggleFps(enabled: boolean): void {
    const path = this.files + '/show-fps';
    try {
      if (enabled) { fileIo.closeSync(fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY)); }
      else if (fileIo.accessSync(path)) { fileIo.unlinkSync(path); }
      this.showFps = enabled;
    } catch (error) { this.notice = this.t('dmHarmonyFpsSaveFailed'); }
  }

  private boot(): void {
    if (!this.ready || this.busy || this.started || this.needsRestart) { return; }
    this.error = '';
    this.notice = '';
    if (!this.callbacksReady) {
      dm.setCallbacks(
        (id: number, title: string, sub: string, guide: string, initial: string, maxChars: number, nameRule: number, numericMax: number) => {
          this.pad.reset(); this.text = initial; this.inputError = '';
          this.request = { id, title, sub, guide, initial, maxChars, nameRule, numericMax };
        },
        (code: number) => {
          this.started = false;
          this.needsRestart = true;
          this.stopFps();
          this.error = `${this.t('dmAndroidStartFailed', String(code))} ${this.t('dmHarmonyRestartApp')}`;
          if (code === -7) { this.error += `\n${this.t('dmHarmonyExecProbe', dm.execProbe())}`; }
        });
      this.callbacksReady = true;
    }
    try { display.getDefaultDisplaySync() && dm.refresh(display.getDefaultDisplaySync().refreshRate); } catch (e) { }
    const rc = dm.start(this.files, this.files, this.dataDir(), this.lang.startsWith('zh') ? 'zh-Hans' : 'en-US');
    if (rc > 0) {
      this.started = true;
      this.focusGame();
      if (this.showFps) {
        this.lastPresents = dm.presentCount();
        this.lastFpsAt = Date.now();
        this.fpsTimer = setInterval(() => {
          const n = dm.presentCount(), t = Date.now();
          this.fps = this.t('dmHarmonyFpsValue', String(Math.round((n - this.lastPresents) * 1000 / Math.max(1, t - this.lastFpsAt))));
          this.lastPresents = n;
          this.lastFpsAt = t;
        }, 1000);
      }
    } else {
      this.error = this.t('dmAndroidStartFailed', String(rc));
      if ([-2, -5].includes(rc)) { this.error += '\n' + this.t('dmHarmonyNeedData'); }
      if ([-3, -4, -9, -10, -11, -12].includes(rc)) {
        this.error += '\n' + this.t('dmHarmonyNeedPack');
      }
    }
  }
  // 调试版内置运行包（out / shaders / overlay / ui-art 压成 dm-pack.zip，见 entry/build-profile.json5 的 default 目标和 scripts/stage_debug_pack.sh）：
  // 首次启动或包版本变了，解压到 files/dm（游戏照常从那里读），玩家只需导入游戏的 data。发布版没有内置包，直接跳过
  // Debug builds bundle the runtime pack (out / shaders / overlay / ui-art zipped as dm-pack.zip, see the default target in entry/build-profile.json5 and
  // scripts/stage_debug_pack.sh): on first launch or when the pack version changed it is unpacked into files/dm (where the game reads it), so players only import the
  // game's data. Release builds have no bundled pack and skip this
  private async preparePack(): Promise<void> {
    const src = getContext(this).resourceDir + '/dm';
    let want = '', have = '';
    try { want = fileIo.readTextSync(src + '/pack-version.txt').trim(); } catch (e) { return; }
    try { have = fileIo.readTextSync(this.files + '/dm/pack-version.txt').trim(); } catch (e) { }
    if (want === have) { return; }
    try {
      if (!fileIo.accessSync(this.files + '/dm')) { fileIo.mkdirSync(this.files + '/dm', true); }   // 已存在时 mkdirSync 抛 13900015（以前包一更新就在这里失败）| mkdirSync throws 13900015 when it exists (pack updates used to fail right here)
      try { fileIo.unlinkSync(this.files + '/dm/pack-version.txt'); } catch (e) { }   // 解压到一半被杀：下次重来 | killed halfway: unpack again next time
      this.importing = this.t('dmHarmonyUnpacking');
      // 先解到临时目录再逐项替换：decompressFile 不覆盖已有文件（13900015），直接解到 files/dm 时包一更新就失败、旧包一直留着。
      // 只替换包里有的顶层项（out / shaders / overlay / ui-art / 管线种子），本机清单、缓存、env、标记文件不动
      // Unpack into a temporary folder, then replace entry by entry: decompressFile does not overwrite existing files (13900015), so unpacking straight into files/dm failed
      // on every pack update and the old pack stayed. Only top-level entries present in the pack (out / shaders / overlay / ui-art / pipeline seed) are replaced; the device
      // list, caches, env and marker files are left alone
      const tmp = this.files + '/dm-pack-tmp';
      try { fileIo.rmdirSync(tmp); } catch (e) { }
      if (!fileIo.accessSync(tmp)) { fileIo.mkdirSync(tmp, true); }
      await zlib.decompressFile(src + '/dm-pack.zip', tmp);
      for (const name of fileIo.listFileSync(tmp)) {
        const dst = this.files + '/dm/' + name;
        try { if (fileIo.statSync(dst).isDirectory()) { fileIo.rmdirSync(dst); } else { fileIo.unlinkSync(dst); } } catch (e) { }
        fileIo.renameSync(tmp + '/' + name, dst);
      }
      try { fileIo.rmdirSync(tmp); } catch (e) { }
      fileIo.copyFileSync(src + '/pack-version.txt', this.files + '/dm/pack-version.txt');
      hilog.info(0xD2E0, 'XblPack', 'runtime pack %{public}s unpacked', want);
    } catch (e) { hilog.error(0xD2E0, 'XblPack', 'pack unpack failed: %{public}s', JSON.stringify(e)); }
    this.importing = '';
  }
  private async transfer(action: string): Promise<void> {
    if (this.busy || this.started) { return; }
    this.busy = true;
    this.notice = '';
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    try {
      if (action === 'export') {
        this.notice = await exportSaves(context, this.t);
      } else {
        const uri = await selectFolder(context);
        if (!uri) { return; }
        if (action === 'resources') {
          this.notice = await importResources(this.files, uri, this.t, (message: string) => { this.importing = message; });
          this.error = '';
        } else {
          this.importing = this.t('dmHarmonySaveImporting');
          this.notice = await importSaves(this.files, uri, this.t);
        }
      }
    } catch (error) {
      this.notice = this.t('dmHarmonyTransferError', (error as Error).message ?? String(error));
    } finally {
      this.importing = '';
      this.busy = false;
    }
  }
  private finishInput(accepted: boolean): void {
    if (!this.request) { return; }
    let result = accepted ? this.text : '';
    if (accepted && this.request.numericMax >= 0) {
      const digits = result.replace(/[^0-9]/g, '');
      result = digits ? String(Math.min(this.request.numericMax, Number(digits))) : '';
    } else if (accepted && this.request.nameRule && result) {
      let name = '', bytes = 0, separator = false;
      for (let i = 0; i < result.length; i++) {
        const char = result.charAt(i);
        const han = /[\u3400-\u4dbf\u4e00-\u9fff]/.test(char);
        const mark = char === '-' || char === '_';
        if (!han && !/[a-zA-Z]/.test(char) && (!mark || separator)) { continue; }
        if (bytes + (han ? 3 : 1) > 15) { continue; }
        bytes += han ? 3 : 1;
        separator = separator || mark;
        name += char;
      }
      if (bytes < 2) { this.inputError = this.t('dmNameTooShort'); return; }
      if (/^[-_]|[-_]$/.test(name)) { this.inputError = this.t('dmNameEdge'); return; }
      result = name;
    }
    dm.textResult(this.request.id, accepted, result);
    this.request = null;
    this.focusGame();
  }

  build() {
    Stack() {
      XComponent({ id: 'dm', type: XComponentType.SURFACE, libraryname: 'dm' })
        .width('100%').height('100%')
        .onLoad(() => { this.preparePack().then(() => { this.ready = true; }); })
      IosOverlay({
        inputEnabled: this.started && this.request === null,
        overlayRoot: this.files + '/dm/overlay',
        artRoot: this.files + '/dm/ui-art',
        safeLeft: px2vp(this.safeLeftPx), safeRight: px2vp(this.safeRightPx), safeBottom: px2vp(this.safeBottomPx)
      }).width('100%').height('100%')
      if (this.fps) {
        Text(this.fps).fontColor('#ff66dd66').fontSize(14).backgroundColor('#66000000').padding({ left: 8, right: 8, top: 2, bottom: 2 })
          .borderRadius(6).position({ x: '50%', y: 6 }).translate({ x: '-50%' }).hitTestBehavior(HitTestMode.Transparent)
      }
      if (!this.started) {
        Scroll() {
          Column({ space: 12 }) {
            Text('D2R Mobile').fontColor('#ffcdb98c').fontSize(24).fontWeight(FontWeight.Medium)
            if (this.error) { Text(this.error).fontColor('#ffc6a482').fontSize(14) }
            if (this.importing || this.notice) {
              Text(this.importing || this.notice).fontColor('#ffd8d0bc').fontSize(14)
            }
            Row({ space: 12 }) {
              Button(this.t('dmHarmonyStart')).enabled(this.ready && !this.busy && !this.needsRestart).onClick(() => this.boot())
              Button(this.t('dmHarmonyResources')).enabled(this.ready && !this.busy).onClick(() => { this.transfer('resources'); })
            }
            Row({ space: 12 }) {
              Button(this.t('dmHarmonySaveImport')).enabled(this.ready && !this.busy).onClick(() => { this.transfer('import'); })
              Button(this.t('dmHarmonySaveExport')).enabled(this.ready && !this.busy).onClick(() => { this.transfer('export'); })
              Checkbox({ name: 'fps' }).select(this.showFps).enabled(this.ready && !this.busy)
                .onChange((enabled: boolean) => this.toggleFps(enabled))
                .accessibilityText(this.t('dmHarmonyShowFps'))
              Text(this.t('dmHarmonyShowFps')).fontColor('#ffd8d0bc').fontSize(14)
            }
          }.padding(20).width('100%')
        }.width('82%').constraintSize({ maxWidth: 580, maxHeight: '88%' }).backgroundColor('#ee141210').borderRadius(12)
      }
      if (this.request) {
        Column({ space: 12 }) {
          Text(this.request.title || this.t('dmInputTitle')).fontColor('#ffcdb98c').fontSize(18)
          if (this.request.sub) { Text(this.request.sub).fontColor('#ffd8d0bc').fontSize(14) }
          TextInput({ text: this.text, placeholder: this.request.nameRule ? this.t('dmNameBlankRandom') : this.request.guide })
            .maxLength(this.request.maxChars > 0 ? this.request.maxChars : 64)
            .type(this.request.numericMax >= 0 ? InputType.Number : InputType.Normal)
            .onChange((v: string) => { this.text = v; this.inputError = ''; })
            .onSubmit(() => this.finishInput(true))
          if (this.inputError) { Text(this.inputError).fontColor('#ffc6a482').fontSize(14) }
          Row({ space: 24 }) {
            Button(this.t('dmCancel')).onClick(() => this.finishInput(false))
            Button(this.t('dmConfirm')).onClick(() => this.finishInput(true))
          }
        }.padding(24).backgroundColor('#ee141210').borderRadius(12).width('60%')
      }
    }.width('100%').height('100%').backgroundColor('#000000')
      .id('dm-game').focusable(true)
      .onBlur(() => { this.pad.reset(); })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down && this.started && !this.request) { this.pad.reset(); this.focusGame(); }
      })
      .onKeyEventDispatch((event: KeyEvent) => this.started && this.request === null && this.pad.key(event))
      .onFocusAxisEvent((event: FocusAxisEvent) => { if (this.started && this.request === null) { this.pad.axes(event); } })
  }
}
