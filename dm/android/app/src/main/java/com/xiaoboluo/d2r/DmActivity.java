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
import android.hardware.input.InputManager;
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
import android.util.SparseArray;
import android.view.InputDevice;
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
import java.util.Locale;

public class DmActivity extends Activity implements SurfaceHolder.Callback, DisplayManager.DisplayListener, InputManager.InputDeviceListener {
    static native int nativeStart(String filesDir, String extDir, String romfsSrc, String lang);
    static native void nativeSurface(Object surface, int w, int h);
    static native void nativeRefresh(float hz);
    static native void nativeTouch(int n, int[] ids, float[] xs, float[] ys);
    static native void nativeFocus(int ev);
    static native void nativeTextResult(long id, boolean accepted, String text);
    // 一只手柄的现值（电平）整只送原生层（gamepad_android.c）：deviceId = InputDevice 的 id；kind / buttons 见下面的常量
    // One controller's current values (levels) are handed over whole (gamepad_android.c): deviceId = the InputDevice id; kind / buttons as in the constants below
    static native void nativePad(int deviceId, int kind, int buttons, float lx, float ly, float rx, float ry);
    static native void nativePadGone(int deviceId);

    private static WeakReference<DmActivity> current = new WeakReference<>(null);
    private static volatile int bootFailure;
    private AlertDialog inputDialog;

    private SurfaceView view;
    private FrameLayout gameFrame;
    private TouchOverlay touchOverlay;
    private boolean started;
    private boolean importing;
    private boolean libraryLoaded;
    private boolean resumed;
    private DisplayManager displays;
    private InputManager inputManager;
    private final SparseArray<PadState> pads = new SparseArray<>();

    // ---- 手柄（iOS GameController 那层的安卓对接；合并 / 模式 / 键帽逻辑都在原生 gamepad_android.c）----
    // ---- Controllers (the Android counterpart of iOS's GameController layer; merging / modes / glyphs live in native gamepad_android.c) ----
    // 通用键位：与 gamepad_android.c 的 GPB_* 同一组数字。面键是「按位置」的（下 = BUTTON_A、右 = BUTTON_B、左 = BUTTON_X、上 = BUTTON_Y），
    // 位置 → 游戏键的排法按设备类型在原生层做。
    // Generic bits: the same numbers as GPB_* in gamepad_android.c. Face buttons are positional (bottom = BUTTON_A, right = BUTTON_B, left =
    // BUTTON_X, top = BUTTON_Y); how positions map to game buttons is decided per device kind in the native layer.
    private static final int GPB_A = 1, GPB_B = 2, GPB_X = 4, GPB_Y = 8, GPB_L1 = 16, GPB_R1 = 32, GPB_L2 = 64, GPB_R2 = 128,
        GPB_LS = 256, GPB_RS = 512, GPB_START = 1024, GPB_BACK = 2048, GPB_DL = 4096, GPB_DU = 8192, GPB_DR = 16384, GPB_DD = 32768;
    private static final int KIND_OTHER = 0, KIND_NINTENDO = 1, KIND_XBOX = 2, KIND_PS = 3;

    /** 一只手柄的现值：按键电平 + 四根轴（±1，上为正），有变化才送原生层。 | One controller's current values: button levels + four axes (±1, up positive), handed over only on change. */
    private static final class PadState {
        int kind, buttons, sentButtons = -1;
        float lx, ly, rx, ry, sentLx, sentLy, sentRx, sentRy;
        boolean sent;
        void setBit(int bit, boolean down) { if (down) buttons |= bit; else buttons &= ~bit; }
        void publish(int id) {
            if (sent && buttons == sentButtons && lx == sentLx && ly == sentLy && rx == sentRx && ry == sentRy) return;
            DmActivity.nativePad(id, kind, buttons, lx, ly, rx, ry);
            sentButtons = buttons; sentLx = lx; sentLy = ly; sentRx = rx; sentRy = ry; sent = true;
        }
    }

    /** 手柄键（按钮 / 十字键 / 开始 / 选择）：返回通用键位，0 = 不是手柄键。BACK / MENU 只认真实手柄设备（见 dispatchKeyEvent），不算注入。 | A controller key (buttons / D-pad / start / select): returns the generic bit, 0 = not a controller key. BACK / MENU count only from real controller devices (see dispatchKeyEvent), never from injection. */
    private static int padKeyBit(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return GPB_A;
            case KeyEvent.KEYCODE_BUTTON_B: return GPB_B;
            case KeyEvent.KEYCODE_BUTTON_X: return GPB_X;
            case KeyEvent.KEYCODE_BUTTON_Y: return GPB_Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return GPB_L1;
            case KeyEvent.KEYCODE_BUTTON_R1: return GPB_R1;
            case KeyEvent.KEYCODE_BUTTON_L2: return GPB_L2;
            case KeyEvent.KEYCODE_BUTTON_R2: return GPB_R2;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return GPB_LS;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return GPB_RS;
            case KeyEvent.KEYCODE_BUTTON_START: return GPB_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return GPB_BACK;
            case KeyEvent.KEYCODE_DPAD_LEFT: return GPB_DL;
            case KeyEvent.KEYCODE_DPAD_UP: return GPB_DU;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return GPB_DR;
            case KeyEvent.KEYCODE_DPAD_DOWN: return GPB_DD;
            case KeyEvent.KEYCODE_MENU: return GPB_START;
            case KeyEvent.KEYCODE_BACK: return GPB_BACK;
            default: return 0;
        }
    }

    /** 手柄类型（面键排法与键帽样式，原生层用）：vendor id 认不出的再用名字认。 | The controller kind (face layout and glyph style, used by the native layer): by vendor id first, then by name. */
    private static int deviceKind(InputDevice device) {
        if (device == null) return KIND_OTHER;
        int vendor = device.getVendorId();
        if (vendor == 0x054c) return KIND_PS;
        if (vendor == 0x045e) return KIND_XBOX;
        if (vendor == 0x057e) return KIND_NINTENDO;
        String name = device.getName() == null ? "" : device.getName().toLowerCase(Locale.ROOT);
        if (name.contains("xbox")) return KIND_XBOX;
        if (name.contains("dualsense") || name.contains("dualshock")) return KIND_PS;
        if (name.contains("switch") || name.contains("nintendo") || name.contains("pro controller")) return KIND_NINTENDO;
        return KIND_OTHER;
    }

    private PadState padOf(int id, InputDevice device) {
        PadState pad = pads.get(id);
        if (pad == null) { pad = new PadState(); pad.kind = deviceKind(device); pads.put(id, pad); }
        return pad;
    }

    // 手柄按键：按设备记电平，整只手柄现值送原生层。手柄上重复的 DOWN（按住不放）不改电平，publish 里比过就不再送。
    // 来源只认真实手柄（SOURCE_GAMEPAD / SOURCE_JOYSTICK）；没有设备的 BUTTON_*/十字键 = adb `input keyevent` 注入（真设备都带
    // deviceId ≥ 0），同 iOS dm_gamepad_inject 的测试路。BACK / MENU 不走注入：手势返回也长这样，吃了它返回手势就失灵。
    // Controller buttons: levels per device, the whole controller handed to the native layer. Repeated DOWNs (a held button) do not change the
    // level, and publish already compares before sending. Only real controllers are accepted (SOURCE_GAMEPAD / SOURCE_JOYSTICK); BUTTON_* / D-pad
    // keys without a device are adb `input keyevent` injection (real devices always have deviceId >= 0), the same test path as iOS dm_gamepad_inject.
    // BACK / MENU never come from injection: the back gesture looks exactly like that, and swallowing it would break the gesture.
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (started) {
            int bit = padKeyBit(event.getKeyCode());
            InputDevice device = event.getDevice();
            boolean fromPad = device != null && (device.getSources() & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) != 0;
            boolean injected = (device == null || event.getDeviceId() <= 0)
                && event.getKeyCode() != KeyEvent.KEYCODE_BACK && event.getKeyCode() != KeyEvent.KEYCODE_MENU;
            if (bit != 0 && (fromPad || injected) && (event.getAction() == KeyEvent.ACTION_DOWN || event.getAction() == KeyEvent.ACTION_UP)) {
                PadState pad = padOf(event.getDeviceId(), device);
                pad.setBit(bit, event.getAction() == KeyEvent.ACTION_DOWN);
                pad.publish(event.getDeviceId());
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    // 摇杆 / 十字键帽 / 扳机轴：与 iOS 相反安卓「上为负」，这里翻成正，原生层照 GameController 的值乘 32767。模拟扳机轴过半算按下，
    // 与数字扳机的键位取或（不少手柄两样都报）。
    // Sticks / hat / trigger axes: Android is "up is negative", opposite to iOS, so flip to positive here; the native layer multiplies by 32767 as
    // GameController values. An analog trigger past half counts as pressed, ORed with the digital trigger's key (many pads report both).
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (started && event.getActionMasked() == MotionEvent.ACTION_MOVE
                && (event.getSource() & (InputDevice.SOURCE_JOYSTICK | InputDevice.SOURCE_GAMEPAD)) != 0) {
            PadState pad = padOf(event.getDeviceId(), event.getDevice());
            pad.lx = event.getAxisValue(MotionEvent.AXIS_X);
            pad.ly = -event.getAxisValue(MotionEvent.AXIS_Y);
            pad.rx = event.getAxisValue(MotionEvent.AXIS_Z);
            pad.ry = -event.getAxisValue(MotionEvent.AXIS_RZ);
            float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X), hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            pad.setBit(GPB_DL, hatX < -0.5f); pad.setBit(GPB_DR, hatX > 0.5f);
            pad.setBit(GPB_DU, hatY < -0.5f); pad.setBit(GPB_DD, hatY > 0.5f);
            pad.setBit(GPB_L2, event.getAxisValue(MotionEvent.AXIS_LTRIGGER) > 0.5f);
            pad.setBit(GPB_R2, event.getAxisValue(MotionEvent.AXIS_RTRIGGER) > 0.5f);
            pad.publish(event.getDeviceId());
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    private static String kindName(int kind) {
        return kind == KIND_XBOX ? "Xbox" : kind == KIND_PS ? "PlayStation" : kind == KIND_NINTENDO ? "Nintendo" : "other";
    }

    @Override public void onInputDeviceAdded(int id) {
        InputDevice device = InputDevice.getDevice(id);
        if (device != null && (device.getSources() & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) != 0)
            android.util.Log.i("dm", String.format(Locale.ROOT, "手柄：连上 %s（vendor %04x product %04x）→ %s",
                device.getName(), device.getVendorId(), device.getProductId(), kindName(deviceKind(device))));
    }
    @Override public void onInputDeviceRemoved(int id) {
        if (pads.indexOfKey(id) >= 0) { pads.remove(id); if (started) nativePadGone(id); android.util.Log.i("dm", "手柄：断开 device " + id); }
    }
    @Override public void onInputDeviceChanged(int id) { pads.remove(id); }

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
        inputManager = getSystemService(InputManager.class);
        if (inputManager != null) inputManager.registerInputDeviceListener(this, null);
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
        File part = new File(runtime, "import.part");
        deleteTree(part);
        long sum = 0;
        for (String item : IMPORT_ITEMS) sum += treeSize(new File(source, item));
        final long total = sum;
        long[] done = {0}; int[] shown = {-1};
        byte[] buffer = new byte[1 << 20];
        for (String item : IMPORT_ITEMS) copyTree(new File(source, item), new File(part, item), buffer, bytes -> {
            done[0] += bytes;
            int p = (int) (done[0] * 100 / Math.max(1, total));
            if (p != shown[0]) { shown[0] = p; progress.percent(p); }
        });
        for (String item : IMPORT_ITEMS) {
            File from = new File(part, item), to = new File(runtime, item);
            deleteTree(to);
            if (!from.renameTo(to)) throw new IOException("rename " + item);
        }
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

    private void startHost() {
        if (bootFailure != 0) { showBootFailure(); return; }
        if (started || importing || !resumed || !ensureFileAccess()) return;
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
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
        touchOverlay.setInputEnabled(true);
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
    @Override protected void onResume() { super.onResume(); resumed = true; immersive(); startHost(); updateRefresh(); if (started) { nativeFocus(3); if (touchOverlay != null && bootFailure == 0) touchOverlay.setInputEnabled(true); } }
    @Override protected void onPause() { resumed = false; if (inputDialog != null) inputDialog.dismiss(); releaseTouches(); if (touchOverlay != null) touchOverlay.setInputEnabled(false); if (started) nativeFocus(0); super.onPause(); }
    @Override protected void onStart() { super.onStart(); if (started) nativeFocus(2); }
    @Override protected void onStop() { if (started) nativeFocus(1); com.xiaoboluo.d2r.widget.HeroWidgetProvider.requestUpdate(this); super.onStop(); }
    @Override protected void onDestroy() { if (displays != null) displays.unregisterDisplayListener(this); if (inputManager != null) inputManager.unregisterInputDeviceListener(this); if (inputDialog != null) inputDialog.dismiss(); if (current.get() == this) current.clear(); super.onDestroy(); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersive(); else releaseTouches(); }

    @Override public void surfaceCreated(SurfaceHolder h) {}
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {
        if (libraryLoaded) nativeSurface(h.getSurface(), w, ht);
        updateRefresh();
        startHost();
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) { releaseTouches(); if (libraryLoaded) nativeSurface(null, 0, 0); }

}
