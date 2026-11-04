package com.xiaoboluo.d2r;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;

// 存档导出 / 导入（#140、#174、#193）：存档在 App 的内部目录 files/save，没 root 拿不出来，卸载就没了。不加菜单，按 #140 的提法用游戏文件夹
// Download/S2X/D2R Mobile/ 里的记号：
// · 放一个名为 export 的文件（或文件夹）再打开 App：files/save 整个拷到 save-export/<日期-时间>/，然后删掉记号；
// · 放一个 save-import 文件夹再打开 App：里面的存档（一个角色一个文件夹，<名字>/<名字>.d2s，与导出的一样；也认导出那一层 <日期-时间>/）**加进**
//   files/save，手机上已有的一样不动：同名角色不导（报给玩家），设置（Settings）只在手机上还没有时才导。共享仓库（ModernSharedStash…）手机上没有就整个拷进来；
//   已经有了就并：备份那份打成包放进局内并回的收件箱 files/save/_cloudinbox/<毫秒>-import/mine.pack（格式同 dm_cloudsync.c pack_write，不带基准 base.pack），
//   进局打开储藏箱时由 dm_cloudmerge.c 把「现在仓库里没有的」物品逐件放进背包（同一件不重复给）、金币加进共享仓库——和 iOS 上 iCloud 两份仓库分叉的并回同一条路。
//   同一份共享仓库（内容摘要相同，记在条目里的 import.sha256）只并一次，免得重复导入把金币加两遍。
//   这台手机自己导出的备份（导出时写进 export-source.txt 的安装标识与本机 files/save-transfer-id 相同）导回来时共享仓库不并：
//   金币会再加一遍，备份之后拿出仓库的物品也会再给一份；别的手机的备份照常并。卸载重装后标识是新的（那时手机上也没有共享仓库了）。
//   导完把 save-import 改名为 save-import.done-<日期-时间>，下次不会再导；里面认不出存档就一个字节不动、改名为 save-import.rejected-<日期-时间>。
// 只在原生库加载之前做（DmActivity.startHost）：游戏还没挂载存档，files/save 是上次提交的版本；残留的 save.work 下次挂载时丢弃（hle_fs.c）。
// 纯 Java，不碰 Android 的类，可在主机上测（dm/android/tests/SaveTransferTest.java）
// Save export / import (#140, #174, #193): saves live in the App's internal files/save, which cannot be reached without root and goes away on uninstall.
// No menu is added; as #140 proposes, markers in the game folder Download/S2X/D2R Mobile/ drive it:
// - put a file (or folder) named export there and open the App: files/save is copied to save-export/<date-time>/, then the marker is deleted;
// - put a save-import folder there and open the App: the saves in it (one folder per character, <name>/<name>.d2s, the same as an export; the
//   <date-time>/ level of an export is accepted too) are **added** to files/save, and nothing already on the phone is touched: a character whose
//   name is taken is not imported (and is reported to the player), and the settings (Settings) are only imported while the phone has none. A shared
//   stash (ModernSharedStash...) the phone lacks is copied in whole; one it already has is merged: the backup's copy is packed into the in-game
//   merge inbox files/save/_cloudinbox/<millis>-import/mine.pack (the format of dm_cloudsync.c pack_write, without a base.pack), and when the
//   stash is opened in game dm_cloudmerge.c puts the items the current stash does not have into the inventory one by one (the same item is not
//   given twice) and adds the gold to the shared stash -- the same path as merging a forked iCloud stash on iOS. The same shared stash (same
//   content digest, kept in the entry as import.sha256) is merged only once, so importing it again does not add its gold twice. A backup this
//   phone exported itself (the install id written to export-source.txt on export equals this phone's files/save-transfer-id) is not merged into
//   the shared stash when imported back: its gold would be added again, and items taken out of the stash since the backup would be given a second
//   time; backups from other phones are merged as usual. After an uninstall and reinstall the id is new (and the phone has no shared stash then). Afterwards save-import is renamed to save-import.done-<date-time>, so it is not imported again; when no
//   saves are recognised in it, nothing is touched and it is renamed to save-import.rejected-<date-time>.
// Only done before the native library loads (DmActivity.startHost): the game has not mounted the saves yet, so files/save is the last committed
// version; a leftover save.work is discarded at the next mount (hle_fs.c). Plain Java without Android classes, testable on the host
// (dm/android/tests/SaveTransferTest.java)
final class SaveTransfer {
    private SaveTransfer() {}

    static final int EXPORTED = 1, EXPORT_EMPTY = 2, IMPORTED = 3, IMPORT_REJECTED = 4, FAILED = 5;
    static final String EXPORT_MARKER = "export", EXPORT_DIR = "save-export", IMPORT_DIR = "save-import", INBOX = "_cloudinbox";
    static final String SOURCE_FILE = "export-source.txt", ID_FILE = "save-transfer-id";
    static final int MERGE_NONE = 0, MERGE_QUEUED = 1, MERGE_ALREADY = 2, MERGE_SAME_PHONE = 3;

    // kind = 上面的结果之一；path = 相对游戏文件夹的路径（导出到哪 / 认不出时改成的名字）或导进来的名字（逗号隔开），失败时是原因；
    // skipped = 导入时手机上已有、没导的名字（逗号隔开，没有为 null）
    // kind = one of the results above; path = a path relative to the game folder (where the export went / what an unrecognised folder was renamed
    // to), or the names imported (comma separated), or the reason on failure; skipped = the names already on the phone and not imported (comma
    // separated, null when none)
    // merge = 共享仓库：MERGE_NONE 没并 / MERGE_QUEUED 放进了收件箱、进局开箱时并 / MERGE_ALREADY 这一份以前并过 / MERGE_SAME_PHONE 这台手机自己的备份，不并
    // merge = shared stash: MERGE_NONE not merged / MERGE_QUEUED put in the inbox, merged on stash open in game / MERGE_ALREADY this copy was merged before /
    // MERGE_SAME_PHONE a backup of this same phone, not merged
    static final class Result {
        final int kind, merge; final String path, skipped;
        Result(int kind, String path) { this(kind, path, null, MERGE_NONE); }
        Result(int kind, String path, String skipped, int merge) { this.kind = kind; this.path = path; this.skipped = skipped; this.merge = merge; }
    }

    // game = Download/S2X/D2R Mobile/，save = files/save，stamp = 日期-时间（如 20261101-153000）；没有记号时返回空表
    // game = Download/S2X/D2R Mobile/, save = files/save, stamp = date-time (e.g. 20261101-153000); returns an empty list without markers
    static List<Result> run(File game, File save, String stamp) {
        List<Result> out = new ArrayList<>();
        File marker = new File(game, EXPORT_MARKER);
        if (marker.exists()) {
            try {
                if (!hasFiles(save)) out.add(new Result(EXPORT_EMPTY, null));
                else out.add(new Result(EXPORTED, exportTo(game, save, stamp)));
                deleteTree(marker);
            } catch (IOException e) { out.add(new Result(FAILED, reason(e))); }
        }
        File in = new File(game, IMPORT_DIR);
        if (in.isDirectory()) {
            try {
                File root = savesRoot(in);
                if (root == null) {
                    File rejected = unused(new File(game, IMPORT_DIR + ".rejected-" + stamp));
                    if (!in.renameTo(rejected)) throw new IOException("rename " + IMPORT_DIR);
                    out.add(new Result(IMPORT_REJECTED, rejected.getName()));
                } else out.add(importFrom(game, root, in, save, stamp));
            } catch (IOException e) { out.add(new Result(FAILED, reason(e))); }
        }
        return out;
    }

    private static String exportTo(File game, File save, String stamp) throws IOException {
        File target = unused(new File(new File(game, EXPORT_DIR), stamp));
        copyComplete(save, target, installId(save));
        return EXPORT_DIR + "/" + target.getName();
    }

    // 逐项加：角色文件夹、共享仓库文件夹、Settings；手机上已有同名的跳过（已有的共享仓库改走并回收件箱）。每项先拷进 files/save.import.part/<名字>（存档目录旁边，不在里面），拷完再改名到位，
    // 中途失败不留半个角色；别的文件（说明、系统生成的小文件）不导
    // Add item by item: character folders, shared stash folders, Settings; anything whose name the phone already has is skipped (a shared stash the
// phone already has goes to the merge inbox instead). Each item is
    // copied into files/save.import.part/<name> first (next to the save directory, not inside it) and renamed into place once complete, so a failure halfway leaves no half character; other
    // files (notes, small files the system makes) are not imported
    private static Result importFrom(File game, File root, File in, File save, String stamp) throws IOException {
        File[] list = root.listFiles();
        if (list == null) throw new IOException("list " + root.getName());
        if (!save.isDirectory() && !save.mkdirs()) throw new IOException("mkdir " + save.getName());
        File part = new File(save.getAbsoluteFile().getParentFile(), save.getName() + ".import.part");
        deleteTree(part);
        StringBuilder added = new StringBuilder(), skipped = new StringBuilder();
        List<File> stashes = new ArrayList<>();
        byte[] buffer = new byte[1 << 16];
        try {
            for (File c : list) {
                if (!c.isDirectory() || !(isCharacter(c) || isStash(c) || c.getName().equals("Settings"))) continue;
                File dst = new File(save, c.getName());
                if (dst.exists() && isStash(c)) { stashes.add(c); continue; }
                if (dst.exists()) { append(skipped, c.getName()); continue; }
                File tmp = new File(part, c.getName());
                copyTree(c, tmp, buffer);
                if (!tmp.renameTo(dst)) throw new IOException("rename " + c.getName());
                append(added, c.getName());
            }
        } finally { deleteTree(part); }
        int merge = stashes.isEmpty() ? MERGE_NONE : fromThisPhone(root, save) ? MERGE_SAME_PHONE : queueMerge(save, stashes);
        // 加完：save-import 改不了名也不算失败（下次打开再导一遍，已有的都会跳过）
        // Done adding: failing to rename save-import is not a failure (the next start imports again, and everything already there is skipped)
        File done = unused(new File(game, IMPORT_DIR + ".done-" + stamp));
        in.renameTo(done);
        return new Result(IMPORTED, added.length() == 0 ? null : added.toString(), skipped.length() == 0 ? null : skipped.toString(), merge);
    }

    // 备份那份共享仓库进并回收件箱：files/save/_cloudinbox/<毫秒>-import/{mine.pack, import.sha256}，先写在 .part 里、齐了再改名（收件箱里只会看到整个条目）。
    // 包：magic "SWPK1\n\0\0" | 时刻 u64 | 标志 u32（0）| 文件数 u32 | 每个文件：路径长 u16、路径（相对存档目录）、修改时间 u64（毫秒）、长度 u32、内容，小端（dm_cloudsync.c）。
    // 收件箱里已有摘要相同的条目就不再放
    // The backup's shared stash goes into the merge inbox: files/save/_cloudinbox/<millis>-import/{mine.pack, import.sha256}, written in .part first and
    // renamed once complete (the inbox only ever sees whole entries). Pack: magic "SWPK1\n\0\0" | timestamp u64 | flags u32 (0) | file count u32 | per file:
    // path length u16, path (relative to the save directory), modification time u64 (ms), length u32, contents, little endian (dm_cloudsync.c). Not
    // added again when the inbox already has an entry with the same digest
    private static int queueMerge(File save, List<File> stashes) throws IOException {
        List<File> files = new ArrayList<>(); List<String> rels = new ArrayList<>();
        for (File d : stashes) collect(d, d.getName(), files, rels);
        if (files.size() > 64) throw new IOException("stash files " + files.size());
        Integer[] order = new Integer[files.size()];
        for (int i = 0; i < order.length; i++) order[i] = i;
        Arrays.sort(order, (a, b) -> rels.get(a).compareTo(rels.get(b)));
        byte[][] data = new byte[order.length][];
        MessageDigest md;
        try { md = MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException e) { throw new IOException("sha256"); }
        for (int k = 0; k < order.length; k++) {
            int i = order[k]; data[k] = Files.readAllBytes(files.get(i).toPath());
            md.update(rels.get(i).getBytes(StandardCharsets.UTF_8)); md.update((byte) 0); md.update(data[k]); md.update((byte) 0);
        }
        String digest = hex(md.digest());
        File inbox = new File(save, INBOX);
        File[] entries = inbox.listFiles();
        if (entries != null) for (File e : entries) {
            File h = new File(e, "import.sha256");
            if (h.isFile() && digest.equals(new String(Files.readAllBytes(h.toPath()), StandardCharsets.US_ASCII).trim())) return MERGE_ALREADY;
        }
        long now = System.currentTimeMillis();
        File entry = unused(new File(inbox, now + "-import"));
        File part = new File(entry.getPath() + ".part");
        deleteTree(part);
        if (!part.mkdirs()) throw new IOException("mkdir " + part.getName());
        try (OutputStream out = new FileOutputStream(new File(part, "mine.pack"))) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            b.write(new byte[] { 'S', 'W', 'P', 'K', '1', '\n', 0, 0 }); le(b, now, 8); le(b, 0, 4); le(b, order.length, 4);
            for (int k = 0; k < order.length; k++) {
                int i = order[k]; byte[] p = rels.get(i).getBytes(StandardCharsets.UTF_8);
                if (p.length >= 256) throw new IOException("path " + rels.get(i));
                le(b, p.length, 2); b.write(p); le(b, files.get(i).lastModified(), 8); le(b, data[k].length, 4); b.write(data[k]);
            }
            b.writeTo(out);
        }
        Files.write(new File(part, "import.sha256").toPath(), (digest + "\n").getBytes(StandardCharsets.US_ASCII));
        if (!part.renameTo(entry)) { deleteTree(part); throw new IOException("rename " + entry.getName()); }
        return MERGE_QUEUED;
    }

    // 这次安装的标识：files/save-transfer-id，第一次导出时生成（只在导出时写，平时不多出文件）；卸载时随 App 数据一起没了
    // This install's id: files/save-transfer-id, created on the first export (written only when exporting, so no extra file otherwise); it goes away
    // with the App's data on uninstall
    private static File idFile(File save) { return new File(save.getAbsoluteFile().getParentFile(), ID_FILE); }

    private static String installId(File save) throws IOException {
        File f = idFile(save);
        if (f.isFile()) { String s = new String(Files.readAllBytes(f.toPath()), StandardCharsets.US_ASCII).trim(); if (!s.isEmpty()) return s; }
        String id = UUID.randomUUID().toString();
        Files.write(f.toPath(), (id + "\n").getBytes(StandardCharsets.US_ASCII));
        return id;
    }

    // 备份是这台手机自己导出的：备份里的 export-source.txt 与本机的标识相同。没有标识（别的手机、旧版导出、手拷的存档）都当别的手机
    // The backup was exported by this phone: the backup's export-source.txt equals this phone's id. No id (another phone, an older export, saves
    // copied by hand) counts as another phone
    private static boolean fromThisPhone(File root, File save) throws IOException {
        File src = new File(root, SOURCE_FILE), id = idFile(save);
        if (!src.isFile() || !id.isFile()) return false;
        String a = new String(Files.readAllBytes(src.toPath()), StandardCharsets.US_ASCII).trim(), b = new String(Files.readAllBytes(id.toPath()), StandardCharsets.US_ASCII).trim();
        return !a.isEmpty() && a.equals(b);
    }

    private static void collect(File f, String rel, List<File> files, List<String> rels) throws IOException {
        if (f.isDirectory()) {
            File[] list = f.listFiles();
            if (list == null) throw new IOException("list " + f.getName());
            for (File c : list) collect(c, rel + "/" + c.getName(), files, rels);
        } else if (f.isFile()) { files.add(f); rels.add(rel); }
    }

    private static void le(ByteArrayOutputStream b, long v, int n) { for (int i = 0; i < n; i++) b.write((int) (v >>> (8 * i)) & 0xff); }

    private static String hex(byte[] d) { StringBuilder s = new StringBuilder(); for (byte x : d) s.append(String.format("%02x", x)); return s.toString(); }

    private static void append(StringBuilder b, String name) { if (b.length() > 0) b.append(", "); b.append(name); }

    private static boolean isCharacter(File dir) { return new File(dir, dir.getName() + ".d2s").isFile(); }

    private static boolean isStash(File dir) {
        File[] inner = dir.getName().startsWith("ModernSharedStash") ? dir.listFiles() : null;
        if (inner != null) for (File f : inner) if (f.isFile() && f.getName().endsWith(".d2i")) return true;
        return false;
    }

    // 存档根：一个角色一个文件夹（<名字>/<名字>.d2s）或共享仓库文件夹（ModernSharedStash…/*.d2i）；导出那一层 <日期-时间>/ 往下找一层
    // The saves root: one folder per character (<name>/<name>.d2s) or a shared stash folder (ModernSharedStash.../*.d2i); one level further down for
    // the <date-time>/ level of an export
    static File savesRoot(File dir) {
        if (looksLikeSaves(dir)) return dir;
        File[] list = dir.listFiles();
        if (list == null) return null;
        File only = null; int dirs = 0;
        for (File c : list) if (c.isDirectory()) { dirs++; only = c; }
        return dirs == 1 && looksLikeSaves(only) ? only : null;
    }

    private static boolean looksLikeSaves(File dir) {
        File[] list = dir.listFiles();
        if (list == null) return false;
        for (File c : list) if (c.isDirectory() && (isCharacter(c) || isStash(c))) return true;
        return false;
    }

    private static boolean hasFiles(File dir) {
        File[] list = dir.listFiles();
        if (list == null) return false;
        for (File c : list) if (c.isFile() || hasFiles(c)) return true;
        return false;
    }

    // 拷到 <目标>.part、写上来源标识再改名：中途失败不留半份 | Copy to <target>.part, write the source id, then rename: a failure halfway leaves no half copy
    private static void copyComplete(File from, File to, String source) throws IOException {
        File part = new File(to.getPath() + ".part");
        deleteTree(part);
        copyTree(from, part, new byte[1 << 16]);
        Files.write(new File(part, SOURCE_FILE).toPath(), (source + "\n").getBytes(StandardCharsets.US_ASCII));
        File parent = to.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent.getName());
        if (!part.renameTo(to)) { deleteTree(part); throw new IOException("rename " + to.getName()); }
    }

    private static File unused(File f) {
        File t = f;
        for (int i = 2; t.exists() || new File(t.getPath() + ".part").exists(); i++) t = new File(f.getPath() + "-" + i);
        return t;
    }

    private static void copyTree(File from, File to, byte[] buffer) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to.getName());
            File[] list = from.listFiles();
            if (list == null) throw new IOException("list " + from.getName());
            for (File c : list) copyTree(c, new File(to, c.getName()), buffer);
            return;
        }
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        }
    }

    private static void deleteTree(File f) throws IOException {
        File[] list = f.isDirectory() ? f.listFiles() : null;
        if (list != null) for (File c : list) deleteTree(c);
        if (f.exists() && !f.delete()) throw new IOException("delete " + f.getName());
    }

    private static String reason(IOException e) { return e.getMessage() == null ? e.toString() : e.getMessage(); }
}
