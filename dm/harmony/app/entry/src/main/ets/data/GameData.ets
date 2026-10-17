import { fileIo as fs, hash as fileHash, fileUri, picker, statfs as statvfs } from '@kit.CoreFileKit';
import { common, appRecovery } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RESOURCE_FILES, ResourceEntry } from './ResourceManifest';
import { ImportCommitIo, commitVerifiedImport } from './ImportCommit';
import { TouchImportIo, TouchImportStat, finalizeInactiveTouchImport } from './TouchOverlayImport';

interface VerifiedFile { name: string; bytes: number; mtime: number; sha256: string; }
interface Receipt { files: VerifiedFile[]; }
interface ShaderEntry {key: string; bytes: number; sha256: string;}
export interface GamePaths { ready: boolean; romfs: string; save: string; temporary: string; missing: string[]; verified: string[]; }

export class GameData {
  private context: common.UIAbilityContext;
  private cancelled: boolean = false;
  cancel(): void { this.cancelled=true; }
  private ensureActive(): void { if(this.cancelled) {throw new Error('已停止导入，已完成的文件保留，可继续导入剩余资料。');} }
  constructor(context: common.UIAbilityContext) { this.context=context; }
  private async directory(path: string): Promise<void> { if(!await fs.access(path)) {await fs.mkdir(path,true);} }
  private async saveText(path: string,text: string): Promise<void> {
    const part=path+'.new';const f=await fs.open(part,fs.OpenMode.CREATE|fs.OpenMode.READ_WRITE|fs.OpenMode.TRUNC);
    try { await fs.write(f.fd,text);await fs.fsync(f.fd); } finally {await fs.close(f);}
    await fs.rename(part,path);
  }
  async initialize(): Promise<GamePaths> {
    const base=this.context.filesDir;
    const paths: GamePaths={ready:false,romfs:base+'/game-data',save:base+'/saves',temporary:base+'/session-temp',missing:[],verified:[]};
    for(const p of [paths.romfs,paths.save,paths.temporary,base+'/incoming',base+'/game-shaders']) {await this.directory(p);}
    return paths;
  }
  // Explicit integration entry only; startup still configures overlay OFF.
  // The staged candidate is immutable and remains inactive after this returns.
  async finalizeTouchCandidate(): Promise<string> {
    this.ensureActive();
    const base=this.context.filesDir;
    await this.directory(base+'/touch-overlay.incoming');
    await this.directory(base+'/touch-overlays');
    const io: TouchImportIo={
      exists: async (path: string): Promise<boolean> => fs.access(path),
      stat: async (path: string): Promise<TouchImportStat> => {
        const value=await fs.stat(path);return {isFile:value.isFile(),bytes:value.size};
      },
      hash: async (path: string): Promise<string> => fileHash.hash(path,'sha256'),
      move: async (from: string,to: string): Promise<void> => fs.rename(from,to)
    };
    return finalizeInactiveTouchImport(io,base);
  }
  async restartApplication(): Promise<void> {
    const path=this.context.filesDir+'/last-app-restart.txt';const now=Date.now();
    if(await fs.access(path)) {
      const previous=Number(await fs.readText(path));
      if(Number.isFinite(previous)&&now-previous<65000) {throw new Error(`请再等 ${Math.ceil((65000-(now-previous))/1000)} 秒后重启应用。`);}
    }
    await this.saveText(path,String(now));
    appRecovery.setRestartWant({bundleName:'com.xiaoboluo.harmony.bootstrap',abilityName:'EntryAbility'});
    appRecovery.restartApp();
  }
  async check(progress: (message: string)=>void,full: boolean=false): Promise<GamePaths> {
    const paths=await this.initialize();let previous: Receipt={files:[]};
    const receiptPath=this.context.filesDir+'/data-receipt.json';
    if(await fs.access(receiptPath)) {try {previous=JSON.parse(await fs.readText(receiptPath)) as Receipt;} catch (_) {previous={files:[]};}}
    const next: Receipt={files:[]};
    for(let i=0;i<RESOURCE_FILES.length;i++) {
      this.ensureActive();
      const item=RESOURCE_FILES[i];const target=paths.romfs+'/'+item.name;
      const incoming=this.context.filesDir+'/incoming/'+item.name;
      progress(`核验游戏资料 ${i+1}/${RESOURCE_FILES.length}`);
      let incomingVerified=false;
      if(!await fs.access(target) && await fs.access(incoming)) {
        const st=await fs.stat(incoming);
        if(st.size===item.bytes && (await fileHash.hash(incoming,'sha256')).toLowerCase()===item.sha256) {
          await fs.rename(incoming,target);
          incomingVerified=true;
        }
      }
      if(!await fs.access(target)) {paths.missing.push(item.name);continue;}
      const st=await fs.stat(target);
      const saved=previous.files.find((f: VerifiedFile)=>f.name===item.name);
      let valid=st.isFile() && st.size===item.bytes;
      if(valid && !incomingVerified && (full || !saved || saved.mtime!==st.mtime || saved.bytes!==st.size || saved.sha256!==item.sha256)) {
        valid=(await fileHash.hash(target,'sha256')).toLowerCase()===item.sha256;
      }
      if(valid) {paths.verified.push(item.name);next.files.push({name:item.name,bytes:st.size,mtime:st.mtime,sha256:item.sha256});}
      else {throw new Error(`资料校验失败：${item.name}。已保留原文件，请检查来源。`);}
    }
    paths.ready=paths.missing.length===0;
    await this.saveText(receiptPath,JSON.stringify(next));
    await this.saveText(this.context.filesDir+'/game-paths.json',JSON.stringify(paths));
    return paths;
  }
  async selectAndImport(progress: (message: string)=>void, repairCorrupt: boolean=false): Promise<GamePaths> {
    const paths=await this.initialize();
    const options=new picker.DocumentSelectOptions();options.maxSelectNumber=100;
    options.selectMode=picker.DocumentSelectMode.FILE;
    progress('请在系统文件页选择已下载到本机的资料；云端文件需要先下载。');
    hilog.info(0,'XblImport','picker opened');
    const uris=await new picker.DocumentViewPicker(this.context).select(options);
    hilog.info(0,'XblImport','picker returned count=%{public}d',uris.length);
    if(uris.length===0) {throw new Error('已取消选择，现有资料未改变。');}
    this.ensureActive();
    let matched=0;
    for(const uri of uris) {
      this.ensureActive();
      const name=new fileUri.FileUri(uri).name;
      const item=RESOURCE_FILES.find((f: ResourceEntry)=>f.name===name);
      if(!item) {continue;}
      matched++;
      const target=paths.romfs+'/'+name;
      if(await fs.access(target)) {
        if(!repairCorrupt) {continue;}
        const existing=await fs.stat(target);
        if(existing.isFile() && existing.size===item.bytes &&
          (await fileHash.hash(target,'sha256')).toLowerCase()===item.sha256) {continue;}
      }
      if(await statvfs.getFreeSize(this.context.filesDir)<item.bytes+536870912) {throw new Error('存储空间不足，请至少预留当前文件大小和512MB空间。');}
      const from=await fs.open(uri,fs.OpenMode.READ_ONLY);
      const partial=this.context.filesDir+'/incoming/'+name+'.partial';
      try {
        if((await fs.stat(from.fd)).size!==item.bytes) {throw new Error(`文件大小不符：${name}`);}
        const to=await fs.open(partial,fs.OpenMode.CREATE|fs.OpenMode.READ_WRITE|fs.OpenMode.TRUNC);
        try {
          hilog.info(0,'XblImport','copy begin %{public}s bytes=%{public}d',name,item.bytes);
          const buffer=new ArrayBuffer(1048576);let copied=0;let lastUpdate=0;
          while(copied<item.bytes) {
            this.ensureActive();
            const count=await fs.read(from.fd,buffer,{length:Math.min(buffer.byteLength,item.bytes-copied)});
            if(count<=0) {throw new Error(`文件读取中断：${name}`);}
            let written=0;
            while(written<count) {
              const bytes=await fs.write(to.fd,buffer.slice(written,count));
              if(bytes<=0) {throw new Error(`文件写入中断：${name}`);}
              written+=bytes;
            }
            copied+=count;
            if(Date.now()-lastUpdate>200 || copied===item.bytes) {
              progress(`正在导入 ${name} · ${Math.floor(copied/item.bytes*100)}%（${Math.floor(copied/1048576)}/${Math.ceil(item.bytes/1048576)} MB）`);
              lastUpdate=Date.now();
            }
          }
          await fs.fsync(to.fd);
        } finally {await fs.close(to);}
      } finally {await fs.close(from);}
      this.ensureActive();
      progress(`正在校验 ${name}，大文件需要一些时间`);
      if((await fileHash.hash(partial,'sha256')).toLowerCase()!==item.sha256) {throw new Error(`文件内容不符：${name}，原件未改动。`);}
      this.ensureActive();
      // Keep verified incoming/<name> recoverable by check() across process exit.
      const staged=this.context.filesDir+'/incoming/'+name;
      await fs.rename(partial,staged);
      const backupDirectory=this.context.filesDir+'/incoming/repair-backups';
      if(repairCorrupt) {await this.directory(backupDirectory);}
      const io: ImportCommitIo={
        exists: async (path: string): Promise<boolean> => fs.access(path),
        move: async (from: string,to: string): Promise<void> => fs.rename(from,to)
      };
      await commitVerifiedImport(io,staged,target,backupDirectory+'/'+name+'.'+Date.now(),repairCorrupt);
      // Persist each completed file so a later cancellation does not require hashing it again.
      const receiptPath=this.context.filesDir+'/data-receipt.json';let receipt: Receipt={files:[]};
      if(await fs.access(receiptPath)) {try {receipt=JSON.parse(await fs.readText(receiptPath)) as Receipt;} catch (_) {receipt={files:[]};}}
      const stat=await fs.stat(target);
      receipt.files=receipt.files.filter((f: VerifiedFile)=>f.name!==name);
      receipt.files.push({name:name,bytes:stat.size,mtime:stat.mtime,sha256:item.sha256});
      await this.saveText(receiptPath,JSON.stringify(receipt));
      hilog.info(0,'XblImport','verified %{public}s',name);
    }
    if(matched===0) {throw new Error('未选到游戏资料文件。请选择原始 data 目录里的文件，不是文件夹或压缩包。');}
    return this.check(progress);
  }
  async checkShaders(progress: (message: string)=>void): Promise<void> {
    const root=this.context.filesDir+'/game-shaders';
    const manifestPath=root+'/manifest.json';
    if(!await fs.access(manifestPath)) {throw new Error('图形库尚未部署完成，请等待开发端更新；无需重新导入游戏资料。');}
    const items=JSON.parse(await fs.readText(manifestPath)) as ShaderEntry[];
    if(items.length!==6927) {throw new Error('图形库清单不完整。');}
    const receiptPath=this.context.filesDir+'/shader-receipt.json';let previous: Receipt={files:[]};
    if(await fs.access(receiptPath)) {try {previous=JSON.parse(await fs.readText(receiptPath)) as Receipt;} catch (_) {previous={files:[]};}}
    const saved=new Map<string,VerifiedFile>();for(const item of previous.files) {saved.set(item.name,item);}
    const next: Receipt={files:[]};const names=new Set<string>();
    for(let i=0;i<items.length;i++) {
      const item=items[i];
      if(!/^[a-f0-9]{16}$/.test(item.key)||names.has(item.key)||!/^[a-f0-9]{64}$/.test(item.sha256)) {throw new Error('图形库清单格式错误。');}
      names.add(item.key);const name=item.key+'.xgs';const path=root+'/'+name;
      if(i%100===0) {progress(`正在校验图形库 ${i}/${items.length}`);}
      const st=await fs.stat(path);if(!st.isFile()||st.size!==item.bytes) {throw new Error(`图形文件不完整：${name}`);}
      const old=saved.get(name);
      if(!old||old.mtime!==st.mtime||old.bytes!==st.size||old.sha256!==item.sha256) {
        if((await fileHash.hash(path,'sha256')).toLowerCase()!==item.sha256) {throw new Error(`图形文件校验失败：${name}`);}
      }
      next.files.push({name:name,bytes:st.size,mtime:st.mtime,sha256:item.sha256});
    }
    await this.saveText(receiptPath,JSON.stringify(next));
    hilog.info(0,'XblImport','shader library verified count=6927');
  }
}
