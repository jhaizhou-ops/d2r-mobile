// Frozen offline candidate: original resources and saves are never destinations.
// This imports an inactive candidate only. It neither selects nor activates it.
export interface TouchImportFile { path: string; bytes: number; sha256: string; }
export interface TouchImportStat { isFile: boolean; bytes: number; }
export interface TouchImportIo {
  exists(path: string): Promise<boolean>;
  stat(path: string): Promise<TouchImportStat>;
  hash(path: string): Promise<string>;
  move(from: string, to: string): Promise<void>;
}
export const TOUCH_IMPORT_ID: string = '61dadc74929f59f6d28c399e650d09c5179c8066531b9eb9ecd0df78a68fd226';
export const TOUCH_IMPORT_FILES: TouchImportFile[] = [
  { path: 'data/.build.config', bytes: 882, sha256: '636eb1077cdff110bb18d6c60f94207441ff8ad43d50c6d85fa9950d52f40620' },
  { path: 'data/00-00000007.data', bytes: 6700760, sha256: 'bb26ee24afd0e329f9274ea1d31c052d5b6e32c8b8a781e1122970762749944a' },
  { path: 'touch_strings.tsv', bytes: 1315, sha256: '64db66fe7191f74d275be663b02845cc1e854d47e7ddc398e0e36e86062df7e9' }
];

export async function verifyInactiveTouchImport(io: TouchImportIo, root: string): Promise<void> {
  if (await io.exists(root + '/ctrl_touch')) { throw new Error('触摸候选带有未授权激活标记。'); }
  for (const item of TOUCH_IMPORT_FILES) {
    const path = root + '/' + item.path;
    const stat = await io.stat(path);
    if (!stat.isFile || stat.bytes !== item.bytes ||
      (await io.hash(path)).toLowerCase() !== item.sha256) {
      throw new Error('触摸候选校验失败：' + item.path);
    }
  }
}

// Caller serializes import and creates the two app-owned parent directories.
// Hashes come from the compiled manifest, never from a user-supplied manifest.
export async function finalizeInactiveTouchImport(io: TouchImportIo, filesDir: string): Promise<string> {
  if (!filesDir.startsWith('/') || filesDir.endsWith('/') || filesDir.includes('//') ||
    filesDir.split('/').some((part: string) => part === '.' || part === '..')) {
    throw new Error('触摸导入目录无效。');
  }
  const staged = filesDir + '/touch-overlay.incoming/' + TOUCH_IMPORT_ID;
  const target = filesDir + '/touch-overlays/' + TOUCH_IMPORT_ID;
  if (await io.exists(target)) { await verifyInactiveTouchImport(io, target); return target; }
  await verifyInactiveTouchImport(io, staged);
  await io.move(staged, target);
  try { await verifyInactiveTouchImport(io, target); }
  catch (error) {
    if (!await io.exists(staged) && await io.exists(target)) { await io.move(target, staged); }
    throw error;
  }
  return target;
}
