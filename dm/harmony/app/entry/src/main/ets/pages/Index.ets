// 游戏页：XComponent（原生 Vulkan 画面，libdm.so）+ 透明触控层 + 起名输入框 + 启动错误提示。
// 数据目录沿用老前端在同一应用沙箱里已导入的游戏数据（files/game-data）；新核心的存档单独放 files/save，不碰老存档 files/saves。
// Game page: XComponent (native Vulkan output from libdm.so) + transparent touch overlay + naming dialog + boot error text.
// Game data reuses what the old front end already imported into this app's sandbox (files/game-data); the new core keeps its saves in
// files/save, separate from the old saves in files/saves.
import dm from 'libdm.so';
import { fileIo, fileUri, picker } from '@kit.CoreFileKit';
import { zlib, BusinessError } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { display, window } from '@kit.ArkUI';
import { IosOverlay } from '../components/IosOverlay';
import { importGamePack, PackError, PackProgress, PackResult, PACK_CANCELLED } from '../data/GamePackImport';
import { UI_TEXT_EN, UI_TEXT_ZH } from '../generated/UiText';

interface InputRequest { id: number; title: string; sub: string; guide: string; initial: string; maxChars: number; nameRule: number; numericMax: number }

@Entry
@Component
struct Index {
  @State started: boolean = false;
  @State error: string = '';
  @State request: InputRequest | null = null;
  @State text: string = '';
  @State fps: string = '';
  @State showFps: boolean = false;
  @State toolsOpen: boolean = false;
  @State logTail: string = '';
  @State showLog: boolean = false;
  @State developerExperimentsOpen: boolean = false;
  @State experimentsOpen: boolean = false;
  @State experimentBits: number = 0;
  @State experimentsReady: boolean = false;
  @State experimentError: string = '';
  @State experimentSaved: boolean = false;
  @State canImport: boolean = false;
  @State importing: string = '';
  @State packBusy: boolean = false;
  @State folderBusy: boolean = false;
  private packCancel: boolean = false;
  private bundledRuntime: boolean = false;
  private fpsTimer: number = -1;
  private lastPresents: number = 0;
  private lastFpsAt: number = 0;
  @StorageProp('safeLeftPx') safeLeftPx: number = 0;
  @StorageProp('safeRightPx') safeRightPx: number = 0;
  @StorageProp('safeBottomPx') safeBottomPx: number = 0;
  private files: string = AppStorage.get<string>('filesDir') ?? '';
  private lang: string = AppStorage.get<string>('lang') ?? 'zh';

  private t(key: string): string { return (this.lang.startsWith('zh') ? UI_TEXT_ZH : UI_TEXT_EN)[key] ?? key; }
  private dataDir(): string {
    const base = this.files + '/game-data';
    try { if (fileIo.accessSync(base + '/data/.build.config') || fileIo.accessSync(base + '/data/build.config')) { return base + '/data'; } } catch (e) { }
    return base;
  }
  private boot(): void {
    dm.setCallbacks(
      (id: number, title: string, sub: string, guide: string, initial: string, maxChars: number, nameRule: number, numericMax: number) => {
        this.toolsOpen = false; this.showLog = false;
        this.text = initial; this.request = { id, title, sub, guide, initial, maxChars, nameRule, numericMax };
      },
      (code: number) => {
        if ([-9, -10, -13].indexOf(code) >= 0) { this.startupFailure(code); return; }
        this.started = false; this.canImport = false;
        if (this.fpsTimer >= 0) { clearInterval(this.fpsTimer); this.fpsTimer = -1; }
        const message = this.t('dmAndroidStartFailed') || '启动失败';
        this.error = message.indexOf('%d') >= 0 ? message.replace('%d', String(code)) : `${message} (${code})`;
      });
    try { display.getDefaultDisplaySync() && dm.refresh(display.getDefaultDisplaySync().refreshRate); } catch (e) { }
    const rc = dm.start(this.files, this.files, this.dataDir(), this.lang.startsWith('zh') ? 'zh-Hans' : 'en-US');
    if (rc > 0) {
      this.started = true;
      // 调试帧率（顶部居中，同老版）：每秒取出图帧数差 | Debug FPS (top center, as the old build): presented-frame difference per second
      this.lastPresents = dm.presentCount(); this.lastFpsAt = Date.now();
      this.fpsTimer = setInterval(() => {
        const n = dm.presentCount(), t = Date.now();
        this.fps = `${Math.round((n - this.lastPresents) * 1000 / Math.max(1, t - this.lastFpsAt))} FPS`;
        this.lastPresents = n; this.lastFpsAt = t;
      }, 1000);
    } else {
      this.startupFailure(rc);
    }
  }
  private startupFailure(rc: number): void {
    this.started = false;
    if (this.fpsTimer >= 0) { clearInterval(this.fpsTimer); this.fpsTimer = -1; }
      const dataMissing = rc === -2 || rc === -5;
      this.canImport = dataMissing || (!this.bundledRuntime && [-3, -4, -9, -10, -11, -12].indexOf(rc) >= 0);
      const zh = this.lang.startsWith('zh');
      let detail = '';
      if (dataMissing) {
        detail = zh ? '请选择游戏原始 Data 文件夹，完整保留里面的文件和目录。' : 'Select the original game Data folder with all its contents.';
      } else if (this.bundledRuntime || rc === -13) {
        detail = zh ? '应用内置运行组件检查失败。请保留已导入的游戏数据和存档，向开发者反馈此错误码；无需导入 dm 或重新复制 Data。' : 'Bundled runtime validation failed. Keep your game data and saves and report this error code. Do not import dm or recopy Data.';
      } else if (this.canImport) {
        detail = zh ? '此空壳需要与应用版本配套的运行包。' : 'This shell requires a matching runtime pack.';
      }
      this.error = `${zh ? '游戏启动准备失败' : 'Game startup failed'} (${rc})\n${detail}`;
      hilog.error(0xD2E0, 'XblBoot', 'startup failed code=%{public}d bundled=%{public}s', rc, String(this.bundledRuntime));
  }
  // selfuse 邀请版（包括 Release）内置运行组件；公共空壳才需要外部运行包。
  private async preparePack(): Promise<boolean> {
    const src = getContext(this).resourceDir + '/dm';
    let want = '', have = '';
    try { want = fileIo.readTextSync(src + '/pack-version.txt').trim(); } catch (e) {
      try { this.bundledRuntime = fileIo.accessSync(src + '/dm-pack.zip'); } catch (ignored) { }
      if (!this.bundledRuntime) { return true; }
      this.error = '内置组件版本信息缺失 (PACK-MARKER)，请反馈开发者；请保留 Data 和存档。'; return false;
    }
    this.bundledRuntime = true;
    if (!want) { this.error = '内置组件版本信息为空 (PACK-MARKER)，请反馈开发者。'; return false; }
    try { have = fileIo.readTextSync(this.files + '/dm/pack-version.txt').trim(); } catch (e) { }
    if (want === have) { return true; }
    try {
      if (!fileIo.accessSync(this.files + '/dm')) { fileIo.mkdirSync(this.files + '/dm', true); }   // 已存在时 mkdirSync 抛 13900015（以前包一更新就在这里失败）| mkdirSync throws 13900015 when it exists (pack updates used to fail right here)
      try { fileIo.unlinkSync(this.files + '/dm/pack-version.txt'); } catch (e) { }   // 解压到一半被杀：下次重来 | killed halfway: unpack again next time
      this.importing = '首次启动，正在解压游戏组件（约 1 分钟）…';
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
    } catch (e) {
      hilog.error(0xD2E0, 'XblPack', 'pack unpack failed: %{public}s', JSON.stringify(e));
      this.error = `内置组件解压失败 (PACK-UNPACK/${(e as BusinessError).code})，请检查存储空间后重启应用；请保留 Data 和存档。`;
      this.importing = ''; this.canImport = false; return false;
    }
    this.importing = ''; return true;
  }
  // 导入单文件游戏包（.d2r，电脑上 dm/tools/d2r_gamepack.py 生成；大包拆成 .001、.002… 时一起选中）。见 data/GamePackImport.ets。
  // Import a single-file game package (.d2r, made on the PC by dm/tools/d2r_gamepack.py; select all .001, .002, ... parts of a split one). See data/GamePackImport.ets.
  private packMessage(e: PackError): string {
    const zh = this.lang.startsWith('zh'), d = e.detail;
    switch (e.code) {
      case 'no_space': { const p = d.split(' '); return zh ? `手机空间不够：包 ${p[0]} GB，剩余 ${p[1]} GB（还要留 0.5 GB）` : `Not enough storage: package ${p[0]} GB, free ${p[1]} GB (0.5 GB more is needed)`; }
      case 'not_pack': return zh ? '这不是暗黑2游戏包（.d2r）。请选电脑上 d2r_gamepack.py 生成的文件。' : 'This is not a Diablo II game package (.d2r). Pick the file made by d2r_gamepack.py.';
      case 'truncated': return zh ? '游戏包不完整（文件被截断或分片没选全），请重新拷贝后再导入。' : 'The package is incomplete (truncated, or not all parts selected). Copy it again and retry.';
      case 'unsafe_path': case 'unexpected_entry': case 'unsupported_entry': case 'bad_tar': return zh ? `游戏包内容不对（${e.code} ${d}），请重新生成。` : `Unexpected package content (${e.code} ${d}); please rebuild it.`;
      case 'empty_pack': return zh ? '游戏包里没有游戏数据。' : 'The package contains no game data.';
      case 'read_failed': return zh ? `读不了选中的文件：${d}` : `Cannot read the selected file: ${d}`;
      default: return zh ? `导入失败（${e.code}）：${d}，请检查手机存储空间后重新导入` : `Import failed (${e.code}): ${d}. Check storage and retry.`;
    }
  }
  private async importPack(): Promise<void> {
    if (this.packBusy || this.folderBusy) { return; }
    const zh = this.lang.startsWith('zh');
    let uris: string[] = [];
    try {
      const opts = new picker.DocumentSelectOptions(); opts.maxSelectNumber = 32;
      uris = await new picker.DocumentViewPicker(getContext(this) as common.UIAbilityContext).select(opts);
      if (!uris || uris.length === 0) { return; }
    } catch (e) { this.importing = zh ? '打不开文件选择器' : 'Cannot open the file picker'; return; }
    this.packBusy = true; this.packCancel = false; this.importing = zh ? '正在读取游戏包…' : 'Reading the package…';
    let win: window.Window | undefined = undefined;
    try { win = await window.getLastWindow(getContext(this) as common.UIAbilityContext); await win.setWindowKeepScreenOn(true); } catch (e) { }
    try {
      const r: PackResult = await importGamePack(uris, this.files, this.bundledRuntime, (p: PackProgress) => {
        const pct = p.total > 0 ? Math.floor(p.bytes * 100 / p.total) : 0;
        this.importing = zh ? `正在导入游戏包：${pct}%（${Math.floor(p.bytes / 1048576)} / ${Math.floor(p.total / 1048576)} MB，${p.files} 个文件）`
          : `Importing: ${pct}% (${Math.floor(p.bytes / 1048576)} / ${Math.floor(p.total / 1048576)} MB, ${p.files} files)`;
      }, () => this.packCancel);
      hilog.info(0xD2E0, 'XblImport', 'pack imported files=%{public}d data=%{public}d dm=%{public}d skippedDm=%{public}s', r.files, r.dataFiles, r.dmEntries, String(r.skippedDm));
      if (this.started) {
        // 游戏已经在跑：不再启动第二次；新数据下次打开时生效 | The game is already running: no second start; the new data takes effect next launch
        this.importing = zh ? `导入完成（${r.files} 个文件）。请在游戏里保存并退出，再重新打开应用。` : `Imported (${r.files} files). Save and exit in the game, then reopen the app.`;
        setTimeout(() => { if (!this.packBusy) { this.importing = ''; } }, 8000);
      } else {
        this.importing = zh ? '导入完成，正在启动…' : 'Imported, starting…'; this.error = ''; this.canImport = false;
        this.boot(); this.importing = '';
      }
    } catch (e) {
      const pe = e instanceof PackError ? e as PackError : new PackError('write_failed', JSON.stringify(e));
      hilog.warn(0xD2E0, 'XblImport', 'pack import: %{public}s', pe.message);
      this.importing = pe.code === PACK_CANCELLED ? (zh ? '已取消导入。已经完整写入的文件保留，重新导入即可补齐。' : 'Import cancelled. Files already written stay; import again to complete.') : this.packMessage(pe);
    } finally {
      this.packBusy = false;
      if (win) { try { await win.setWindowKeepScreenOn(false); } catch (e) { } }
    }
  }
  // 导入游戏资源：选手机上的「D2R Mobile」文件夹（里面 data/ = 游戏数据，dm/ = 电脑上生成的运行包：out、shaders、overlay、ui-art），
  // 拷进本应用：data → files/romfs/data，dm/* → files/dm/*。拷完重新启动。不删源文件夹。
  // Import game resources: pick the "D2R Mobile" folder on the phone (data/ = game data, dm/ = runtime pack generated on the PC: out, shaders, overlay, ui-art),
  // copied into this app: data → files/romfs/data, dm/* → files/dm/*. Restart when done. The source folder is not deleted.
  private async importResources(): Promise<void> {
    if (this.folderBusy || this.packBusy) { return; }
    this.folderBusy = true;
    try { await this.importFolder(); } finally { this.folderBusy = false; }
  }
  private async importFolder(): Promise<void> {
    try {
      const opts = new picker.DocumentSelectOptions(); opts.selectMode = picker.DocumentSelectMode.FOLDER; opts.maxSelectNumber = 1;
      const uris = await new picker.DocumentViewPicker(getContext(this) as common.UIAbilityContext).select(opts);
      if (!uris || uris.length === 0) { return; }
      const root = uris[0];
      // Accept the data directory itself as well as its parent; bundled components are already prepared.
      // 内置组件已就绪时，可直接选 data 本身，也兼容原来的父目录选择方式。
      const directData = decodeURIComponent(root).replace(/\/+$/, '').split('/').pop()?.toLowerCase() === 'data';
      this.importing = '正在检查文件夹…';
      const parts: string[][] = (directData || this.bundledRuntime) ? [['data', this.files + '/romfs']] : [['data', this.files + '/romfs'], ['dm/out', this.files + '/dm'], ['dm/shaders', this.files + '/dm'], ['dm/overlay', this.files + '/dm'], ['dm/ui-art', this.files + '/dm'], ['dm/derived.tsv', this.files + '/dm'], ['dm/pack_info.txt', this.files + '/dm']];
      let copied = 0;
      const failed: string[] = [];
      for (const part of parts) {
        const src = directData ? root : root + '/' + part[0];
        // 目录已存在时 mkdirSync 抛 13900015（同 preparePack）| mkdirSync throws 13900015 when the directory exists (as in preparePack)
        if (!fileIo.accessSync(part[1])) { fileIo.mkdirSync(part[1], true); }
        const name = part[0].split('/').pop() as string, target = part[1] + '/' + name;
        // 只导入 data 时，源目录可能没有组件信息文件；先拷到临时文件，成功后才替换，保留内置包的信息。
        // A data-only folder has no runtime metadata. Stage file copies before replacing the bundled metadata.
        const isFile = name.indexOf('.') >= 0;
        const copyTarget = isFile ? target + '.importing' : target;
        if (isFile) { try { fileIo.unlinkSync(copyTarget); } catch (e) { } }
        try {
          // copy 的目录目标必须先存在；全新壳没有 out/shaders/overlay/ui-art，不能把目标缺失误判成源缺失。
          // Directory copy needs an existing destination; a fresh shell has none of these runtime directories yet.
          if (name.indexOf('.') < 0 && !fileIo.accessSync(target)) { fileIo.mkdirSync(target, true); }
          await fileIo.copy(src, fileUri.getUriFromPath(copyTarget), {
            progressListener: (p: fileIo.Progress) => {
              this.importing = `正在导入 ${part[0]}：${Math.floor(p.processedSize / 1048576)} / ${Math.floor(p.totalSize / 1048576)} MB`;
            }
          });
          if (isFile) {
            fileIo.copyFileSync(copyTarget, target);
            try { fileIo.unlinkSync(copyTarget); } catch (e) { }
          }
        } catch (e) {
          if (isFile) { try { fileIo.unlinkSync(copyTarget); } catch (cleanupError) { } }
          // 13900002 = 选中的文件夹里没有这一项（只导入 data 或只导入 dm 都可以）；其余错误算导入失败，不启动，免得新旧组件混用
          // 13900002 = the picked folder has no such entry (importing only data or only dm is fine); any other error fails the import without starting, so old and new parts never mix
          const code = (e as BusinessError).code;
          hilog.warn(0xD2E0, 'XblImport', 'copy %{public}s: %{public}s', part[0], JSON.stringify(e));
          if (code !== 13900002) { failed.push(`${part[0]} (${code})`); }
          continue;
        }
        // 有的系统把源目录拷成目标目录下的子目录：拉平 | Some systems copy the source directory as a subdirectory of the target: flatten it
        try { if (fileIo.accessSync(target + '/' + name)) { const inner = target + '/' + name; for (const f of fileIo.listFileSync(inner)) { fileIo.renameSync(inner + '/' + f, target + '/' + f); } fileIo.rmdirSync(inner); } } catch (e) { }
        copied++;
      }
      if (failed.length) { this.importing = `导入没有完成（${failed.join('、')}），请检查手机存储空间后重新导入`; return; }
      if (!copied) { this.importing = this.bundledRuntime ? '没有找到游戏数据，请直接选择 Data 文件夹。' : '没有找到 data 或运行包，请检查选择的文件夹。'; return; }
      this.importing = '导入完成，正在启动…'; this.error = ''; this.canImport = false;
      this.boot(); this.importing = '';
    } catch (e) { this.importing = `导入失败：${JSON.stringify(e)}`; }
  }
  aboutToDisappear(): void {
    if (this.fpsTimer >= 0) { clearInterval(this.fpsTimer); this.fpsTimer = -1; }
  }
  private readLog(): void {
    try {
      const path = this.files + '/dm/boot.log';
      const size = fileIo.statSync(path).size;
      this.logTail = fileIo.readTextSync(path, { offset: Math.max(0, size - 16384), length: Math.min(size, 16384) });
    } catch (e) { this.logTail = this.lang.startsWith('zh') ? '当前没有可读取的游戏日志。' : 'No game log is available yet.'; }
    this.showLog = true;
  }
  private readExperiments(): void {
    this.experimentsReady = false; this.experimentError = ''; this.experimentSaved = false;
    try {
      const flags = dm.experimentFlags();
      if (flags < 0) {
        this.experimentError = `${this.lang.startsWith('zh') ? '读取实验设置失败' : 'Could not read experiment settings'} (${flags})`;
        return;
      }
      this.experimentBits = flags; this.experimentsReady = true;
    } catch (e) { this.experimentError = this.lang.startsWith('zh') ? '读取实验设置失败，请重新打开工具面板。' : 'Could not read experiment settings. Reopen the tools panel.'; }
  }
  private saveExperiment(mask: number, enabled: boolean): void {
    if (!this.experimentsReady) { return; }
    let flags = enabled ? this.experimentBits | mask : this.experimentBits & ~mask;
    if (enabled && (mask === 32 || mask === 64)) { flags = (flags & ~96) | mask; }
    this.writeExperimentFlags(flags);
  }
  private writeExperimentFlags(flags: number): void {
    if (!this.experimentsReady) { return; }
    this.experimentError = ''; this.experimentSaved = false;
    try {
      const result = dm.setExperimentFlags(flags);
      if (result < 0) {
        this.experimentError = `${this.lang.startsWith('zh') ? '保存失败，设置未更改' : 'Save failed; settings unchanged'} (${result})`;
        return;
      }
      this.experimentBits = flags; this.experimentSaved = true;
    } catch (e) { this.experimentError = this.lang.startsWith('zh') ? '保存失败，设置未更改。' : 'Save failed; settings unchanged.'; }
  }
  private finishInput(accepted: boolean): void {
    if (!this.request) { return; }
    dm.textResult(this.request.id, accepted, accepted ? this.text : ''); this.request = null;
  }

  build() {
    Stack() {
      XComponent({ id: 'dm', type: XComponentType.SURFACE, libraryname: 'dm' })
        .width('100%').height('100%')
        .onLoad(() => { this.preparePack().then((ready: boolean) => { if (ready) { this.boot(); } }); })
      IosOverlay({
        inputEnabled: this.started && this.request === null && !this.toolsOpen,
        overlayRoot: this.files + '/dm/overlay',
        artRoot: this.files + '/dm/ui-art',
        safeLeft: px2vp(this.safeLeftPx), safeRight: px2vp(this.safeRightPx), safeBottom: px2vp(this.safeBottomPx)
      }).width('100%').height('100%')
      if (this.showFps && this.fps && !this.toolsOpen && !this.request) {
        Text(this.fps).fontColor('#ff66dd66').fontSize(14).backgroundColor('#66000000').padding({ left: 8, right: 8, top: 2, bottom: 2 })
          .borderRadius(6).position({ x: '50%', y: 6 }).translate({ x: '-50%' }).hitTestBehavior(HitTestMode.Transparent)
      }
      if (!this.request) {
        Button('···', { type: ButtonType.Circle }).width(36).height(36)
          .fontSize(22).fontColor('#F1F3F5').backgroundColor('#8021262D')
          .accessibilityText(this.lang.startsWith('zh') ? '游戏工具与诊断' : 'Game tools and diagnostics')
          .position({ x: px2vp(this.safeLeftPx) + 8, y: 8 })
          .onClick(() => { this.toolsOpen = !this.toolsOpen; this.showLog = false; this.experimentsOpen = false; if (this.toolsOpen) { this.readExperiments(); } })
      }
      if (this.toolsOpen && !this.request) {
        Column({ space: 14 }) {
          Row() {
            Text(this.lang.startsWith('zh') ? '游戏工具' : 'Game tools').fontSize(22).fontWeight(FontWeight.Bold).fontColor('#F1F3F5')
            Blank()
            Button(this.lang.startsWith('zh') ? '关闭' : 'Close').onClick(() => { this.toolsOpen = false; })
          }.width('100%')
          Scroll() {
            Column({ space: 14 }) {
          Row() {
            Text(this.lang.startsWith('zh') ? '显示帧率' : 'Show FPS').fontSize(16).fontColor('#F1F3F5')
            Blank()
            Toggle({ type: ToggleType.Switch, isOn: this.showFps }).onChange((value: boolean) => { this.showFps = value; })
          }.width('100%')
          Text(this.lang.startsWith('zh') ? '帧率为应用呈现计数；上屏稳定性以性能采样为准。' : 'FPS uses application present counts; display pacing requires a performance capture.')
            .fontSize(12).fontColor('#B7C2CF').width('100%')
          Button(this.lang.startsWith('zh') ? '查看最近日志' : 'Recent logs').width('100%').onClick(() => { this.readLog(); })
          Button(this.lang.startsWith('zh') ? '导入或更新游戏包（.d2r）' : 'Import or update game package (.d2r)').width('100%')
            .enabled(!this.packBusy && !this.folderBusy).onClick(() => { this.toolsOpen = false; this.importPack(); })
          if (this.showLog) {
            Scroll() { Text(this.logTail).fontSize(11).fontColor('#DCE4EE').copyOption(CopyOptions.LocalDevice).width('100%') }.height(150).width('100%')
          }
          Button(this.lang.startsWith('zh') ? (this.experimentsOpen ? '收起实验性能选项 ▴' : '实验性能选项 ▾') : (this.experimentsOpen ? 'Hide performance experiments ▴' : 'Performance experiments ▾'))
            .width('100%').onClick(() => { this.experimentsOpen = !this.experimentsOpen; })
          if (this.experimentsOpen) {
            Column({ space: 12 }) {
              Button(this.lang.startsWith('zh') ? '恢复推荐设置' : 'Restore recommended settings')
                .enabled(this.experimentsReady).onClick(() => { this.writeExperimentFlags(0); })
              Row({ space: 12 }) {
                Text(this.lang.startsWith('zh') ? '性能优先画面（界面文字略糊）' : 'Performance display (softer text)').layoutWeight(1).fontColor('#FFFFFF')
                Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 32768) !== 0 }).enabled(this.experimentsReady)
                  .onChange((value: boolean) => { this.saveExperiment(32768, value); })
              }.width('100%')
              Button(this.lang.startsWith('zh') ? '开发者对照（不建议改动）' : 'Developer comparisons')
                .onClick(() => { this.developerExperimentsOpen = !this.developerExperimentsOpen; })
              if (this.developerExperimentsOpen) {
                Text(this.lang.startsWith('zh') ? '仅用于性能对照；开启不代表帧率或画质一定提升。' : 'For performance comparisons only; enabling an experiment does not guarantee an improvement.')
                  .fontSize(12).fontColor('#B7C2CF').width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '关闭 60 帧 GPU 预算（对照）' : 'Disable 60 FPS GPU budget (control)').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 1) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(1, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '关闭新增清屏优化' : 'Disable the new clear optimization').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 2) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(2, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '减少短抖动引起的降帧' : 'Confirm brief stalls before lowering frame rate').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 4) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(4, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '开启 OpenGTX 关键线程登记' : 'Enable OpenGTX key thread registration').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 8) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(8, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? 'PerfHint 不上报线程号' : 'PerfHint without thread IDs').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 16) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(16, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '帧率区间 B：30–120，期望60' : 'Rate range B: 30–120, expected 60').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 32) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(32, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '帧率区间 C：0–120，期望60' : 'Rate range C: 0–120, expected 60').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 64) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(64, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '关闭 90 Hz 节拍修正（负对照）' : 'Disable 90 Hz pacing correction (control)').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 128) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(128, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '原生尺寸上屏（实验）' : 'Native-size presentation (experimental)').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 256) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(256, value); })
                }.width('100%')
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '出帧分段测量（实验）' : 'Frame stage measurements (experimental)').fontSize(15).fontColor('#F1F3F5').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 512) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(512, value); })
                }.width('100%')
                Row() {
                  Text(this.lang.startsWith('zh') ? '高频核请求（实验）' : 'High-frequency core requests (experimental)').fontSize(14).fontColor('#FFFFFF').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 1024) === 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(1024, !value); })
                }.width('100%')
                Row() {
                  Text(this.lang.startsWith('zh') ? '停顿后清除旧掉帧计数' : 'Clear stale late-frame counts after stalls').fontSize(14).fontColor('#FFFFFF').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 2048) === 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(2048, !value); })
                }.width('100%')
                Row() {
                  Text(this.lang.startsWith('zh') ? '图像缓存快速查找（实验）' : 'Image cache fast lookup (experimental)').fontSize(14).fontColor('#FFFFFF').layoutWeight(1)
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 4096) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(4096, value); })
                }.width('100%')
                Text(this.lang.startsWith('zh') ? '保存并退出游戏后，关闭并重新打开应用生效。不会自动退出游戏。' : 'Save and exit the game, then close and reopen the app to apply. The game will not be closed automatically.')
                  .fontSize(13).fontColor('#E8C67A').width('100%')
                Button(this.lang.startsWith('zh') ? '采集一次绘制性能' : 'Capture drawing performance once')
                  .onClick(() => {
                    const result = dm.profileDescriptors();
                    this.experimentError = result === 0 ? (this.lang.startsWith('zh') ? '3秒后同时采集DESC与DRAW，共60帧；请关闭面板。' : 'DESC + DRAW: 60 frames in 3 seconds; close this panel.') : `Capture unavailable (${result})`;
                  })
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '描述符整套复用' : 'Descriptor set reuse').layoutWeight(1).fontColor('#FFFFFF')
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 16384) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(16384, value); })
                }
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '跳过相同 BC 贴图重传' : 'Skip identical BC uploads').layoutWeight(1).fontColor('#FFFFFF')
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 65536) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(65536, value); })
                }
                Row({ space: 12 }) {
                  Text(this.lang.startsWith('zh') ? '关闭试升余量与 3D 回升（对照）' : 'Disable probe headroom and 3D relax (control)').layoutWeight(1).fontColor('#FFFFFF')
                  Toggle({ type: ToggleType.Switch, isOn: (this.experimentBits & 131072) !== 0 }).enabled(this.experimentsReady)
                    .onChange((value: boolean) => { this.saveExperiment(131072, value); })
                }
              }
              if (this.experimentSaved) {
                Text(this.lang.startsWith('zh') ? '已保存，重新打开应用后生效。' : 'Saved. Applies after reopening the app.').fontSize(12).fontColor('#91D7A3').width('100%')
              }
              if (this.experimentError) { Text(this.experimentError).fontSize(12).fontColor('#FFB4AB').width('100%') }
            }.width('100%').padding(12).backgroundColor('#303741').borderRadius(12)
          }
          Text(this.lang.startsWith('zh') ? '离开游戏请使用游戏菜单中的“保存并退出”，确保角色进度写入。' : 'Use Save and Exit in the game menu to preserve character progress.')
            .fontSize(13).fontColor('#B7C2CF').width('100%')
            }.width('100%')
          }.width('100%').layoutWeight(1)
        }.width('70%').height('86%').constraintSize({ maxWidth: 520 }).padding(20).backgroundColor('#F2222730').borderRadius(24)
      }
      // Modal priority: name input > tools > startup/import status; never draw two panels together.
      if (!this.request && !this.toolsOpen && (this.error || this.importing)) {
        Column({ space: 16 }) {
          if (this.error) { Text(this.error).fontColor('#ffcdb98c').fontSize(16) }
          if (this.canImport && !this.packBusy && !this.folderBusy) {
            Button(this.lang.startsWith('zh') ? '导入游戏包（一个 .d2r 文件）' : 'Import game package (one .d2r file)').onClick(() => { this.importPack(); })
            Button('导入 data（可直接选 data 文件夹）').onClick(() => { this.importResources(); })
          }
          if (this.importing) { Text(this.importing).fontColor('#ffd8d0bc').fontSize(15) }
          if (this.packBusy) {
            Button(this.lang.startsWith('zh') ? '取消导入' : 'Cancel import').onClick(() => { this.packCancel = true; })
          }
        }.padding(24).backgroundColor('#cc000000').borderRadius(8)
      }
      if (this.request) {
        Column({ space: 12 }) {
          Text(this.request.title || this.t('dmInputTitle')).fontColor('#F1F3F5').fontSize(22).fontWeight(FontWeight.Bold)
          if (this.request.sub) { Text(this.request.sub).fontColor('#B7C2CF').fontSize(14) }
          TextInput({ text: this.text, placeholder: this.request.nameRule ? this.t('dmNameBlankRandom') : this.request.guide })
            .height(52).fontSize(20).fontColor('#F1F3F5').placeholderColor('#9AA7B5')
            .backgroundColor('#383F49').borderRadius(14).padding({ left: 16, right: 16 })
            .maxLength(this.request.maxChars > 0 ? this.request.maxChars : 64)
            .type(this.request.numericMax >= 0 ? InputType.Number : InputType.Normal)
            .onChange((v: string) => { this.text = v; })
            .onSubmit(() => this.finishInput(true))
          Row({ space: 24 }) {
            Button(this.t('dmCancel')).onClick(() => this.finishInput(false))
            Button(this.t('dmConfirm')).onClick(() => this.finishInput(true))
          }
        }.padding(24).backgroundColor('#F2222730').borderRadius(24).width('80%').constraintSize({ maxWidth: 520 })
      }
    }.width('100%').height('100%').backgroundColor('#000000')
  }
}
