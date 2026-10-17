import host from 'libxblhost.so';
import { KeyCode } from '@kit.InputKit';
import { gameWindow } from '../lifecycle/GameWindow';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';

// Semi-transparent controller overlay drawn over the game surface.
// Left: floating stick (appears where the thumb lands). Right: Switch layout
// face buttons (A right, B bottom, X top, Y left). Top: shoulders, -/+.
interface PadButton { label: string; mask: number; x: number; y: number; r: number; }
// In-game touch HUD drawn from the native iOS touch core (sw_touch_controls): values only, no hit testing here —
// every touch reaches the core through the surface route. Index layout matches GetTouchHud in game_touch.cpp.
interface HudCircle { key: string; label: string; x: number; y: number; r: number; pressed: boolean; small: boolean;
  kind: number; glyph: number; atlas: number; frame: number; }   // kind: 0 plain, 1 skill key (iOS bronze ring), 2 map (native icon)
const KIND_PLAIN: number = 0;
const KIND_SKILL: number = 1;
const KIND_MAP: number = 2;
// iOS SwTouchOverlay: skill keys idle at 60 % (shell only; the icon stays crisp), full while held.
const SKILL_IDLE_ALPHA: number = 0.6;
// Skill atlases (official SkillIcon sprites, silhouette variant as on iOS): 65 px frames every 68 px, generated from the
// player's own game files with the touch overlay (files/touch-overlay/touchart), never shipped in the app.
const ATLAS_FRAME: number = 65;
const ATLAS_PITCH: number = 68;
const ATLAS_WIDTH: number[] = [4080, 4080, 4080, 4080, 4080, 4080, 4080, 4080, 2720];
const TC_COUNT: number = 12;
const TC_LABELS: string[] = ['攻击', 'F1', 'F2', 'F3', 'F4', '', '', '', '', '', '', '地图'];

const STICK_RADIUS: number = 56;
const DEAD_ZONE: number = 0.12;

@Component
export struct FloatingPad {
  @Prop @Watch('reset') active: boolean = false;
  @Prop padWidth: number = 640;
  @Prop padHeight: number = 360;
  @StorageLink('padReset') @Watch('reset') resetEpoch: number = 0;
  @State private pressed: number = 0;
  @State private stickActive: boolean = false;
  @State private originX: number = 0;
  @State private originY: number = 0;
  @State private knobX: number = 0;
  @State private knobY: number = 0;
  @StorageLink('padHidden') private hidden: boolean = false;
  @State private frontPad: boolean = false;
  @StorageLink('padBack') @Watch('backPressed') private backEpoch: number = 0;
  private backPressed(): void {
    if (this.menuOpen || this.exitAsk) { this.menuOpen = false; this.exitAsk = false; return; }
    this.sources.set('sys:back', 2); this.publish(); setTimeout(() => { this.sources.delete('sys:back'); this.publish(); }, 150);
  }
  @State private menuOpen: boolean = false;      // top-left game menu (quality, controls, exit)
  @State private exitAsk: boolean = false;       // exit confirmation
  // Dynamic 3D scale (iOS-style): drop the 3D render scale while the frame rate sags (heat), restore it when steady.
  @State private dynScale: number = 0;           // 0 = not adjusted (mode ceiling)
  private slowSecs: number = 0;
  private fastSecs: number = 0;   // touch overlay: menus are tapped directly; the Switch pad only on request
  @StorageLink('fpsShown') private fpsShown: boolean = true;
  @State private fps: string = '';
  @State private fastMode: boolean = true;
  @State private hud: number[] = [];
  private artDir: string = '';
  // Positions of the five skill keys from the last combat frame (overlay vp): in panels / NPC dialogue the same circles
  // become the iOS navigation keys (SwTouchOverlay navigationMode labels), so the player can always confirm / go back.
  private navSpots: number[][] = [];
  private hudTimer: number = -1;
  private fpsTimer: number = -1;
  private fpsFrames: number = 0;
  private fpsAt: number = 0;
  private stickFinger: number = -1;
  private sources: Map<string, number> = new Map<string, number>();
  private lx: number = 0;
  private loggedLx: number = 0;
  private loggedLy: number = 0;
  private loggedMask: number = 0;
  private ly: number = 0;

  // Layout scales with the short screen edge (phones ~360vp, Pura X / wide folds
  // ~470vp+) and follows the 16:9 game image: on tall phones the side bars hold
  // the controls; on wide folds the face buttons sit above the game's HUD bar.
  private padScale(): number { return Math.min(1.0, Math.max(0.68, Math.min(this.padWidth, this.padHeight) / 470)); }
  // The game image fills the whole surface (native aspect, edge-to-edge).
  private videoRect(): number[] { return [0, 0, this.padWidth, this.padHeight]; }
  private stickRadius(): number { return STICK_RADIUS * this.padScale(); }
  private buttons(): PadButton[] {
    const w = this.padWidth, h = this.padHeight, s = this.padScale(), v = this.videoRect();
    const face = 27 * s, small = 21 * s, d = 50 * s, mini = 17 * s;
    const side = v[0];                       // width of each side bar (0 when the image fills the width)
    const roomy = side >= 2 * (d + face) + 8;
    const cx = roomy ? w - side / 2 : w - 18 * s - d - face;
    const cy = roomy ? h * 0.62 : Math.min(h * 0.6, v[1] + v[3] * 0.82 - d - face);
    const top = Math.max(small + 8, v[1] + small + 6);
    const dx = roomy ? side / 2 : 18 * s + 2 * mini + 4, dy = roomy ? h * 0.3 : h * 0.34;
    return [
      { label: 'A', mask: 1, x: cx + d, y: cy, r: face * 1.1 },
      { label: 'B', mask: 2, x: cx, y: cy + d, r: face },
      { label: 'X', mask: 4, x: cx, y: cy - d, r: face },
      { label: 'Y', mask: 8, x: cx - d, y: cy, r: face },
      { label: 'L', mask: 64, x: 12 + small, y: top, r: small },
      { label: 'ZL', mask: 256, x: 12 + small * 3 + 10 * s, y: top, r: small },
      { label: 'R', mask: 128, x: w - 12 - small, y: top, r: small },
      { label: 'ZR', mask: 512, x: w - 12 - small * 3 - 10 * s, y: top, r: small },
      { label: '−', mask: 2048, x: w / 2 - 150 * s, y: top, r: mini },
      { label: '+', mask: 1024, x: w / 2 + 150 * s, y: top, r: mini },
      { label: '↑', mask: 8192, x: dx, y: dy - 2 * mini - 2, r: mini },
      { label: '↓', mask: 32768, x: dx, y: dy + 2 * mini + 2, r: mini },
      { label: '←', mask: 4096, x: dx - 2 * mini - 2, y: dy, r: mini },
      { label: '→', mask: 16384, x: dx + 2 * mini + 2, y: dy, r: mini }
    ];
  }

  private publish(): void {
    let mask = 0;
    this.sources.forEach((value: number) => { mask |= value; });
    this.pressed = this.active ? mask : 0;
    const lx = this.active ? this.lx : 0, ly = this.active ? this.ly : 0;
    host.setGameButtons(this.pressed, lx, ly, 0, 0);
    if (lx !== this.loggedLx || ly !== this.loggedLy || this.pressed !== this.loggedMask) { this.loggedLx = lx; this.loggedLy = ly; this.loggedMask = this.pressed;
      hilog.info(0, 'XblPad', 'publish mask=%{public}d lx=%{public}d ly=%{public}d', this.pressed, lx, ly); }
  }
  private reset(): void {
    this.sources.clear(); this.stickFinger = -1; this.stickActive = false; this.lx = 0; this.ly = 0;
    this.knobX = 0; this.knobY = 0; this.publish(); host.cancelGameButtons();
  }
  aboutToAppear(): void {
    this.fpsAt = Date.now(); try { this.fpsFrames = host.getPresentedFrames(); } catch (e) { this.fpsFrames = 0; }
    try { this.fastMode = host.getScaleMode(); } catch (e) { this.fastMode = true; }
    try { this.artDir = 'file://' + getContext(this).filesDir + '/touch-overlay/touchart/'; } catch (e) { this.artDir = ''; }
    this.hudTimer = setInterval(() => {
      if (!this.active) { if (this.hud.length) { this.hud = []; } return; }
      try { const h = host.getTouchHud(); if (h.length !== this.hud.length || h.some((v: number, i: number) => v !== this.hud[i])) { this.hud = h; }
        if (this.touchMode()) { const spots: number[][] = []; for (let t = 0; t <= 4; t++) { const e = 16 + t * 5; if (h[e + 2] > 0) { spots.push([this.hx(h[e]), this.hy(h[e + 1]), this.hs(h[e + 2])]); } } if (spots.length === 5) { this.navSpots = spots; } }
      } catch (e) { }
    }, 33);
    this.fpsTimer = setInterval(() => {
      let frames = 0; try { frames = host.getPresentedFrames(); } catch (e) { return; }
      const now = Date.now(), dt = (now - this.fpsAt) / 1000;
      if (dt > 0) { this.fps = `${Math.round((frames - this.fpsFrames) / dt)} FPS`; }
      hilog.info(0, 'XblFps', 'frames=%{public}d prev=%{public}d dt=%{public}f', frames, this.fpsFrames, dt);
      this.fpsFrames = frames; this.fpsAt = now;
      if (dt > 0) { this.adaptScale(Math.round(Number(this.fps.split(' ')[0]))); }
    }, 1000);
  }
  aboutToDisappear(): void { if (this.fpsTimer >= 0) { clearInterval(this.fpsTimer); this.fpsTimer = -1; } if (this.hudTimer >= 0) { clearInterval(this.hudTimer); this.hudTimer = -1; } this.reset(); }
  // Knob offset from the base centre, clamped to the base radius (logical units).
  private knob(axis: number): number {
    const h = this.hud, dx = h[11] - h[9], dy = h[12] - h[10], d = Math.hypot(dx, dy), r = h[15];
    const k = d > r && d > 0 ? r / d : 1; return (axis === 0 ? dx : dy) * k;
  }
  // Game running with a panel open (mode 1 = game, navigation != 0).
  private panelMode(): boolean { return this.hud.length >= 3 && this.hud[0] === 0 && this.hud[1] === 1 && this.hud[2] !== 0; }
  private panelButtons(): PadButton[] {
    // Side margins beside the panel frame (the tab row and panel content stay tappable).
    const s = this.padScale(), r = 24 * s, w = this.padWidth, h = this.padHeight, lx = 22 + r, rx = w - 22 - r;
    return [{ label: '上页', mask: 64, x: lx, y: h * 0.42, r: r }, { label: '下页', mask: 128, x: rx, y: h * 0.42, r: r },
      { label: '确认', mask: 1, x: rx, y: h * 0.42 + r * 2.6, r: r }, { label: '返回', mask: 2, x: rx, y: h * 0.42 + r * 5.2, r: r }];
  }
  // iOS navigation labels on the skill circles: main = confirm / swap (A), skill 1 = back (B), 2 = action (X), 3 = bind (Y), 4 = next page (R).
  private navButtons(): PadButton[] {
    const labels: string[] = ['确认', '返回', '操作', '绑定', '下一页'], masks: number[] = [1, 2, 4, 8, 128];
    const out: PadButton[] = [];
    if (this.navSpots.length === 5) {
      for (let i = 0; i < 5; i++) { out.push({ label: labels[i], mask: masks[i], x: this.navSpots[i][0], y: this.navSpots[i][1], r: this.navSpots[i][2] * (i === 0 ? 0.8 : 1) }); }
    } else {
      const s = this.padScale(), r = 28 * s, w = this.padWidth, h = this.padHeight, cx = w - 70 * s, cy = h * 0.72;
      out.push({ label: labels[0], mask: 1, x: cx, y: cy, r: r * 1.2 }, { label: labels[1], mask: 2, x: cx - 80 * s, y: cy + 40 * s, r: r },
        { label: labels[2], mask: 4, x: cx - 95 * s, y: cy - 45 * s, r: r }, { label: labels[3], mask: 8, x: cx - 40 * s, y: cy - 110 * s, r: r },
        { label: labels[4], mask: 128, x: cx + 30 * s, y: cy - 140 * s, r: r });
    }
    const s2 = this.padScale(), rl = 24 * s2;
    out.push({ label: '上一页', mask: 64, x: 22 + rl, y: this.padHeight * 0.42, r: rl });
    return out;
  }
  private menuButtons(): PadButton[] {
    const s = this.padScale(), r = 24 * s, w = this.padWidth, top = r + 12;
    return [{ label: '菜单', mask: 1024, x: w - 14 - r, y: top, r: r }, { label: '地图', mask: 2048, x: w - 14 - r * 3 - 10 * s, y: top, r: r }];
  }
  // Top-left normally; when the Switch pad is shown its L / ZL keys own that corner, so the menu moves beside the − key.
  private menuX(): number { return (this.hudV4() && this.frontPad && !this.touchMode()) || (!this.hudV4() && !this.touchMode() && !this.panelMode()) ? this.padWidth / 2 - 230 * this.padScale() : 14; }
  private scaleCeiling(): number { return this.fastMode ? 0.45 : 0.53; }
  // In combat only: 3 s under 26 fps -> one step down (0.05, floor 0.35); 20 s at 29+ -> one step back up to the ceiling.
  private adaptScale(fps: number): void {
    if (!this.touchMode() || fps <= 0) { this.slowSecs = 0; this.fastSecs = 0; return; }
    const cur = this.dynScale > 0 ? this.dynScale : this.scaleCeiling();
    if (fps < 26) { this.slowSecs++; this.fastSecs = 0; } else if (fps >= 29) { this.fastSecs++; this.slowSecs = 0; } else { this.slowSecs = 0; this.fastSecs = 0; }
    let next = cur;
    if (this.slowSecs >= 3 && cur > 0.351) { next = Math.max(0.35, cur - 0.05); this.slowSecs = 0; }
    else if (this.fastSecs >= 20 && cur < this.scaleCeiling() - 0.001) { next = Math.min(this.scaleCeiling(), cur + 0.05); this.fastSecs = 0; }
    if (Math.abs(next - cur) > 0.001) {
      this.dynScale = Math.abs(next - this.scaleCeiling()) < 0.001 ? 0 : next;
      try { host.setScale3d(next); } catch (e) { }
      hilog.info(0, 'XblScale', 'dynamic 3D scale %{public}f (fps %{public}d)', next, fps);
    }
  }
  private pressStart(): void {   // open the game's own pause menu (保存并退出 lives there)
    this.sources.set('menu:start', 1024); this.publish();
    setTimeout(() => { this.sources.delete('menu:start'); this.publish(); }, 150);
  }
  private quitApp(): void {
    this.reset();
    try { (getContext(this) as common.UIAbilityContext).terminateSelf(); } catch (e) { gameWindow.request(false); }
  }
  private touchMode(): boolean { return this.hud.length > 16 && this.hud[0] === 1; }
  // Full iOS touch overlay loaded (front end taps directly, bottom bar drawn by the game): no Switch pad, no extra menu keys.
  private hudV4(): boolean { return this.hud.length >= 96 ? this.hud[95] === 1 : (this.hud.length >= 4 && this.hud[0] === 0 && this.hud[3] === 1); }
  // Logical core point -> overlay vp.
  private hx(x: number): number { const h = this.hud; return (h[3] + x / h[1] * h[5]) * this.padWidth; }
  private hy(y: number): number { const h = this.hud; return (h[4] + y / h[2] * h[6]) * this.padHeight; }
  private hs(r: number): number { const h = this.hud; return r / h[2] * h[6] * this.padHeight; }
  private hudCircles(): HudCircle[] {
    const h = this.hud, out: HudCircle[] = [];
    for (let t = 0; t < TC_COUNT; t++) {
      const e = 16 + t * 5; if (!TC_LABELS[t] || h[e + 2] <= 0 || h[e + 4] === 0) { continue; }
      const gi = 16 + TC_COUNT * 5 + 4 + t * 3, skill = t <= 4, art = skill && h.length >= 96;
      out.push({ key: `t${t}`, label: TC_LABELS[t], x: this.hx(h[e]), y: this.hy(h[e + 1]), r: this.hs(h[e + 2]), pressed: h[e + 3] === 1, small: t === 11,
        kind: skill ? KIND_SKILL : (t === 11 && this.artDir && this.hudV4() ? KIND_MAP : KIND_PLAIN),
        glyph: art ? h[gi] : 0, atlas: art ? h[gi + 1] : 0, frame: art ? h[gi + 2] : 0 });
    }
    const w = 16 + TC_COUNT * 5;
    if (h[w + 2] > 0) { out.push({ key: 'swap', label: h[7] ? 'Ⅱ' : 'Ⅰ', x: this.hx(h[w]), y: this.hy(h[w + 1]), r: this.hs(h[w + 2]), pressed: h[w + 3] === 1, small: false,
      kind: KIND_SKILL, glyph: 3, atlas: 0, frame: 0 }); }
    return out;
  }

  private buttonTouch(event: TouchEvent, mask: number): void {
    event.stopPropagation();
    if (!this.active) { this.reset(); return; }
    event.changedTouches.forEach((point: TouchObject) => {
      const source = `b${mask}:${point.id}`;
      if (event.type === TouchType.Down) { this.sources.set(source, mask); }
      else if (event.type === TouchType.Up || event.type === TouchType.Cancel) { this.sources.delete(source); }
    });
    this.publish();
  }

  // Stick zone: local coordinates of the left-bottom touch area.
  private stickTouch(event: TouchEvent): void {
    event.stopPropagation();
    if (!this.active) { this.reset(); return; }
    event.changedTouches.forEach((point: TouchObject) => {
      if (event.type === TouchType.Down && this.stickFinger < 0) {
        this.stickFinger = point.id; this.stickActive = true;
        this.originX = point.x; this.originY = point.y; this.knobX = 0; this.knobY = 0;
        this.lx = 0; this.ly = 0;
      } else if (point.id === this.stickFinger) {
        if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
          this.stickFinger = -1; this.stickActive = false; this.knobX = 0; this.knobY = 0; this.lx = 0; this.ly = 0;
        } else {
          let dx = point.x - this.originX, dy = point.y - this.originY;
          const len = Math.hypot(dx, dy);
          const R = this.stickRadius();
          if (len > R) { dx = dx / len * R; dy = dy / len * R; }
          this.knobX = dx; this.knobY = dy;
          const m = Math.min(len, R) / R;
          if (m < DEAD_ZONE || len <= 0) { this.lx = 0; this.ly = 0; }
          else {
            const scaled = (m - DEAD_ZONE) / (1 - DEAD_ZONE);
            // iOS sw_touch_controls.c: the game rotates the stick 45° into world space without the
            // isometric 2:1 compensation; pre-correct so the character walks where the finger points.
            let ex = dx / len, ey = -dy / len; const n = Math.hypot(ex, 2 * ey); ex = ex / n; ey = 2 * ey / n;
            this.lx = Math.round(ex * scaled * 32767);
            this.ly = Math.round(ey * scaled * 32767);
          }
        }
      }
    });
    this.publish();
  }

  // Touches on the overlay's empty area go to the game exactly like touches on
  // the game surface (world mapper / UI mouse), relative to the 16:9 image.
  private surfaceTouch(event: TouchEvent): void {
    if (!this.active) { return; }
    const v = this.videoRect(), vw = v[2], vh = v[3], ox = v[0], oy = v[1];
    const phase = event.type === TouchType.Down ? 0 : event.type === TouchType.Move ? 1 : event.type === TouchType.Up ? 2 : 3;
    event.changedTouches.forEach((point: TouchObject) => {
      try { host.sendSurfaceTouch(phase, point.id, point.x - ox, point.y - oy, vw, vh); } catch (error) { /* invalid geometry */ }
    });
  }

  private handleKey(event: KeyEvent): boolean {
    if (!this.active) { return false; }
    let mask = 0;
    switch (event.keyCode) {
      case KeyCode.KEYCODE_BUTTON_A: case KeyCode.KEYCODE_ENTER: case KeyCode.KEYCODE_DPAD_CENTER: mask = 1; break;
      case KeyCode.KEYCODE_BUTTON_B: case KeyCode.KEYCODE_ESCAPE: mask = 2; break;
      case KeyCode.KEYCODE_BUTTON_X: mask = 4; break;
      case KeyCode.KEYCODE_BUTTON_Y: mask = 8; break;
      case KeyCode.KEYCODE_BUTTON_L1: mask = 64; break;
      case KeyCode.KEYCODE_BUTTON_R1: mask = 128; break;
      case KeyCode.KEYCODE_BUTTON_L2: mask = 256; break;
      case KeyCode.KEYCODE_BUTTON_R2: mask = 512; break;
      case KeyCode.KEYCODE_BUTTON_START: mask = 1024; break;
      case KeyCode.KEYCODE_BUTTON_SELECT: mask = 2048; break;
      case KeyCode.KEYCODE_DPAD_LEFT: mask = 4096; break;
      case KeyCode.KEYCODE_DPAD_UP: mask = 8192; break;
      case KeyCode.KEYCODE_DPAD_RIGHT: mask = 16384; break;
      case KeyCode.KEYCODE_DPAD_DOWN: mask = 32768; break;
      default: return false;
    }
    const source = `key:${event.keyCode}`;
    if (event.type === KeyType.Down) { this.sources.set(source, mask); }
    else if (event.type === KeyType.Up) { this.sources.delete(source); }
    this.publish(); return true;
  }

  @Builder
  private menuItem(label: string, action: () => void) {
    Text(label).fontSize(14).fontColor('rgba(232,214,170,0.95)').width(200).height(36).textAlign(TextAlign.Center)
      .backgroundColor('rgba(40,31,21,0.85)').border({ width: 1, color: 'rgba(120,94,56,0.8)', radius: 4 })
      .onClick(() => { action(); })
  }

  @Builder
  private navButton(b: PadButton) {
    Stack({ alignContent: Alignment.Center }) {
      Circle().width(b.r * 2).height(b.r * 2).fill((this.pressed & b.mask) ? 'rgba(22,17,12,0.92)' : 'rgba(22,17,12,0.62)')
        .stroke((this.pressed & b.mask) ? 'rgba(153,120,71,1)' : 'rgba(153,120,71,0.8)').strokeWidth(Math.max(1.5, b.r * 0.075)).hitTestBehavior(HitTestMode.None)
      Text(b.label).fontSize(Math.max(11, b.r * (b.label.length > 2 ? 0.42 : 0.5))).fontColor('rgba(232,214,170,0.95)').hitTestBehavior(HitTestMode.None)
    }.width(b.r * 2).height(b.r * 2).position({ x: b.x - b.r, y: b.y - b.r })
      .onTouch((event: TouchEvent) => { this.buttonTouch(event, b.mask); })
  }

  @Builder
  private padButton(b: PadButton) {
    Stack({ alignContent: Alignment.Center }) {
      Circle().width(b.r * 2).height(b.r * 2)
        .fill((this.pressed & b.mask) ? 'rgba(255,255,255,0.45)' : 'rgba(20,24,30,0.28)')
        .stroke('rgba(255,255,255,0.45)').strokeWidth(1.5).hitTestBehavior(HitTestMode.None)
      Text(b.label).fontSize(Math.max(11, b.r * 0.62)).fontColor('rgba(255,255,255,0.9)').fontWeight(FontWeight.Medium)
        .hitTestBehavior(HitTestMode.None)
    }.width(b.r * 2).height(b.r * 2).position({ x: b.x - b.r, y: b.y - b.r })
      .onTouch((event: TouchEvent) => { this.buttonTouch(event, b.mask); })
  }

  private ringWidth(r: number): number { return Math.max(1.5, r * 0.075); }

  @Builder
  private hudCircle(c: HudCircle) {
    if (c.kind === KIND_SKILL) {
      // iOS / PC look: dark round base + bronze ring (bright outer, dark inner); real skill icon, cross on an empty slot.
      Stack({ alignContent: Alignment.Center }) {
        Circle().width(c.r * 2).height(c.r * 2).fill(c.pressed ? 'rgba(22,17,12,0.92)' : `rgba(22,17,12,${0.80 * SKILL_IDLE_ALPHA})`)
        Circle().width(c.r * 2 - this.ringWidth(c.r)).height(c.r * 2 - this.ringWidth(c.r)).fill(Color.Transparent)
          .stroke(`rgba(153,120,71,${c.pressed ? 1 : 0.9 * SKILL_IDLE_ALPHA})`).strokeWidth(this.ringWidth(c.r))
        Circle().width(c.r * 2 - this.ringWidth(c.r) * 2.6).height(c.r * 2 - this.ringWidth(c.r) * 2.6).fill(Color.Transparent)
          .stroke(`rgba(31,23,13,${0.9 * SKILL_IDLE_ALPHA})`).strokeWidth(this.ringWidth(c.r) * 0.5)
        if (c.glyph === 2 && this.artDir) {
          // Atlas frame clipped out of the silhouette sheet, 64 % of the key's diameter.
          Stack() {
            Image(`${this.artDir}sil_sw_native_skill_${c.atlas}.png`).width(ATLAS_WIDTH[c.atlas] * c.r * 1.28 / ATLAS_FRAME).height(c.r * 1.28)
              .position({ x: -c.frame * ATLAS_PITCH * c.r * 1.28 / ATLAS_FRAME, y: 0 }).objectFit(ImageFit.Fill).interpolation(ImageInterpolation.High)
              .opacity(c.pressed ? 1 : 0.92)
          }.width(c.r * 1.28).height(c.r * 1.28).clip(true)
        } else if (c.glyph === 3) {
          Text(c.label).fontSize(c.r * 0.8).fontColor('rgba(217,201,166,1)')
        } else if (c.glyph === 1 || c.key === 't0') {
          // Empty slot (or the main key without art): bronze cross, never a fake attack icon.
          Stack() {
            Rect().width(c.r * 0.6).height(Math.max(1.5, c.r * 0.085)).fill(`rgba(153,120,71,${c.pressed ? 1 : 0.9})`)
            Rect().width(Math.max(1.5, c.r * 0.085)).height(c.r * 0.6).fill(`rgba(153,120,71,${c.pressed ? 1 : 0.9})`)
          }.width(c.r * 0.6).height(c.r * 0.6)
        } else {
          Text(c.label).fontSize(Math.max(10, c.r * 0.36)).fontColor('rgba(217,201,166,0.84)')
        }
      }.width(c.r * 2).height(c.r * 2).position({ x: c.x - c.r, y: c.y - c.r }).hitTestBehavior(HitTestMode.None)
    } else if (c.kind === KIND_MAP) {
      // Native AutoMap button art (three 36 px frames side by side; frame 1 = pressed).
      Stack() {
        Image(`${this.artDir}sw_native_map.png`).width(c.r * 2 * 3).height(c.r * 2).position({ x: c.pressed ? -c.r * 2 : 0, y: 0 }).objectFit(ImageFit.Fill)
          .opacity(c.pressed ? 1 : 0.82)
      }.width(c.r * 2).height(c.r * 2).clip(true).position({ x: c.x - c.r, y: c.y - c.r }).hitTestBehavior(HitTestMode.None)
    } else {
      Stack({ alignContent: Alignment.Center }) {
        Circle().width(c.r * 2).height(c.r * 2)
          .fill(c.pressed ? 'rgba(255,255,255,0.40)' : 'rgba(20,24,30,0.26)').stroke('rgba(255,255,255,0.50)').strokeWidth(1.5)
        Text(c.label).fontSize(Math.max(11, c.r * (c.small ? 0.5 : 0.62))).fontColor('rgba(255,255,255,0.92)').fontWeight(FontWeight.Medium)
      }.width(c.r * 2).height(c.r * 2).position({ x: c.x - c.r, y: c.y - c.r }).hitTestBehavior(HitTestMode.None)
    }
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      if (!this.hidden && this.touchMode()) {
        // Joystick: floating base where the thumb landed (faint resting base otherwise), knob clamped to the base.
        if (this.hud[8] === 1) {
          Circle().width(this.hs(this.hud[15]) * 2).height(this.hs(this.hud[15]) * 2).fill('rgba(22,17,12,0.25)').stroke('rgba(168,135,82,0.32)').strokeWidth(1.5)
            .position({ x: this.hx(this.hud[9]) - this.hs(this.hud[15]), y: this.hy(this.hud[10]) - this.hs(this.hud[15]) }).hitTestBehavior(HitTestMode.None)
          Circle().width(this.hs(this.hud[15]) * 0.7).height(this.hs(this.hud[15]) * 0.7).fill('rgba(168,135,82,0.55)').stroke('rgba(217,201,166,0.7)').strokeWidth(1)
            .position({ x: this.hx(this.hud[9] + this.knob(0)) - this.hs(this.hud[15]) * 0.35, y: this.hy(this.hud[10] + this.knob(1)) - this.hs(this.hud[15]) * 0.35 }).hitTestBehavior(HitTestMode.None)
        } else {
          Circle().width(this.hs(this.hud[15]) * 2).height(this.hs(this.hud[15]) * 2).fill('rgba(22,17,12,0.10)').stroke('rgba(168,135,82,0.14)').strokeWidth(1)
            .position({ x: this.hx(this.hud[13]) - this.hs(this.hud[15]), y: this.hy(this.hud[14]) - this.hs(this.hud[15]) }).hitTestBehavior(HitTestMode.None)
        }
        ForEach(this.hudCircles(), (c: HudCircle) => { this.hudCircle(c); }, (c: HudCircle) => `${c.key}-${c.pressed}-${c.label}-${Math.round(c.x)}-${Math.round(c.y)}`)
        // Menu (+: inventory / skills / character tabs) and map (−), top right: the touch core's own + cell sits on the HUD bar.
        if (!this.hudV4()) {
          ForEach(this.menuButtons(), (b: PadButton) => { this.padButton(b); }, (b: PadButton) => `m-${b.label}-${this.padWidth}-${this.padHeight}`)
        }
      }
      if (!this.hidden && this.panelMode() && this.hudV4()) {
        ForEach(this.navButtons(), (b: PadButton) => { this.navButton(b); }, (b: PadButton) => `n-${b.label}-${Math.round(b.x)}-${Math.round(b.y)}`)
      }
      if (!this.hidden && this.panelMode() && !this.hudV4()) {
        // In-game panels (inventory, skills, character, map menus): tap the UI directly; only tab switching and back stay as keys.
        ForEach(this.panelButtons(), (b: PadButton) => { this.padButton(b); }, (b: PadButton) => `p-${b.label}-${this.padWidth}-${this.padHeight}`)
      }
      if (!this.hidden && !this.touchMode() && !this.panelMode() && (!this.hudV4() || this.frontPad)) {
      // Floating stick zone (left 45% of width, lower 55% of height).
      Stack({ alignContent: Alignment.TopStart }) {
        if (this.stickActive) {
          Circle().width(this.stickRadius() * 2).height(this.stickRadius() * 2).fill('rgba(20,24,30,0.30)')
            .stroke('rgba(255,255,255,0.45)').strokeWidth(1.5)
            .position({ x: this.originX - this.stickRadius(), y: this.originY - this.stickRadius() }).hitTestBehavior(HitTestMode.None)
          Circle().width(40 * this.padScale()).height(40 * this.padScale()).fill('rgba(255,255,255,0.55)')
            .position({ x: this.originX + this.knobX - 20 * this.padScale(), y: this.originY + this.knobY - 20 * this.padScale() }).hitTestBehavior(HitTestMode.None)
        } else {
          Circle().width(this.stickRadius() * 2).height(this.stickRadius() * 2).fill('rgba(20,24,30,0.14)')
            .stroke('rgba(255,255,255,0.22)').strokeWidth(1)
            .position({ x: Math.max(16, this.videoRect()[0] / 2 - this.stickRadius()), y: this.padHeight * 0.55 * 0.55 - this.stickRadius() })
            .hitTestBehavior(HitTestMode.None)
        }
      }.width(this.padWidth * 0.45).height(this.padHeight * 0.55).position({ x: 0, y: this.padHeight * 0.45 })
        .onTouch((event: TouchEvent) => { this.stickTouch(event); })

      ForEach(this.buttons(), (b: PadButton) => { this.padButton(b); }, (b: PadButton) => `${b.label}-${this.padWidth}-${this.padHeight}`)

      }
      if (this.fpsShown && this.fps) {
        Text(this.fps).fontSize(11).fontColor('rgba(120,255,140,0.9)').backgroundColor('rgba(0,0,0,0.35)')
          .borderRadius(6).padding({ left: 6, right: 6, top: 2, bottom: 2 })
          .position({ x: this.padWidth / 2 - 30, y: 4 }).hitTestBehavior(HitTestMode.None)
      }
      // Game menu button (top left, clear of the minimap): quality, controls, exit. Panels own the screen edges.
      if (!this.panelMode()) {
        Stack() {
          Circle().width(36).height(36).fill('rgba(22,17,12,0.55)').stroke('rgba(153,120,71,0.85)').strokeWidth(1.5)
          Column({ space: 3 }) {
            Rect().width(14).height(2).fill('rgba(217,201,166,0.95)')
            Rect().width(14).height(2).fill('rgba(217,201,166,0.95)')
            Rect().width(14).height(2).fill('rgba(217,201,166,0.95)')
          }
        }.width(36).height(36).position({ x: this.menuX(), y: 12 })
        .onClick(() => { this.menuOpen = !this.menuOpen; this.exitAsk = false; })
        .onTouch((event: TouchEvent) => { event.stopPropagation(); })
      }
      if (this.menuOpen && !this.exitAsk) {
        Column({ space: 6 }) {
          this.menuItem(this.fastMode ? '画质：流畅' : '画质：清晰', () => { this.fastMode = !this.fastMode; this.dynScale = 0; try { host.setScaleMode(this.fastMode); } catch (e) { } })
          if (this.hudV4() && !this.touchMode()) {
            this.menuItem(this.frontPad ? '收起手柄按键' : '显示手柄按键', () => { this.reset(); this.frontPad = !this.frontPad; if (this.frontPad && this.hidden) { this.hidden = false; try { host.setTouchControls(true); } catch (e) { } } this.menuOpen = false; })
          } else {
            this.menuItem(this.hidden ? '显示触屏按键' : '隐藏触屏按键', () => { this.reset(); this.hidden = !this.hidden; try { host.setTouchControls(!this.hidden); } catch (e) { } this.menuOpen = false; })
          }
          this.menuItem(this.fpsShown ? '隐藏帧率' : '显示帧率', () => { this.fpsShown = !this.fpsShown; })
          this.menuItem('退出游戏', () => { this.exitAsk = true; })
        }.padding(10).backgroundColor('rgba(14,11,8,0.92)').border({ width: 1.5, color: 'rgba(153,120,71,0.9)', radius: 6 })
        .position({ x: this.menuX(), y: 56 }).onTouch((event: TouchEvent) => { event.stopPropagation(); })
      }
      if (this.exitAsk) {
        // Exit: in a game the character is saved by the game's own 保存并退出; offer that first.
        Column({ space: 10 }) {
          Text('退出游戏').fontSize(18).fontColor('rgba(232,198,122,1)').fontWeight(FontWeight.Medium)
          Text(this.touchMode() || this.panelMode() ? '建议先在游戏菜单里选「保存并退出」回到角色界面，再退出应用。' : '确定要退出小菠萝吗？')
            .fontSize(13).fontColor('rgba(217,201,166,0.9)').width(260).textAlign(TextAlign.Center)
          if (this.touchMode() || this.panelMode()) {
            this.menuItem('打开游戏菜单（保存并退出）', () => { this.exitAsk = false; this.menuOpen = false; this.pressStart(); })
          }
          this.menuItem('退出应用', () => { this.quitApp(); })
          this.menuItem('取消', () => { this.exitAsk = false; this.menuOpen = false; })
        }.padding(16).backgroundColor('rgba(14,11,8,0.95)').border({ width: 1.5, color: 'rgba(153,120,71,0.9)', radius: 8 })
        .position({ x: this.padWidth / 2 - 150, y: this.padHeight * 0.22 }).width(300).alignItems(HorizontalAlign.Center)
        .onTouch((event: TouchEvent) => { event.stopPropagation(); })
      }
    }.width(this.padWidth).height(this.padHeight).hitTestBehavior(HitTestMode.Default)
      .onTouch((event: TouchEvent) => { this.surfaceTouch(event); })
      .id('floating-pad').focusable(true).defaultFocus(true)
      .onAppear(() => { try { this.getUIContext().getFocusController().requestFocus('floating-pad'); } catch (error) { /* focus is optional (physical gamepad only) */ } })
      .onKeyEvent((event: KeyEvent) => this.handleKey(event))
      .onBlur(() => { this.reset(); })
  }
}
