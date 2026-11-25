// 触控覆盖层：iOS DmTouchOverlay.m（drawRect / drawV4）与 DmTouchControlArt.h 的逐段搬运，画在 ArkUI Canvas 上。
// 状态全部来自原生（touch_ohos.c 的绘图快照，与 iOS 同一个触控状态机 dm_touch_controls.c）；触点原样交原生，不在这里判手势。
// 素材同 iOS：底栏 v4（overlay/hud_v4_ui）、技能图集（overlay/touchart）及其银色剪影（ui-art/sil_*，解出时按 dm_skill_silver 去色，见 silver）、
// 技能键标签（ui-art/lbl_*）、通用槽底（安装包 rawfile dm_mobile_socket_neutral.png = dm_mobile_socket.png 按 dm_socket_neutralize 去色）。
// ArkUI Canvas 没有 saturation / luminosity 混合：去色都在素材生成时做好，效果同 iOS。
// Touch overlay: a section-by-section port of iOS DmTouchOverlay.m (drawRect / drawV4) and DmTouchControlArt.h onto an ArkUI Canvas.
// All state comes from native code (the drawing snapshot of touch_ohos.c, the same touch state machine dm_touch_controls.c as iOS); touches go to native unchanged, no gesture logic here.
// Art as iOS: bottom bar v4 (overlay/hud_v4_ui), skill atlases (overlay/touchart) and their silver silhouettes (ui-art/sil_*, desaturated on decode as dm_skill_silver, see silver),
// skill button labels (ui-art/lbl_*), the generic socket (package rawfile dm_mobile_socket_neutral.png = dm_mobile_socket.png desaturated as dm_socket_neutralize).
// ArkUI Canvas has no saturation / luminosity blend modes: the desaturation is baked in when the art is generated, same result as iOS.
import dm from 'libdm.so';
import { fileIo } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

// touch_ohos.h DM_OS_*
const VISIBLE = 0, VX = 1, VY = 2, VW = 3, VH = 4, NAV = 5, SNAV = 6, DIALOG = 7, BLOCKED = 8, SET = 9, MAPCANCEL = 10, DIFFOPEN = 11,
  PLUSHELD = 12, BARVIS = 13, DEAD = 14, JOYACT = 15, JOYX = 16, JOYY = 17, JOYR = 18, LX = 19, LY = 20, SWX = 21, SWY = 22, SWR = 23,
  SWHELD = 24, COVER = 25, TARGETS = 32, TSTRIDE = 6, GLYPHS = TARGETS + 12 * TSTRIDE, GSTRIDE = 8, HUD = GLYPHS + 6 * GSTRIDE, FLOATS = HUD + 16;
// dm_touch_controls.h 控件号 | widget ids
const TC_MAIN = 0, TC_SKILL4 = 4, TC_PORTAL = 5, TC_MENU = 10, TC_MAP = 11, TC_COUNT = 12;
const TA_UNKNOWN = 0, TA_EMPTY = 1, TA_GLYPH = 2;

interface Glyph { file?: string | null; adv?: number; dx?: number; dy?: number }
type GlyphMap = Record<string, Glyph>;
interface HudUi {
  base: Record<string, number>; slots: number[][]; inner: number[];
  exp: Record<string, number | number[]>; pick: Record<string, number>;
  lv: Record<string, number | GlyphMap>; vital: Record<string, number | GlyphMap>; badge: Record<string, number | GlyphMap>;
}
interface Rect { x: number; y: number; w: number; h: number }

function rgba(r: number, g: number, b: number, a: number): string { return `rgba(${Math.round(r * 255)},${Math.round(g * 255)},${Math.round(b * 255)},${a})`; }
function gray(w: number, a: number): string { return rgba(w, w, w, a); }
function cfmt(format: string, ...values: number[]): string { let i = 0; return format.replace(/%%|%d/g, (m: string) => m === '%%' ? '%' : String(values[i++] ?? 0)); }
function inset(r: Rect, d: number): Rect { return { x: r.x + d, y: r.y + d, w: r.w - 2 * d, h: r.h - 2 * d }; }
function buttonRect(x: number, y: number, r: number): Rect { return { x: x - r, y: y - r, w: r * 2, h: r * 2 }; }

@Component
export struct IosOverlay {
  @Prop @Watch('onEnabled') inputEnabled: boolean = false;
  @Prop overlayRoot: string = '';
  @Prop artRoot: string = '';
  @Prop safeLeft: number = 0;
  @Prop safeRight: number = 0;
  @Prop safeBottom: number = 0;
  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);
  private s: Float32Array = new Float32Array(FLOATS);
  private cw: number = 0;
  private chh: number = 0;
  private timer: number = -1;
  private images: Map<string, image.PixelMap | null> = new Map();
  private sizes: Map<string, number[]> = new Map();
  private frames: Map<string, image.PixelMap | null> = new Map();
  private socket: image.PixelMap | null = null;
  private hudUi: HudUi | null = null;
  private text: Map<string, string> = new Map();

  aboutToAppear(): void {
    try {
      const rm = (getContext(this) as common.UIAbilityContext).resourceManager;
      const src = image.createImageSource(rm.getRawFdSync('dm_mobile_socket_neutral.png'));
      this.socket = src.createPixelMapSync(); src.release();
    } catch (e) { this.socket = null; }
    // 同 iOS CADisplayLink：每帧跑触控计时，快照变了才重画 | As the iOS CADisplayLink: run touch timers every frame, redraw only when the snapshot changed
    this.timer = setInterval(() => {
      if (!this.inputEnabled) { return; }
      if (dm.touchTick(this.s) === 1) { this.draw(); }
    }, 16);
  }
  aboutToDisappear(): void {
    clearInterval(this.timer); dm.touchCancel();
    this.images.forEach((p: image.PixelMap | null) => { p?.release(); });
    this.frames.forEach((p: image.PixelMap | null) => { p?.release(); });
    this.socket?.release(); this.socket = null;
    this.images.clear(); this.frames.clear(); this.sizes.clear();
  }
  onEnabled(): void {
    this.text.clear(); // Keep decoded art across input dialogs; release when the page is destroyed.
    if (!this.hudUi) {
      try { this.hudUi = JSON.parse(fileIo.readTextSync(this.overlayRoot + '/hud_v4_ui/ui.json')) as HudUi; } catch (e) { this.hudUi = null; }
    }
    if (!this.inputEnabled) { dm.touchCancel(); this.ctx.clearRect(0, 0, this.cw, this.chh); }
    this.screen();
  }
  private screen(): void { if (this.cw > 0 && this.chh > 0) { dm.touchScreen(this.cw, this.chh, this.safeLeft, this.safeRight, this.safeBottom); } }
  private ui(key: string): string { let v = this.text.get(key); if (v === undefined) { v = dm.uiText(key); this.text.set(key, v); } return v; }

  // ---- 素材 | Art ----
  private img(path: string): image.PixelMap | null {
    if (this.images.has(path)) { return this.images.get(path) ?? null; }
    let pm: image.PixelMap | null = null;
    try {
      if (fileIo.accessSync(path) && fileIo.statSync(path).size <= 8 * 1024 * 1024) {
        const src = image.createImageSource(path); pm = src.createPixelMapSync(); src.release();
        const info = pm.getImageInfoSync(); this.sizes.set(path, [info.size.width, info.size.height]);
      }
    } catch (e) { pm = null; }
    this.images.set(path, pm); return pm;
  }
  private v4img(name: string): image.PixelMap | null { return this.img(this.overlayRoot + '/hud_v4_ui/' + name); }
  private v4size(name: string): number[] { return this.sizes.get(this.overlayRoot + '/hud_v4_ui/' + name) ?? [0, 0]; }
  // 鸿蒙 Canvas 对 PixelMap 按源矩形裁画不出图：按区域单独解出一帧并缓存（iOS frameImage 同义）
  // HarmonyOS Canvas draws nothing for a PixelMap with a source rectangle: decode each frame as its own region and cache it (same meaning as iOS frameImage)
  private frame(path: string, x: number, y: number, w: number, h: number): image.PixelMap | null {
    const key = `${path}:${x},${y},${w},${h}`;
    if (this.frames.has(key)) { return this.frames.get(key) ?? null; }
    let pm: image.PixelMap | null = null;
    try { if (fileIo.accessSync(path)) { const src = image.createImageSource(path); pm = src.createPixelMapSync({ desiredRegion: { x: x, y: y, size: { width: w, height: h } } }); src.release(); } } catch (e) { pm = null; hilog.warn(0xD2E0, 'XblOverlay', 'frame decode failed %{public}s %{public}s', key, JSON.stringify(e)); }
    if (!pm) { hilog.warn(0xD2E0, 'XblOverlay', 'frame missing %{public}s', key); }
    else if (path.indexOf('/sil_') >= 0) { this.silver(pm); }
    this.frames.set(key, pm); return pm;
  }
  // 技能剪影去色（同 iOS dm_skill_silver：饱和度混合成灰，保留明暗）；导入包里是彩色原图，解出时做一次，已是灰度的旧包结果不变
  // Desaturate skill silhouettes (as iOS dm_skill_silver: saturation blend to gray, shading kept); the import pack carries the colour originals, done once when
  // decoded; packs that are already grayscale come out unchanged
  private silver(pm: image.PixelMap): void {
    try {
      const buf = new ArrayBuffer(pm.getPixelBytesNumber()); pm.readPixelsToBufferSync(buf);
      const p = new Uint8Array(buf), bgra = pm.getImageInfoSync().pixelFormat === image.PixelMapFormat.BGRA_8888;
      const wr = bgra ? .11 : .3, wb = bgra ? .3 : .11;
      for (let i = 0; i + 3 < p.length; i += 4) { const y = Math.round(p[i] * wr + p[i + 1] * .59 + p[i + 2] * wb); p[i] = y; p[i + 1] = y; p[i + 2] = y; }
      pm.writeBufferToPixelsSync(buf);
    } catch (e) { hilog.warn(0xD2E0, 'XblOverlay', 'silver failed %{public}s', JSON.stringify(e)); }
  }
  // skillImage:held:——银色剪影优先（不裁边），没有剪影退回原图集（裁 8%）| Silver silhouette first (no trim); without it fall back to the atlas (8% trim)
  private skillImage(g: number): image.PixelMap | null {
    const o = GLYPHS + g * GSTRIDE;
    if (this.s[o] !== TA_GLYPH || this.s[o + 1] < 0) { return null; }
    const atlas = this.s[o + 1], fx = this.s[o + 2], fy = this.s[o + 3], fw = this.s[o + 4], fh = this.s[o + 5];
    const sil = `${this.artRoot}/sil_dm_native_skill_${atlas}.png`;
    let pm = this.frame(sil, fx, fy, fw, fh);
    if (pm) { return pm; }
    const trim = Math.round(fw * .08);
    return this.frame(`${this.overlayRoot}/touchart/dm_native_skill_${atlas}.png`, fx + trim, fy + trim, fw - 2 * trim, fh - 2 * trim);
  }

  // ---- DmTouchControlArt.h ----
  private ellipse(r: Rect): void { const c = this.ctx; c.beginPath(); c.ellipse(r.x + r.w / 2, r.y + r.h / 2, r.w / 2, r.h / 2, 0, 0, Math.PI * 2); }
  private vgrad(r: Rect, stops: string[]): CanvasGradient {
    const g = this.ctx.createLinearGradient(r.x + r.w / 2, r.y, r.x + r.w / 2, r.y + r.h);
    stops.forEach((c: string, i: number) => g.addColorStop(stops.length > 1 ? i / (stops.length - 1) : 0, c)); return g;
  }
  private buttonHighlight(r: Rect): void {
    const c = this.ctx; c.save(); c.strokeStyle = gray(.64, .72); c.lineWidth = r.w * .015; this.ellipse(inset(r, r.w * .052)); c.stroke(); c.restore();
  }
  private buttonDraw(r: Rect, held: boolean, empty: boolean, primary: boolean): void {
    const c = this.ctx, side = r.w, cx = r.x + r.w / 2, cy = r.y + r.h / 2, rim = inset(r, side * .018), face = inset(r, side * .055);
    c.save();
    c.save(); this.ellipse(rim); c.clip();
    c.fillStyle = this.vgrad(rim, [rgba(.26, .28, .28, empty ? .22 : .9), rgba(.055, .065, .07, empty ? .22 : .95), rgba(.16, .11, .10, empty ? .16 : .7)]);
    c.fillRect(rim.x, rim.y, rim.w, rim.h); c.restore();
    c.strokeStyle = rgba(.008, .012, .015, empty ? .38 : .9); c.lineWidth = side * .025; this.ellipse(rim); c.stroke();
    c.save(); this.ellipse(face); c.clip();
    c.fillStyle = this.vgrad(face, [rgba(.021, .021, .021, empty ? .08 : .94), rgba(.05, .05, .05, empty ? .16 : .94)]);
    c.fillRect(face.x, face.y, face.w, face.h); c.restore();
    if (empty) { c.strokeStyle = gray(held ? .72 : .62, held ? .80 : .60); }
    else { c.strokeStyle = rgba(held ? .64 : .26, held ? .64 : .29, held ? .64 : .28, held ? .72 : .55); }
    c.lineWidth = side * (held ? .018 : (empty ? .012 : .009)); this.ellipse(face); c.stroke();
    if (primary && !empty) { c.strokeStyle = rgba(.31, .23, .22, .52); c.lineWidth = side * .012; this.ellipse(inset(r, side * .035)); c.stroke(); }
    if (empty) {
      const arm = side * .055; c.strokeStyle = rgba(.60, .65, .64, held ? .75 : .25); c.lineWidth = side * .018; c.lineCap = 'round';
      c.beginPath(); c.moveTo(cx - arm, cy); c.lineTo(cx + arm, cy); c.moveTo(cx, cy - arm); c.lineTo(cx, cy + arm); c.stroke();
    }
    c.restore();
  }
  // drawButton:held:empty:primary:——有槽底素材且非空槽：槽底（已去色）+ 按下高光；否则程序画 | With socket art and a non-empty slot: socket (desaturated) + held highlight; otherwise drawn procedurally
  private drawButton(r: Rect, held: boolean, empty: boolean, primary: boolean): void {
    if (this.socket && !empty) { this.ctx.drawImage(this.socket, r.x, r.y, r.w, r.h); if (held) { this.buttonHighlight(r); } }
    else { this.buttonDraw(r, held, empty, primary); }
  }
  private joystickBase(r: Rect, active: boolean): void {
    const c = this.ctx, side = r.w, cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    c.save();
    const g = c.createRadialGradient(cx, cy, 0, cx, cy, side * .48);
    g.addColorStop(0, gray(.025, active ? .10 : .045)); g.addColorStop(.78, gray(.025, active ? .22 : .12)); g.addColorStop(1, gray(.025, 0));
    c.fillStyle = g; c.fillRect(r.x, r.y, r.w, r.h);
    c.lineWidth = side * .004; c.lineCap = 'round';
    for (let i = 0; i < 4; i++) {
      const angle = i * Math.PI / 2;
      c.strokeStyle = gray(.63, active ? .12 : .055);
      c.beginPath(); c.arc(cx, cy, side * .416, angle + .20, angle + Math.PI / 2 - .20, false); c.stroke();
      c.save(); c.translate(cx, cy); c.rotate(angle);
      const x = side * .414, k = side * .028;
      c.strokeStyle = gray(.68, active ? .25 : .13);
      c.beginPath(); c.moveTo(x + k, 0); c.lineTo(x - k, -k * .65); c.lineTo(x - k * .42, 0); c.lineTo(x - k, k * .65); c.closePath(); c.stroke();
      c.beginPath(); c.moveTo(x - k * .42, 0); c.lineTo(x - k * 2.1, 0); c.stroke();
      c.restore();
    }
    c.restore();
  }
  private joystickThumb(r: Rect, active: boolean): void {
    const c = this.ctx, side = r.w, disc = inset(r, side * .055), cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    c.save();
    c.save(); this.ellipse(disc); c.clip();
    c.fillStyle = this.vgrad(disc, [gray(.23, active ? .78 : .46), gray(.14, active ? .82 : .49), gray(.065, active ? .84 : .52)]);
    c.fillRect(disc.x, disc.y, disc.w, disc.h); c.restore();
    c.strokeStyle = gray(.025, active ? .65 : .32); c.lineWidth = side * .035; this.ellipse(disc); c.stroke();
    c.strokeStyle = gray(.64, active ? .40 : .22); c.lineWidth = side * .014; c.beginPath(); c.arc(cx, cy, side * .437, Math.PI * 1.07, Math.PI * 1.91, false); c.stroke();
    c.strokeStyle = gray(.42, active ? .20 : .10); c.lineWidth = side * .012; c.beginPath(); c.arc(cx, cy, side * .410, Math.PI * 1.12, Math.PI * 1.88, false); c.stroke();
    c.restore();
  }
  private caption(s: string, r: Rect, size: number, alpha: number): void {
    const c = this.ctx; c.save(); c.font = `${size}vp serif`; c.textAlign = 'center'; c.textBaseline = 'top';
    c.fillStyle = rgba(.85, .79, .65, alpha); c.fillText(s, r.x + r.w / 2, r.y); c.restore();
  }

  // ---- drawV4：底部 UI v4（效果图 2868×1320 像素，按视图高缩放、以中线居中）| Bottom UI v4 (mockup pixels 2868×1320, scaled by view height, centered) ----
  private v4text(str: string, glyphs: GlyphMap, x: number, baseline: number, align: number, W: number, u: number): void {
    let adv = 0; for (const ch of str) { adv += glyphs[ch]?.adv ?? 0; }
    let pen = align === 1 ? x - adv : x - adv / 2;
    for (const ch of str) {
      const gm = glyphs[ch]; if (!gm) { continue; }
      const im = gm.file ? this.v4img(gm.file) : null;
      if (im) { const wh = this.v4size(gm.file as string); this.ctx.drawImage(im, (pen + (gm.dx ?? 0) - 1434) * u + W / 2, (baseline + (gm.dy ?? 0)) * u, wh[0] * u, wh[1] * u); }
      pen += gm.adv ?? 0;
    }
  }
  private drawV4(W: number, H: number): void {
    const ui = this.hudUi, s = this.s, c = this.ctx;
    if (!ui || !s[BARVIS]) { return; }
    const u = H / 1320, X = (px: number) => (px - 1434) * u + W / 2, h = HUD;
    const full = this.v4img('base_full.png'), bo = ui.base;
    if (full) { const wh = this.v4size('base_full.png'); c.drawImage(full, X(bo['x']), bo['y'] * u, wh[0] * u, wh[1] * u); }
    { const s6 = ui.slots[5], held = s[PLUSHELD] !== 0, alert = s[h + 7] > 0 || s[h + 8] > 0;
      const name = alert ? (held ? 'lvl_1.png' : 'lvl_0.png') : (held ? 'lvl_d1.png' : 'lvl_d0.png'), btn = this.v4img(name);
      const r: Rect = { x: X(s6[0]), y: s6[1] * u, w: (s6[2] - s6[0]) * u, h: (s6[3] - s6[1]) * u };
      if (btn && s[h + 7] > 0) { c.save(); c.shadowBlur = 10 * u; c.shadowColor = 'rgba(255,209,89,0.95)'; c.drawImage(btn, r.x, r.y, r.w, r.h); c.restore(); }
      if (btn) { c.drawImage(btn, r.x, r.y, r.w, r.h); } }
    const e = ui.exp, x0 = e['x0'] as number, x1 = e['x1'] as number, y = e['y'] as number, eh = e['h'] as number;
    const col = (a: number | number[]) => { const v = a as number[]; return `rgb(${v[0]},${v[1]},${v[2]})`; };
    c.fillStyle = col(e['track']); c.fillRect(X(x0), y * u, (x1 - x0) * u, eh * u);
    c.strokeStyle = col(e['edge']); c.lineWidth = u; c.strokeRect(X(x0) + u / 2, y * u + u / 2, (x1 - x0 - 1) * u, (eh - 1) * u);
    c.fillStyle = col(e['fill']); c.fillRect(X(x0 + 1), (y + 1) * u, (x1 - x0 - 2) * u * s[h + 6] / 1000, (eh - 2) * u);
    const ticks = e['ticks'] as number; c.fillStyle = col(e['track']);
    for (let t = 1; t < ticks; t++) { c.fillRect(X(x0 + Math.round((x1 - x0) * t / ticks)), (y + 1) * u, u, (eh - 2) * u); }
    const vt = ui.vital;
    this.v4text(cfmt(this.ui('dmHudLife'), s[h + 1], s[h + 2]), vt['glyphs'] as GlyphMap, vt['left_x'] as number, vt['baseline'] as number, 0, W, u);
    this.v4text(cfmt(this.ui('dmHudMana'), s[h + 3], s[h + 4]), vt['glyphs'] as GlyphMap, vt['right_x'] as number, vt['baseline'] as number, 0, W, u);
    const lv = ui.lv;
    this.v4text(`Lv ${s[h + 5]} · ${Math.floor(s[h + 6] / 10)}%`, lv['glyphs'] as GlyphMap, lv['x'] as number, lv['baseline'] as number, 0, W, u);
    const slots = ui.slots, inn = ui.inner, bd = ui.badge;
    for (let i = 1; i <= 4; i++) { const n = s[h + 8 + i]; if (!n) { continue; }
      this.v4text(String(n), bd['glyphs'] as GlyphMap, slots[i][0] + inn[2] + (bd['dx'] as number), slots[i][1] + inn[3] + (bd['dy'] as number), 1, W, u); }
    const digit = (n: number, cx: number, cy: number, alpha: number) => { const d = this.v4img(`diff_${n}.png`); if (!d) { return; } const wh = this.v4size(`diff_${n}.png`);
      c.save(); c.globalAlpha = alpha; c.drawImage(d, X(cx) - wh[0] * u / 2, cy * u - wh[1] * u / 2, wh[0] * u, wh[1] * u); c.restore(); };
    const icx = (inn[0] + inn[2] + 1) / 2, icy = (inn[1] + inn[3] + 1) / 2, players = s[h + 13];
    if (players >= 1 && players <= 8) { digit(players, slots[0][0] + icx, slots[0][1] + icy, 1); }
    if (s[DIFFOPEN]) {
      const pk = ui.pick, tile = this.v4img('slot.png'), twh = this.v4size('slot.png');
      for (let i = 0; i < 8; i++) {
        const cx = pk['center_x'] + (i - 3.5) * pk['pitch'], cy = pk['y'], a = (i + 1 === players) ? 1 : .82;
        if (tile) { c.save(); c.globalAlpha = a; c.drawImage(tile, X(cx - twh[0] / 2), (cy - twh[1] / 2) * u, twh[0] * u, twh[1] * u); c.restore(); }
        digit(i + 1, cx, cy, a);
      }
    }
  }

  // ---- drawRect ----
  private draw(): void {
    const c = this.ctx, s = this.s;
    c.clearRect(0, 0, this.cw, this.chh);
    if (s[COVER] === 1) { c.fillStyle = '#000000'; c.fillRect(0, 0, this.cw, this.chh); return; }   // 一键重开黑屏 | quick-restart black cover
    if (!s[VISIBLE]) { return; }
    const W = s[VW], H = s[VH], nav = s[NAV] !== 0, k = s[JOYR] / 60;
    if (k <= 0) { return; }
    c.save(); c.translate(s[VX], s[VY]);
    const labels: string[] = new Array<string>(TC_COUNT).fill(''); labels[TC_MAP] = this.ui('dmTcMap');
    if (nav) { labels[0] = this.ui('dmTcNavConfirm'); labels[1] = this.ui('dmTcNavBack'); labels[2] = this.ui('dmTcNavAction'); labels[3] = this.ui('dmTcNavBind'); labels[4] = this.ui('dmTcNavNext'); }
    for (let i = 0; i < TC_COUNT; i++) {
      const t = TARGETS + i * TSTRIDE; if (!s[t + 3]) { continue; }
      if (i > TC_PORTAL && i <= TC_MENU) { continue; }   // 腰带 / 十字由底栏画 | belt / cross are drawn by the bottom bar
      const tx = s[t], ty = s[t + 1], tr = s[t + 2], held = s[t + 4] !== 0;
      let b = buttonRect(tx, ty, tr);
      const opacity = held ? .6 : .28;
      if (!nav && i === TC_MAP && s[MAPCANCEL]) { c.fillStyle = rgba(.55, .05, .04, .36); c.fillRect(b.x, b.y, b.w, b.h); labels[i] = this.ui('dmTcCancel'); }
      const skillKey = !nav && (i <= TC_SKILL4 || i === TC_PORTAL);
      if (!skillKey) {
        const circle = this.img(this.overlayRoot + '/touchart/dm_native_circle.png');
        if (circle) { c.save(); c.globalAlpha = opacity; c.drawImage(circle, b.x, b.y, b.w, b.h); c.restore(); }
        else { c.fillStyle = gray(.02, .16); this.ellipse(b); c.fill(); c.strokeStyle = rgba(.66, .53, .32, .28); c.lineWidth = k; this.ellipse(b); c.stroke(); }
      }
      let icon: image.PixelMap | null = null, showCaption = true, ssKind = 0, ssCharges = 0;
      if (!nav && i <= TC_SKILL4) {
        const g = GLYPHS + i * GSTRIDE, st = s[g];
        if (st !== TA_EMPTY && st !== TA_UNKNOWN) { ssKind = s[g + 6]; ssCharges = s[g + 7]; }
        icon = this.skillImage(i); showCaption = st !== TA_EMPTY && !icon;
      } else if (!nav && i === TC_PORTAL) { icon = this.skillImage(5); }
      else if (!nav && (i === TC_MENU || i === TC_MAP)) { icon = this.frame(`${this.overlayRoot}/touchart/${i === TC_MAP ? 'dm_native_map' : 'dm_native_menu'}.png`, held ? 36 : 0, 0, 36, 36); }
      if (!nav && i === TC_MAP && s[MAPCANCEL]) { icon = null; showCaption = true; }
      const emptySkill = skillKey && !icon && (!showCaption || i === TC_MAIN);
      if (skillKey) { this.drawButton(b, held, emptySkill, i === TC_MAIN); }
      if (emptySkill) { continue; }
      if (skillKey) {
        const bare = icon !== null, spent = (ssKind & 2) !== 0 && ssCharges <= 0;
        if (icon) {
          const size = b.w * (bare ? .76 : .68), ir: Rect = { x: tx - size / 2, y: ty - (bare ? 0 : tr * .12) - size / 2, w: size, h: size };
          c.save(); this.ellipse(inset(b, b.w * .09)); c.clip();
          if (spent) { c.fillStyle = gray(.30, 1); this.ellipse(inset(b, b.w * .08)); c.fill();
            c.globalAlpha = .55; c.drawImage(icon, ir.x, ir.y, ir.w, ir.h); c.globalAlpha = 1;
            c.fillStyle = gray(0, .35); this.ellipse(inset(b, b.w * .06)); c.fill(); }
          else { c.globalAlpha = held ? 1 : .94; c.drawImage(icon, ir.x, ir.y, ir.w, ir.h); }   // iOS 有一圈淡光晕（图形阴影）；ArkUI 的阴影按整张图的矩形画，成了灰方块，所以不画 | iOS adds a faint glow (shadow); ArkUI draws the shadow from the image's whole rectangle (a gray square), so it is omitted
          c.restore();
        }
        if (ssKind & 1) {   // 「Ⅱ」角标 = 技能在另一组武器上 | "Ⅱ" badge = skill on the other weapon set
          const br = tr * .30, bx = tx + tr * .74, by = ty - tr * .74, bb = buttonRect(bx, by, br), lw2 = inset(bb, Math.max(1, br * .08));
          c.fillStyle = rgba(.11, .094, .078, .95); this.ellipse(bb); c.fill();
          c.lineWidth = Math.max(1, br * .14); c.strokeStyle = rgba(.63, .50, .27, 1); this.ellipse(lw2); c.stroke();
          const ts = br * 1.15; this.caption('Ⅱ', { x: bx - br, y: by - ts * .62, w: br * 2, h: ts * 1.4 }, ts, 1);
        }
        if ((ssKind & 2) && this.hudUi) { const u = H / 1320, bd = this.hudUi.badge;
          this.v4text(String(ssCharges < 0 ? 0 : ssCharges), bd['glyphs'] as GlyphMap, (tx + tr * .66 - W / 2) / u + 1434, (ty + tr * .42) / u, 1, W, u); }
        if (bare) { continue; }
        const ln = i === TC_MAIN ? 'lbl_main.png' : `lbl_f${i + (s[SET] ? 4 : 0)}.png`, lbl = this.img(`${this.artRoot}/${ln}`);
        if (lbl) { const wh = this.sizes.get(`${this.artRoot}/${ln}`) ?? [1, 1], lh = tr * (i === TC_MAIN ? .30 : .36), lw = wh[0] * lh / wh[1];
          c.drawImage(lbl, tx - lw / 2, ty + tr * .50 - lh / 2, lw, lh); }
        continue;
      }
      if (icon) { const size = b.w * (i === TC_MAIN ? .55 : .62); c.save(); c.globalAlpha = held ? 1 : .82; c.drawImage(icon, tx - size / 2, ty - size / 2, size, size); c.restore(); showCaption = false; }
      const textSize = (i === TC_MAIN ? 13 : 10) * k;
      if (showCaption) { this.caption(labels[i], { x: b.x, y: ty - textSize * .6, w: b.w, h: textSize * 1.6 }, textSize, held ? 1 : .84); }
    }
    if (!nav && s[SWR] > 0) {   // 技能组切换键 | skill set switch
      const b = buttonRect(s[SWX], s[SWY], s[SWR]), sheld = s[SWHELD] !== 0;
      this.drawButton(b, sheld, false, false);
      const ts = 12 * k; this.caption(s[SET] ? 'Ⅱ' : 'Ⅰ', { x: b.x, y: s[SWY] - ts * .65, w: b.w, h: ts * 1.6 }, ts, sheld ? 1 : .86);
    }
    this.drawV4(W, H);
    if (s[DEAD] || s[SNAV]) { c.restore(); return; }
    const moving = s[JOYACT] !== 0, ox = s[JOYX], oy = s[JOYY], radius = s[JOYR];
    this.joystickBase({ x: ox - radius, y: oy - radius, w: radius * 2, h: radius * 2 }, moving);
    const dx = s[LX] * radius, dy = -s[LY] * radius;
    this.joystickThumb({ x: ox + dx - 23 * k, y: oy + dy - 23 * k, w: 46 * k, h: 46 * k }, moving);
    c.restore();
  }

  // ---- 触点原样交原生（窗口坐标 vp）| Touches go to native unchanged (window coordinates, vp) ----
  private onTouchEvent(e: TouchEvent): void {
    if (!this.inputEnabled) { return; }
    if (e.type === TouchType.Move) { for (const t of e.touches) { dm.touchEvent(1, t.id, t.x, t.y); } return; }
    const phase = e.type === TouchType.Down ? 0 : e.type === TouchType.Up ? 2 : e.type === TouchType.Cancel ? 3 : -1;
    if (phase < 0) { return; }
    for (const t of e.changedTouches) { dm.touchEvent(phase, t.id, t.x, t.y); }
  }

  build() {
    Canvas(this.ctx)
      .width('100%').height('100%')
      .onAreaChange((_: Area, now: Area) => { this.cw = Number(now.width); this.chh = Number(now.height); this.screen(); })
      .onTouch((e: TouchEvent) => this.onTouchEvent(e))
  }
}
