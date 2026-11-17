// 在游戏启动前导入存档文件夹，或通过系统选择器保存 ZIP 备份。
// Import a save folder before launching the game, or export a ZIP backup through the document picker.
import { common } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { zlib } from '@kit.BasicServicesKit';
import { fileIo, fileUri, picker } from '@kit.CoreFileKit';
import dm from 'libdm.so';
import { ShellText } from './ShellText';

const SOURCE = 'export-source.txt';

function folder(path: string): boolean {
  return fileIo.accessSync(path) && fileIo.statSync(path).isDirectory();
}

function stash(path: string, name: string): boolean {
  return name.startsWith('ModernSharedStash') && fileIo.listFileSync(path).some((file: string) =>
    file.endsWith('.d2i') && fileIo.statSync(path + '/' + file).isFile());
}

function saves(root: string, settings: boolean = false): string[] {
  if (!folder(root)) { return []; }
  return fileIo.listFileSync(root).filter((name: string) => {
    if (name.startsWith('.') || name.startsWith('_')) { return false; }
    const path = root + '/' + name;
    const hero = path + '/' + name + '.d2s';
    return folder(path) && ((settings && name === 'Settings' && fileIo.listFileSync(path).length > 0) ||
      (fileIo.accessSync(hero) && fileIo.statSync(hero).isFile()) || stash(path, name));
  });
}

function findRoot(path: string, levels: number): string {
  if (saves(path, true).length > 0) { return path; }
  if (levels === 0) { return ''; }
  const directories = fileIo.listFileSync(path).filter((name: string) => folder(path + '/' + name));
  return directories.length === 1 ? findRoot(path + '/' + directories[0], levels - 1) : '';
}

export async function selectFolder(context: common.UIAbilityContext): Promise<string> {
  const options = new picker.DocumentSelectOptions();
  options.selectMode = picker.DocumentSelectMode.FOLDER;
  options.maxSelectNumber = 1;
  const uris = await new picker.DocumentViewPicker(context).select(options);
  return uris[0]?.replace(/\/+$/, '') ?? '';
}

export async function importSaves(files: string, uri: string, t: ShellText): Promise<string> {
  const temp = files + '/save-import.part';
  const save = files + '/save';
  if (folder(temp)) { await fileIo.rmdir(temp); }
  await fileIo.mkdir(temp);
  try {
    await fileIo.copy(uri, fileUri.getUriFromPath(temp));
    const root = findRoot(temp, 2);
    if (!root) { throw new Error(t('dmHarmonyNoSaves')); }
    if (!folder(save)) { await fileIo.mkdir(save); }
    const id = files + '/save-transfer-id';
    const src = root + '/' + SOURCE;
    const sameDevice = fileIo.accessSync(id) && fileIo.accessSync(src) &&
      fileIo.readTextSync(id).trim() === fileIo.readTextSync(src).trim();
    let added = 0;
    let queued = 0;
    const skipped: string[] = [];
    for (const name of saves(root, true)) {
      const dst = save + '/' + name;
      const from = root + '/' + name;
      if (!fileIo.accessSync(dst)) {
        await fileIo.rename(from, dst);
        added++;
      } else if (stash(from, name) && !sameDevice) {
        const result = dm.importStash(save, root, name);
        if (result < 0) { throw new Error(t('dmHarmonyStashImportError', name, String(added))); }
        if (result > 0) { queued++; }
        else { skipped.push(name); }
      } else { skipped.push(name); }
    }
    const summary = [t('dmHarmonySavesAdded', String(added))];
    if (queued) { summary.push(t('dmHarmonyStashesQueued', String(queued))); }
    if (skipped.length) { summary.push(t('dmHarmonySavesSkipped', skipped.join(', '))); }
    return summary.join(' ');
  } finally {
    await fileIo.rmdir(temp);
  }
}

export async function exportSaves(context: common.UIAbilityContext, t: ShellText): Promise<string> {
  const files = context.filesDir;
  const root = files + '/save';
  const entries = saves(root, true);
  if (!entries.length) { return t('dmAndroidSaveExportEmpty'); }
  const idPath = files + '/save-transfer-id';
  if (!fileIo.accessSync(idPath)) {
    const id = fileIo.openSync(idPath, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
    try { fileIo.writeSync(id.fd, util.generateRandomUUID() + '\n'); }
    finally { fileIo.closeSync(id); }
  }
  const temp = files + '/save-export.part';
  if (folder(temp)) { await fileIo.rmdir(temp); }
  await fileIo.mkdir(temp);
  try {
    const sourceId = temp + '/' + SOURCE;
    const zip = temp + '/backup.zip';
    await fileIo.copyFile(idPath, sourceId);
    await zlib.compressFiles(entries.map((name: string) => root + '/' + name).concat(sourceId), zip, {});
    const options = new picker.DocumentSaveOptions();
    const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');
    options.newFileNames = [`D2R-save-${stamp}.zip`];
    const uris = await new picker.DocumentViewPicker(context).save(options);
    if (!uris.length) { return ''; }
    const target = fileIo.openSync(uris[0], fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
    try { await fileIo.copyFile(zip, target.fd); }
    finally { fileIo.closeSync(target); }
    return t('dmHarmonySavesExported');
  } finally {
    await fileIo.rmdir(temp);
  }
}
