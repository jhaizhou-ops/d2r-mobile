package com.xiaoboluo.d2r;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;

// 暂停菜单「分享诊断日志」的安卓侧文件（#216）：头部（外壳给的版本、机型、温控 / 电量 / 内存 / 存储）+ 本局与上一局的关键读数（各取最后一条）+
// 本局日志 + 上一局日志。安卓的 boot.log 是追加写的（jni_android.c log_init，每次启动接着写），一局从它的第一行「boot: 内部目录」算起
// （jni_android.c nativeStart），所以从文件尾往前找这一行切出本局与上一局。一局太长时留开头 1 MB 与结尾 5 MB，中间写明略去多少字节。
// 与 iOS dm_sharelog.m 同一份格式（搜行用的中文键保持中文）；只放游戏与宿主自己写的日志，安卓的路径里没有用户名，不另外洗。
// 纯 Java、不碰安卓的类，可在主机上测（dm/android/tests/ShareLogTest.java）
// The Android side of the pause menu's Share Diagnostics file (#216): a header (version, device, thermal / battery / memory / storage, given by
// the shell) + the key readings of this run and the previous one (the last line of each) + this run's log + the previous run's log. On Android
// boot.log is appended to (jni_android.c log_init, every start writes on), and a run starts at its first line, "boot: 内部目录" (jni_android.c
// nativeStart), so searching backwards from the end of the file for that line cuts out this run and the previous one. A run that is too long
// keeps its first 1 MB and last 5 MB, with the number of bytes left out written in between. The same format as iOS dm_sharelog.m (the
// Chinese search keys stay Chinese); only the logs the game and the host write themselves, and Android paths hold no user name, so nothing
// is scrubbed. Plain Java without Android classes, testable on the host (dm/android/tests/ShareLogTest.java)
final class ShareLog {
    private ShareLog() {}

    static final String RUN_START = "[dm] boot: 内部目录";
    static final String[] KEYS = { "boot: 内部目录", "NVN caps device", "[perf]", "[nvn] 上屏", "NVN PERF v=1" };
    static final int HEAD = 1 << 20, TAIL = 5 << 20;

    // 在 dir 下写一份 name，返回它。dir 只放这一份导出：每次清空重写（是我们自己的缓存目录，别的不碰）
    // Writes name in dir and returns it. dir holds only this one export: it is emptied every time (our own cache directory; nothing else is touched)
    static File write(File log, File dir, String name, String header) throws IOException {
        if (name.isEmpty() || name.contains("/") || name.startsWith(".")) throw new IOException("name " + name);
        if (dir.isDirectory()) { File[] old = dir.listFiles(); if (old != null) for (File f : old) Files.deleteIfExists(f.toPath()); }
        else if (!dir.mkdirs()) throw new IOException("mkdir " + dir.getName());
        File out = new File(dir, name), tmp = new File(dir, name + ".part");
        ByteBuffer data = map(log);
        long[] runs = runs(data);
        try (OutputStream o = new FileOutputStream(tmp)) {
            StringBuilder h = new StringBuilder(header);
            keyLines(h, "本局关键读数（各取最后一条）", data, runs[0], runs[1]);
            keyLines(h, "上一局关键读数（各取最后一条）", data, runs[2], runs[3]);
            o.write(h.toString().getBytes(StandardCharsets.UTF_8));
            part(o, "本局日志 boot.log", data, runs[0], runs[1]);
            part(o, "上一局日志 boot.log", data, runs[2], runs[3]);
        }
        Files.move(tmp.toPath(), out.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        return out;
    }

    private static ByteBuffer map(File log) throws IOException {
        if (!log.isFile()) return null;
        try (FileChannel c = FileChannel.open(log.toPath(), StandardOpenOption.READ)) {
            return c.map(FileChannel.MapMode.READ_ONLY, 0, c.size());
        }
    }

    // 本局 [起, 止)、上一局 [起, 止)；没有那一局时起止都是 -1。一行都没有「boot: 内部目录」时整份当本局
    // This run [start, end), the previous run [start, end); -1 for both when that run is missing. Without any "boot: 内部目录" line the whole file is this run
    static long[] runs(ByteBuffer d) {
        long[] r = { -1, -1, -1, -1 };
        if (d == null) return r;
        int end = d.limit();
        int cur = lastLineStart(d, RUN_START.getBytes(StandardCharsets.UTF_8), 0, end);
        if (cur < 0) { r[0] = 0; r[1] = end; return r; }
        r[0] = cur; r[1] = end;
        int prev = lastLineStart(d, RUN_START.getBytes(StandardCharsets.UTF_8), 0, cur);
        if (prev >= 0) { r[2] = prev; r[3] = cur; }
        return r;
    }

    // [from, to) 里最后一个在行首的 key 的位置；没有返回 -1
    // The position of the last key at the start of a line in [from, to); -1 when there is none
    private static int lastLineStart(ByteBuffer d, byte[] key, int from, int to) {
        for (int i = to - key.length; i >= from; i--) {
            if ((i == 0 || d.get(i - 1) == '\n') && matches(d, i, key)) return i;
        }
        return -1;
    }

    private static boolean matches(ByteBuffer d, int at, byte[] key) {
        for (int k = 0; k < key.length; k++) if (d.get(at + k) != key[k]) return false;
        return true;
    }

    // [from, to) 里最后一条含 key 的行（不含行尾换行）；没有返回 null
    // The last line in [from, to) containing key (without the newline); null when there is none
    static String lastLine(ByteBuffer d, long from, long to, String key) {
        if (d == null || from < 0) return null;
        byte[] k = key.getBytes(StandardCharsets.UTF_8);
        for (int i = (int) to - k.length; i >= from; i--) {
            if (!matches(d, i, k)) continue;
            int s = i, e = i;
            while (s > from && d.get(s - 1) != '\n') s--;
            while (e < to && d.get(e) != '\n') e++;
            byte[] b = new byte[e - s];
            for (int j = 0; j < b.length; j++) b[j] = d.get(s + j);
            return new String(b, StandardCharsets.UTF_8);
        }
        return null;
    }

    private static void keyLines(StringBuilder h, String title, ByteBuffer d, long from, long to) {
        h.append("\n—— ").append(title).append(" ——\n");
        if (d == null || from < 0) { h.append("（没有这份日志）\n"); return; }
        for (String key : KEYS) {
            String l = lastLine(d, from, to, key);
            h.append(l != null ? l : "（没有「" + key + "」行）").append('\n');
        }
    }

    // 一局的日志：超过 HEAD + TAIL 时留开头 HEAD 字节与结尾 TAIL 字节（结尾从下一行行首起），中间写明略去多少
    // One run's log: beyond HEAD + TAIL, its first HEAD bytes and last TAIL bytes are kept (the tail starts at the next line), with the bytes left out written in between
    static void part(OutputStream o, String title, ByteBuffer d, long from, long to) throws IOException {
        long n = from < 0 ? -1 : to - from;
        o.write(("\n\n===== " + title + "（" + (n < 0 ? "没有这份日志" : n + " 字节") + "）=====\n").getBytes(StandardCharsets.UTF_8));
        if (n <= 0) return;
        if (n <= (long) HEAD + TAIL) { copy(o, d, (int) from, (int) to); return; }
        int headEnd = (int) from + HEAD, tailStart = (int) to - TAIL;
        while (tailStart < to && d.get(tailStart - 1) != '\n') tailStart++;
        copy(o, d, (int) from, headEnd);
        o.write(("\n…（中间 " + (tailStart - headEnd) + " 字节略去）…\n").getBytes(StandardCharsets.UTF_8));
        copy(o, d, tailStart, (int) to);
    }

    private static void copy(OutputStream o, ByteBuffer d, int from, int to) throws IOException {
        byte[] b = new byte[1 << 16];
        for (int i = from; i < to; ) {
            int n = Math.min(b.length, to - i);
            for (int j = 0; j < n; j++) b[j] = d.get(i + j);
            o.write(b, 0, n); i += n;
        }
    }
}
