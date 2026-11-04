// 安卓外壳：一个全屏横屏的 SurfaceView，原生层（libdm.so，dm/android/jni_android.c）跑宿主与游戏。
// 对应 iOS 的 main.m（启动链）与 front_ios.m（视图、触屏、生命周期）；本类只做系统对接，游戏逻辑与界面都在原生层。
// Android shell: a full-screen landscape SurfaceView; the native layer (libdm.so, dm/android/jni_android.c) runs the host and the game.
// Counterpart of iOS's main.m (startup chain) and front_ios.m (view, touch, lifecycle); this class only does system integration, game logic and UI are all in the native layer.
package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.ErrnoException;
import android.system.Os;
import android.text.Editable;
import android.text.InputType;
import android.text.SpanWatcher;
import android.text.Spannable;
import android.text.Spanned;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.inputmethod.BaseInputConnection;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.EditText;
import android.widget.FrameLayout;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class DmActivity extends Activity implements SurfaceHolder.Callback, DisplayManager.DisplayListener {
    static native int nativeStart(String filesDir, String extDir, String romfsSrc, String lang);
    static native void nativeSurface(Object surface, int w, int h);
    static native void nativeRefresh(float hz);
    static native void nativeTouch(int n, int[] ids, float[] xs, float[] ys);
    static native void nativeFocus(int ev);
    static native void nativeTextResult(long id, boolean accepted, String text);
    // 越肩镜头的体感转向（CamSensor）：开哪种传感器（0 = 不开），每次转动的弧度
    // Sensor steering of the over-the-shoulder camera (CamSensor): which sensor to listen to (0 = none), each turn in radians
    static native int nativeCamSensor();
    static native void nativeCamTurn(float rad);
    // 镜头档（nativeStart 之后才准）与「此刻在局里、镜头在出图」（CamButtons）
    // The camera mode (valid only after nativeStart) and "in a game right now with the camera drawing" (CamButtons)
    static native int nativeCamMode();
    static native int nativeCamActive();
    // 越肩档的操作风格（1 = 原神式：没有左右转按钮）
    // The over-the-shoulder control style (1 = Genshin style: no turn buttons)
    static native int nativeCamStyle();
    // 拖技能键瞄准时地上那个环（CamThreats）：5 × 5 网格点，返回 25 = 正在瞄
    // The ground ring while a skill button is dragged to aim (CamThreats): a 5 x 5 grid of points, returns 25 = aiming
    static native int nativeCamAim(float[] out);
    // 画面外的怪（CamThreats）：每条 5 个数，返回条数
    // Monsters off the picture (CamThreats): 5 numbers each, returns the count
    static native int nativeCamThreats(float[] out);
    // 小地图上的朝向箭头（CamThreats）：4 个数，返回 1 = 该画
    // Heading arrow on the minimap (CamThreats): 4 numbers, returns 1 = draw it
    static native int nativeCamHeading(float[] out);
    static native void nativePad(long buttons, int lx, int ly, int rx, int ry);
    static native void nativePadMode(int pad);
    static native void nativePadGlyph(int glyph);

    private static WeakReference<DmActivity> current = new WeakReference<>(null);
    private static volatile int bootFailure;
    private AlertDialog inputDialog;

    private SurfaceView view;
    private FrameLayout gameFrame;
    private TouchOverlay touchOverlay;
    private boolean started;
    private boolean importing, savesTransferred;
    private boolean libraryLoaded;
    private boolean resumed;
    private DisplayManager displays;
    private CamSensor camSensor;
    private CamButtons camButtons;
    private CamThreats camThreats;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        // 调试包：am start … --ez dmArtDump true 只把触屏控件底图写到 files/dm/artdump/ 就退出（dm/android/tools/art_compare.sh 用）
        // Debug builds: am start … --ez dmArtDump true only writes the touch control art to files/dm/artdump/ and exits (used by dm/android/tools/art_compare.sh)
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra("dmArtDump", false)) {
            try { new TouchOverlay(this, null, null).dumpArt(new File(getFilesDir(), "dm/artdump")); } catch (IOException e) { android.util.Log.e("dm", "art dump: " + e); }
            finish(); return;
        }
        current = new WeakReference<>(this);
        displays = getSystemService(DisplayManager.class);
        if (displays != null) displays.registerDisplayListener(this, null);
        camSensor = new CamSensor(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        gameFrame = new FrameLayout(this);
        gameFrame.addView(view, new FrameLayout.LayoutParams(-1, -1));
        setContentView(gameFrame);
        immersive();
    }

    private void immersive() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }

    // 实体手柄：按键与摇杆在 PadInput 里合成宿主的手柄状态（nativePad → dm_pad_set），与触屏的虚拟手柄在游戏取手柄时合并。
    // 手柄事件在这里吃掉，不再往下传给视图（否则会触发触摸控件、系统返回等）。
    // Physical controller: keys and sticks are merged in PadInput into the host controller state (nativePad -> dm_pad_set), combined with
    // the touch stick when the game reads the controller. Controller events are consumed here and not passed on to the views.
    // 原生库载入之前（导入进度页、「导入包版本不对」等启动错误页）手柄事件不进 PadInput：它要调原生方法，库还没载入就是
    // UnsatisfiedLinkError，App 直接闪退（#178：装好新版、导入之前连上手柄就闪退）。这时交给系统，照普通界面处理
    // Before the native library is loaded (the import progress page, start errors such as "the import pack is for another version"), controller
    // events do not go to PadInput: it calls native methods, which throw UnsatisfiedLinkError while the library is not loaded and kill the app
    // (#178: connecting a controller after installing a new version, before the import, crashed it). The system handles them as on any other screen
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (libraryLoaded && PadInput.onKey(e)) return true;
        return super.dispatchKeyEvent(e);
    }

    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        if (libraryLoaded && PadInput.onMotion(e)) return true;
        return super.onGenericMotionEvent(e);
    }

    // 手柄布局下碰屏 = 切回触屏布局，这一根手指整根不执行（此刻触控控件还没露出来，落点可能正好压在看不见的按钮上；
    // iOS 的 front_ios.m 同样把这根手指吞掉）。之后的手指正常。
    // Touching the screen in the controller layout = back to the touch layout, and that finger is swallowed for its whole life (the touch
    // widgets are not shown yet and the landing point may sit on an invisible button; iOS front_ios.m swallows it the same way).
    private final Set<Integer> swallowed = new HashSet<>();

    @Override public boolean dispatchTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            boolean wasPad = PadInput.isPadMode();
            PadInput.onTouch();
            if (wasPad) swallowed.add(e.getPointerId(0));
        }
        boolean swallowThis = false;
        for (int i = 0; i < e.getPointerCount(); i++) if (swallowed.contains(e.getPointerId(i))) swallowThis = true;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) swallowed.clear();
        else if (action == MotionEvent.ACTION_POINTER_UP) swallowed.remove(e.getPointerId(e.getActionIndex()));
        if (swallowThis) return true;
        return super.dispatchTouchEvent(e);
    }

    // 手柄布局隐藏触控控件（iOS 由覆盖层的 _padMode 控制） | The controller layout hides the touch widgets (iOS does it through _padMode)
    static void onPadMode(boolean pad) {
        DmActivity a = current.get();
        if (a == null) return;
        a.applyPadMode(pad);
    }

    private void applyPadMode(boolean pad) {
        if (touchOverlay == null) return;
        touchOverlay.setInputEnabled(!pad);
        touchOverlay.setVisibility(pad ? View.GONE : View.VISIBLE);
    }

    // 资源在玩家放的位置读（缺省 Download/S2X/D2R Mobile/data），要「所有文件访问」权限；没有就引导去系统设置页，回来再启动
    // Resources are read where the player put them (default Download/S2X/D2R Mobile/data), which needs "all files access"; without it, guide the player to the system settings page and start after returning
    private boolean ensureFileAccess() {
        if (Environment.isExternalStorageManager()) return true;
        TextView t = new TextView(this);
        t.setText(R.string.dmAndroidPermission);
        t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        t.setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))));
        setContentView(t);
        return false;
    }

    // 导入包：玩家在电脑上用 dm/tools/d2rm_android.py 和自己的游戏文件做出 android 文件夹，拷到 Download/S2X/D2R Mobile/ 下；
    // 这里把它拷进内部目录 files/dm/（out、shaders、overlay、derived.tsv、pack_info.txt）。pack_info.txt 与已导入的逐字相同就不再拷。
    // 先拷到 files/dm/import.part/，齐了再逐项换上，pack_info.txt 最后放：中途被杀或空间不够，下次打开从头再导，不会用到半份。
    // 原生库加载之前做：不含游戏内容的发布包，几张表在库加载时就从 derived.tsv 读（dm/host/dm_derived_rt.h）。
    // Import pack: the player makes the android folder on a computer with dm/tools/d2rm_android.py and their own game files and copies it to
    // Download/S2X/D2R Mobile/; this copies it into the internal files/dm/ (out, shaders, overlay, derived.tsv, pack_info.txt). Nothing is copied
    // when pack_info.txt is the same as the imported one. It goes to files/dm/import.part/ first and replaces the items one by one once complete,
    // pack_info.txt last: if the app is killed or runs out of space, the next start imports again and half a copy is never used.
    // Done before the native library loads: in the release APK without game content, a few tables are read from derived.tsv at load time.
    private static final String[] IMPORT_ITEMS = {"out", "shaders", "overlay", "derived.tsv", "pack_info.txt"};
    // 可有可无的导入项：走覆盖层的自定义开关的各组合（物品注释与旧译名，#191；jni_android.c 启动时按开关挑一份）。
    // 旧导入包没有它，照常导入；新导入包没有它时，旧的那份删掉
    // Optional import items: the overlays for custom switches that go through the overlay (item notes and old names, #191; jni_android.c picks one at start).
    // Older import packs lack it and still import; when a new import pack lacks it, the old copy is removed
    private static final String[] OPTIONAL_ITEMS = {"overlay_variants"};

    private static String readText(File f) {
        try { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (IOException e) { return null; }
    }

    private static String packProduct(String info) {
        if (info != null) for (String line : info.split("\n")) if (line.startsWith("PRODUCT=")) return line.substring(8).trim();
        return null;
    }

    // true = 可以接着启动；false = 正在导入或已显示错误
    // true = startup can continue; false = importing, or an error is shown
    private boolean ensureImported(File source) {
        if (importing) return false;
        File runtime = new File(getFilesDir(), "dm");
        String want = readText(new File(source, "pack_info.txt")), have = readText(new File(runtime, "pack_info.txt"));
        if (want != null && !want.equals(have)) {
            String product = packProduct(want);
            if (!BuildConfig.DM_PRODUCT.equals(product)) { showStartError(getString(R.string.dmAndroidImportVersion, product)); return false; }
            importing = true;
            TextView t = new TextView(this);
            t.setTextSize(20); t.setPadding(64, 64, 64, 64); t.setText(getString(R.string.dmAndroidImporting, 0));
            setContentView(t);
            new Thread(() -> {
                String error = null;
                try { importPack(source, runtime, percent -> runOnUiThread(() -> t.setText(getString(R.string.dmAndroidImporting, percent)))); }
                catch (IOException e) { error = e.getMessage() == null ? e.toString() : e.getMessage(); }
                String failure = error;
                runOnUiThread(() -> {
                    importing = false;
                    if (failure != null) showStartError(getString(R.string.dmAndroidImportFailed, failure));
                    else startHost();
                });
            }, "dm-import").start();
            return false;
        }
        // 已导入的是给别的版本做的（App 升级了、导入包没重做）：同样要重做导入包。开发时直接推进来的没有 pack_info.txt，不核
        // The imported pack was made for another version (the app was updated, the pack was not remade): it has to be remade too.
        // Files pushed directly during development have no pack_info.txt and are not checked
        String product = packProduct(have);
        if (have != null && !BuildConfig.DM_PRODUCT.equals(product)) { showStartError(getString(R.string.dmAndroidImportVersion, product)); return false; }
        return true;
    }

    private interface Progress { void percent(int p); }

    private static void importPack(File source, File runtime, Progress progress) throws IOException {
        for (String item : IMPORT_ITEMS) if (!new File(source, item).exists()) throw new IOException("missing " + item);
        List<String> items = new ArrayList<>(Arrays.asList(IMPORT_ITEMS));
        for (String item : OPTIONAL_ITEMS) if (new File(source, item).exists()) items.add(item);
        File part = new File(runtime, "import.part");
        deleteTree(part);
        long sum = 0;
        for (String item : items) sum += treeSize(new File(source, item));
        final long total = sum;
        long[] done = {0}; int[] shown = {-1};
        byte[] buffer = new byte[1 << 20];
        for (String item : items) copyTree(new File(source, item), new File(part, item), buffer, bytes -> {
            done[0] += bytes;
            int p = (int) (done[0] * 100 / Math.max(1, total));
            if (p != shown[0]) { shown[0] = p; progress.percent(p); }
        });
        for (String item : items) {
            File from = new File(part, item), to = new File(runtime, item);
            deleteTree(to);
            if (!from.renameTo(to)) throw new IOException("rename " + item);
        }
        for (String item : OPTIONAL_ITEMS) if (!items.contains(item)) deleteTree(new File(runtime, item));
        deleteTree(part);
    }

    private interface Copied { void bytes(long n); }

    private static long treeSize(File f) {
        if (!f.isDirectory()) return f.length();
        long n = 0; File[] list = f.listFiles();
        if (list != null) for (File c : list) n += treeSize(c);
        return n;
    }

    private static void copyTree(File from, File to, byte[] buffer, Copied copied) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to.getName());
            File[] list = from.listFiles();
            if (list == null) throw new IOException("list " + from.getName());
            for (File c : list) copyTree(c, new File(to, c.getName()), buffer, copied);
            return;
        }
        File parent = to.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent.getName());
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            for (int n; (n = in.read(buffer)) > 0; ) { out.write(buffer, 0, n); copied.bytes(n); }
        }
    }

    private static void deleteTree(File f) throws IOException {
        File[] list = f.isDirectory() ? f.listFiles() : null;
        if (list != null) for (File c : list) deleteTree(c);
        if (f.exists() && !f.delete()) throw new IOException("delete " + f.getName());
    }

    // 存档导出 / 导入（SaveTransfer）：游戏文件夹里放了 export 记号或 save-import 文件夹时，在原生库加载之前、每个进程做一次，结果用一条提示告诉玩家
    // Save export / import (SaveTransfer): with an export marker or a save-import folder in the game folder, done once per process before the native
    // library loads, and the result is told to the player in a toast
    private void transferSaves(File game) {
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        for (SaveTransfer.Result r : SaveTransfer.run(game, new File(getFilesDir(), "save"), stamp)) {
            String where = r.path == null ? null : "Download/S2X/D2R Mobile/" + r.path;
            String text = switch (r.kind) {
                case SaveTransfer.EXPORTED -> getString(R.string.dmAndroidSaveExported, where);
                case SaveTransfer.EXPORT_EMPTY -> getString(R.string.dmAndroidSaveExportEmpty);
                case SaveTransfer.IMPORTED -> r.path == null ? (r.skipped == null ? null : getString(R.string.dmAndroidSaveImportNothing, r.skipped))
                    : r.skipped == null ? getString(R.string.dmAndroidSaveImported, r.path) : getString(R.string.dmAndroidSaveImportedSome, r.path, r.skipped);
                case SaveTransfer.IMPORT_REJECTED -> getString(R.string.dmAndroidSaveImportRejected, r.path);
                default -> getString(R.string.dmAndroidSaveTransferFailed, r.path);
            };
            android.util.Log.i("dm", "存档导出 / 导入：结果 " + r.kind + "，" + r.path + (r.skipped == null ? "" : "，已有没导：" + r.skipped) + (r.merge == SaveTransfer.MERGE_NONE ? "" : "，共享仓库并回 " + r.merge));
            if (text != null) Toast.makeText(this, text, Toast.LENGTH_LONG).show();
            if (r.merge != SaveTransfer.MERGE_NONE)
                Toast.makeText(this, getString(r.merge == SaveTransfer.MERGE_QUEUED ? R.string.dmAndroidSaveStashMerge : r.merge == SaveTransfer.MERGE_ALREADY ? R.string.dmAndroidSaveStashMerged : R.string.dmAndroidSaveStashSamePhone), Toast.LENGTH_LONG).show();
        }
    }

    private void startHost() {
        if (bootFailure != 0) { showBootFailure(); return; }
        if (started || importing || !resumed || !ensureFileAccess()) return;
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (!libraryLoaded && !savesTransferred) { savesTransferred = true; transferSaves(new File(downloads, "S2X/D2R Mobile")); }
        if (!libraryLoaded && !ensureImported(new File(downloads, "S2X/D2R Mobile/android"))) return;
        if (gameFrame.getParent() == null) setContentView(gameFrame);
        if (!libraryLoaded) {
            // 与 jni_android.c 选运行目录同一条规则：内部目录有主程序就用内部目录，否则用外部私有目录
            // Same rule as jni_android.c's choice of runtime directory: internal storage when it has the main module, otherwise external private storage
            File ext = getExternalFilesDir(null), internal = new File(getFilesDir(), "dm");
            File runtime = new File(internal, "out/main.patched.bin").canRead() || ext == null ? internal : new File(ext, "dm");
            try { Os.setenv("DM_DERIVED", new File(runtime, "derived.tsv").getAbsolutePath(), true); } catch (ErrnoException e) { showStartError(getString(R.string.dmAndroidStorage)); return; }
            try { System.loadLibrary("dm"); libraryLoaded = true; }
            catch (UnsatisfiedLinkError error) { showStartError(getString(R.string.dmAndroidLibrary)); return; }
        }
        if (touchOverlay == null) {
            int[] gameIds = {R.string.dmAndroidAttack, R.string.dmAndroidSkill1, R.string.dmAndroidSkill2, R.string.dmAndroidSkill3, R.string.dmAndroidSkill4,
                R.string.dmAndroidPortal, R.string.dmAndroidPotion1, R.string.dmAndroidPotion2, R.string.dmAndroidPotion3, R.string.dmAndroidPotion4,
                R.string.dmAndroidMenu, R.string.dmAndroidMap, R.string.dmAndroidMove, R.string.dmAndroidSwap, R.string.dmAndroidDifficulty};
            int[] navIds = {R.string.dmConfirm, R.string.dmAndroidBack, R.string.dmAndroidAction, R.string.dmAndroidBind, R.string.dmAndroidNext};
            String[] gameLabels = new String[gameIds.length], navLabels = new String[navIds.length];
            for (int i = 0; i < gameIds.length; i++) gameLabels[i] = getString(gameIds[i]);
            for (int i = 0; i < navIds.length; i++) navLabels[i] = getString(navIds[i]);
            touchOverlay = new TouchOverlay(this, gameLabels, navLabels);
            touchOverlay.setOnApplyWindowInsetsListener((v, insets) -> {
                android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.displayCutout());
                touchOverlay.setSafeInsets(safe.left, safe.right, safe.bottom);
                return insets;
            });
            if (BuildConfig.DEBUG && getIntent().getBooleanExtra("dmNoHud", false)) touchOverlay.setDebugNoHud(true);   // 调试包：量游戏自己的 HUD 时不画自绘层 | debug builds: no overlay drawing while measuring the game's own HUD
            gameFrame.addView(touchOverlay, new FrameLayout.LayoutParams(-1, -1));
            touchOverlay.requestApplyInsets();
        }
        if (!view.getHolder().getSurface().isValid() || view.getWidth() <= 0 || view.getHeight() <= 0) return;
        File ext = getExternalFilesDir(null);
        if (ext == null) { showStartError(getString(R.string.dmAndroidStorage)); return; }
        nativeSurface(view.getHolder().getSurface(), view.getWidth(), view.getHeight());
        updateRefresh();
        // 资源目录缺省 Download/S2X/D2R Mobile/data；没有这个目录、而早先的 Download/data 在时沿用早先的位置，已装好的手机不必搬数据。
        // 按目录判断而不按 .build.config：它也可以由导入的覆盖层提供，资源目录里就没有。
        // The resource directory defaults to Download/S2X/D2R Mobile/data; when that directory does not exist but the earlier
        // Download/data does, the earlier one is kept, so existing installs need not move their data. The test is the directory,
        // not .build.config: the imported overlay can provide that file, and then the resource directory has none.
        File data = new File(downloads, "S2X/D2R Mobile/data"), earlier = new File(downloads, "data");
        if (!data.isDirectory() && earlier.isDirectory()) data = earlier;
        String src = data.getAbsolutePath();
        int result = nativeStart(getFilesDir().getAbsolutePath(), ext.getAbsolutePath(), src, Locale.getDefault().toLanguageTag());
        if (result < 0) {
            int resource = switch (result) {
                case -2 -> R.string.dmAndroidMissingData;
                case -3 -> R.string.dmAndroidMissingModules;
                case -4 -> R.string.dmAndroidMissingShaders;
                case -5 -> R.string.dmAndroidMissingConfig;
                case -9 -> R.string.dmAndroidMissingOverlay;
                case -10 -> R.string.dmAndroidInvalidOverlay;
                default -> 0;
            };
            showStartError(resource == 0 ? getString(R.string.dmAndroidStartFailed, result) : getString(resource)); return;
        }
        started = true;
        applyPadMode(PadInput.isPadMode());
        if (camSensor != null) camSensor.start(nativeCamSensor());
        // 越肩镜头的左右转按钮：盖在触屏层上面，只收按钮上的触摸。镜头可以在局中从「自定义」页打开（dm_cam.c dm_cam_mode），所以总是加上：
        // 镜头没在出图时按钮不画、触摸全部漏给下面的触屏层（nativeCamActive，每帧问一次）
        // Turn buttons of the over-the-shoulder camera: above the touch layer, taking only touches on the buttons. The camera can be turned on mid-game from
        // the Custom page (dm_cam.c dm_cam_mode), so they are always added: while the camera is not drawing the buttons are not drawn and every touch falls
        // through to the touch layer below (nativeCamActive, asked every frame)
        // 画面外的怪的箭头与地图上的朝向箭头放在按钮下面一层：只画不收触摸，镜头关着时什么都不画
        // The off-picture monster arrows and the map heading arrow go one layer below the buttons: drawing only, no touches, nothing drawn while the camera is off
        if (camButtons == null) {
            camThreats = new CamThreats(this); gameFrame.addView(camThreats, new FrameLayout.LayoutParams(-1, -1));
            camButtons = new CamButtons(this); gameFrame.addView(camButtons, new FrameLayout.LayoutParams(-1, -1)); }
        if (camButtons != null) camButtons.start();
        if (camThreats != null) camThreats.start();
        nativeFocus(3);
    }

    private void showStartError(String message) {
        TextView t = new TextView(this);
        t.setText(message); t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        setContentView(t);
    }

    // 原生线程只投递主线程工作，不持有 Activity，也不直接操作视图。
    // Native threads only post UI work; they neither retain the Activity nor touch views.
    private static void showNativeFailure(int code) {
        bootFailure = code;
        DmActivity activity = current.get();
        if (activity != null) activity.runOnUiThread(() -> {
            if (!activity.isDestroyed()) activity.showBootFailure();
        });
    }

    private void showBootFailure() {
        releaseTouches();
        showStartError(getString(bootFailure == -6 ? R.string.dmAndroidGraphicsFailed : R.string.dmAndroidHostStopped));
    }

    private static void showNativeInput(long id, String title, String sub, String guide, String initial,
                                        int maxChars, int nameRule, int numericMax) {
        DmActivity activity = current.get();
        if (activity == null) { nativeTextResult(id, false, ""); return; }
        activity.runOnUiThread(() -> activity.openInput(id, title, sub, guide, initial, maxChars, nameRule != 0, numericMax));
    }

    private void openInput(long id, String title, String sub, String guide, String initial,
                           int maxChars, boolean nameRule, int numericMax) {
        if (!resumed || isFinishing() || isDestroyed() || inputDialog != null) { nativeTextResult(id, false, ""); return; }
        releaseTouches();
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setInputType(numericMax >= 0 ? InputType.TYPE_CLASS_NUMBER : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setHint(nameRule ? getString(R.string.dmNameBlankRandom) : guide);
        input.setText(TextInputRules.normalize(initial, maxChars, nameRule, numericMax));
        input.setSelection(input.length());
        String message = sub + (sub.isEmpty() || guide.isEmpty() ? "" : "\n") + guide;
        if (nameRule) message += (message.isEmpty() ? "" : "\n") + getString(R.string.dmNameRule);
        if (numericMax >= 0) message = getString(R.string.dmAndroidQuantityRange, numericMax);
        if (title.isEmpty()) title = getString(nameRule ? R.string.dmNameTitle : numericMax >= 0 ? R.string.dmQtyTitle : R.string.dmInputTitle);
        final boolean[] finished = {false};
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(title).setMessage(message).setView(input)
            .setPositiveButton(R.string.dmConfirm, null).setNegativeButton(R.string.dmCancel, (d, which) -> {})
            .create();
        inputDialog = dialog;
        dialog.setOnDismissListener(d -> {
            if (inputDialog == dialog) inputDialog = null;
            if (!finished[0]) { finished[0] = true; nativeTextResult(id, false, ""); }
            if (resumed) immersive();
        });
        dialog.setOnShowListener(d -> {
            Runnable check = () -> {
                boolean composing = BaseInputConnection.getComposingSpanStart(input.getText()) >= 0;
                int error = nameRule ? TextInputRules.nameError(input.getText().toString()) : 0;
                input.setError(composing ? getString(R.string.dmNamePickIme) : error == 1 ? getString(R.string.dmNameTooShort) : error == 2 ? getString(R.string.dmNameEdge) : null);
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(!composing && error == 0);
            };
            Runnable settle = () -> {
                Editable text = input.getText();
                if (BaseInputConnection.getComposingSpanStart(text) < 0) {
                    String normalized = TextInputRules.normalize(text.toString(), maxChars, nameRule, numericMax);
                    if (!normalized.contentEquals(text)) { input.setText(normalized); input.setSelection(input.length()); return; }
                }
                check.run();
            };
            // 输入法结束组字（点候选、按完成）时常常只去掉组字标记、文字不变：TextWatcher 收不到，「确定」一直灰、提示停在「先选完字」。
            // 组字状态一变就和文字变化一样处理（规整 + 重判）：SpanWatcher 挂在文字上，Editable.Factory 让每次 setText 换上的新文字也挂着它。
            // When the IME finishes composing (picking a candidate, Done) it often only removes the composing span and keeps the text: TextWatcher is not
            // called, so OK stays disabled with "finish choosing characters". A change of composing state is handled like a text change (normalize + check):
            // a SpanWatcher sits on the text, and an Editable.Factory attaches it to the new text after every setText.
            SpanWatcher composingWatch = new SpanWatcher() {
                boolean composing = BaseInputConnection.getComposingSpanStart(input.getText()) >= 0;
                private void changed(Spannable text) {
                    boolean now = BaseInputConnection.getComposingSpanStart(text) >= 0;
                    if (now != composing) { composing = now; input.post(settle); }
                }
                public void onSpanAdded(Spannable text, Object what, int start, int end) { changed(text); }
                public void onSpanRemoved(Spannable text, Object what, int start, int end) { changed(text); }
                public void onSpanChanged(Spannable text, Object what, int ostart, int oend, int nstart, int nend) { changed(text); }
            };
            input.setEditableFactory(new Editable.Factory() {
                @Override public Editable newEditable(CharSequence source) {
                    Editable e = super.newEditable(source);
                    e.setSpan(composingWatch, 0, e.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
                    return e;
                }
            });
            input.getText().setSpan(composingWatch, 0, input.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
            input.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                public void onTextChanged(CharSequence s, int start, int before, int count) {}
                public void afterTextChanged(Editable text) { settle.run(); }
            });
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                finished[0] = true;
                nativeTextResult(id, true, input.getText().toString());
                dialog.dismiss();
            });
            check.run();
            input.requestFocus();
            if (dialog.getWindow() != null) dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        });
        dialog.show();
    }

    private void releaseTouches() {
        if (touchOverlay != null) touchOverlay.cancelTouches();
        if (started) nativeTouch(0, new int[0], new float[0], new float[0]);
    }

    private void updateRefresh() {
        if (libraryLoaded) nativeRefresh(view.getDisplay() == null ? 0 : view.getDisplay().getRefreshRate());
    }
    @Override public void onDisplayAdded(int id) { updateRefresh(); }
    @Override public void onDisplayRemoved(int id) { updateRefresh(); }
    @Override public void onDisplayChanged(int id) { updateRefresh(); }
    @Override protected void onResume() { super.onResume(); resumed = true; immersive(); startHost(); updateRefresh(); if (started) { nativeFocus(3); if (touchOverlay != null && bootFailure == 0) applyPadMode(PadInput.isPadMode()); if (camSensor != null) camSensor.start(nativeCamSensor()); if (camButtons != null) camButtons.start(); if (camThreats != null) camThreats.start(); } }
    @Override protected void onPause() { resumed = false; if (camSensor != null) camSensor.stop(); if (camButtons != null) camButtons.stop(); if (camThreats != null) camThreats.stop(); if (inputDialog != null) inputDialog.dismiss(); releaseTouches(); PadInput.reset(); if (touchOverlay != null) touchOverlay.setInputEnabled(false); if (started) nativeFocus(0); super.onPause(); }
    @Override protected void onStart() { super.onStart(); if (started) nativeFocus(2); }
    @Override protected void onStop() { if (started) nativeFocus(1); com.xiaoboluo.d2r.widget.HeroWidgetProvider.requestUpdate(this); super.onStop(); }
    @Override protected void onDestroy() { if (displays != null) displays.unregisterDisplayListener(this); if (inputDialog != null) inputDialog.dismiss(); if (current.get() == this) current.clear(); super.onDestroy(); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersive(); else releaseTouches(); }

    @Override public void surfaceCreated(SurfaceHolder h) {}
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {
        if (libraryLoaded) nativeSurface(h.getSurface(), w, ht);
        updateRefresh();
        startHost();
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) { releaseTouches(); if (libraryLoaded) nativeSurface(null, 0, 0); }

}
