import host, { GameKeyboardRequest } from 'libxblhost.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { bundleManager } from '@kit.AbilityKit';
import { GameData, GamePaths } from '../data/GameData';
import { MenuControls } from '../components/MenuControls';
import { GamePad } from '../components/GamePad';
import { FloatingPad } from '../components/FloatingPad';
import { gameWindow } from '../lifecycle/GameWindow';
import { fitGameViewport } from '../lifecycle/GameLayout';
import { gameScreenAwake } from '../lifecycle/GameScreenAwake';
import { SaveSync } from '../data/SaveSync';

// Release flow: open straight into the game; the test/diagnostics page is developer mode
// (tap the title 7 times to toggle, persisted).
// Release: the developer page is never remembered across launches (7 taps on the title toggles it for this run only).
AppStorage.setOrCreate('devMode', false);


// 开发验证页，不是游戏界面。只在原生调用返回后显示成功。
@Entry
@Component
struct Index {
  @StorageLink('testKeepScreenOn') @Watch('testAwakeChanged') private testKeepScreenOn: boolean = false;
  @StorageLink('screenAwakeError') private screenAwakeError: string = '';
  private testAwakeChanged(): void { gameScreenAwake.setTestKeepScreenOn(this.testKeepScreenOn); }
  @StorageLink('gameFullscreen') private fullscreen: boolean = false;
  @StorageLink('devMode') private devMode: boolean = false;
  private titleTaps: number = 0;
  private titleTapAt: number = 0;
  @StorageLink('gameFullscreenRequested') private fullscreenRequested: boolean = false;
  @StorageLink('gameWindowError') private windowError: string = '';
  @StorageLink('gameInsetLeftPx') private insetLeft: number = 0;
  @StorageLink('gameInsetTopPx') private insetTop: number = 0;
  @StorageLink('gameInsetRightPx') private insetRight: number = 0;
  @StorageLink('gameInsetBottomPx') private insetBottom: number = 0;
  @State private pageWidth: number = 360;
  @State private pageHeight: number = 640;
  // Fullscreen is edge-to-edge: the game renders at the screen's own aspect (runtime XBL_SCREEN).
  private edge(value: number): number { return this.fullscreen ? 0 : 24; }
  private contentWidth(): number { return Math.max(0, this.pageWidth-this.edge(this.insetLeft)-this.edge(this.insetRight)); }
  private surfaceHeight(): number {
    if (!this.fullscreen) { return this.contentWidth()*9/16; }
    return Math.max(0, this.pageHeight-this.edge(this.insetTop)-this.edge(this.insetBottom));
  }
  private surfaceWidth(): number { return this.fullscreen ? this.contentWidth() : fitGameViewport(this.contentWidth(),this.surfaceHeight()).width; }
  // In the game the back gesture is the game's own back key (B: close panel / previous menu), never a jump to the launcher page.
  onBackPress(): boolean {
    if(this.fullscreen && this.gameActive) { if(!this.keyboard) {AppStorage.setOrCreate('padBack', (AppStorage.get<number>('padBack') ?? 0) + 1);} return true; }
    if(this.fullscreenRequested || this.fullscreen) { gameWindow.request(false); return true; } return false;
  }
  @State private appVersion: string = '';
  @State private dataMessage: string = '正在检查游戏资料';
  @State private dataReady: boolean = false;
  @State private dataDamageDetected: boolean = false;
  @State private importing: boolean = false;
  private dataManager: GameData | undefined = undefined;
  @State private diagnostics: boolean = false;
  @State private gameActive: boolean = false;
  @State private menuControls: boolean = true;
  @State private fullControls: boolean = false;
  @State private startupAttempted: boolean = false;
  @State private keyboard: GameKeyboardRequest | null = null;
  @State private keyboardText: string = '';
  @State private keyboardError: string = '';
  onPageHide(): void {
    gameWindow.request(false);
    host.setGameButtons(0);host.setGameTouch(false,0,0);
    AppStorage.setOrCreate('padReset', (AppStorage.get<number>('padReset') ?? 0) + 1);
  }
  aboutToDisappear(): void { gameWindow.request(false); gameScreenAwake.setGameActive(false); }
  private respondKeyboard(confirmed: boolean): void {
    if(!this.keyboard) {return;}
    try {host.answerGameKeyboard(this.keyboard.id,confirmed,this.keyboardText);this.keyboard=null;this.keyboardError='';}
    catch(error) {this.keyboardError=String(error);}
  }
  @State private launchMessage: string = '游戏启动接入验证版；遇到未支持功能会停止并显示诊断。';
  private async prepareData(importFiles: boolean=false,repairCorrupt: boolean=false,full: boolean=false): Promise<void> {
    if(this.running) {return;}
    this.running=true;this.importing=importFiles;
    try {
      const manager=new GameData(this.getUIContext().getHostContext() as common.UIAbilityContext);
      this.dataManager=manager;
      const progress=(message: string): void=>{this.dataMessage=message;};
      const result: GamePaths=importFiles?await manager.selectAndImport(progress,repairCorrupt):await manager.check(progress,full);
      this.dataReady=result.ready;
      this.dataDamageDetected=false;
      this.dataMessage=result.ready?'游戏资料已校验 · 28/28':`还缺 ${result.missing.length} 个资料文件：${result.missing.slice(0,3).join('、')}${result.missing.length>3?' 等':''}。可继续选择导入。`;
    } catch(error) {
      this.dataMessage=String(error).replace(/^Error: /,'');
      if(this.dataMessage.startsWith('资料校验失败：')) {this.dataDamageDetected=true;this.dataReady=false;}
      hilog.error(0,'XblImport','%{public}s',this.dataMessage);
    }
    finally {this.running=false;this.importing=false;this.dataManager=undefined;}
  }
  private async checkStartup(): Promise<void> {
    if(this.running || !this.dataReady || this.startupAttempted) {return;}
    this.launchMessage='正在检查系统初始化';
    await this.runRuntime();
    if(this.runtime.includes('sdkOSInitialization=RETURNED')) {
      this.launchMessage='系统初始化检查通过；这次只运行诊断。重启应用后可执行完整启动。';
    } else if(this.runtime.includes('already attempted')) {
      this.launchMessage='本次进程已做过初始化检查，需重启应用后重试。';
    } else {
      this.launchMessage='系统初始化检查失败，已停止执行；请展开开发诊断查看具体原因。资料无需重新导入。';

    }
  }
  private async launchGame(): Promise<void> {
    if(this.running || !this.dataReady || this.startupAttempted) {return;}
    this.running=true;
    try {
      await new GameData(this.getUIContext().getHostContext() as common.UIAbilityContext).checkShaders((message:string)=>{this.launchMessage=message;});
    } catch(error) {this.launchMessage=String(error);this.running=false;return;}
    this.startupAttempted=true;
    { const ctx=this.getUIContext().getHostContext() as common.UIAbilityContext;   // cross-device save sync: merge before the game reads its saves
      if(await SaveSync.ensure(ctx)) {this.launchMessage='正在同步存档…';SaveSync.run(ctx,true);} }
    this.running=true;this.gameActive=true;this.launchMessage='正在启动游戏，加载过程中请保持应用前台。';
    gameScreenAwake.setGameActive(true);
    this.menuControls=false;gameWindow.request(true); // play in landscape fullscreen with floating controls
    const timer=setInterval(()=>{
      this.launchMessage=host.runtimeProgress();
      const request=host.getGameKeyboard();
      if(request && (!this.keyboard || request.id!==this.keyboard.id)) {this.keyboard=request;this.keyboardText=request.initial;this.keyboardError='';hilog.info(0,'XblKeyboard','request-polled id=%{public}d',request.id);}
      else if(!request) {this.keyboard=null;}
    },500);
    try {
      this.runtime=await host.runGame((this.getUIContext().getHostContext() as common.UIAbilityContext).filesDir);
      const status=this.runtime.match(/runtimeRc=(\d+)/);
      const code=status?Number(status[1]):-1;
      if(code===43) {this.launchMessage='游戏请求了尚未支持的系统服务，已停止。资料完整，无需重新导入。';}
      else if(code===49) {
        const reason=this.runtime.split('\n').find((line:string)=>line.startsWith('symbol='));
        this.launchMessage='游戏在系统或图形调用处停止。'+(reason?reason.substring(7):'具体原因见开发诊断。');
      }
      else if(code===0) {this.launchMessage='游戏主程序已返回，未持续运行；需要继续检查提前退出的原因。';}
      else {this.launchMessage=`游戏已停止（状态 ${code}）。具体原因见开发诊断。`;}

      hilog.info(0,'XblRuntime','%{public}s',this.runtime);
    } catch(error) {this.launchMessage=String(error);}
    finally {gameWindow.request(false);clearInterval(timer);this.keyboard=null;this.gameActive=false;gameScreenAwake.setGameActive(false);this.running=false;}
  }
  private async restartApplication(): Promise<void> {
    if(this.running) {return;}
    this.running=true;
    try {await new GameData(this.getUIContext().getHostContext() as common.UIAbilityContext).restartApplication();}
    catch(error) {this.launchMessage=String(error).replace(/^Error: /,'');this.running=false;}
  }
  @State private report: string = '等待调用原生库';
  @State private checks: string = '原生能力检查尚未执行';
  @State private running: boolean = false;
  @State private gpu: string = 'GPU四核检查尚未执行';
  @State private present: string = '独立彩格测试尚未执行（与Logo重放和游戏呈现无关）';
  @State private presentSubmitted: boolean = false;
  @State private real: string = '真实ASSAO计算核尚未执行';
  @State private logo: string = '原着色器与原Logo重放尚未执行';
  @State private runtime: string = '原SDK初始化尚未执行';
  private async runRuntime(): Promise<void> {
    if(this.running || this.startupAttempted) {return;}
    this.startupAttempted=true;
    this.running=true;this.runtime='执行原SDK初始化，尚未进入游戏主函数';
    try {
      this.runtime=await host.runRuntimeStartup((this.getUIContext().getHostContext() as common.UIAbilityContext).filesDir);
      hilog.info(0x0000,'XblRuntime','%{public}s',this.runtime);
    } catch(error) {this.runtime=`初始化失败：${String(error)}`;}
    finally {this.running=false;}
  }

  private async runLogo(): Promise<void> {
    if(this.running || this.presentSubmitted) { return; }
    this.running=true;
    try {
      this.logo=await host.runReal2D((this.getUIContext().getHostContext() as common.UIAbilityContext).filesDir);
      this.presentSubmitted=this.logo.includes('windowPresent=SUBMITTED');
      const parts=Math.ceil(this.logo.length/1500);
      for(let i=0;i<parts;i++) { hilog.info(0x0000,'XblLogo','logo[%{public}d/%{public}d]: %{public}s',i+1,parts,this.logo.substring(i*1500,(i+1)*1500)); }
    } catch(error) { this.logo=`原Logo重放失败：${String(error)}`; }
    finally {this.running=false;}
  }

  private async runReal(): Promise<void> {
    if (this.running) { return; }
    this.running = true;
    try {
      this.real = await host.runRealShader();
      const parts = Math.ceil(this.real.length / 1500);
      for (let i = 0; i < parts; i++) {
        hilog.info(0x0000, 'XblReal', 'real[%{public}d/%{public}d]: %{public}s', i + 1, parts,
          this.real.substring(i * 1500, (i + 1) * 1500));
      }
    } catch (error) { this.real = `真实计算核失败：${String(error)}`; }
    finally { this.running = false; }
  }

  private async showGpu(): Promise<void> {
    if (this.running || this.presentSubmitted) { return; }
    this.running = true;
    this.present = '正在提交单帧GPU测试图';
    try {
      this.present = await host.presentGpuTest();
      this.presentSubmitted = this.present.includes('windowPresent=SUBMITTED');
      const parts = Math.ceil(this.present.length / 1500);
      for (let i = 0; i < parts; i++) {
        hilog.info(0x0000, 'XblPresent', 'present[%{public}d/%{public}d]: %{public}s',
          i + 1, parts, this.present.substring(i * 1500, (i + 1) * 1500));
      }
    } catch (error) { this.present = `呈现失败：${String(error)}`; }
    finally { this.running = false; }
  }

  private async runGpu(): Promise<void> {
    if (this.running) { return; }
    this.running = true;
    this.gpu = 'GPU检查进行中，每次提交最多等待5秒。';
    try {
      const raw = await host.runGpuChecks();
      // 原始读回完整留在日志，页面省略十六进制数据以便查看判定。
      this.gpu = raw.split('\n').filter((line: string) => !line.startsWith('readbackHex ')).join('\n');
      const parts = Math.ceil(raw.length / 1500);
      for (let i = 0; i < parts; i++) {
        hilog.info(0x0000, 'XblGpu', 'gpu[%{public}d/%{public}d]: %{public}s',
          i + 1, parts, raw.substring(i * 1500, (i + 1) * 1500));
      }
    } catch (error) {
      this.gpu = `GPU检查失败：${String(error)}`;
    } finally { this.running = false; }
  }

  private async runChecks(): Promise<void> {
    if (this.running) { return; }
    this.running = true;
    this.checks = '后台执行原生检查中；图形仅枚举能力，不绘制游戏。';
    try {
      this.checks = await host.runPlatformChecks();
      // 分块避免单条日志被系统截断，保留序号供设备取证重组。
      const parts = Math.ceil(this.checks.length / 1500);
      for (let i = 0; i < parts; i++) {
        hilog.info(0x0000, 'XblBootstrap', 'probe[%{public}d/%{public}d]: %{public}s',
          i + 1, parts, this.checks.substring(i * 1500, (i + 1) * 1500));
      }
    } catch (error) {
      this.checks = `原生能力检查失败：${String(error)}`;
      hilog.error(0x0000, 'XblBootstrap', '%{public}s', this.checks);
    } finally {
      this.running = false;
    }
  }

  private tapTitle(): void {
    const now = Date.now(); if (now - this.titleTapAt > 1500) { this.titleTaps = 0; } this.titleTapAt = now;
    if (++this.titleTaps >= 7) { this.titleTaps = 0; this.devMode = !this.devMode; }
  }
  private async importAndLaunch(): Promise<void> {
    await this.prepareData(true);
    if (this.dataReady && !this.devMode) { this.launchGame(); }
  }
  aboutToAppear(): void {
    bundleManager.getBundleInfoForSelf(0).then((info: bundleManager.BundleInfo) => {
      this.appVersion = info.versionName;
    }).catch((error: Error) => {
      hilog.warn(0, 'XblBootstrap', 'Cannot read application version: %{public}s', String(error));
    });
    this.prepareData().then(() => { if (this.dataReady && !this.devMode) { this.launchGame(); } });
    try {
      const result = host.getHostReport();
      this.report = `原生桥接已返回\n宿主状态：${result.hostState}\n` +
        `设备：${result.deviceModel} (${result.deviceType})\n` +
        `系统：${result.osVersion}\n运行 API：${result.runtimeApi}\n` +
        `设备 ABI：${result.deviceAbi}\n编译 ABI：${result.compiledAbi}\n` +
        `指针位数：${result.pointerBits}\n编译器：${result.compiler}`;
      hilog.info(0x0000, 'XblBootstrap', 'native report: %{public}s', JSON.stringify(result));
    } catch (error) {
      this.report = `原生桥接失败：${String(error)}`;
      hilog.error(0x0000, 'XblBootstrap', '%{public}s', this.report);
    }
  }

  build() {
    Scroll() {
      Column({ space: this.fullscreen ? 0 : 16 }) {
        if (!this.fullscreen && !this.devMode) {
          Column({ space: 14 }) {
            Text('小菠萝2').fontSize(30).fontWeight(FontWeight.Bold).fontColor('#E8C67A').onClick(() => { this.tapTitle(); })
            Text('暗黑破坏神 II：狱火重生 · 鸿蒙版').fontSize(13).fontColor($r('app.color.text_secondary'))
            if (!this.dataReady) {
              if (this.running) {
                LoadingProgress().width(40).height(40)
                Text(this.dataMessage).fontSize(14).fontColor($r('app.color.text_primary')).textAlign(TextAlign.Center)
                if (this.importing) {
                  Button('停止导入').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
                    .onClick(() => { this.dataManager?.cancel(); this.dataMessage = '正在停止；系统文件选择页请点取消或返回。'; })
                }
              } else {
                Text('首次使用：导入游戏资料').fontSize(18).fontColor($r('app.color.text_primary')).fontWeight(FontWeight.Medium)
                Text('1. 准备你自己购买的 Nintendo Switch 版游戏资料（共 28 个文件）。\n2. 点击下方按钮，在系统文件选择器里选中全部资料文件。\n3. 导入完成后自动进入游戏。原资料只读，存档单独保存。')
                  .fontSize(14).fontColor($r('app.color.text_primary')).lineHeight(22).width('86%')
                Text(this.dataMessage).fontSize(13).fontColor($r('app.color.text_secondary')).width('86%')
                Button('选择并导入游戏资料').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
                  .onClick(() => { this.importAndLaunch(); })
                if (this.dataDamageDetected) {
                  Button('选择文件修复损坏资料').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
                    .onClick(() => { this.prepareData(true, true); })
                }
              }
            } else if (this.startupAttempted && !this.running) {
              Text(this.launchMessage).fontSize(14).fontColor($r('app.color.text_primary')).width('86%')
              Button('重新启动').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
                .onClick(() => { this.restartApplication(); })
            } else if (this.gameActive) {
              Text('游戏运行中 · 点「横屏全屏」回到游戏').fontSize(14).fontColor($r('app.color.text_secondary'))
            } else if (this.running) {
              LoadingProgress().width(40).height(40)
              Text('正在加载游戏…').fontSize(14).fontColor($r('app.color.text_primary'))
            } else {
              Button('进入游戏').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
                .onClick(() => { this.launchGame(); })
            }
          }.width('100%').padding({ top: 48 }).alignItems(HorizontalAlign.Center)
        }
        if (!this.fullscreen && this.devMode) {
        Button(this.startupAttempted?'本次启动已执行，重启应用可重试':'启动游戏（接入验证）').enabled(!this.running && this.dataReady && !this.startupAttempted).backgroundColor((!this.running && this.dataReady && !this.startupAttempted) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running && this.dataReady && !this.startupAttempted) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(()=>{this.launchGame();})
        Text('小菠萝2').fontColor($r('app.color.text_primary')).fontSize(22).fontWeight(FontWeight.Bold).onClick(() => { this.tapTitle(); })
        Text(this.appVersion ? `鸿蒙开发版 · ${this.appVersion}` : '鸿蒙开发版').fontColor($r('app.color.text_primary')).fontSize(14)
        Row({space:8}) {
          Toggle({type:ToggleType.Switch,isOn:this.testKeepScreenOn})
            .onChange((enabled:boolean)=>{this.testKeepScreenOn=enabled;})
          Text('测试时保持亮屏').fontColor($r('app.color.text_primary')).fontSize(14)
        }
        if(this.screenAwakeError) { Text(`保持亮屏失败：${this.screenAwakeError}`).fontColor($r('app.color.text_error')).fontSize(12) }
        }
        if(this.keyboard && !this.fullscreen) {
          Column({space:8}) {
            Text(this.keyboard.title).fontColor($r('app.color.text_primary')).fontSize(20)
            Text(this.keyboard.subtitle).fontColor($r('app.color.text_primary')).fontSize(14)
            TextInput({text:this.keyboardText,placeholder:this.keyboard.guide})
              .backgroundColor($r('app.color.input_background')).fontColor($r('app.color.text_primary')).placeholderColor($r('app.color.text_secondary'))
              .maxLength(this.keyboard.maximum).onChange((value:string)=>{this.keyboardText=value;})
            Text(this.keyboardError).fontColor($r('app.color.text_error')).fontSize(14)
            Row({space:16}) {
              Button('确认').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text')).onClick(()=>{this.respondKeyboard(true);})
              Button('取消').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text')).onClick(()=>{this.respondKeyboard(false);})
            }
          }.width('100%').onAppear(()=>{if(this.keyboard){hilog.info(0,'XblKeyboard','window-shown id=%{public}d',this.keyboard.id);}})
        }
        Stack({alignContent:Alignment.Center}) {
          XComponent({ id: 'gpuTestSurface', type: XComponentType.SURFACE, libraryname: 'xblhost' })
            .width(this.surfaceWidth()).height(this.fullscreen ? this.surfaceHeight() : this.surfaceWidth()*9/16).backgroundColor('#081118')
          if (this.gameActive && this.fullscreen && !this.keyboard) {
            FloatingPad({ active: this.gameActive && !this.keyboard, padWidth: this.contentWidth(), padHeight: this.surfaceHeight() })
          }
          if (this.gameActive && this.fullscreen && this.keyboard) {
            // In-game text entry (character name): game-styled panel over the game, kept in the upper part so the
            // system keyboard does not cover it. Blank name = random name (runtime).
            Column() {
              Column({ space: 10 }) {
                Text(this.keyboard.title || '输入角色名称').fontSize(18).fontColor('#E8C67A').fontWeight(FontWeight.Medium)
                if (this.keyboard.subtitle) { Text(this.keyboard.subtitle).fontSize(13).fontColor('#BFAE8A') }
                TextInput({ text: this.keyboardText, placeholder: '留空自动随机起名' })
                  .width(320).height(42).fontSize(16).fontColor('#F1E3C0').placeholderColor('#7D6E55')
                  .backgroundColor('#15100B').border({ width: 1, color: '#7A5E36', radius: 4 }).caretColor('#E8C67A')
                  .maxLength(this.keyboard.maximum).defaultFocus(true).enterKeyType(EnterKeyType.Done)
                  .onChange((value: string) => { this.keyboardText = value; })
                  .onSubmit(() => { this.respondKeyboard(true); })
                if (this.keyboardError) { Text(this.keyboardError).fontColor('#E06A50').fontSize(12) }
                Row({ space: 14 }) {
                  Text('确定').fontSize(15).fontColor('#F1E3C0').width(120).height(38).textAlign(TextAlign.Center)
                    .backgroundColor('#3A2A18').border({ width: 1.5, color: '#9A7A47', radius: 4 })
                    .onClick(() => { this.respondKeyboard(true); })
                  Text('取消').fontSize(15).fontColor('#BFAE8A').width(120).height(38).textAlign(TextAlign.Center)
                    .backgroundColor('#1C150E').border({ width: 1, color: '#5E4A2E', radius: 4 })
                    .onClick(() => { this.respondKeyboard(false); })
                }
              }.padding({ left: 24, right: 24, top: 16, bottom: 16 }).backgroundColor('rgba(12,9,6,0.94)')
              .border({ width: 2, color: '#8A6A3A', radius: 6 }).alignItems(HorizontalAlign.Center)
              .onAppear(() => { if (this.keyboard) { hilog.info(0, 'XblKeyboard', 'overlay-shown id=%{public}d', this.keyboard.id); } })
            }.width('100%').height('100%').backgroundColor('rgba(0,0,0,0.45)').justifyContent(FlexAlign.Start)
            .padding({ top: 24 }).alignItems(HorizontalAlign.Center)
          }

        }.width('100%').height(this.surfaceHeight()).backgroundColor('#081118')
        if (this.gameActive && !this.fullscreen) {
          Button(this.fullscreenRequested ? '退出全屏' : '横屏全屏').height(36)
            .backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
            .onClick(() => { if(!this.fullscreenRequested) {this.menuControls=false;} gameWindow.request(!this.fullscreenRequested); })
          Text(this.menuControls ? '收起菜单操作' : '展开菜单操作').fontSize(12)
            .fontColor($r('app.color.text_secondary')).onClick(() => { this.menuControls = !this.menuControls; })
          if (this.menuControls) {
            Text(this.fullControls ? '切换为菜单操作' : '展开完整操作').fontSize(12)
              .fontColor($r('app.color.text_secondary')).onClick(() => { this.fullControls = !this.fullControls; })
            if (this.fullControls) { GamePad({ active: this.gameActive && !this.keyboard, availableWidth: this.contentWidth() }) }
            else { MenuControls({ active: this.gameActive && !this.keyboard, availableWidth: this.contentWidth() }) }
          }
        }
        if(this.windowError) { Text(this.windowError).fontColor($r('app.color.text_error')).fontSize(12) }
        if (!this.fullscreen && this.devMode) {
        Text(this.dataMessage).fontColor($r('app.color.text_primary')).fontSize(16).width('100%')
        if(this.startupAttempted && !this.running) {
          Button('重新启动应用').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text')).onClick(()=>{this.restartApplication();})
        }
        Text(this.launchMessage).fontColor($r('app.color.text_primary')).fontSize(14).width('100%')
        Button(this.dataReady?'游戏资料已就绪':'选择并导入游戏资料').enabled(!this.running && !this.dataReady).backgroundColor((!this.running && !this.dataReady) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running && !this.dataReady) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(()=>{this.prepareData(true);})
        if(this.importing) {
          Button('停止导入').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text')).onClick(()=>{this.dataManager?.cancel();this.dataMessage='正在停止；系统文件选择页请点取消或返回。';})
        }
        Button('检查已传入资料').enabled(!this.running).backgroundColor((!this.running) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(()=>{this.prepareData();})
        if(this.dataDamageDetected) {
          Text('可重新选择损坏的资料文件进行修复；原损坏文件会保留备份，正常文件会跳过。')
            .fontColor($r('app.color.text_secondary')).fontSize(12)
          Button('选择文件修复损坏资料').enabled(!this.running)
            .backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
            .onClick(()=>{this.prepareData(true,true);})
        }
        Text('原资料只读；存档保存在独立目录。').fontColor($r('app.color.text_primary')).fontSize(12)
        Button(this.diagnostics?'收起开发诊断':'开发诊断').backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text')).onClick(()=>{this.diagnostics=!this.diagnostics;})
        if(this.diagnostics) {
        Button('完整校验游戏资料').enabled(!this.running)
          .backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
          .onClick(()=>{this.prepareData(false,false,true);})
        Text('高级诊断会占用本次进程；运行后须重启应用才能启动游戏。').fontColor($r('app.color.text_secondary')).fontSize(12)
        Button('高级诊断：检查初始化').enabled(!this.running && this.dataReady && !this.startupAttempted).backgroundColor((!this.running && this.dataReady && !this.startupAttempted) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running && this.dataReady && !this.startupAttempted) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(()=>{this.checkStartup();})
        Button('高级诊断：仅执行阶段2').enabled(!this.running && !this.startupAttempted).backgroundColor((!this.running && !this.startupAttempted) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running && !this.startupAttempted) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(() => {this.runRuntime();})
        Text(this.runtime).fontColor($r('app.color.text_primary')).fontSize(12).width('100%')
        Button('重放原版Logo并核对读回').enabled(!this.running && !this.presentSubmitted).backgroundColor((!this.running && !this.presentSubmitted) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running && !this.presentSubmitted) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(() => { this.runLogo(); })
        Text(this.logo).fontColor($r('app.color.text_primary')).fontSize(12).width('100%')
        Button('执行真实ASSAO清零核').enabled(!this.running).backgroundColor((!this.running) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(() => { this.runReal(); })
        Text(this.real).fontColor($r('app.color.text_primary')).fontSize(12).width('100%')
        Button(this.presentSubmitted ? '测试图已提交，重启应用可重测' : '呈现GPU测试图（单帧）')
          .enabled(!this.running && !this.presentSubmitted).backgroundColor((!this.running && !this.presentSubmitted) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running && !this.presentSubmitted) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(() => { this.showGpu(); })
        Button(this.running ? '检查进行中' : '运行GPU计算与纹理读回')
          .enabled(!this.running).backgroundColor((!this.running) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(() => { this.runGpu(); })
        Text(this.present).fontColor($r('app.color.text_primary')).fontSize(12).width('100%')
        Text(this.gpu).fontColor($r('app.color.text_primary')).fontSize(14).width('100%')
        Text(this.report).fontColor($r('app.color.text_primary')).fontSize(16).width('100%')
        Button(this.running ? '检查进行中' : '运行开发能力检查')
          .enabled(!this.running).backgroundColor((!this.running) ? $r('app.color.button_background') : $r('app.color.button_disabled_background')).fontColor((!this.running) ? $r('app.color.button_text') : $r('app.color.button_disabled_text')).onClick(() => { this.runChecks(); })
        Text(this.checks).fontColor($r('app.color.text_primary')).fontSize(14).width('100%')
        }
        }
      }.padding({left:this.edge(this.insetLeft),top:this.edge(this.insetTop),right:this.edge(this.insetRight),bottom:this.edge(this.insetBottom)})
        .width('100%').alignItems(HorizontalAlign.Start)
    }.height('100%').width('100%').backgroundColor($r('app.color.page_background'))
      .scrollable(this.fullscreen ? ScrollDirection.None : ScrollDirection.Vertical)
      .onAreaChange((oldArea: Area, area: Area) => { this.pageWidth=Number(area.width); this.pageHeight=Number(area.height); })
  }
}
