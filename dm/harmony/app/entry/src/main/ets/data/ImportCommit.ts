export interface ImportCommitIo {
  exists(path: string): Promise<boolean>;
  move(from: string, to: string): Promise<void>;
}

// Input has already passed size/SHA validation. Never remove the old file.
export async function commitVerifiedImport(io: ImportCommitIo, staged: string, target: string,
  backup: string, repair: boolean): Promise<void> {
  if (!await io.exists(target)) { await io.move(staged, target); return; }
  if (!repair) { throw new Error('现有资料已保留，替换损坏资料需使用修复入口。'); }
  if (await io.exists(backup)) { throw new Error('修复备份路径已存在，现有资料未改变。'); }
  await io.move(target, backup);
  try { await io.move(staged, target); }
  catch (error) {
    if (!await io.exists(target)) { await io.move(backup, target); }
    throw error;
  }
}
