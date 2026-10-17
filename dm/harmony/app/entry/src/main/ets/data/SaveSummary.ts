import { fileIo } from '@kit.CoreFileKit';

// Read-only summary of the player's characters (D2R .d2s, version 0x61+): class @0x18, level @0x1B, last-played unix
// seconds @0x20, UTF-8 name @0x12B; experience / gold from the "gf" stats section (9-bit stat id, LSB-first bit fields).
export interface SaveSummary {
  name: string; className: string; classIndex: number; level: number; lastPlayed: number; count: number;
  exp: number; gold: number; expPermille: number;   // progress inside the current level, 0..1000
}

const CLASSES: string[] = ['亚马逊', '法师', '死灵法师', '圣骑士', '野蛮人', '德鲁伊', '刺客', '术士'];
// 由用户自己的游戏表生成；仓库不带数据。 | Generated from the user's game table; data stays local.
import { EXP } from './Experience.generated';
// Bit widths of stats 0..15 (ItemStatCost CSvBits): str, ene, dex, vit, statpts, newskills, hp, maxhp, mana, maxmana,
// stamina, maxstamina, level, experience, gold, goldbank.
const WIDTH: number[] = [10, 10, 10, 10, 10, 8, 21, 21, 21, 21, 21, 21, 7, 32, 25, 25];

function stats(bytes: Uint8Array, start: number): Map<number, number> {
  const out = new Map<number, number>(); let pos = 0;
  const read = (n: number): number => {
    let v = 0;
    for (let i = 0; i < n; i++, pos++) { const b = start + (pos >> 3); if (b >= bytes.length) { return -1; } if ((bytes[b] >> (pos & 7)) & 1) { v += Math.pow(2, i); } }
    return v;
  };
  for (let k = 0; k < 32; k++) {
    const id = read(9); if (id < 0 || id === 0x1ff || id >= WIDTH.length) { break; }
    const v = read(WIDTH[id]); if (v < 0) { break; } out.set(id, v);
  }
  return out;
}

function readSave(path: string): SaveSummary | undefined {
  let file: fileIo.File | undefined = undefined;
  try {
    file = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY);
    const buf = new ArrayBuffer(2048); const n = fileIo.readSync(file.fd, buf);
    if (n < 0x15b) { return undefined; }
    const v = new DataView(buf); const bytes = new Uint8Array(buf, 0, n);
    if (v.getUint32(0, true) !== 0xaa55aa55) { return undefined; }
    const nb = new Uint8Array(buf, 0x12b, 0x30);
    let end = nb.indexOf(0); if (end < 0) { end = nb.length; }
    let name = ''; for (let i = 0; i < end; i++) { name += String.fromCharCode(nb[i]); }
    try { name = decodeURIComponent(escape(name)); } catch (e) { /* keep latin1 */ }
    const cls = v.getUint8(0x18); let level = v.getUint8(0x1b);
    let exp = 0, gold = 0;
    for (let i = 0x100; i + 1 < n; i++) {
      if (bytes[i] === 0x67 && bytes[i + 1] === 0x66) {   // "gf"
        const st = stats(bytes, i + 2); exp = st.get(13) ?? 0; gold = (st.get(14) ?? 0) + (st.get(15) ?? 0);
        if (st.has(12)) { level = st.get(12) ?? level; }
        break;
      }
    }
    let permille = 0;
    if (level >= 1 && level < 99) { const a = EXP[level - 1], b = EXP[level]; permille = b > a ? Math.max(0, Math.min(1000, Math.floor((exp - a) * 1000 / (b - a)))) : 0; }
    else if (level >= 99) { permille = 1000; }
    return { name: name, className: CLASSES[cls] ?? '冒险者', classIndex: cls < CLASSES.length ? cls : -1, level: level,
      lastPlayed: v.getUint32(0x20, true), count: 1, exp: exp, gold: gold, expPermille: permille };
  } catch (e) { return undefined; }
  finally { if (file) { fileIo.closeSync(file); } }
}

// All characters, most recently played first.
export function allSaves(filesDir: string): SaveSummary[] {
  const root = filesDir + '/saves'; const out: SaveSummary[] = [];
  try {
    for (const dir of fileIo.listFileSync(root)) {
      const path = `${root}/${dir}/${dir}.d2s`;
      if (!fileIo.accessSync(path)) { continue; }
      const s = readSave(path); if (s) { out.push(s); }
    }
  } catch (e) { return out; }
  out.sort((a: SaveSummary, b: SaveSummary) => b.lastPlayed - a.lastPlayed);
  out.forEach((s: SaveSummary) => { s.count = out.length; });
  return out;
}

export function latestSave(filesDir: string): SaveSummary | undefined {
  const all = allSaves(filesDir); return all.length ? all[0] : undefined;
}
