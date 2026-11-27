// 单文件游戏包导入（同 S2Arm 的 .s2arm 做法）：电脑上 `d2r_gamepack.py` 把「D2R Mobile」文件夹（data/ = 游戏数据，dm/ = 运行包）
// 写成一个不压缩的 POSIX/PAX tar（.d2r；大包可拆成 name.d2r.001、.002…，一起选中），手机上用系统文件选择器选这一个文件导入。
// 第一项必须是 d2r-pack.json（format = d2r-pack），否则一字节都不写。data/… 逐个文件写到 files/romfs/data/…：先写 <名>.d2rimp，写完改名覆盖，
// 中途失败或取消时目标只会是旧文件或完整的新文件；不删除包里没有的旧文件（同文件夹导入）。dm/… 只在没有内置运行组件时导入：
// 先解到 files/dm-import-tmp，全部写完后逐个顶层项替换（同 preparePack）。源文件不改不删。
// Single-file game package import (same approach as S2Arm's .s2arm): on the PC `d2r_gamepack.py` writes the "D2R Mobile" folder (data/ = game data,
// dm/ = runtime pack) as one uncompressed POSIX/PAX tar (.d2r; a large one can be split into name.d2r.001, .002, ... and selected together), and the
// phone imports that one file through the system file picker. The first entry must be d2r-pack.json (format = d2r-pack), otherwise not a byte is written.
// data/... is written file by file to files/romfs/data/...: first as <name>.d2rimp, renamed over the target when complete, so after a failure or a
// cancel each target is either the old file or the complete new one; old files missing from the package are not deleted (as with the folder import).
// dm/... is imported only when the app has no bundled runtime: unpacked into files/dm-import-tmp, then each top-level entry is replaced once
// everything is written (as preparePack). The source file is never modified or deleted.
import { fileIo as fs, statfs } from '@kit.CoreFileKit';

const BLOCK: number = 512;
const CHUNK: number = 8 * 1024 * 1024;
const DM_ENTRIES: string[] = ['out', 'shaders', 'overlay', 'ui-art', 'derived.tsv', 'pack_info.txt'];

// 错误用固定代码抛出，界面按代码出中英文提示 | Errors are thrown with fixed codes; the UI maps them to zh/en messages
export const PACK_CANCELLED: string = 'd2r-pack-cancelled';
export class PackError extends Error {
  code: string; detail: string;
  constructor(code: string, detail: string) { super(code + (detail ? ' ' + detail : '')); this.code = code; this.detail = detail; }
}
export interface PackProgress { files: number; bytes: number; total: number; }
export interface PackResult { files: number; bytes: number; dataFiles: number; dmEntries: number; skippedDm: boolean; }
interface PackManifest { format?: string; version?: number; product?: string; }

class Part { fd: number; start: number; size: number; constructor(fd: number, start: number, size: number) { this.fd = fd; this.start = start; this.size = size; } }
// 按文件名顺序把各分片当作一个连续的流读 | Reads the parts in file-name order as one concatenated stream
class Reader {
  pos: number = 0; size: number;
  private parts: Part[];
  private buf: ArrayBuffer = new ArrayBuffer(CHUNK);
  private start: number = 0; private end: number = 0; private base: number = 0;
  constructor(parts: Part[], size: number) { this.parts = parts; this.size = size; }
  private async readAt(offset: number, into: ArrayBuffer, length: number): Promise<number> {
    for (const p of this.parts) {
      if (offset >= p.start && offset < p.start + p.size) {
        return await fs.read(p.fd, into, { offset: offset - p.start, length: Math.min(length, p.start + p.size - offset) });
      }
    }
    return 0;
  }
  // 返回从当前位置起恰好 n 字节（n ≤ CHUNK）| Returns exactly n bytes (n <= CHUNK) from the current position
  async take(n: number): Promise<Uint8Array> {
    if (n > CHUNK) { throw new PackError('bad_tar', 'block'); }
    if (this.end - this.start < n) {
      const keep = this.end - this.start;
      const fresh = new ArrayBuffer(CHUNK);
      if (keep > 0) { new Uint8Array(fresh).set(new Uint8Array(this.buf, this.start, keep)); }
      this.buf = fresh; this.base += this.start; this.start = 0; this.end = keep;
      while (this.end < n) {
        const room = CHUNK - this.end;
        const scratch = new ArrayBuffer(room);
        const got = await this.readAt(this.base + this.end, scratch, room);
        if (got <= 0) { throw new PackError('truncated', ''); }
        new Uint8Array(this.buf).set(new Uint8Array(scratch, 0, got), this.end);
        this.end += got;
      }
    }
    const view = new Uint8Array(this.buf, this.start, n);
    this.start += n; this.pos += n;
    return view;
  }
}

function text(b: Uint8Array, off: number, len: number): string {
  let end = off;
  while (end < off + len && b[end] !== 0) { end++; }
  let s = '';
  for (let i = off; i < end; i++) { s += String.fromCharCode(b[i]); }
  return s;
}
function octal(b: Uint8Array, off: number, len: number): number {
  if ((b[off] & 0x80) !== 0) {   // GNU base-256
    let v = 0;
    for (let i = off + 1; i < off + len; i++) { v = v * 256 + b[i]; }
    return v;
  }
  const t = text(b, off, len).trim();
  return t.length > 0 ? parseInt(t, 8) : 0;
}
function utf8(b: Uint8Array): string {
  let out = ''; let i = 0;
  while (i < b.length) {
    const c = b[i++];
    if (c < 0x80) { out += String.fromCharCode(c); }
    else if (c < 0xE0) { out += String.fromCharCode(((c & 0x1F) << 6) | (b[i++] & 0x3F)); }
    else if (c < 0xF0) { out += String.fromCharCode(((c & 0x0F) << 12) | ((b[i++] & 0x3F) << 6) | (b[i++] & 0x3F)); }
    else {
      const cp = ((c & 0x07) << 18) | ((b[i++] & 0x3F) << 12) | ((b[i++] & 0x3F) << 6) | (b[i++] & 0x3F);
      const u = cp - 0x10000; out += String.fromCharCode(0xD800 + (u >> 10), 0xDC00 + (u & 0x3FF));
    }
  }
  return out;
}
function pax(b: Uint8Array): Map<string, string> {
  const map = new Map<string, string>(); let i = 0;
  while (i < b.length) {
    let sp = i; while (sp < b.length && b[sp] !== 0x20) { sp++; }
    const len = parseInt(utf8(b.subarray(i, sp)), 10);
    if (!(len > 0) || i + len > b.length) { break; }
    const record = utf8(b.subarray(sp + 1, i + len - 1));
    const eq = record.indexOf('=');
    if (eq > 0) { map.set(record.substring(0, eq), record.substring(eq + 1)); }
    i += len;
  }
  return map;
}
function safe(path: string): string {
  const parts = path.split('/').filter((p: string) => p.length > 0 && p !== '.');
  if (parts.length === 0 || path.startsWith('/') || parts.some((p: string) => p === '..' || p.indexOf('\\') >= 0)) {
    throw new PackError('unsafe_path', path);
  }
  return parts.join('/');
}
function baseName(uri: string): string {
  const s = decodeURIComponent(uri);
  return s.substring(s.lastIndexOf('/') + 1);
}
function ensureDir(dir: string): void { if (!fs.accessSync(dir)) { fs.mkdirSync(dir, true); } }
function removeEntry(path: string): void {
  try { if (fs.statSync(path).isDirectory()) { fs.rmdirSync(path); } else { fs.unlinkSync(path); } } catch (e) { }
}

// 包里第一项之前的大小上限（清单只有几百字节）| Size limit of the manifest entry (a few hundred bytes)
const MANIFEST_MAX: number = 64 * 1024;

export async function importGamePack(uris: string[], filesDir: string, bundledRuntime: boolean,
    progress: (p: PackProgress) => void, cancelled?: () => boolean): Promise<PackResult> {
  const ordered = uris.slice().sort((a: string, b: string) => baseName(a) < baseName(b) ? -1 : baseName(a) > baseName(b) ? 1 : 0);
  const opened: fs.File[] = []; const parts: Part[] = []; let total = 0;
  const closeAll = () => { for (const f of opened) { try { fs.closeSync(f); } catch (e) { } } };
  try {
    for (const u of ordered) {
      const f = fs.openSync(u, fs.OpenMode.READ_ONLY); opened.push(f);
      const size = fs.statSync(f.fd).size; parts.push(new Part(f.fd, total, size)); total += size;
    }
  } catch (e) { closeAll(); throw new PackError('read_failed', JSON.stringify(e)); }
  // 数据逐个文件覆盖：只需要比包多一点的空间（运行包解到临时目录，也在包大小以内）
  // Data is replaced file by file: only slightly more space than the package is needed (the runtime pack's temporary folder is within the package size)
  const free = statfs.getFreeSizeSync(filesDir);
  if (free < total + 512 * 1024 * 1024) {
    closeAll(); throw new PackError('no_space', `${(total / 1073741824).toFixed(1)} ${(free / 1073741824).toFixed(1)}`);
  }
  const romfs = filesDir + '/romfs', dmTmp = filesDir + '/dm-import-tmp';
  const reader = new Reader(parts, total);
  let files = 0, written = 0, dataFiles = 0, last = 0, sawManifest = false, skippedDm = false;
  let longName = '', paxPath = '', paxSize = -1, partial = '';
  try {
    removeEntry(dmTmp);
    while (reader.pos + BLOCK <= total) {
      if (cancelled !== undefined && cancelled()) { throw new PackError(PACK_CANCELLED, ''); }
      const h = await reader.take(BLOCK);
      let empty = true;
      for (let i = 0; i < BLOCK; i++) { if (h[i] !== 0) { empty = false; break; } }
      if (empty) { break; }
      const type = String.fromCharCode(h[156] === 0 ? 0x30 : h[156]);
      let name = text(h, 0, 100);
      const prefix = text(h, 345, 155);
      if (prefix.length > 0) { name = prefix + '/' + name; }
      let size = octal(h, 124, 12);
      if (paxSize >= 0) { size = paxSize; }
      if (paxPath.length > 0) { name = paxPath; } else if (longName.length > 0) { name = longName; }
      const padded = Math.ceil(size / BLOCK) * BLOCK;
      if (type === 'x' || type === 'g' || type === 'L') {
        if (size > MANIFEST_MAX) { throw new PackError('bad_tar', 'header ' + size); }
        const data = new Uint8Array(size); let got = 0;
        while (got < size) { const n = Math.min(CHUNK, size - got); data.set(await reader.take(n), got); got += n; }
        if (padded > size) { await reader.take(padded - size); }
        if (type === 'x') { const m = pax(data); paxPath = m.get('path') ?? ''; paxSize = m.has('size') ? parseInt(m.get('size') as string, 10) : -1; }
        else if (type === 'L') { longName = utf8(data.subarray(0, data.indexOf(0) >= 0 ? data.indexOf(0) : data.length)); }
        continue;
      }
      paxPath = ''; paxSize = -1; longName = '';
      const rel = safe(name);
      if (!sawManifest) {
        // 清单必须是第一项：认不出的包一字节都不写 | The manifest must come first: nothing is written for a package we do not recognize
        if (rel !== 'd2r-pack.json' || type !== '0' || size > MANIFEST_MAX) { throw new PackError('not_pack', rel); }
        const data = new Uint8Array(size); let got = 0;
        while (got < size) { const n = Math.min(CHUNK, size - got); data.set(await reader.take(n), got); got += n; }
        if (padded > size) { await reader.take(padded - size); }
        let m: PackManifest = {};
        try { m = JSON.parse(utf8(data)) as PackManifest; } catch (e) { throw new PackError('not_pack', 'json'); }
        if (m.format !== 'd2r-pack' || m.version !== 1) { throw new PackError('not_pack', `${m.format}/${m.version}`); }
        sawManifest = true; continue;
      }
      // 目标：data/… → files/romfs/data/…；dm/<顶层项>/… → files/dm-import-tmp/…（没有内置组件时）；其余拒收
      // Destination: data/... -> files/romfs/data/...; dm/<top-level entry>/... -> files/dm-import-tmp/... (without bundled runtime); anything else is refused
      let dest = '', isData = false, skip = false;
      const segs = rel.split('/');
      if (segs[0] === 'data') { dest = romfs + '/' + rel; isData = true; }
      else if (segs[0] === 'dm' && segs.length >= 2 && DM_ENTRIES.indexOf(segs[1]) >= 0) {
        if (bundledRuntime) { skip = true; skippedDm = true; } else { dest = dmTmp + '/' + segs.slice(1).join('/'); }
      } else { throw new PackError('unexpected_entry', rel); }
      if (type === '5') { if (!skip) { ensureDir(dest); } continue; }
      if (type !== '0') { throw new PackError('unsupported_entry', `${type} ${rel}`); }
      if (skip) {
        let left = padded;
        while (left > 0) { const n = Math.min(CHUNK, left); await reader.take(n); left -= n; }
        continue;
      }
      ensureDir(dest.substring(0, dest.lastIndexOf('/')));
      partial = dest + '.d2rimp';
      const out = fs.openSync(partial, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try {
        let left = size;
        while (left > 0) {
          if (cancelled !== undefined && cancelled()) { throw new PackError(PACK_CANCELLED, ''); }
          const n = Math.min(CHUNK, left);
          const view = await reader.take(n);
          let off = 0;
          while (off < n) { off += await fs.write(out.fd, view.buffer.slice(view.byteOffset + off, view.byteOffset + n)); }
          left -= n; written += n;
          const now = Date.now();
          if (now - last > 400) { last = now; progress({ files: files, bytes: reader.pos, total: total }); }
        }
      } finally { fs.closeSync(out); }
      fs.renameSync(partial, dest); partial = '';
      if (padded > size) { await reader.take(padded - size); }
      files++; if (isData) { dataFiles++; }
    }
    if (!sawManifest) { throw new PackError('not_pack', 'empty'); }
    // 运行包：全部写完才替换顶层项 | Runtime pack: top-level entries are replaced only after everything is written
    let dmEntries = 0;
    if (fs.accessSync(dmTmp)) {
      ensureDir(filesDir + '/dm');
      for (const n of fs.listFileSync(dmTmp)) { removeEntry(filesDir + '/dm/' + n); fs.renameSync(dmTmp + '/' + n, filesDir + '/dm/' + n); dmEntries++; }
      removeEntry(dmTmp);
    }
    if (dataFiles === 0 && dmEntries === 0) { throw new PackError('empty_pack', ''); }
    progress({ files: files, bytes: total, total: total });
    return { files: files, bytes: written, dataFiles: dataFiles, dmEntries: dmEntries, skippedDm: skippedDm };
  } catch (error) {
    // 只清掉本次的半截文件和运行包临时目录；已经完整替换的数据文件保留（与文件夹导入一样，重新导入即可补齐）
    // Only this import's partial file and the runtime temporary folder are cleaned up; data files already fully replaced stay (as with the folder import; importing again completes them)
    if (partial) { try { fs.unlinkSync(partial); } catch (e) { } }
    removeEntry(dmTmp);
    if (error instanceof PackError) { throw error as PackError; }
    throw new PackError('write_failed', JSON.stringify(error));
  } finally { closeAll(); }
}
