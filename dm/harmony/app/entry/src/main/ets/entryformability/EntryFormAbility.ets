import { formBindingData, FormExtensionAbility, formInfo, formProvider } from '@kit.FormKit';
import { Want } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import { allSaves, SaveSummary } from '../data/SaveSummary';

// Service card: heroes read-only from the save files (portrait, level, experience bar, gold); tapping opens the game.
// Portraits are the game's own class icons extracted from the player's data (files/touch-overlay/touchart/class_N.png),
// passed to the card as image file descriptors; without them the card shows the class initial.
const MAX_HEROES: number = 3;
class HeroRow {
  name: string = ''; cls: string = ''; level: number = 0; pct: number = 0; bar: number = 0; gold: number = 0; img: string = ''; initial: string = '';
}
class CardData {
  has: boolean = false; count: number = 0; heroes: HeroRow[] = []; wide: boolean = false;
  name: string = '小菠萝2'; cls: string = ''; level: number = 0; pct: number = 0; bar: number = 0; gold: number = 0;
  img: string = ''; initial: string = ''; time: string = ''; formImages: Record<string, number> = {};
}
// 2x4 cards show the hero list; the dimension is only known when the card is added, so it is remembered per card id.
function wideFile(filesDir: string): string { return `${filesDir}/form-wide.txt`; }
function readWide(filesDir: string): string[] {
  try { return fileIo.readTextSync(wideFile(filesDir)).split(String.fromCharCode(10)).filter((l: string) => l.length > 0); } catch (e) { return []; }
}
function rememberWide(filesDir: string, formId: string, wide: boolean): void {
  let ids = readWide(filesDir).filter((l: string) => l !== formId); if (wide) { ids.push(formId); }
  try { const f = fileIo.openSync(wideFile(filesDir), fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC); fileIo.writeSync(f.fd, ids.join(String.fromCharCode(10))); fileIo.closeSync(f); } catch (e) { /* card falls back to 2x2 */ }
}

function timeText(sec: number): string {
  if (!sec) { return ''; }
  const d = new Date(sec * 1000);
  return `${d.getMonth() + 1}月${d.getDate()}日 ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

function buildData(filesDir: string, wide: boolean): CardData {
  const heroes: SaveSummary[] = allSaves(filesDir);
  const images: Record<string, number> = {};
  const list: HeroRow[] = [];
  heroes.slice(0, MAX_HEROES).forEach((h: SaveSummary, i: number) => {
    let img = '';
    if (h.classIndex >= 0) {
      const p = `${filesDir}/touch-overlay/touchart/class_${h.classIndex}.png`;
      try { if (fileIo.accessSync(p)) { const f = fileIo.openSync(p, fileIo.OpenMode.READ_ONLY); images[`hero${i}`] = f.fd; img = `memory://hero${i}`; } } catch (e) { img = ''; }
    }
    const r = new HeroRow(); r.name = h.name; r.cls = h.className; r.level = h.level; r.pct = Math.floor(h.expPermille / 10);
    r.bar = h.expPermille; r.gold = h.gold; r.img = img; r.initial = h.className.substring(0, 1); list.push(r);
  });
  const top = heroes.length ? heroes[0] : undefined;
  const data = new CardData(); data.has = heroes.length > 0; data.count = heroes.length; data.heroes = list; data.wide = wide;
  if (top) { data.name = top.name; data.cls = top.className; data.level = top.level; data.pct = Math.floor(top.expPermille / 10); data.bar = top.expPermille; data.gold = top.gold; }
  if (list.length) { data.img = list[0].img; data.initial = list[0].initial; }
  data.time = top ? `上次游玩 ${timeText(top.lastPlayed)}` : '点开进入游戏';
  data.formImages = images;
  return data;
}

export default class EntryFormAbility extends FormExtensionAbility {
  onAddForm(want: Want): formBindingData.FormBindingData {
    const dim = want.parameters ? want.parameters[formInfo.FormParam.DIMENSION_KEY] as number : 0;
    const id = want.parameters ? want.parameters[formInfo.FormParam.IDENTITY_KEY] as string : '';
    const wide = dim === formInfo.FormDimension.Dimension_2_4;
    if (id) { rememberWide(this.context.filesDir, id, wide); }
    return formBindingData.createFormBindingData(buildData(this.context.filesDir, wide));
  }
  onUpdateForm(formId: string): void {
    const wide = readWide(this.context.filesDir).indexOf(formId) >= 0;
    formProvider.updateForm(formId, formBindingData.createFormBindingData(buildData(this.context.filesDir, wide))).catch(() => {});
  }
  onRemoveForm(formId: string): void { rememberWide(this.context.filesDir, formId, false); }
  onFormEvent(formId: string, message: string): void { this.onUpdateForm(formId); }
  onAcquireFormState(want: Want): formInfo.FormState { return formInfo.FormState.READY; }
}
