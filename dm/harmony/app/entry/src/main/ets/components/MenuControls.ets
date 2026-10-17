import { controlScale } from '../lifecycle/GameLayout';
import host from 'libxblhost.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { KeyCode } from '@kit.InputKit';
import { MenuButtons } from '../input/MenuButtons';

interface MenuKey { label: string; mask: number; }
const MENU_KEYS: MenuKey[] = [
  { label: '←', mask: 4096 }, { label: '↑', mask: 8192 },
  { label: '↓', mask: 32768 }, { label: '→', mask: 16384 },
  { label: 'A', mask: 1 }, { label: 'B', mask: 2 }
];

@Component
export struct MenuControls {
  @Prop @Watch('reset') availableWidth: number = 328;
  private unit(value: number): number { return value*controlScale(this.availableWidth,328); }
  @Prop @Watch('reset') active: boolean = false;
  @StorageLink('padReset') @Watch('reset') resetEpoch: number = 0;
  @State private pressed: number = 0;
  private buttons: MenuButtons = new MenuButtons();
  private receiptArmed: boolean = false;
  private armReceipt(): void {
    if (this.receiptArmed) { return; }
    this.receiptArmed = true;
    try { host.setInputReceiptBudget(64); }
    catch (error) { hilog.warn(0, 'XblInput', 'receipt unavailable: %{public}s', String(error)); }
  }
  private publish(): void {
    this.pressed = this.active ? this.buttons.value() : 0;
    host.setGameButtons(this.pressed, 0, 0);
  }
  private reset(): void { this.buttons.reset(); this.publish(); host.cancelGameButtons(); }
  aboutToDisappear(): void { this.reset(); }
  private touch(event: TouchEvent, mask: number): void {
    event.stopPropagation();
    if (!this.active || event.type === TouchType.Cancel) { this.reset(); return; }
    event.changedTouches.forEach((point: TouchObject) => {
      const source = `touch:${point.id}`;
      if (event.type === TouchType.Down) { this.armReceipt(); this.buttons.press(source, mask); }
      else if (event.type === TouchType.Up || point.x < 0 || point.x > this.unit(48) || point.y < 0 || point.y > this.unit(44)) {
        this.buttons.release(source);
      }
    });
    this.publish();
  }
  private handleKey(event: KeyEvent): boolean {
    if (!this.active) { return false; }
    let mask = 0;
    switch (event.keyCode) {
      case KeyCode.KEYCODE_DPAD_LEFT: mask = 4096; break;
      case KeyCode.KEYCODE_DPAD_UP: mask = 8192; break;
      case KeyCode.KEYCODE_DPAD_RIGHT: mask = 16384; break;
      case KeyCode.KEYCODE_DPAD_DOWN: mask = 32768; break;
      case KeyCode.KEYCODE_ENTER:
      case KeyCode.KEYCODE_DPAD_CENTER:
      case KeyCode.KEYCODE_BUTTON_A: mask = 1; break;
      case KeyCode.KEYCODE_ESCAPE:
      case KeyCode.KEYCODE_BUTTON_B: mask = 2; break;
      default: return false;
    }
    const source = `key:${event.keyCode}`;
    if (event.type === KeyType.Down) { this.armReceipt(); this.buttons.press(source, mask); }
    else if (event.type === KeyType.Up) { this.buttons.release(source); }
    this.publish();
    return true;
  }
  build() {
    Row({ space:this.unit(8) }) {
      ForEach(MENU_KEYS, (key: MenuKey) => {
        Button(key.label).width(this.unit(48)).height(this.unit(44)).fontSize(this.unit(18))
          .backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
          .opacity((this.pressed & key.mask) ? 0.7 : 1).enabled(this.active)
          .onTouch((event: TouchEvent) => { this.touch(event, key.mask); })
      }, (key: MenuKey) => key.label)
    }.id('menu-controls').focusable(true).defaultFocus(true)
      .onAppear(() => { this.getUIContext().getFocusController().requestFocus('menu-controls'); })
      .onKeyEvent((event: KeyEvent) => this.handleKey(event))
      .onBlur(() => { this.reset(); })
  }
}
