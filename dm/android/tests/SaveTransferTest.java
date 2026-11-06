package com.xiaoboluo.d2r;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/** 只在临时目录里造假存档，不碰玩家目录。 | Builds fake saves in a temporary directory only; never touches a player's directory. */
public final class SaveTransferTest {
    private static int checks;
    private static final String VAULT = "ModernSharedStashSoftCoreV2.swvault";
    private static void check(boolean value, String label) { ++checks; if (!value) throw new AssertionError(label); }
    private static void write(File f, String text) throws IOException { f.getParentFile().mkdirs(); Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8)); }
    private static String read(File f) throws IOException { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); }
    private static void character(File dir, String name, String body) throws IOException {
        write(new File(dir, name + "/" + name + ".d2s"), body);
        write(new File(dir, name + "/" + name + ".key"), "key " + name);
    }
    private static void stash(File dir, String body) throws IOException { write(new File(dir, "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"), body); }
    private static File fresh(File base, String name) {
        File d = new File(base, name); new File(d, "D2R Mobile").mkdirs(); new File(d, "files").mkdirs(); return d;
    }

    private static void copyDir(File from, File to) throws IOException {
        if (!from.isDirectory()) { to.getParentFile().mkdirs(); Files.copy(from.toPath(), to.toPath()); return; }
        to.mkdirs(); for (File c : from.listFiles()) copyDir(c, new File(to, c.getName()));
    }

    public static void main(String[] args) throws IOException {
        File base = Files.createTempDirectory("dm-save-transfer").toFile();
        // 没有记号：什么都不做 | No marker: nothing happens
        { File d = fresh(base, "none"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "v1");
          check(SaveTransfer.run(game, save, "20261101-120000").isEmpty(), "no markers, no results");
          check(!new File(game, "save-export").exists() && read(new File(save, "yyc/yyc.d2s")).equals("v1"), "nothing written"); }
        // 导出：整份拷到 save-export/<时间>/，记号删掉，原存档不动；同一时刻再导出一次换名 -2
        // Export: a full copy to save-export/<time>/, the marker deleted, the saves untouched; exporting again at the same moment gets -2
        { File d = fresh(base, "export"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save");
          character(save, "yyc", "v1"); stash(save, "stash"); write(new File(save, "Settings/Settings.json"), "{}");
          write(new File(game, "export"), "");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261101-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.EXPORTED && r.get(0).path.equals("save-export/20261101-120000"), "export result");
          File out = new File(game, "save-export/20261101-120000");
          check(read(new File(out, "yyc/yyc.d2s")).equals("v1") && read(new File(out, "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i")).equals("stash")
                && read(new File(out, "Settings/Settings.json")).equals("{}"), "export copies everything");
          check(!new File(game, "export").exists() && !new File(out.getPath() + ".part").exists(), "marker and part gone");
          check(read(new File(save, "yyc/yyc.d2s")).equals("v1"), "saves untouched by export");
          new File(game, "export").mkdirs();
          r = SaveTransfer.run(game, save, "20261101-120000");
          check(r.size() == 1 && r.get(0).path.equals("save-export/20261101-120000-2") && new File(game, "save-export/20261101-120000-2/yyc/yyc.d2s").isFile(), "a folder marker works, the name gets -2"); }
        // 没有存档时导出：只删记号 | Export without saves: only the marker is deleted
        { File d = fresh(base, "empty"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); save.mkdirs(); new File(save, "yyc").mkdirs();
          write(new File(game, "export"), "");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261101-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.EXPORT_EMPTY && !new File(game, "export").exists() && !new File(game, "save-export").exists(), "empty export"); }
        // 导入只加不换：新角色加进来；同名角色、已有的共享仓库与设置一样不动；说明文件、不认识的文件夹不导
        // Import adds and never replaces: a new character is added; a same-name character, the existing shared stash and settings stay as they are;
        // a notes file and an unknown folder are not imported
        { File d = fresh(base, "import"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save");
          character(save, "yyc", "old"); stash(save, "phone stash"); write(new File(save, "Settings/Settings.json"), "phone");
          File in = new File(game, "save-import"); character(in, "Bob", "new"); character(in, "yyc", "other phone"); stash(in, "other stash");
          write(new File(in, "Settings/Settings.json"), "other"); write(new File(in, "readme.txt"), "x"); write(new File(in, "junk/notes.txt"), "x");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261101-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && "Bob".equals(r.get(0).path), "only Bob imported");
          String sk = r.get(0).skipped;
          check(sk != null && sk.contains("yyc") && !sk.contains("ModernSharedStashSoftCoreV2") && sk.contains("Settings") && !sk.contains("junk"), "skipped list (the stash is merged, not skipped)");
          // 已有的共享仓库：备份那份进并回收件箱 _cloudinbox/<毫秒>-import/mine.pack，手机上的不动 | An existing shared stash: the backup's copy goes to the merge inbox, the phone's is untouched
          check(r.get(0).merge == SaveTransfer.MERGE_QUEUED, "stash merge queued");
          File[] entries = new File(save, "_cloudinbox").listFiles();
          check(entries != null && entries.length == 1 && entries[0].getName().endsWith("-import") && new File(entries[0], "import.sha256").isFile(), "one inbox entry");
          byte[] pk = Files.readAllBytes(new File(entries[0], "mine.pack").toPath());
          java.nio.ByteBuffer bb = java.nio.ByteBuffer.wrap(pk).order(java.nio.ByteOrder.LITTLE_ENDIAN);
          check(new String(pk, 0, 5, StandardCharsets.US_ASCII).equals("SWPK1") && pk[5] == '\n' && pk[6] == 0 && pk[7] == 0, "pack magic");
          bb.position(8); long stamp = bb.getLong(); int flags = bb.getInt(), nf = bb.getInt();
          check(stamp > 0 && flags == 0 && nf == 1, "pack header");
          int pl = bb.getShort() & 0xffff; byte[] pth = new byte[pl]; bb.get(pth); long mt = bb.getLong(); int sz = bb.getInt(); byte[] body = new byte[sz]; bb.get(body);
          check(new String(pth, StandardCharsets.UTF_8).equals("ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i") && mt > 0
                && new String(body, StandardCharsets.UTF_8).equals("other stash") && !bb.hasRemaining(), "pack file entry");
          check(read(new File(save, "Bob/Bob.d2s")).equals("new") && read(new File(save, "Bob/Bob.key")).equals("key Bob"), "Bob added");
          check(read(new File(save, "yyc/yyc.d2s")).equals("old") && read(new File(save, "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i")).equals("phone stash")
                && read(new File(save, "Settings/Settings.json")).equals("phone"), "nothing on the phone replaced");
          check(!new File(save, "readme.txt").exists() && !new File(save, "junk").exists() && !new File(d, "files/save.import.part").exists(), "no extras, part gone");
          check(!new File(game, "save-export").exists(), "no backup needed: nothing replaced");
          check(!in.exists() && new File(game, "save-import.done-20261101-120000/Bob/Bob.d2s").isFile(), "save-import renamed done");
          // 同一份再导一次：不再进收件箱（金币不会加两遍） | The same stash imported again: not queued again (gold is not added twice)
          stash(in, "other stash"); character(in, "Cid", "c");
          List<SaveTransfer.Result> r2 = SaveTransfer.run(game, save, "20261101-130000");
          check(r2.size() == 1 && r2.get(0).merge == SaveTransfer.MERGE_ALREADY && "Cid".equals(r2.get(0).path) && new File(save, "_cloudinbox").listFiles().length == 1, "same stash not merged twice");
        }
        // 刚装好、还没有存档：导出那一层（save-import/<时间>/…）整份进来，共享仓库与设置也导
        // A fresh install with no saves: an export's own level (save-import/<time>/...) comes in whole, shared stash and settings too
        { File d = fresh(base, "nested"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save");
          File in = new File(game, "save-import/20261031-090000"); character(in, "Bob", "nested"); character(in, "Ann", "a"); stash(in, "s"); write(new File(in, "Settings/Settings.json"), "x");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261101-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && r.get(0).skipped == null && r.get(0).path.contains("Bob") && r.get(0).path.contains("Ann"), "fresh import result");
          check(read(new File(save, "Bob/Bob.d2s")).equals("nested") && read(new File(save, "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i")).equals("s")
                && new File(save, "Settings/Settings.json").isFile(), "everything in on a fresh install"); }
        // 全都已有：什么都不加，照样改名 .done | Everything already there: nothing added, still renamed .done
        { File d = fresh(base, "same"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "phone");
          character(new File(game, "save-import"), "yyc", "import");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261101-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && r.get(0).path == null && "yyc".equals(r.get(0).skipped) && read(new File(save, "yyc/yyc.d2s")).equals("phone"), "all skipped"); }
        // 认不出存档：一个字节不动，改名 .rejected-<时间> | Saves not recognised: nothing is touched, renamed .rejected-<time>
        { File d = fresh(base, "reject"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "keep");
          write(new File(game, "save-import/readme.txt"), "x"); write(new File(game, "save-import/Bob/notes.txt"), "x");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261101-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORT_REJECTED && r.get(0).path.equals("save-import.rejected-20261101-120000"), "rejected result");
          check(read(new File(save, "yyc/yyc.d2s")).equals("keep") && !new File(save, "Bob").exists() && new File(game, "save-import.rejected-20261101-120000/readme.txt").isFile(), "nothing touched"); }
        // 两个记号都在：先导出现有存档，再导入 | Both markers: export the current saves first, then import
        { File d = fresh(base, "both"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "old");
          write(new File(game, "export"), ""); character(new File(game, "save-import"), "Bob", "new");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261101-120000");
          check(r.size() == 2 && r.get(0).kind == SaveTransfer.EXPORTED && r.get(1).kind == SaveTransfer.IMPORTED, "export then import");
          check(read(new File(game, "save-export/20261101-120000/yyc/yyc.d2s")).equals("old") && !new File(game, "save-export/20261101-120000/Bob").exists()
                && read(new File(save, "Bob/Bob.d2s")).equals("new") && read(new File(save, "yyc/yyc.d2s")).equals("old"), "both done"); }
        // 这台手机自己的备份导回来：共享仓库不并；同一份拿到别的手机上照常并；导入不写标识文件
        // A backup of this same phone imported back: the shared stash is not merged; the same backup on another phone is merged as usual; importing writes no id file
        { File d = fresh(base, "same"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "v"); stash(save, "phone stash");
          write(new File(game, "export"), "");
          check(SaveTransfer.run(game, save, "20261101-120000").get(0).kind == SaveTransfer.EXPORTED, "same-phone export");
          File exp = new File(game, "save-export/20261101-120000"), id = new File(d, "files/save-transfer-id");
          check(id.isFile() && read(new File(exp, SaveTransfer.SOURCE_FILE)).trim().equals(read(id).trim()) && read(id).trim().length() == 36, "export carries this install's id");
          String first = read(id);
          copyDir(exp, new File(game, "save-import/20261101-120000"));
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261101-130000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && r.get(0).merge == SaveTransfer.MERGE_SAME_PHONE && !new File(save, "_cloudinbox").exists(), "same phone: stash not merged");
          check(read(new File(save, "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i")).equals("phone stash") && !new File(save, SaveTransfer.SOURCE_FILE).exists() && read(id).equals(first), "stash, saves and id untouched");
          File d2 = fresh(base, "other"), game2 = new File(d2, "D2R Mobile"), save2 = new File(d2, "files/save"); character(save2, "zed", "z"); stash(save2, "other stash");
          copyDir(exp, new File(game2, "save-import"));
          r = SaveTransfer.run(game2, save2, "20261101-140000");
          File[] inbox = new File(save2, "_cloudinbox").listFiles();
          check(r.size() == 1 && r.get(0).merge == SaveTransfer.MERGE_QUEUED && inbox != null && inbox.length == 1, "another phone: stash merged");
          check(!new File(d2, "files/save-transfer-id").exists() && read(new File(save2, "yyc/yyc.d2s")).equals("v"), "importing writes no id; the character is added"); }
        // 只有游戏设置的 save-import：算存档，手机上没有设置就导进来，有了就报「已在手机上」，不当成认不出改名 rejected
        // A save-import holding only the game settings: counts as saves; imported when the phone has no settings, reported as already on the phone otherwise, not renamed to rejected
        { File d = fresh(base, "settings-new"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "v");
          write(new File(game, "save-import/Settings/Settings.json"), "{\"lang\":1}");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261104-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && "Settings".equals(r.get(0).path) && read(new File(save, "Settings/Settings.json")).equals("{\"lang\":1}"), "settings-only import onto a phone without settings");
          check(new File(game, "save-import.done-20261104-120000").isDirectory() && !new File(game, "save-import.rejected-20261104-120000").exists(), "settings-only import renamed done"); }
        { File d = fresh(base, "settings-have"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); write(new File(save, "Settings/Settings.json"), "phone");
          write(new File(game, "save-import/Settings/Settings.json"), "backup");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261104-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && r.get(0).path == null && "Settings".equals(r.get(0).skipped) && read(new File(save, "Settings/Settings.json")).equals("phone"), "settings-only import onto a phone with settings: skipped, not rejected"); }
        // 中途失败：先导的 Ann 拷好了，Bob 有个文件读不出 → Ann 撤回，存档回到导入之前，save-import 留着下次再导
        // Failing halfway: Ann (first by name) is copied, one of Bob's files cannot be read -> Ann is taken back, the saves are as before, save-import stays for next time
        { File d = fresh(base, "halfway"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "v");
          File in = new File(game, "save-import"); character(in, "Ann", "a"); character(in, "Bob", "b");
          File bad = new File(in, "Bob/Bob.key");
          if (bad.setReadable(false, false) && !bad.canRead()) {
              List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261104-120000");
              check(r.size() == 1 && r.get(0).kind == SaveTransfer.FAILED, "halfway failure reported as failed");
              check(!new File(save, "Ann").exists() && !new File(save, "Bob").exists() && read(new File(save, "yyc/yyc.d2s")).equals("v"), "halfway failure: the already-added character is taken back");
              check(in.isDirectory() && !new File(d, "files/save.import.part").exists(), "halfway failure: save-import kept, part gone");
              bad.setReadable(true, false);
          } else System.out.println("SaveTransferTest: running as root, the halfway-failure case is skipped"); }
        // 编年史仓库侧文件 <共享仓库名>.swvault（存档根的单个文件，#241）：导出带上；刚装好的手机导入时拷进来
        // The Chronicle stash side file <shared stash name>.swvault (a single file at the saves root, #241): the export carries it; importing onto a fresh phone copies it in
        { File d = fresh(base, "vault-fresh"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save");
          character(save, "Bob", "b"); stash(save, "s"); write(new File(save, VAULT), "vault items");
          write(new File(game, "export"), "");
          check(SaveTransfer.run(game, save, "20261107-120000").get(0).kind == SaveTransfer.EXPORTED, "vault: export");
          File exp = new File(game, "save-export/20261107-120000");
          check(read(new File(exp, VAULT)).equals("vault items"), "vault: the export carries the Chronicle stash side file");
          File d2 = fresh(base, "vault-fresh-2"), game2 = new File(d2, "D2R Mobile"), save2 = new File(d2, "files/save");
          copyDir(exp, new File(game2, "save-import/20261107-120000"));
          List<SaveTransfer.Result> r = SaveTransfer.run(game2, save2, "20261107-130000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && r.get(0).merge == SaveTransfer.MERGE_NONE && r.get(0).path.contains(VAULT), "vault: fresh import result lists the vault");
          check(read(new File(save2, VAULT)).equals("vault items") && read(new File(save2, "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i")).equals("s")
                && read(new File(save2, "Bob/Bob.d2s")).equals("b"), "vault: fresh import copies the vault next to the stash");
          check(!new File(d2, "files/save.import.part").exists(), "vault: part gone"); }
        // 只有侧文件的 save-import（从旧导出里只拿编年史那份补回来）：认得出、拷进来，不碰共享仓库、不进收件箱
        // A save-import holding only the side file (restoring just the Chronicle from an older export): recognised and copied in, the shared stash untouched, nothing queued
        { File d = fresh(base, "vault-only"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save");
          character(save, "yyc", "v"); stash(save, "phone stash");
          write(new File(game, "save-import/" + VAULT), "vault items");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261107-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && VAULT.equals(r.get(0).path) && r.get(0).merge == SaveTransfer.MERGE_NONE, "vault-only import accepted");
          check(read(new File(save, VAULT)).equals("vault items") && read(new File(save, "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i")).equals("phone stash")
                && !new File(save, "_cloudinbox").exists(), "vault-only import: vault in, stash untouched, nothing queued");
          check(new File(game, "save-import.done-20261107-120000").isDirectory(), "vault-only import renamed done"); }
        // 手机上已有侧文件：内容一样什么都不做；不一样就进并回收件箱（包里路径 = 文件名），手机上那份不动
        // The phone already has a side file: the same contents do nothing; different ones go to the merge inbox (path in the pack = the file name), the phone's copy untouched
        { File d = fresh(base, "vault-have"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save");
          stash(save, "phone stash"); write(new File(save, VAULT), "phone vault");
          write(new File(game, "save-import/" + VAULT), "phone vault");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261107-120000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && r.get(0).path == null && r.get(0).merge == SaveTransfer.MERGE_NONE && !new File(save, "_cloudinbox").exists(), "same vault: nothing to do");
          write(new File(game, "save-import/" + VAULT), "other vault");
          r = SaveTransfer.run(game, save, "20261107-130000");
          check(r.size() == 1 && r.get(0).kind == SaveTransfer.IMPORTED && r.get(0).merge == SaveTransfer.MERGE_QUEUED, "different vault: merge queued");
          check(read(new File(save, VAULT)).equals("phone vault"), "different vault: the phone's vault untouched");
          File[] entries = new File(save, "_cloudinbox").listFiles();
          check(entries != null && entries.length == 1, "different vault: one inbox entry");
          java.nio.ByteBuffer bb = java.nio.ByteBuffer.wrap(Files.readAllBytes(new File(entries[0], "mine.pack").toPath())).order(java.nio.ByteOrder.LITTLE_ENDIAN);
          bb.position(20); int nf = bb.getInt(); int pl = bb.getShort() & 0xffff; byte[] pth = new byte[pl]; bb.get(pth); bb.getLong(); byte[] body = new byte[bb.getInt()]; bb.get(body);
          check(nf == 1 && new String(pth, StandardCharsets.UTF_8).equals(VAULT) && new String(body, StandardCharsets.UTF_8).equals("other vault") && !bb.hasRemaining(), "different vault: pack holds the backup's vault at the root");
          write(new File(game, "save-import/" + VAULT), "other vault");
          r = SaveTransfer.run(game, save, "20261107-140000");
          check(r.get(0).merge == SaveTransfer.MERGE_ALREADY && new File(save, "_cloudinbox").listFiles().length == 1, "different vault: not queued twice"); }
        // 共享仓库和侧文件都不同：同一个条目里两份都有（按路径排序，侧文件在前）；没有侧文件时的摘要与原先一样（旧条目照样认作已并过）
        // Both the shared stash and the side file differ: one entry holds both (sorted by path, the side file first); without a side file the digest is the same as before (old entries are still recognised as merged)
        { File d = fresh(base, "vault-both"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save");
          stash(save, "phone stash"); write(new File(save, VAULT), "phone vault");
          File in = new File(game, "save-import"); stash(in, "other stash"); write(new File(in, VAULT), "other vault");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261107-120000");
          check(r.size() == 1 && r.get(0).merge == SaveTransfer.MERGE_QUEUED, "stash + vault: merge queued");
          File[] entries = new File(save, "_cloudinbox").listFiles();
          java.nio.ByteBuffer bb = java.nio.ByteBuffer.wrap(Files.readAllBytes(new File(entries[0], "mine.pack").toPath())).order(java.nio.ByteOrder.LITTLE_ENDIAN);
          bb.position(20); int nf = bb.getInt(); String[] paths = new String[nf];
          for (int i = 0; i < nf; i++) { byte[] pth = new byte[bb.getShort() & 0xffff]; bb.get(pth); paths[i] = new String(pth, StandardCharsets.UTF_8); bb.getLong(); int len = bb.getInt(); bb.position(bb.position() + len); }
          check(nf == 2 && paths[0].equals(VAULT) && paths[1].equals("ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"), "stash + vault: one pack, both files"); }
        // 同一台手机导回来：侧文件不同也不并（同共享仓库）；手机上没有的照样拷进来
        // Imported back on the same phone: a different side file is not merged (as for the stash); one the phone lacks is still copied in
        { File d = fresh(base, "vault-same-phone"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save");
          stash(save, "phone stash"); write(new File(save, VAULT), "before"); write(new File(game, "export"), "");
          SaveTransfer.run(game, save, "20261107-120000");
          File exp = new File(game, "save-export/20261107-120000");
          write(new File(save, VAULT), "after");
          copyDir(exp, new File(game, "save-import"));
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261107-130000");
          check(r.size() == 1 && r.get(0).merge == SaveTransfer.MERGE_SAME_PHONE && read(new File(save, VAULT)).equals("after") && !new File(save, "_cloudinbox").exists(), "same phone: a different vault is not merged");
          new File(save, VAULT).delete(); copyDir(exp, new File(game, "save-import"));
          r = SaveTransfer.run(game, save, "20261107-140000");
          check(r.size() == 1 && read(new File(save, VAULT)).equals("before"), "same phone: a missing vault is copied in"); }
        // 名字不是共享仓库的 .swvault、侧文件之外的散文件：不导
        // A .swvault not named after a shared stash, and loose files other than the side file: not imported
        { File d = fresh(base, "vault-other"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "v");
          File in = new File(game, "save-import"); character(in, "Bob", "b"); write(new File(in, "notes.swvault"), "x"); write(new File(in, "ModernSharedStashSoftCoreV2.d2i"), "x");
          List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261107-120000");
          check(r.size() == 1 && "Bob".equals(r.get(0).path) && !new File(save, "notes.swvault").exists() && !new File(save, "ModernSharedStashSoftCoreV2.d2i").exists(), "other loose files not imported"); }
        // 中途失败：先拷进来的侧文件也撤回
        // Failing halfway: a side file already copied in is taken back too
        { File d = fresh(base, "vault-halfway"), game = new File(d, "D2R Mobile"), save = new File(d, "files/save"); character(save, "yyc", "v");
          File in = new File(game, "save-import"); write(new File(in, VAULT), "vault items"); character(in, "Zed", "z");
          File bad = new File(in, "Zed/Zed.key");
          if (bad.setReadable(false, false) && !bad.canRead()) {
              List<SaveTransfer.Result> r = SaveTransfer.run(game, save, "20261107-120000");
              check(r.size() == 1 && r.get(0).kind == SaveTransfer.FAILED && !new File(save, VAULT).exists() && !new File(save, "Zed").exists(), "halfway failure: the copied vault is taken back");
              bad.setReadable(true, false);
          } else System.out.println("SaveTransferTest: running as root, the vault halfway-failure case is skipped"); }
        System.out.println("SaveTransferTest OK, checks=" + checks);
    }
}
