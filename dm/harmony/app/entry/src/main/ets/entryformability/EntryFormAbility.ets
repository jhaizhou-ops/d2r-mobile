import { formBindingData, FormExtensionAbility, formInfo, formProvider } from '@kit.FormKit';
import { Want } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import { i18n } from '@kit.LocalizationKit';
import { allSaves, SaveSummary } from '../data/SaveSummary';

// 桌面服务卡片：只读存档列出英雄（等级、职业、金币、上次游玩），点开进游戏。2×2 = 最近一位，2×4 = 最多三位，4×4 = 最多六位加「继续游戏」。
// 卡片尺寸只在添加时知道，按卡片 id 记在 files/form-ids.txt；应用退到后台时（EntryAbility）刷新全部卡片。
// Home-screen service card: heroes read-only from the saves (level, class, gold, last played); tapping opens the game. 2×2 = the latest hero,
// 2×4 = up to three, 4×4 = up to six plus "continue". The card size is only known when the card is added, so it is kept per card id in
// files/form-ids.txt; all cards are refreshed when the app goes to the background (EntryAbility).
const CLASSES_ZH: string[] = ['亚马逊', '法师', '死灵法师', '圣骑士', '野蛮人', '德鲁伊', '刺客', '术士'];
const CLASSES_EN: string[] = ['Amazon', 'Sorceress', 'Necromancer', 'Paladin', 'Barbarian', 'Druid', 'Assassin', 'Warlock'];

class HeroRow {
  name: string = ''; cls: string = ''; initial: string = ''; img: string = ''; classIndex: number = -1; level: number = 0; gold: string = ''; time: string = '';
}
class CardData {
  dim: number = 2; has: boolean = false; count: number = 0; heroes: HeroRow[] = [];
  formImages: Record<string, number> = {};
  title: string = ''; empty: string = ''; levelWord: string = ''; goldWord: string = ''; countText: string = ''; resume: string = '';
}

function zh(): boolean { try { return i18n.System.getSystemLanguage().startsWith('zh'); } catch (e) { return true; } }

function timeText(sec: number, cn: boolean): string {
  if (!sec) { return ''; }
  const d = new Date(sec * 1000), hm = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  return cn ? `${d.getMonth() + 1}月${d.getDate()}日 ${hm}` : `${d.getMonth() + 1}/${d.getDate()} ${hm}`;
}

// size：2 = 2×2，4 = 2×4，8 = 4×4 | size: 2 = 2×2, 4 = 2×4, 8 = 4×4
export function buildCard(filesDir: string, size: number): CardData {
  const cn = zh(), all: SaveSummary[] = allSaves(filesDir), max = size === 8 ? 6 : size === 4 ? 3 : 1;
  const data = new CardData(); data.dim = size; data.has = all.length > 0; data.count = all.length;
  data.title = cn ? '暗黑破坏神 II' : 'Diablo II';
  data.empty = cn ? '还没有英雄 · 点开创建' : 'No heroes yet · tap to create one';
  data.levelWord = cn ? '级' : 'Lv'; data.goldWord = cn ? '金币' : 'Gold';
  data.countText = cn ? `共 ${all.length} 位英雄` : `${all.length} heroes`;
  data.resume = cn ? '继续游戏' : 'Continue';
  data.heroes = all.slice(0, max).map((h: SaveSummary) => {
    const r = new HeroRow(), names = cn ? CLASSES_ZH : CLASSES_EN;
    r.name = h.name; r.classIndex = h.classIndex; r.cls = h.classIndex >= 0 && h.classIndex < names.length ? names[h.classIndex] : (cn ? '冒险者' : 'Hero');
    r.initial = r.cls.substring(0, 1); r.level = h.level; r.gold = h.gold.toLocaleString(); r.time = timeText(h.lastPlayed, cn);
    return r;
  });
  return data;
}

// The portraits are generated from the player's own HIREABLES class icons, never shipped as source assets.
class PreparedCard {
  data: CardData = new CardData();
  opened: fileIo.File[] = [];
}
const initialImages: Map<string, fileIo.File[]> = new Map();
function closeImages(opened: fileIo.File[]): void {
  for (const file of opened) { try { fileIo.closeSync(file); } catch (e) { } }
}
function releaseInitialImages(id: string): void {
  const old = initialImages.get(id);
  if (old) { closeImages(old); initialImages.delete(id); }
}
function prepareCard(filesDir: string, size: number): PreparedCard {
  const card = new PreparedCard(); card.data = buildCard(filesDir, size);
  for (const hero of card.data.heroes) {
    if (hero.classIndex < 0 || hero.classIndex >= CLASSES_ZH.length) { continue; }
    const key = `class${hero.classIndex}`;
    if (card.data.formImages[key] !== undefined) { hero.img = `memory://${key}`; continue; }
    for (const base of ['/dm/overlay/touchart/', '/touch-overlay/touchart/']) {
      try {
        const path = `${filesDir}${base}class_${hero.classIndex}.png`;
        if (!fileIo.accessSync(path)) { continue; }
        const file = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY);
        card.opened.push(file); card.data.formImages[key] = file.fd; hero.img = `memory://${key}`;
        break;
      } catch (e) { /* Missing artwork keeps the class initial. */ }
    }
  }
  return card;
}
function updateCard(filesDir: string, size: number, id: string): void {
  const card = prepareCard(filesDir, size);
  // Keep descriptors open until FormKit finishes copying them to the card host.
  formProvider.updateForm(id, formBindingData.createFormBindingData(card.data))
    .then(() => { releaseInitialImages(id); })
    .catch(() => {})
    .finally(() => { closeImages(card.opened); });
}

// 卡片 id → 尺寸，一行一个「id 尺寸」| Card id → size, one "id size" per line
function idsFile(filesDir: string): string { return `${filesDir}/form-ids.txt`; }
export function readForms(filesDir: string): Map<string, number> {
  const m = new Map<string, number>();
  try {
    for (const l of fileIo.readTextSync(idsFile(filesDir)).split(String.fromCharCode(10))) {
      const p = l.split(' '); if (p.length === 2) { m.set(p[0], Number(p[1])); }
    }
  } catch (e) { /* 还没有卡片 | no cards yet */ }
  return m;
}
function writeForms(filesDir: string, m: Map<string, number>): void {
  const lines: string[] = []; m.forEach((v: number, k: string) => { lines.push(`${k} ${v}`); });
  try {
    const f = fileIo.openSync(idsFile(filesDir), fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
    fileIo.writeSync(f.fd, lines.join(String.fromCharCode(10))); fileIo.closeSync(f);
  } catch (e) { /* 记不下就按 2×2 显示 | falls back to 2×2 */ }
}

// 刷新全部卡片（应用退到后台时调）| Refresh every card (called when the app goes to the background)
export function refreshForms(filesDir: string): void {
  readForms(filesDir).forEach((size: number, id: string) => {
    updateCard(filesDir, size, id);
  });
}

function sizeOf(want: Want): number {
  const dim = want.parameters ? want.parameters[formInfo.FormParam.DIMENSION_KEY] as number : 0;
  return dim === formInfo.FormDimension.Dimension_4_4 ? 8 : dim === formInfo.FormDimension.Dimension_2_4 ? 4 : 2;
}

export default class EntryFormAbility extends FormExtensionAbility {
  onAddForm(want: Want): formBindingData.FormBindingData {
    const id = want.parameters ? want.parameters[formInfo.FormParam.IDENTITY_KEY] as string : '', size = sizeOf(want);
    if (id) { const m = readForms(this.context.filesDir); m.set(id, size); writeForms(this.context.filesDir, m); }
    const card = prepareCard(this.context.filesDir, size);
    releaseInitialImages(id); initialImages.set(id, card.opened);
    return formBindingData.createFormBindingData(card.data);
  }
  onUpdateForm(formId: string): void {
    const size = readForms(this.context.filesDir).get(formId) ?? 2;
    updateCard(this.context.filesDir, size, formId);
  }
  onRemoveForm(formId: string): void { releaseInitialImages(formId); const m = readForms(this.context.filesDir); m.delete(formId); writeForms(this.context.filesDir, m); }
  onFormEvent(formId: string, message: string): void { this.onUpdateForm(formId); }
  onAcquireFormState(want: Want): formInfo.FormState { return formInfo.FormState.READY; }
}
