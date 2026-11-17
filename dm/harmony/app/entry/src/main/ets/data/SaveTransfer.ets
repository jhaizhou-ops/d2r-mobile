import { common } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { zlib } from '@kit.BasicServicesKit';
import { fileIo, fileUri, picker } from '@kit.CoreFileKit';
import dm from 'libdm.so';

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
  if (saves(path).length > 0) { return path; }
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

export async function importSaves(files: string, uri: string): Promise<string> {
  const temp = files + '/save-import.part';
  const save = files + '/save';
  if (folder(temp)) { await fileIo.rmdir(temp); }
  await fileIo.mkdir(temp);
  try {
    await fileIo.copy(uri, fileUri.getUriFromPath(temp));
    const root = findRoot(temp, 2);
    if (!root) { throw new Error('没有找到角色或共享仓库。请先解压备份，再选择存档文件夹。'); }
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
        if (result < 0) { throw new Error(`共享仓库 ${name} 导入失败，已添加 ${added} 项，可以重试。`); }
        if (result > 0) { queued++; }
        else { skipped.push(name); }
      } else { skipped.push(name); }
    }
    return `已添加 ${added} 项。${queued ? ` ${queued} 份共享仓库将在打开储藏箱时合并。` : ''}` +
      (skipped.length ? ` 已有或已导入的存档已跳过（${skipped.join('、')}）。` : '');
  } finally {
    await fileIo.rmdir(temp);
  }
}

export async function exportSaves(context: common.UIAbilityContext): Promise<string> {
  const files = context.filesDir;
  const root = files + '/save';
  const entries = saves(root, true);
  if (!entries.length) { return '还没有可导出的存档。'; }
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
    return '存档已导出。恢复时先解压，再选择解压后的文件夹。';
  } finally {
    await fileIo.rmdir(temp);
  }
}
