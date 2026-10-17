import { controlScale } from '../lifecycle/GameLayout';
import host from 'libxblhost.so';
import { KeyCode } from '@kit.InputKit';
import { ControllerState, ControllerKey, SHOULDER_KEYS, FACE_KEYS, NAV_KEYS } from '../input/ControllerState';

@Component
export struct GamePad {
  @Prop @Watch('reset') availableWidth: number = 270;
  private unit(value: number): number { return value*controlScale(this.availableWidth,270); }
  @Prop @Watch('reset') active: boolean = false;
  @StorageLink('padReset') @Watch('reset') resetEpoch: number = 0;
  @State private buttons: number = 0;
  @State private lx: number = 0;
  @State private ly: number = 0;
  @State private rx: number = 0;
  @State private ry: number = 0;
  private model: ControllerState = new ControllerState();
  private publish(): void {
    const state = this.model.frame();
    this.buttons = this.active ? state.buttons : 0;
    this.lx = this.active ? state.lx : 0; this.ly = this.active ? state.ly : 0;
    this.rx = this.active ? state.rx : 0; this.ry = this.active ? state.ry : 0;
    host.setGameButtons(this.buttons, this.lx, this.ly, this.rx, this.ry);
  }
  private reset(): void { this.model.reset(); this.publish(); host.cancelGameButtons(); }
  aboutToDisappear(): void { this.reset(); }
  private touch(event: TouchEvent, mask: number, side: number): void {
    event.stopPropagation();
    if (!this.active || event.type === TouchType.Cancel) { this.reset(); return; }
    event.changedTouches.forEach((point: TouchObject) => {
      const source = `touch:${point.id}`;
      const phase = event.type === TouchType.Down ? 'down' : (event.type === TouchType.Up ? 'up' : 'move');
      this.model.touch(source, phase, mask, side, point.x/controlScale(this.availableWidth,270), point.y/controlScale(this.availableWidth,270));
    });
    this.publish();
  }
  private handleKey(event: KeyEvent): boolean {
    if (!this.active) { return false; }
    let mask = 0;
    switch (event.keyCode) {
      case KeyCode.KEYCODE_BUTTON_A: case KeyCode.KEYCODE_ENTER: case KeyCode.KEYCODE_DPAD_CENTER: mask=1; break;
      case KeyCode.KEYCODE_BUTTON_B: case KeyCode.KEYCODE_ESCAPE: mask=2; break;
      case KeyCode.KEYCODE_BUTTON_X: mask=4; break;
      case KeyCode.KEYCODE_BUTTON_Y: mask=8; break;
      case KeyCode.KEYCODE_BUTTON_THUMBL: mask=16; break;
      case KeyCode.KEYCODE_BUTTON_THUMBR: mask=32; break;
      case KeyCode.KEYCODE_BUTTON_L1: mask=64; break;
      case KeyCode.KEYCODE_BUTTON_R1: mask=128; break;
      case KeyCode.KEYCODE_BUTTON_L2: mask=256; break;
      case KeyCode.KEYCODE_BUTTON_R2: mask=512; break;
      case KeyCode.KEYCODE_BUTTON_START: mask=1024; break;
      case KeyCode.KEYCODE_BUTTON_SELECT: mask=2048; break;
      case KeyCode.KEYCODE_DPAD_LEFT: mask=4096; break;
      case KeyCode.KEYCODE_DPAD_UP: mask=8192; break;
      case KeyCode.KEYCODE_DPAD_RIGHT: mask=16384; break;
      case KeyCode.KEYCODE_DPAD_DOWN: mask=32768; break;
      default: return false;
    }
    const source = `key:${event.keyCode}`;
    if (event.type === KeyType.Down) { this.model.press(source, mask); }
    else if (event.type === KeyType.Up) { this.model.release(source); }
    this.publish(); return true;
  }
  @Builder
  private keyButton(key: ControllerKey) {
    Button(key.label).width(this.unit(40)).height(this.unit(40)).fontSize(this.unit(14))
      .backgroundColor($r('app.color.button_background')).fontColor($r('app.color.button_text'))
      .opacity((this.buttons & key.mask) ? 0.7 : 1).enabled(this.active)
      .onTouch((event: TouchEvent) => { this.touch(event, key.mask, -1); })
  }
  @Builder
  private stick(side: number) {
    Stack({alignContent: Alignment.Center}) {
      Circle().width(this.unit(68)).height(this.unit(68)).fill($r('app.color.input_background')).stroke($r('app.color.text_secondary'))
        .hitTestBehavior(HitTestMode.None)
      Text(side === 0 ? 'L' : 'R').fontColor($r('app.color.text_secondary')).fontSize(this.unit(12)).hitTestBehavior(HitTestMode.None)
      Circle().width(this.unit(22)).height(this.unit(22)).fill($r('app.color.button_background'))
        .translate({x:(side===0?this.lx:this.rx)/32767*this.unit(24),y:-(side===0?this.ly:this.ry)/32767*this.unit(24)})
        .hitTestBehavior(HitTestMode.None)
    }.width(this.unit(72)).height(this.unit(72)).onTouch((event: TouchEvent) => { this.touch(event, 0, side); })
  }
  build() {
    Column({space:this.unit(8)}) {
      Row({space:this.unit(6)}) { ForEach(SHOULDER_KEYS, (key: ControllerKey) => { this.keyButton(key); }, (key: ControllerKey) => key.label) }
      Row({space:this.unit(8)}) {
        this.stick(0)
        this.stick(1)
        Column({space:this.unit(4)}) {
          Row({space:this.unit(4)}) { this.keyButton(FACE_KEYS[0]); this.keyButton(FACE_KEYS[1]); }
          Row({space:this.unit(4)}) { this.keyButton(FACE_KEYS[2]); this.keyButton(FACE_KEYS[3]); }
        }
      }
      Row({space:this.unit(6)}) { ForEach(NAV_KEYS, (key: ControllerKey) => { this.keyButton(key); }, (key: ControllerKey) => key.label) }
    }.id('game-controls').focusable(true).defaultFocus(true)
      .onAppear(() => { this.getUIContext().getFocusController().requestFocus('game-controls'); })
      .onKeyEvent((event: KeyEvent) => this.handleKey(event))
      .onBlur(() => { this.reset(); })
  }
}
