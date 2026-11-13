package com.xiaoboluo.d2r;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** 只在临时目录里造假日志。 | Builds fake logs in a temporary directory only. */
public final class ShareLogTest {
    private static int checks;
    private static void check(boolean value, String label) { ++checks; if (!value) throw new AssertionError(label); }
    private static ByteBuffer buf(String s) { return ByteBuffer.wrap(s.getBytes(StandardCharsets.UTF_8)); }
    private static String read(File f) throws IOException { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); }

    public static void main(String[] args) throws IOException {
        String run1 = "[dm] boot: 内部目录 a\n[dm] NVN caps device=\"Adreno (TM) 830\" one\n[dm] [perf] 帧率 50\n";
        String run2 = "[dm] boot: 内部目录 b\n[dm] [perf] 帧率 60\nx [dm] boot: 内部目录 not at a line start\n[dm] [perf] 帧率 61\n";
        String both = run1 + run2;
        // 两局：本局从最后一个行首的「boot: 内部目录」起，上一局到它为止；行中间的不算
        // Two runs: this run starts at the last "boot: 内部目录" at a line start, the previous one ends there; one in mid-line does not count
        long[] r = ShareLog.runs(buf(both));
        int split = run1.getBytes(StandardCharsets.UTF_8).length, end = both.getBytes(StandardCharsets.UTF_8).length;
        check(r[0] == split && r[1] == end && r[2] == 0 && r[3] == split, "two runs split at the run start");
        check("[dm] [perf] 帧率 61".equals(ShareLog.lastLine(buf(both), r[0], r[1], "[perf]")), "last key line of this run");
        check("[dm] [perf] 帧率 50".equals(ShareLog.lastLine(buf(both), r[2], r[3], "[perf]")), "last key line of the previous run");
        check(ShareLog.lastLine(buf(both), r[0], r[1], "NVN caps device") == null, "a key only in the previous run is not found in this one");
        // 一局：没有上一局；没有标记行：整份当本局；空文件、没有文件
        // One run: no previous run; no marker line: the whole file is this run; an empty file, no file
        r = ShareLog.runs(buf(run1));
        check(r[0] == 0 && r[2] == -1 && r[3] == -1, "one run, no previous");
        r = ShareLog.runs(buf("no marker\nat all\n"));
        check(r[0] == 0 && r[1] == 17 && r[2] == -1, "no marker: the whole file is this run");
        r = ShareLog.runs(buf(""));
        check(r[0] == 0 && r[1] == 0 && r[2] == -1, "empty file");
        r = ShareLog.runs(null);
        check(r[0] == -1 && r[2] == -1, "no file");
        // 太长的一局：留开头 HEAD 与结尾 TAIL（结尾从行首起），中间写明略去多少；短的整段照抄
        // A run that is too long: the first HEAD and the last TAIL bytes (the tail from a line start), with the bytes left out in between; a short one is copied whole
        StringBuilder big = new StringBuilder();
        int line = 0;
        while (big.length() < ShareLog.HEAD + ShareLog.TAIL + 300000) big.append(String.format("line %07d %s\n", line++, "x".repeat(40)));
        ByteBuffer b = buf(big.toString());
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        ShareLog.part(o, "t", b, 0, b.limit());
        String out = o.toString(StandardCharsets.UTF_8);
        int cut = out.indexOf("字节略去");
        check(cut > 0 && out.startsWith("\n\n===== t（" + b.limit() + " 字节）=====\nline 0000000 "), "long run: header and head");
        String tail = out.substring(out.indexOf('\n', cut) + 1);
        check(tail.startsWith("line ") && big.toString().endsWith(tail) && tail.length() <= ShareLog.TAIL, "long run: the tail starts at a line and is the end of the log");
        long dropped = Long.parseLong(out.substring(out.indexOf("（中间 ") + 4, out.indexOf(" 字节略去")));
        check(dropped == b.limit() - ShareLog.HEAD - tail.getBytes(StandardCharsets.UTF_8).length, "long run: the bytes left out add up");
        o.reset(); ShareLog.part(o, "t", buf(run1), 0, run1.getBytes(StandardCharsets.UTF_8).length);
        check(o.toString(StandardCharsets.UTF_8).endsWith(run1), "short run copied whole");
        o.reset(); ShareLog.part(o, "t", null, -1, -1);
        check(o.toString(StandardCharsets.UTF_8).contains("没有这份日志"), "missing run says so");
        // 整个文件：头部、两局读数、两局日志；目录里旧的导出清掉；名字带路径的不收
        // The whole file: header, both runs' readings, both runs' logs; an old export in the directory is removed; a name with a path is refused
        File base = Files.createTempDirectory("dm-sharelog").toFile(), log = new File(base, "boot.log"), dir = new File(base, "sharelog");
        Files.write(log.toPath(), both.getBytes(StandardCharsets.UTF_8));
        dir.mkdirs(); Files.write(new File(dir, "old.txt").toPath(), new byte[] { 1 });
        File f = ShareLog.write(log, dir, "D2RMobile_diagnostics_2.3.0_20261113_120000.txt", "D2R Mobile Diagnostics\nheader\n");
        String text = read(f);
        check(f.getParentFile().equals(dir) && dir.listFiles().length == 1, "only the new export is in the directory");
        check(text.startsWith("D2R Mobile Diagnostics\nheader\n\n—— 本局关键读数"), "header first");
        String readings = text.substring(0, text.indexOf("===== 本局日志"));
        String thisRun = readings.substring(0, readings.indexOf("—— 上一局")), prevRun = readings.substring(readings.indexOf("—— 上一局"));
        check(thisRun.contains("x [dm] boot: 内部目录 not at a line start\n") && thisRun.contains("（没有「NVN caps device」行）") && thisRun.contains("[dm] [perf] 帧率 61\n"), "this run's readings, a missing key noted");
        check(prevRun.contains("[dm] NVN caps device=\"Adreno (TM) 830\" one\n") && prevRun.contains("[dm] [perf] 帧率 50\n"), "the previous run's readings");
        check(text.contains("===== 本局日志 boot.log（" + (end - split) + " 字节）=====\n" + run2) && text.endsWith("===== 上一局日志 boot.log（" + split + " 字节）=====\n" + run1), "both runs' logs");
        File none = ShareLog.write(new File(base, "missing.log"), dir, "n.txt", "h\n");
        check(read(none).contains("（没有这份日志）") && dir.listFiles().length == 1, "no log file: the file says so, old export replaced");
        boolean refused = false;
        try { ShareLog.write(log, dir, "../x.txt", "h"); } catch (IOException e) { refused = true; }
        check(refused, "a name with a path is refused");
        System.out.println("ShareLogTest OK, checks=" + checks);
    }
}
