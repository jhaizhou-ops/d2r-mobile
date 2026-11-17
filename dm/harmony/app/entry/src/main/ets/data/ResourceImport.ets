// 将系统选择器授权的资源拷入沙箱，校验之后再提交。
// Stage picker-authorized resources in the sandbox and commit them after validation.
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo, fileUri } from '@kit.CoreFileKit';
import dm from 'libdm.so';
import { ShellText } from './ShellText';

const RUNTIME_DIRS: string[] = ['out', 'shaders', 'overlay', 'ui-art'];
const RUNTIME_FILES: string[] = ['derived.tsv', 'pack_info.txt'];
const VARIANTS: string = 'overlay_variants';

async function remove(path: string): Promise<void> {
  if (!fileIo.accessSync(path)) { return; }
  if (fileIo.statSync(path).isDirectory()) { await fileIo.rmdir(path); }
  else { await fileIo.unlink(path); }
}

function usableData(path: string): boolean {
  return fileIo.accessSync(path) &&
    (fileIo.accessSync(path + '/.build.config') || fileIo.accessSync(path + '/build.config')) &&
    fileIo.listFileSync(path).some((name: string) => name.endsWith('.data'));
}

async function stage(root: string, relative: string, target: string, directory: boolean, t: ShellText,
  progress: (message: string) => void): Promise<boolean> {
  if (directory) { await fileIo.mkdir(target); }
  progress(t('dmHarmonyImportingPart', relative));
  try {
    await fileIo.copy(root + '/' + relative, fileUri.getUriFromPath(target), {
      progressListener: (p: fileIo.Progress) => {
        progress(t('dmHarmonyImportProgress', relative, String(Math.floor(p.processedSize / 1048576)),
          String(Math.floor(p.totalSize / 1048576))));
      }
    });
  } catch (error) {
    if ((error as BusinessError).code !== 13900002) { throw new Error(`${relative}（${(error as BusinessError).code}）`); }
    await remove(target);
    return false;
  }

  // 部分系统版本会把源目录放在目标目录内，提交前先调整到统一层级。
  // Some system versions nest the source inside the destination. Normalize the layout before committing.
  if (directory) {
    const name = relative.substring(relative.lastIndexOf('/') + 1);
    const nested = target + '/' + name;
    if (fileIo.accessSync(nested) && fileIo.statSync(nested).isDirectory()) {
      await fileIo.rename(nested, target + '.nested');
      await fileIo.rmdir(target);
      await fileIo.rename(target + '.nested', target);
    }
  }
  return true;
}

export async function importResources(files: string, root: string, t: ShellText, progress: (message: string) => void): Promise<string> {
  const tmp = files + '/resource-import.part';
  const pack = tmp + '/dm';
  const runtime = files + '/dm';
  const installedData = files + '/romfs/data';
  await remove(tmp);
  await fileIo.mkdir(pack, true);
  try {
    const hasPack = await stage(root, 'dm/pack_info.txt', pack + '/pack_info.txt', false, t, progress);
    if (hasPack) {
      for (const name of RUNTIME_DIRS.concat('derived.tsv')) {
        if (!(await stage(root, 'dm/' + name, pack + '/' + name, name !== 'derived.tsv', t, progress))) {
          throw new Error(t('dmHarmonyPackMissingFile', name));
        }
      }
      await stage(root, 'dm/' + VARIANTS, pack + '/' + VARIANTS, true, t, progress);
      const mismatch = dm.checkPack(pack);
      if (mismatch) { throw new Error(t('dmHarmonyPackMismatch', mismatch)); }
    }

    const needData = !usableData(installedData);
    const hasData = (needData || !hasPack) && await stage(root, 'data', tmp + '/data', true, t, progress);
    if (hasData && !usableData(tmp + '/data')) { throw new Error(t('dmHarmonyInvalidData')); }
    if (!hasData && !hasPack) { throw new Error(t('dmHarmonyPickFolder')); }

    if (hasPack) {
      progress(t('dmHarmonyUpdatingPack'));
      if (!fileIo.accessSync(runtime)) { await fileIo.mkdir(runtime); }
      // 任一步中断时，原生启动会要求重新导入，避免读取新旧混合的组件。
      // If interrupted, native startup requests another import to avoid reading mixed components.
      const pending = runtime + '/import-pending';
      fileIo.closeSync(fileIo.openSync(pending, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY));
      for (const name of RUNTIME_DIRS.concat([VARIANTS], RUNTIME_FILES)) {
        const target = runtime + '/' + name;
        const source = pack + '/' + name;
        await remove(target);
        if (fileIo.accessSync(source)) { await fileIo.rename(source, target); }
      }
      await fileIo.unlink(pending);
    }
    if (hasData) {
      if (!fileIo.accessSync(files + '/romfs')) { await fileIo.mkdir(files + '/romfs'); }
      await remove(installedData);
      await fileIo.rename(tmp + '/data', installedData);
    }
    return hasPack && !hasData && !needData ? t('dmHarmonyPackUpdated') : t('dmHarmonyImportComplete');
  } finally {
    await remove(tmp);
  }
}
