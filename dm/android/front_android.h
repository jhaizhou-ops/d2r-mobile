#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* 真全屏（同 iOS front_ios.m / 鸿蒙 front_ohos.c）：屏幕比 16:9 窄时打开——局内按屏幕原生比例出图、3D 铺满整屏，
   界面按 16:9 由宿主摆好并贴底（dm_uiscreen.c，图形层把界面那一趟绘制下移）；前端各页与读取画面仍是 16:9 居中留边。
   屏比 16:9 宽（普通手机）时不开，行为与原来相同（同高度放宽到屏幕比例）。
   True fullscreen (as iOS front_ios.m / HarmonyOS front_ohos.c): enabled when the screen is narrower than 16:9 -- in a run the
   picture is output at the screen's native aspect with the 3D filling the screen, and the host lays the UI out at 16:9 anchored
   to the bottom (dm_uiscreen.c; the graphics layer shifts the UI draw pass down); front-end pages and the loading screen stay
   16:9 centered with margins. Screens wider than 16:9 (ordinary phones) keep the old behaviour (widen at the same height).
   任意线程可读。 | Readable from any thread. */
int front_android_full(void);
/* 当前窗口（SurfaceView）像素尺寸；收回画面后保留上一次的尺寸，还没报过尺寸时返回 0。
   呈现端（vulkan_present.cpp）用它判断「画面比例与窗口比例一致」——不一致时缓冲取窗口尺寸、由呈现端的留边 blit 摆位置。
   Current window (SurfaceView) pixel size; the last size is kept after the surface is taken back, and 0 is returned before any size
   was reported. The present path (vulkan_present.cpp) uses it to tell "the picture aspect matches the window": when they differ the
   buffer takes the window size and the present path's letterbox blit places the picture. */
int front_android_window_size(int *w, int *h);
/* 游戏视图矩形（px，窗口坐标系，out = {x, y, w, h}）：游戏界面、自绘 HUD / 控件、界面触点都在它里面。
   真全屏局内 = 贴底的界面矩形；真全屏局外（前端页、读取画面）= 窗口里居中的 16:9；没开真全屏 = 与画面同比例的那块
   （比例差 < 1% 就是整个窗口）。与 front_ios.m 的 letterbox_rect、front_ohos_view_rect 同一条规则。
   The game view rectangle (px, window coordinates, out = {x, y, w, h}): the game UI, the custom-drawn HUD / controls and UI touches
   all live inside it. True fullscreen in a run = the bottom-anchored UI rectangle; true fullscreen out of a run (front-end pages,
   loading screen) = the centered 16:9 block in the window; not fullscreen = the block matching the picture aspect (a difference
   below 1% means the whole window). Same rule as front_ios.m's letterbox_rect and front_ohos_view_rect.
   任意线程可调（UI 线程每帧调一次）。 | Callable from any thread (the UI thread calls it once per frame). */
void front_android_view_rect(int win_w, int win_h, int out[4]);
#ifdef __cplusplus
}
#endif
