// 越肩 / 第一人称镜头（实验，缺省关）：输入换算与两个线程之间的共享状态。设计与推导见 docs/over-the-shoulder-camera.zh-CN.md。
// 画面按透视镜头画（dm/gfx/nvn/nvn_mtl.m cam_build，只有 Metal 后端），游戏却只认原版正交画面上的点和方向。做法：不碰游戏，输入在喂给原版前换一次——
//   透视画面上的点 → 打一条射线与某个平面求交 → 按原版正交视图投影投回原版画面上的点。两份矩阵由画面线程每帧发布（dm_cam_publish）。
// · 摇杆 / 瞄准拖拽（dm_cam_remap_stick，取手柄处 hle_hid.c 调）：原版把摇杆转 45° 当世界方向，画面上人走的方向 = (摇杆 x, 摇杆 y / 2)（等距 2:1）。
//   把这个画面方向按参照镜头的水平朝向转成世界方向（上 = 镜头正前），再投回原版画面方向、按 2:1 换回摇杆。推上 = 朝镜头前方走。
// · 点单位（dm_world.c w_hit）：人有高度，点在身上的射线落地会落到人身后。按候选逐个换：射线交「过该单位脚底、正对射线的竖直面」，
//   交点投回原版画面，再用原版那套身体框 / 菱形 / 选择框判命中（dm_cam_tap_for_unit）。
// 开关：环境变量 DM_CAM=1 越肩、=2 第一人称（摇杆相对视线前后走、左右平移，不自动跟随；视线由右半屏拖动 / 转向键 / 体感转）；不设 = 关。关着或后端没发布矩阵（安卓 / 鸿蒙的 Vulkan 后端）时这里全部原样返回。
// Over-the-shoulder / first-person camera (experimental, off by default): input conversion and the state shared between the two threads. Design and derivation: docs/over-the-shoulder-camera.md.
// The picture is drawn with a perspective camera (dm/gfx/nvn/nvn_mtl.m cam_build, Metal backend only), while the game only understands points and directions on the original orthographic picture. Approach: leave the game alone and convert input once before it reaches the game --
//   a point on the perspective picture -> cast a ray and intersect it with a plane -> project back onto the original picture with the original orthographic view-projection. Both matrix sets are published every frame by the render thread (dm_cam_publish).
// - Sticks / aim drag (dm_cam_remap_stick, called at the controller poll in hle_hid.c): the game turns the stick by 45° into a world direction; on screen the character walks along (stick x, stick y / 2) (isometric 2:1).
//   That screen direction is rotated into a world direction by the reference camera's horizontal heading (up = straight ahead of the camera), projected back to an original-picture direction and turned back into a stick value via 2:1. Pushing up = walk towards where the camera looks.
// - Tapping a unit (dm_world.c w_hit): units have height, so a ray through a body lands on the ground behind them. Each candidate is handled on its own: intersect the ray with the vertical plane through that unit's feet facing the ray,
//   project the hit point back onto the original picture, then test it with the original body box / footprint diamond / selection box (dm_cam_tap_for_unit).
// Switch: environment variable DM_CAM=1 over-the-shoulder, =2 first person (the stick walks and strafes relative to the view, no follow; the view turns by dragging on the right half / the turn buttons / sensors); unset = off. When off, or when the backend publishes no matrices (the Vulkan backend on Android / HarmonyOS), everything here returns unchanged.
#include "dm.h"
#include "dm_cursor.h"
#include "dm_uiscreen.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 档：自定义页「镜头视角」（dm_custom.c 键 cam，读盘与点「应用」时调 dm_cam_set_mode）随时可切；环境变量 DM_CAM 设了就以它为准、页上改不动（测试用）
// Mode: the custom page's "Camera view" (dm_custom.c key cam, which calls dm_cam_set_mode on load and on Apply) can switch at any time; when the environment
// variable DM_CAM is set it wins and the page cannot change it (for tests)
static _Atomic int g_mode, g_mode_env = -1;
int dm_cam_mode(void) {
    int e = atomic_load(&g_mode_env);
    if (e < 0) { const char *s = getenv("DM_CAM"); e = s ? atoi(s) : 0x100; atomic_store(&g_mode_env, e); if (e != 0x100) LOG("DM_CAM=%d：镜头档按环境变量定（1 越肩 / 2 第一人称），自定义页改不动", e); }
    return e != 0x100 ? e : atomic_load(&g_mode); }
// 越肩档的操作风格（试验中，环境变量 DM_CAM_STYLE，缺省 0）：0 = 镜头朝摇杆方向追（xbg_cam_follow），摇杆按一次推下时的镜头定方向；
// 1 = 原神式：镜头被人物拖着走（xbg_cam_drag）、不追摇杆，摇杆始终按此刻的镜头定方向，右半屏左右拖转镜头（touch_android.c 给触屏层 cam_orbit）
// Over-the-shoulder control style (on trial, environment variable DM_CAM_STYLE, default 0): 0 = the camera chases the stick direction (xbg_cam_follow), and
// the stick takes its directions from the camera at the start of each push; 1 = Genshin style: the camera is dragged along by the character
// (xbg_cam_drag) instead of chasing the stick, the stick always takes its directions from the camera of the moment, and a horizontal drag on the right
// half turns the camera (touch_android.c gives the touch layer cam_orbit)
// 缺省：Android（Vulkan 后端，xbg_game.cpp 已接原神式）= 1，越肩档就是原神式（用户的决定，2026-10-29）；其余（iOS / Mac 的 Metal 后端还是
// xbg_cam_follow）= 0——那边要是开了 1，宿主的摇杆不再锁参照镜头而镜头还在追摇杆，人会原地打转。鸿蒙也先是 0：它的触屏层（touch_ohos.c）不填 cam_on /
// cam_orbit，原神式又拿掉了转镜头钮和体感，开了就没法转镜头；瞄准环、地图指针也是安卓的 Java 视图画的（维护者 #185 审阅）。DM_CAM_STYLE 写了就以它为准
// （0 = 原来的越肩，对比用）
// Default: Android (the Vulkan backend, xbg_game.cpp already does the Genshin style) = 1, so over-the-shoulder is the Genshin style (the user's decision,
// 2026-10-29); everywhere else (iOS / Mac, whose Metal backend still uses xbg_cam_follow) = 0 -- turning 1 on there would make the host's stick stop locking
// the reference camera while the camera still chases the stick, and the character would spin on the spot. HarmonyOS is 0 for now too: its touch layer
// (touch_ohos.c) does not fill cam_on / cam_orbit, and with the turn buttons and the sensor gone in this style there would be no way to turn the camera; the
// aim ring and the map pointer are drawn by Android's Java views as well (the maintainer's review of #185). DM_CAM_STYLE, when set, wins (0 = the old
// over-the-shoulder, for comparison)
#if defined(__ANDROID__)
#define DM_CAM_STYLE_DEFAULT 1
#else
#define DM_CAM_STYLE_DEFAULT 0
#endif
int dm_cam_style(void) { static _Atomic int st = -1; int v = atomic_load(&st); if (v < 0) { const char *e = getenv("DM_CAM_STYLE"); v = e ? atoi(e) : DM_CAM_STYLE_DEFAULT; atomic_store(&st, v); if (v) LOG("DM_CAM_STYLE=%d：越肩档用原神式操作（镜头被拖着走、摇杆按此刻镜头、右半屏左右拖转镜头）", v); } return v; }
void dm_cam_set_mode(int m) { if (m < 0 || m > 2) m = 0; if (atomic_exchange(&g_mode, m) != m) LOG("镜头视角 → %s（自定义页）%s", m == 1 ? "越肩" : m == 2 ? "第一人称" : "原版", atomic_load(&g_mode_env) >= 0 && atomic_load(&g_mode_env) != 0x100 ? "；环境变量 DM_CAM 设着，以它为准" : ""); }

// 画面线程发布的矩阵（行向量约定：裁剪 = [x y z 1] × M）：原版正交视图投影与逆、透视视图投影与逆、人物落地点 x / z；超过 0.5 秒没更新就当镜头没开
// Matrices published by the render thread (row-vector convention: clip = [x y z 1] * M): original orthographic view-projection and inverse, perspective view-projection and inverse, the character's ground point x / z; not refreshed for 0.5 s = camera treated as off
// ak = 这份矩阵那一帧原版镜头带的前移方向（dm_cam_ahead_pick 认出的，-1 = 没挪），见下面「原版画面往 3D 镜头前方挪」
// ak = the ahead direction the original camera carried in the frame of these matrices (as dm_cam_ahead_pick recognised it, -1 = none), see "Move the
// original picture ahead of the 3D camera" below
typedef struct { float vpo[16], ivpo[16], vpp[16], ivpp[16], pl[2], py; int ak; } cam_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static cam_t g_pub; static double g_pub_t;
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static _Atomic int g_ahead_found = -1, g_ahead_applied = -1;
void dm_cam_publish(const float vpo[16], const float ivpo[16], const float vpp[16], const float ivpp[16], float px, float pz) {
    pthread_mutex_lock(&g_mu); memcpy(g_pub.vpo, vpo, 64); memcpy(g_pub.ivpo, ivpo, 64); memcpy(g_pub.vpp, vpp, 64); memcpy(g_pub.ivpp, ivpp, 64);
    g_pub.pl[0] = px; g_pub.pl[1] = pz; g_pub.ak = atomic_load(&g_ahead_found); g_pub_t = now_s(); pthread_mutex_unlock(&g_mu); }
// 人物所在地面的高度（世界 y；画面线程每帧在 dm_cam_publish 之后给，xbg_cam.h xbg_cam_ground）。地面上画东西（瞄准箭头 dm_cam_aim）要它；
// 没给的后端（Metal）就是 0，同原来
// The height of the character's ground (world y; given by the render thread every frame after dm_cam_publish, xbg_cam.h xbg_cam_ground). Drawing on the
// ground (the aim arrow, dm_cam_aim) needs it; backends that do not give it (Metal) leave 0, as before
void dm_cam_publish_height(float py) { pthread_mutex_lock(&g_mu); g_pub.py = py; pthread_mutex_unlock(&g_mu); }

// 镜头避墙：游戏主线程每帧在碰撞格上量出人物脚下往 32 个方向离第一面墙多远（世界单位，dm_world.c w_cam_clear；方向 k 的角度 = k × 360° / 32，
// 从世界 +x 转向 +z），画面线程按镜头朝后的方向取相邻两个方向里近的那个（墙可能很薄，不插值）。超过 0.5 秒没更新（不在局里、镜头没开）= 不知道，返回 0。
// 顺带给出宿主按子格算的人物位置（世界单位），画面线程拿它和矩阵反推的落地点对一对，核「子格 → 世界」的换算
// Wall avoidance: every frame the game main thread measures on the collision grid how far the first wall is from the character's feet in 32 directions (world
// units, dm_world.c w_cam_clear; direction k is at k * 360 / 32 degrees, turning from world +x towards +z), and the render thread takes the nearer of the two
// directions next to the camera's backward direction (walls can be thin, so no interpolation). Not refreshed for 0.5 s (not in a game, camera off) = unknown,
// returns 0. It also gives the character's position computed by the host from sub-cells (world units), which the render thread compares with the ground point
// derived from the matrices, to check the sub-cell -> world conversion
enum { CAM_DIRS = 32 };
static float g_clear[CAM_DIRS], g_clear_p[2]; static double g_clear_t;
void dm_cam_set_clear(const float d[32], float px, float pz) {
    pthread_mutex_lock(&g_mu); memcpy(g_clear, d, sizeof g_clear); g_clear_p[0] = px; g_clear_p[1] = pz; g_clear_t = now_s(); pthread_mutex_unlock(&g_mu); }
int dm_cam_clearance(float bx, float bz, float *dist, float *px, float *pz) {
    pthread_mutex_lock(&g_mu); int ok = g_clear_t > 0 && now_s() - g_clear_t < 0.5;
    if (ok) { float a = atan2f(bz, bx) / 6.2831853f * CAM_DIRS; if (a < 0) a += CAM_DIRS; int i = (int)a % CAM_DIRS, j = (i + 1) % CAM_DIRS;
        // 原神式取两个方向里远的那个：细东西（树干、柱子、栅栏桩）一般只挡住一个方向，不该把镜头拉进来；真墙两个方向都挡（用户 2026-10-29：避墙太敏感）
        // The Genshin style takes the farther of the two directions: thin things (tree trunks, pillars, fence posts) usually block only one direction and
        // should not pull the camera in; a real wall blocks both (the user, 2026-10-29: the wall avoidance reacts too easily)
        const int far = dm_cam_style() == 1;
        *dist = (g_clear[i] < g_clear[j]) != far ? g_clear[i] : g_clear[j]; *px = g_clear_p[0]; *pz = g_clear_p[1]; }
    pthread_mutex_unlock(&g_mu); return ok; }
// 原版画面往 3D 镜头前方挪（DM_CAM_AHEAD = 挪动量占原版画面半深的比例，缺省 0.5：人物前方占画面 3/4、身后留 1/4，转身时身后也还有东西；0 = 关）：游戏按自己的正交画面决定载入、提交哪些东西，那个画面以人物为中心，
// 一半在 3D 镜头身后用不上，前方远处就是黑块。镜头每帧更新（main 0x89c370）把「镜头位置 = 人物 + 偏移」写进镜头对象（x20 +0x100..+0x108）之后紧接着
// 调 0x1628978（0x89ccc8），那个调用点换成 svc #0x8f（patch_nso.py，hle.h SVC_HOST_CAM_AHEAD）：宿主原样代调，再把镜头位置和偏移（x19 +0xc / +0x14，
// 第二个镜头随后按它摆）加上挪动量。挪动量只取 24 个方向（AHEAD_N）之一、带回差：画面线程每帧拿矩阵反推的人物点减去各个候选，和宿主读到的人物位置比，
// 认出这一帧带的是哪个（相邻两个方向差 2·S·sin(7.5°)，缩放 0.6、比例 0.5 时 S = 44、相差 11.5 个单位；宿主位置的滞后不到 2 个），扣掉它（dm_cam_ahead_pick）。
// 连续变化的挪动量认不出来：画面线程不知道某一帧原版用的是哪一个。画面线程 0.5 秒没来认（Metal 后端不认、镜头关了）就不挪
// Move the original picture ahead of the 3D camera (DM_CAM_AHEAD = the shift as a fraction of the original picture's half depth, default 0.5: 3/4 of the
// picture ahead of the character and 1/4 left behind, so there is still something behind when turning round; 0 = off): the game decides what to load and submit by its own
// orthographic picture, which is centred on the character, so half of it is behind the 3D camera and unused while the far ground ahead shows black patches.
// The per-frame camera update (main 0x89c370) writes "camera position = character + offset" into the camera object (x20 +0x100..+0x108) and right after
// calls 0x1628978 (0x89ccc8); that call site becomes svc #0x8f (patch_nso.py, hle.h SVC_HOST_CAM_AHEAD): the host forwards the call as is, then adds the
// shift to the camera position and to the offset (x19 +0xc / +0x14, which the second camera is placed by next). The shift is one of 24 directions
// (AHEAD_N), with hysteresis: every frame the render thread subtracts each candidate from the character point derived from the matrices, compares with the
// position the host reads, and so recognises which one the frame carries (two neighbouring directions are 2 * S * sin(7.5 degrees) apart: at zoom 0.6
// and fraction 0.5, S = 44 and 11.5 units; the host position lags by less than 2), and removes it (dm_cam_ahead_pick). A continuously varying shift could not be recognised: the render
// thread would not know which one the game used for a given frame. Without a pick from the render thread for 0.5 s (the Metal backend does not pick, the
// camera is off) nothing is moved
enum { AHEAD_N = 24 };
static _Atomic int g_ahead_k = -1, g_ahead_cm = -1;
static _Atomic long long g_ahead_seen_ms;
// 挪多远（世界单位）= 比例 × 原版画面半深。半深 = 正交画面高 H = 53.03·u，u = atan(0.03 / 缩放) / atan(0.03)（xbg_cam.h xbg_cam_ground），缩放 0.6 时 88；
// 非 0 时至少 30，相邻方向才分得开；不缩放（dm_world.c dm_cam_zoom_on）就不挪
// How far (world units) = fraction x the original picture's half depth. The half depth = the orthographic height H = 53.03 * u,
// u = atan(0.03 / zoom) / atan(0.03) (xbg_cam.h xbg_cam_ground), 88 at zoom 0.6; at least 30 when not 0, so neighbouring directions stay apart;
// no shift without the zoom (dm_world.c dm_cam_zoom_on)
static float ahead_len(void) {
    int v = atomic_load(&g_ahead_cm);
    if (v < 0) { float dm_cam_zoom_want(void); int dm_cam_zoom_on(void); const char *e = getenv("DM_CAM_AHEAD"); float f = e ? (float)atof(e) : 0.5f; if (!(f > 0) || !dm_cam_zoom_on()) f = 0; else if (f > 0.9f) f = 0.9f;
        float s = f * 0.75f * 70.7107f * atanf(0.03f / dm_cam_zoom_want()) / atanf(0.03f); if (s > 0 && s < 30) s = 30;
        v = (int)lroundf(s * 100); atomic_store(&g_ahead_cm, v); LOG("原版画面前移：比例 %.2f，挪 %.0f 个单位（%d 个方向）", f, s, AHEAD_N); }
    return v / 100.0f; }
static void ahead_vec(int k, float *x, float *z) { float s = ahead_len(), a = (float)k * (6.2831853f / AHEAD_N); *x = k < 0 ? 0 : s * cosf(a); *z = k < 0 ? 0 : s * sinf(a); }
// svc #0x8f：跑在游戏线程自己的栈上，只做几次读写，不记日志 | svc #0x8f: runs on the game thread's own stack, only a few loads and stores, no logging
void svc_cam_ahead(uint64_t *regs) {
    static uint8_t *B;
    if (!B) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) B = (uint8_t *)g_mod[i].base;
    regs[0] = ((uint64_t (*)(uint64_t))(B + 0x1628978))(regs[0]);
    int k = atomic_load(&g_ahead_k);
    uint8_t *ctl = (uint8_t *)(uintptr_t)regs[19], *cam = (uint8_t *)(uintptr_t)regs[20];
    if (k < 0 || !ctl || !cam || dm_cam_mode() <= 0 || (long long)(now_s() * 1000) - atomic_load(&g_ahead_seen_ms) > 500) { atomic_store(&g_ahead_applied, -1); return; }
    atomic_store(&g_ahead_applied, k);
    float x, z; ahead_vec(k, &x, &z);
    *(float *)(cam + 0x100) += x; *(float *)(cam + 0x108) += z; *(float *)(ctl + 0xc) += x; *(float *)(ctl + 0x14) += z; }
// 画面线程每帧调：(rx, rz) = 矩阵反推的人物点（含挪动），(hx, hz) = 宿主读到的人物位置（host_ok = 0 时没有，改和上一帧扣完的点比），(fx, fz) = 3D 镜头水平朝向。
// 返回这一帧带的挪动量（ox, oz；认不出就沿用上一次认出的），顺带按朝向定下以后各帧要挪的方向
// Called by the render thread every frame: (rx, rz) = the character point derived from the matrices (shift included), (hx, hz) = the position the host
// reads (absent when host_ok = 0; then compare with last frame's corrected point), (fx, fz) = the 3D camera's horizontal heading. Returns the shift this
// frame carries (ox, oz; when nothing matches, the last one recognised), and sets the direction to shift by in later frames from the heading
int dm_cam_ahead_pick(float rx, float rz, int host_ok, float hx, float hz, float fx, float fz, float *ox, float *oz) {
    static int found = -1, have_last, misses, matched; static float lx, lz;
    const float s = ahead_len(), step = 6.2831853f / AHEAD_N, tol = 0.45f * 2 * s * sinf(step / 2);
    if (s <= 0) found = -1;
    else if (host_ok || have_last) {
        const float tx = host_ok ? hx : lx, tz = host_ok ? hz : lz; int best = found; float bd = 1e30f;
        for (int c = -1; c < AHEAD_N; c++) { float vx, vz; ahead_vec(c, &vx, &vz); float d = hypotf(rx - vx - tx, rz - vz - tz); if (d < bd) { bd = d; best = c; } }
        if (bd < tol) { found = best; misses = 0; if (best >= 0 && !matched++) LOG("原版画面前移：认出第一帧挪动（方向 %d / %d）", best, AHEAD_N); }
        else if (++misses == 30) LOG("【黄】原版画面前移：连续 30 帧认不出挪动（最近 %.1f 个单位，宿主位置 %s），沿用方向 %d", bd, host_ok ? "有" : "没有", found); }
    float vx, vz; ahead_vec(found, &vx, &vz); *ox = vx; *oz = vz; lx = rx - vx; lz = rz - vz; have_last = 1; atomic_store(&g_ahead_found, found);
    if (s > 0 && fx * fx + fz * fz > 0.25f) {
        const float a = atan2f(fz, fx); int k = atomic_load(&g_ahead_k); const int kn = (((int)lroundf(a / step)) % AHEAD_N + AHEAD_N) % AHEAD_N;
        if (k < 0 || fabsf(remainderf(a - (float)k * step, 6.2831853f)) > 0.65f * step) atomic_store(&g_ahead_k, kn); }
    atomic_store(&g_ahead_seen_ms, (long long)(now_s() * 1000));
    return found >= 0; }
// 发布的原版矩阵是画面线程上一帧的，游戏这会儿的镜头可能已经换了前移方向（转镜头时每转 15° 换一档）：差 Δ = 现在的挪动 − 那一帧的挪动，
// 原版画面整体跟着挪了 Δ，于是 现在的视图投影 = T(−Δ) × 那一帧的，逆 = 那一帧的逆 × T(Δ)（行向量，T(v) = 末行为 (vx, 0, vz, 1) 的平移）。
// 点按换算、名字牌、全屏地图的人物十字都按游戏这会儿的镜头来，换档那一帧也不偏
// The published original matrices are from the render thread's last frame, and the game's camera may already carry another ahead direction (turning the
// camera steps it every 15 degrees): with delta = the shift now - the shift of that frame, the whole original picture has moved by delta, so the current
// view-projection = T(-delta) x that frame's, and its inverse = that frame's inverse x T(delta) (row vectors, T(v) = translation with last row (vx, 0, vz, 1)).
// Tap conversion, nameplates and the fullscreen map's player cross then follow the game's camera of the moment, even in the frame where the step changes
static int ahead_delta(int ak, float *dx, float *dz) {
    const int k = atomic_load(&g_ahead_applied); if (k == ak) return 0;
    float ax, az, bx, bz; ahead_vec(k, &ax, &az); ahead_vec(ak, &bx, &bz); *dx = ax - bx; *dz = az - bz; return 1; }
static void ahead_fix(float vpo[16], float ivpo[16], float dx, float dz) {
    for (int j = 0; j < 4; j++) vpo[12 + j] -= dx * vpo[j] + dz * vpo[8 + j];
    if (ivpo) for (int i = 0; i < 4; i++) { ivpo[i * 4] += ivpo[i * 4 + 3] * dx; ivpo[i * 4 + 2] += ivpo[i * 4 + 3] * dz; } }
static int cam_get(cam_t *c) {
    if (dm_cam_mode() <= 0) return 0;   // 两档都换算：第一人称以前只认越肩，摇杆一直按原版方向走，转过身就左右颠倒 | both modes: first person used to be left out, so its stick kept the original directions and swapped left and right once turned around
    pthread_mutex_lock(&g_mu); int ok = g_pub_t > 0 && now_s() - g_pub_t < 0.5; if (ok) *c = g_pub; pthread_mutex_unlock(&g_mu);
    float dx, dz; if (ok && ahead_delta(c->ak, &dx, &dz)) ahead_fix(c->vpo, c->ivpo, dx, dz);
    return ok; }

static void xf(const float *M, const float v[4], float o[4]) { for (int j = 0; j < 4; j++) o[j] = v[0] * M[j] + v[1] * M[4 + j] + v[2] * M[8 + j] + v[3] * M[12 + j]; }

// 跟着世界走的界面字（名字牌、血条、地上物品名、飘字）也按透视画面放（dm_uiscreen.c 在游戏的「世界点 → 屏幕」main+0x1237c60 入口换镜头副本）。
// 那个函数算 裁剪 = 投影（镜头 +0x80）× 视图 × 点，并且除以 w，所以透视矩阵也对。副本的投影换成 P' = P × 逆(原版视图投影) × 透视视图投影：
// 按行向量写，视图 × P' = 视图 × 投影 × 逆(视图 × 投影) × 透视 = 透视视图投影。p = 镜头自己的投影（与常量块 +208 相同），排布与这里的矩阵一致。
// 镜头开着（越肩或第一人称）并且画面线程 0.5 秒内发布过矩阵才返回 1；点按与测试量具的单位位置也走这个函数，所以那边拿到的也是透视画面上的坐标
// UI text that follows the world (nameplates, health bars, ground item names, floating text) is placed on the perspective picture too (dm_uiscreen.c swaps a
// copy of the camera at the entry of the game's "world point -> screen" main+0x1237c60). That function computes clip = projection (camera +0x80) x view x point
// and divides by w, so a perspective matrix works. The copy's projection becomes P' = P x inverse(original view-projection) x perspective view-projection:
// in row-vector terms, view x P' = view x projection x inverse(view x projection) x perspective = perspective view-projection. p = the camera's own projection
// (the same as the constant block's +208), laid out like the matrices here. Returns 1 only while the camera is on (over-the-shoulder or first person) and the render thread
// published matrices within 0.5 s; taps and the test tool's unit positions also go through that function, so they get perspective picture coordinates too
static void m4mul(const float *a, const float *b, float *o) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += (double)a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = (float)s; } }
// 跑在游戏线程自己的栈上（dm_uiscreen.c 的观测门），所以不拷整份 cam_t，锁里直接算
// Runs on the game thread's own stack (dm_uiscreen.c's observation gate), so it does not copy a whole cam_t and computes under the lock instead
int dm_cam_label_matrix(const float p[16], float out[16]) {
    if (dm_cam_mode() <= 0) return 0;
    float t[16]; pthread_mutex_lock(&g_mu); int ok = g_pub_t > 0 && now_s() - g_pub_t < 0.5;
    if (ok) { m4mul(p, g_pub.ivpo, t); float dx, dz; if (ahead_delta(g_pub.ak, &dx, &dz)) for (int i = 0; i < 4; i++) { t[i * 4] += t[i * 4 + 3] * dx; t[i * 4 + 2] += t[i * 4 + 3] * dz; } m4mul(t, g_pub.vpp, out); }
    pthread_mutex_unlock(&g_mu); return ok; }
// 归一化画面坐标（0..1，y 向下）↔ 裁剪空间（-1..1，y 向上） | normalized picture coordinates (0..1, y down) <-> clip space (-1..1, y up)
static void n2c(float nx, float ny, float *x, float *y) { *x = nx * 2 - 1; *y = 1 - ny * 2; }
static int proj(const float *VP, const float w[3], float *nx, float *ny) { float v[4] = { w[0], w[1], w[2], 1 }, o[4]; xf(VP, v, o); if (o[3] <= 1e-6f) return 0; *nx = (o[0] / o[3] + 1) / 2; *ny = (1 - o[1] / o[3]) / 2; return 1; }
// 透视画面点的射线：起点（近平面）与方向（反向 Z：近 = 1、远 = 0） | ray of a perspective picture point: origin (near plane) and direction (reversed Z: near = 1, far = 0)
static int ray(const cam_t *c, float nx, float ny, float o[3], float d[3]) {
    float x, y; n2c(nx, ny, &x, &y); float a[4] = { x, y, 1, 1 }, b[4] = { x, y, 0.001f, 1 }, p[4], q[4]; xf(c->ivpp, a, p); xf(c->ivpp, b, q);
    if (fabsf(p[3]) < 1e-9f || fabsf(q[3]) < 1e-9f) return 0;
    for (int i = 0; i < 3; i++) { o[i] = p[i] / p[3]; d[i] = q[i] / q[3] - o[i]; } return 1; }
// 原版正交画面点 → 地面 y = 0（正交的视线是平行的，两个深度取两点连线求交） | original orthographic picture point -> ground y = 0 (orthographic sight lines are parallel: intersect the line through two depths)
static int ortho_ground(const cam_t *c, float nx, float ny, float w[3]) {
    float x, y; n2c(nx, ny, &x, &y); float a[4] = { x, y, 0, 1 }, b[4] = { x, y, 1, 1 }, p[4], q[4]; xf(c->ivpo, a, p); xf(c->ivpo, b, q);
    float dy = q[1] / q[3] - p[1] / p[3]; if (fabsf(dy) < 1e-9f) return 0; float t = -(p[1] / p[3]) / dy;
    for (int i = 0; i < 3; i++) w[i] = p[i] / p[3] + t * (q[i] / q[3] - p[i] / p[3]); return 1; }

int dm_cam_on(void) { cam_t c; return cam_get(&c); }
// 进局满 1.5 秒（dm_uiscreen_ingame 连续为 1）才给地图朝向指针与画面外怪物箭头：读取画面撤掉后头一会儿 3D 场景还在载入，指针先冒出来很怪（OnePlus 13）。
// 每次读取画面（换区域、小站、传送门）都重新计时。前端主线程调（Android Choreographer、iOS 每帧）
// The map heading pointer and the off-picture monster arrows are given only once the run has been in game for 1.5 s (dm_uiscreen_ingame continuously 1): just
// after the loading screen goes away the 3D scene is still loading, and the pointer popping up first looks odd (OnePlus 13). Every loading screen (area
// change, waypoint, portal) restarts the count. Called on the front end's main thread (Android Choreographer, iOS every frame)
static int cam_settled(void) {
    static _Atomic long long since_ms = -1;
    if (!dm_uiscreen_ingame()) { atomic_store(&since_ms, -1); return 0; }
    long long t = (long long)(now_s() * 1000), s0 = atomic_load(&since_ms);
    if (s0 < 0) { atomic_store(&since_ms, t); return 0; }
    return t - s0 >= 1500; }
// 镜头的水平朝向（世界 x / z，单位向量）：小地图上画人物朝向用（front_ios.m mapHeadingSync）。镜头关着或矩阵过期返回 0
// The camera's horizontal heading (world x / z, unit vector): for the facing indicator on the minimap (front_ios.m mapHeadingSync). Returns 0 when the
// camera is off or its matrices are stale
// sx / sy = 全屏地图上人物十字的位置（归一化，y 向下）：游戏把全屏地图画成以人物为中心，十字总在画面正中（不能用透视画面上的位置）。
// 地面高低（原先按 (x, 0, z) 用原版矩阵投，下坡 12 个单位约差四分之一屏高）、缩小时原版镜头水平往后挪、原版画面往前挪（DM_CAM_AHEAD）都不影响它
// （OnePlus 13 实测：原版画面往前挪了 44 个单位，十字仍在正中，按原版矩阵投的点却跟着偏开）
// sx / sy = the player cross on the fullscreen map (normalized, y down): the game draws the fullscreen map centred on the character, so the cross is always
// at the centre of the picture (not at the position on the perspective picture). Neither the ground height (projecting (x, 0, z) with the original matrices
// used to be off by about a quarter of the screen height for a 12-unit drop), nor the original camera sitting further back when zoomed out, nor the
// original picture moved ahead (DM_CAM_AHEAD) changes that (measured on a OnePlus 13: with the original picture 44 units ahead the cross stayed at the
// centre, while the point projected with the original matrices moved off)
int dm_cam_heading(float *fx, float *fz, float *sx, float *sy) {
    cam_t c; float o[3], d[3]; if (!cam_settled() || !cam_get(&c) || !ray(&c, 0.5f, 0.5f, o, d)) return 0;
    float l = hypotf(d[0], d[2]); if (l < 1e-6f) return 0; *fx = d[0] / l; *fz = d[2] / l;
    *sx = 0.5f; *sy = 0.5f;
    return 1; }

// 怪物指示：游戏线程每拍交来玩家附近活着的怪（子格坐标与档位，diag.c cam_threats_tick），前端每帧问哪些不在画面里、该画在屏幕边上哪儿（dm_cam_threats）。
// 方向按镜头水平朝向算：正前方（画面外更远处）→ 上沿，左右 → 两侧，身后 → 下面两角（下沿中间是人和底栏，不放）。out 每条 5 个数：
// 屏幕 x、y（0..1，y 向下，已避开顶边与底栏）、箭头方向（弧度，0 = 朝上、顺时针为正）、紧迫度（0..1，越近越大）、档位（0 普通 / 1 精英 / 2 Boss）；按紧迫度取前 max 条
// Monster indicators: every tick the game thread hands over the living monsters near the player (sub-cell coordinates and tier, diag.c cam_threats_tick),
// and every frame the front end asks which are off the picture and where on the screen edge to draw them (dm_cam_threats). The direction follows the camera's
// horizontal heading: straight ahead (beyond the picture) -> the top edge, left and right -> the sides, behind -> the two bottom corners (the middle of the
// bottom edge holds the character and the bottom bar, so nothing goes there). Each entry of out has 5 numbers: screen x, y (0..1, y down, clear of the top
// edge and the bottom bar), arrow direction (radians, 0 = up, clockwise positive), urgency (0..1, larger when closer), tier (0 normal / 1 elite / 2 Boss);
// the max most urgent are returned
static float g_thr_xy[64]; static int g_thr_tier[32], g_thr_n; static double g_thr_t;
void dm_cam_set_threats(const float *xy, const int *tier, int n) {
    if (n < 0) n = 0; if (n > 32) n = 32;
    pthread_mutex_lock(&g_mu); g_thr_n = n; if (n) { memcpy(g_thr_xy, xy, sizeof(float) * 2 * n); memcpy(g_thr_tier, tier, sizeof(int) * n); } g_thr_t = now_s(); pthread_mutex_unlock(&g_mu); }
int dm_cam_threats(float *out, int max) {
    cam_t c; if (max <= 0 || !cam_settled() || !cam_get(&c)) return 0;
    static float sub = -1, range = -1;
    if (sub < 0) { const char *e = getenv("DM_CAM_SUB"); sub = e ? (float)atof(e) : 2.0f; e = getenv("DM_CAM_THREAT_RANGE"); range = e ? (float)atof(e) : 30; }
    float xy[64]; int tier[32], n;
    pthread_mutex_lock(&g_mu); n = g_thr_t > 0 && now_s() - g_thr_t < 0.5 ? g_thr_n : 0; memcpy(xy, g_thr_xy, sizeof xy); memcpy(tier, g_thr_tier, sizeof tier); pthread_mutex_unlock(&g_mu);
    float o[3], d[3]; if (!n || !ray(&c, 0.5f, 0.5f, o, d)) return 0;
    float fx = d[0], fz = d[2], fl = hypotf(fx, fz); if (fl < 1e-6f) return 0; fx /= fl; fz /= fl; float rx = -fz, rz = fx;
    // 允许的边框：左右 0.06–0.94，上 0.08，下 0.80（底栏之上）；身后超过下角的方向夹到下角
    // Allowed border: left / right 0.06-0.94, top 0.08, bottom 0.80 (above the bottom bar); directions behind beyond the bottom corners clamp to the corners
    const float L = 0.06f, R = 0.94f, T = 0.08f, B = 0.80f, corner = atan2f(R - 0.5f, B - 0.5f);
    int m = 0;
    for (int i = 0; i < n; i++) {
        float wx = xy[2 * i] * sub, wz = xy[2 * i + 1] * sub, v[4] = { wx, 3, wz, 1 }, p[4]; xf(c.vpp, v, p);
        if (p[3] > 0 && fabsf(p[0] / p[3]) < 0.92f && fabsf(p[1] / p[3]) < 0.92f) continue;
        float ex = wx - c.pl[0], ez = wz - c.pl[1], dist = hypotf(ex, ez), th = atan2f(ex * rx + ez * rz, ex * fx + ez * fz), sx, sy;
        if (fabsf(th) > 3.14159265f - corner) { sx = th > 0 ? R : L; sy = B; }
        else { float dx = sinf(th), dy = -cosf(th), t = 1e9f;
            if (dx > 1e-6f) t = fminf(t, (R - 0.5f) / dx); if (dx < -1e-6f) t = fminf(t, (L - 0.5f) / dx);
            if (dy > 1e-6f) t = fminf(t, (B - 0.5f) / dy); if (dy < -1e-6f) t = fminf(t, (T - 0.5f) / dy);
            sx = 0.5f + dx * t; sy = 0.5f + dy * t; }
        float urg = 1 - dist / (range * sub); if (urg < 0) urg = 0; if (urg > 1) urg = 1;
        int slot = m < max ? m++ : -1;
        if (slot < 0) { int low = 0; for (int k = 1; k < max; k++) if (out[k * 5 + 3] < out[low * 5 + 3]) low = k; if (out[low * 5 + 3] >= urg) continue; slot = low; }
        float *e = out + slot * 5; e[0] = sx; e[1] = sy; e[2] = th; e[3] = urg; e[4] = (float)tier[i];
    }
    return m;
}

// 地图上的朝向箭头：透视画面转过之后，地图（原版等距、不转）上看不出镜头朝哪边，这里给出玩家点上那支箭头该画在哪、朝哪。地图开着才给（dm_ui_map_geom）：
// · 角上的小地图（1）：玩家点在小地图框中心（dm_cursor.c ui_map_geom_pump），箭头按框高定大小；
// · 全屏地图（2）：游戏把地图画成以人物为中心，玩家点在画面正中（dm_cam_heading 的 sx / sy，那里有缘由）。
// 方向 = 镜头水平朝向（dm_cam_heading；越肩档推摇杆上 = 往这边走）按原版地图的等距换算：世界方向 (x, z) 在地图上 = ((x − z)×1.6, (x + z)×0.8) 像素（同 dm_map_goals 的换算）。
// out：中心 x、y（0..1，y 向下）、箭头方向（弧度，0 = 朝上、顺时针为正，同 dm_cam_threats）、尺度（屏高的比例：小地图 = 框高，全屏地图 0.5——那里地图放大一倍）
// Heading arrow on the map: once the perspective picture has turned, the map (the original isometric one, which does not turn) no longer shows which way
// the camera looks, so this gives where to draw an arrow on the player point and which way it points. Given only while a map is open (dm_ui_map_geom):
// - the corner minimap (1): the player point is at the centre of the minimap frame (dm_cursor.c ui_map_geom_pump), and the arrow is sized by the frame height;
// - the full-screen map (2): the game draws the map centred on the character, so the player point is at the centre of the picture (dm_cam_heading's
//   sx / sy, which says why).
// Direction = the camera's horizontal heading (dm_cam_heading; in over-the-shoulder, stick up = walk this way) through the original map's isometric mapping: world
// direction (x, z) is ((x - z) * 1.6, (x + z) * 0.8) pixels on the map (the same mapping as dm_map_goals).
// out: centre x, y (0..1, y down), arrow direction (radians, 0 = up, clockwise positive, as in dm_cam_threats), scale (fraction of the screen height:
// minimap = the frame height, full-screen map 0.5 -- the map is drawn twice as large there)
int dm_cam_map_heading(float out[4]) {
    float fx, fz, sx, sy, cell, mm[4]; int map = dm_ui_map_geom(&cell);
    if (!map || !dm_cam_heading(&fx, &fz, &sx, &sy)) return 0;
    float mx = (fx - fz) * 1.6f, my = (fx + fz) * 0.8f;
    if (map == 1) {
        if (!dm_ui_minimap_rect(mm) || !(mm[3] > mm[1])) return 0;
        out[0] = (mm[0] + mm[2]) / 2; out[1] = (mm[1] + mm[3]) / 2; out[3] = mm[3] - mm[1];
    } else { out[0] = sx; out[1] = sy; out[3] = 0.5f; }
    out[2] = atan2f(mx, -my);
    return 1;
}

// 镜头跟随要的朝向（世界 xz，单位向量 × 1000）：瞄准拖拽 > 移动摇杆；由 dm_cam_remap_stick 记下，nvn_mtl.m cam_build 每帧取
// Heading the camera follow wants (world xz, unit vector * 1000): aim drag > move stick; recorded by dm_cam_remap_stick, read every frame by nvn_mtl.m cam_build
static _Atomic int g_want_x1000, g_want_z1000, g_want_on;
// 两根摇杆都没推时，再看放技能的方向（diag.c cam_c2s：玩家到技能目标，子格方向 = 世界方向）：0.8 秒内镜头转去看它。
// 点按技能钮时游戏自己选目标、不给瞄准方向；原地放远程技能人不动，镜头原先只跟位移，打身后的怪时人转过去了、镜头不跟（OnePlus 13 实测）
// When neither stick is pushed, the direction of the last skill cast comes next (diag.c cam_c2s: player to the skill's target, sub-cell direction = world
// direction): for 0.8 s the camera turns to look at it. Tapping a skill button lets the game pick the target and gives no aim direction; a ranged skill cast
// in place does not move the character, and the camera used to follow only displacement, so attacking a monster behind turned the character but not the
// camera (measured on a OnePlus 13)
static _Atomic int g_atk_x1000, g_atk_z1000; static double g_atk_t;
void dm_cam_attack(float dx, float dz) {
    float l = hypotf(dx, dz); if (dm_cam_mode() != 1 || !(l >= 0.05f)) return;
    atomic_store(&g_atk_x1000, (int)lroundf(dx / l * 1000)); atomic_store(&g_atk_z1000, (int)lroundf(dz / l * 1000));
    pthread_mutex_lock(&g_mu); g_atk_t = now_s(); pthread_mutex_unlock(&g_mu);
    static _Atomic int said; if (atomic_fetch_add(&said, 1) < 8) LOG("镜头：放技能，朝目标转 %.2f,%.2f（距 %.1f 子格）", dx / l, dz / l, l); }
int dm_cam_want_dir(float *wx, float *wz) {
    if (atomic_load(&g_want_on)) { *wx = atomic_load(&g_want_x1000) / 1000.0f; *wz = atomic_load(&g_want_z1000) / 1000.0f; return 1; }
    pthread_mutex_lock(&g_mu); double t = g_atk_t; pthread_mutex_unlock(&g_mu);
    if (!(t > 0 && now_s() - t < 0.8)) return 0;
    *wx = atomic_load(&g_atk_x1000) / 1000.0f; *wz = atomic_load(&g_atk_z1000) / 1000.0f; return 1; }
// 原神式（dm_cam_style = 1）：放技能时人转过去对着目标，镜头跟着人转（xbg_game.cpp 按 tau 缓转，xbg_cam_ease）。只给 0.8 秒内的技能方向，
// 而且两根摇杆都没推时才给——推着摇杆时镜头一转，摇杆的方向也跟着转，人就走歪了
// Genshin style (dm_cam_style = 1): casting a skill turns the character to face the target, and the camera turns with the character (xbg_game.cpp eases it
// with tau, xbg_cam_ease). Only a skill direction from the last 0.8 s, and only while neither stick is pushed -- turning the camera under a pushed stick
// would turn the stick's directions too and send the character off course
int dm_cam_attack_dir(float *wx, float *wz) {
    if (atomic_load(&g_want_on)) return 0;
    pthread_mutex_lock(&g_mu); double t = g_atk_t; pthread_mutex_unlock(&g_mu);
    if (!(t > 0 && now_s() - t < 0.8)) return 0;
    *wx = atomic_load(&g_atk_x1000) / 1000.0f; *wz = atomic_load(&g_atk_z1000) / 1000.0f; return 1; }

// 一根摇杆：(x, y) 是 Horizon 摇杆值（上为正）。返回 1 = 改过；wdir 给出这根摇杆在世界里的方向。
// 角度直接按镜头水平朝向转（不走「透视画面上人物所在的点」：那样随人在画面里的位置会偏几度，实测推正上偏 6°）
// One stick: (x, y) is the Horizon stick value (up is positive). Returns 1 = changed; wdir receives this stick's direction in the world.
// The angle is rotated directly by the camera's horizontal heading (not via "the character's point on the perspective picture": that drifts by a few degrees depending on where the character is on screen, measured 6° off when pushing straight up)
static int remap_one(const cam_t *c, int32_t *x, int32_t *y, float wdir[2]) {
    if (!*x && !*y) return 0;
    float mag = hypotf((float)*x, (float)*y), sx = *x / mag, sy = *y / mag * 0.5f, sl = hypotf(sx, sy); sx /= sl; sy /= sl;   // 原版：摇杆 → 画面方向 (x, y / 2) | original: stick -> screen direction (x, y / 2)
    float o[3], d[3]; if (!ray(c, 0.5f, 0.5f, o, d)) return 0; float fx = d[0], fz = d[2], fl = hypotf(fx, fz); if (fl < 1e-9f) return 0; fx /= fl; fz /= fl;   // 镜头水平朝向 | camera horizontal heading
    float wx = sx * -fz + sy * fx, wz = sx * fx + sy * fz, wl = hypotf(wx, wz); wdir[0] = wx / wl; wdir[1] = wz / wl;   // 右 = (−fz, fx) | right = (-fz, fx)
    int W = 0, H = 0; if (!dm_screen_real(&W, &H) || W <= 0 || H <= 0) return 0; float asp = (float)W / H;
    float P0[3] = { c->pl[0], 0, c->pl[1] }, P1[3] = { c->pl[0] + wdir[0] * 10, 0, c->pl[1] + wdir[1] * 10 }, oa[2], ob[2];
    if (!proj(c->vpo, P0, &oa[0], &oa[1]) || !proj(c->vpo, P1, &ob[0], &ob[1])) return 0;
    float ox = (ob[0] - oa[0]) * asp, oy = -(ob[1] - oa[1]), ol = hypotf(ox, oy); if (ol < 1e-9f) return 0; ox /= ol; oy /= ol;   // 原版画面方向（y 向上） | original picture direction (y up)
    float kx = ox, ky = oy * 2, kl = hypotf(kx, ky); kx /= kl; ky /= kl;   // 画面方向 → 摇杆（2:1 反过来） | screen direction -> stick (2:1 reversed)
    *x = (int32_t)lroundf(kx * mag); *y = (int32_t)lroundf(ky * mag); if (*x > 32767) *x = 32767; if (*x < -32767) *x = -32767; if (*y > 32767) *y = 32767; if (*y < -32767) *y = -32767;
    return 1;
}
// 参照镜头按「这一推」锁住：摇杆从 0 变非 0 那一刻的透视镜头当参照，松开前一直用它。不锁的话摇杆相对的是「正在转的镜头」，镜头又追摇杆的世界方向，按住右推会原地转圈。
// 锁住以后：按住右推 = 朝按下时画面的右边一直跑，镜头约 1.5 秒转到人背后；下一次推再按新镜头算
// The reference camera is locked per push: the perspective camera at the moment the stick goes from 0 to non-zero is the reference until release. Without the lock the stick is relative to a camera that is turning while the camera chases the stick's world direction, and holding right runs in circles.
// With the lock: holding right = keep running towards the right of the picture as it was when pressed, and the camera swings behind the character in about 1.5 s; the next push uses the new camera
// 体感转向（安卓 CamSensor.java → jni_android.c → 这里）：DM_CAM_SENSOR=1 转手机 / 转身看四周（陀螺仪），=2 像方向盘那样侧倾手机转（重力）。
// 前端把每次转动（弧度，正 = 往画面右边转）累加进来，画面线程每帧取走一次（xbg_game.cpp）；最近一秒内转过 = 体感转向进行中，镜头跟随让开。
// 体感转过以后参照镜头跟着重锁：玩家转手机就是要换「前方」，推上 = 朝现在看的方向走（跟随引起的转动不重锁，那是上面说的转圈问题）
// Sensor steering (Android CamSensor.java -> jni_android.c -> here): DM_CAM_SENSOR=1 turn the phone / your body to look around (gyroscope), =2 tilt the
// phone like a steering wheel (gravity). The front end adds each turn (radians, positive = towards the right of the picture) here, and the render thread
// takes the sum once per frame (xbg_game.cpp); a turn within the last second = sensor steering in progress, and the camera follow stands aside.
// After a sensor turn the reference camera is re-locked: turning the phone means a new "ahead", so pushing up walks where the player now looks (turns caused
// by the follow are not re-locked, that is the running-in-circles problem described above)
// 体感档只看环境变量（测试开关）：镜头档可以在局中从自定义页切（dm_cam_set_mode），所以不再按启动那一刻的镜头档定；镜头关着时 dm_cam_turn 丢掉转动
// The sensor mode only reads the environment variable (a test switch): the camera mode can be switched on the custom page mid-game (dm_cam_set_mode), so
// this no longer depends on the camera mode at the moment of start; while the camera is off dm_cam_turn drops the turns
// 原神式（dm_cam_style = 1）不用体感，返回 0（转镜头只靠右半屏左右拖） | The Genshin style (dm_cam_style = 1) has no sensor steering and returns 0 (the camera turns only by a horizontal drag on the right half)
int dm_cam_sensor_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_CAM_SENSOR"); m = e ? atoi(e) : 0; if (m) LOG("DM_CAM_SENSOR=%d：体感转向开（1 转手机看四周 / 2 侧倾转向）", m); } return dm_cam_style() == 1 ? 0 : m; }
static float g_turn, g_turn_total; static double g_turn_t;
void dm_cam_turn(float rad) { if (rad == 0 || dm_cam_mode() <= 0) return; pthread_mutex_lock(&g_mu); g_turn += rad; g_turn_total += rad; g_turn_t = now_s(); pthread_mutex_unlock(&g_mu); }
// 俯仰（右半屏空白处上下拖，dm_touch_controls.c）：正 = 视线往上。累计值整个保留（换区域也不复位），画面线程每帧读；夹在 ±1.4 弧度，镜头里再按机位夹实际角度
// Pitch (vertical drag on the empty right half, dm_touch_controls.c): positive = look up. The sum is kept as is (not reset on area changes) and read by the
// render thread every frame; clamped to +-1.4 rad here, and the camera clamps the actual angle for its rig
static _Atomic int g_tilt_x10000;
void dm_cam_tilt(float rad) { int v = atomic_load(&g_tilt_x10000) + (int)lroundf(rad * 10000); if (v > 14000) v = 14000; if (v < -14000) v = -14000; atomic_store(&g_tilt_x10000, v); }
float dm_cam_tilt_get(void) { return atomic_load(&g_tilt_x10000) / 10000.0f; }
void dm_cam_tilt_clamp(float lo, float hi) { int l = (int)lroundf(lo * 10000), h = (int)lroundf(hi * 10000), v = atomic_load(&g_tilt_x10000); if (l <= h && (v < l || v > h)) atomic_store(&g_tilt_x10000, v < l ? l : h); }
float dm_cam_take_turn(void) { pthread_mutex_lock(&g_mu); float r = g_turn; g_turn = 0; pthread_mutex_unlock(&g_mu); return r; }
double dm_cam_turn_age(void) { pthread_mutex_lock(&g_mu); double a = g_turn_t > 0 ? now_s() - g_turn_t : 1e9; pthread_mutex_unlock(&g_mu); return a; }
static float turn_total(void) { pthread_mutex_lock(&g_mu); float t = g_turn_total; pthread_mutex_unlock(&g_mu); return t; }

// 瞄准拖拽（右摇杆，拖技能键）此刻的世界方向与时刻：dm_cam_aim 在地上画箭头用
// The aim drag's (right stick, dragging a skill button) current world direction and time: dm_cam_aim draws the ground arrow from it
static _Atomic int g_aim_x1000, g_aim_z1000; static double g_aim_t;
static cam_t g_ref_l, g_ref_r; static int g_ref_l_on, g_ref_r_on; static float g_ref_l_turn, g_ref_r_turn;
void dm_cam_remap_stick(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    cam_t c; if (!cam_get(&c)) { atomic_store(&g_want_on, 0); g_ref_l_on = g_ref_r_on = 0; return; }
    float tt = turn_total();
    // 原神式（dm_cam_style = 1）：镜头不追摇杆，摇杆每次都按此刻的镜头定方向（不锁参照镜头）；一直往左推就绕着镜头转圈，同原神
    // Genshin style (dm_cam_style = 1): the camera does not chase the stick, so the stick takes its directions from the camera of the moment every time (no
    // reference lock); holding it left circles round the camera, as in Genshin
    const int live = dm_cam_style() == 1 && dm_cam_mode() == 1;
    if (!*lx && !*ly) g_ref_l_on = 0; else if (live || !g_ref_l_on || fabsf(tt - g_ref_l_turn) > 0.035f) { g_ref_l = c; g_ref_l_on = 1; g_ref_l_turn = tt; }
    if (!*rx && !*ry) g_ref_r_on = 0; else if (live || !g_ref_r_on || fabsf(tt - g_ref_r_turn) > 0.035f) { g_ref_r = c; g_ref_r_on = 1; g_ref_r_turn = tt; }
    cam_t cl = c, cr = c; memcpy(cl.vpp, g_ref_l.vpp, 64); memcpy(cl.ivpp, g_ref_l.ivpp, 64); memcpy(cr.vpp, g_ref_r.vpp, 64); memcpy(cr.ivpp, g_ref_r.ivpp, 64);   // 只借参照镜头的朝向；原版正交与人物位置用当前的 | borrow only the reference camera's heading; the original orthographic matrices and character position are current
    float wl[2], wr[2]; int l = g_ref_l_on && remap_one(&cl, lx, ly, wl), r = g_ref_r_on && remap_one(&cr, rx, ry, wr);
    if (r) { atomic_store(&g_aim_x1000, (int)lroundf(wr[0] * 1000)); atomic_store(&g_aim_z1000, (int)lroundf(wr[1] * 1000)); pthread_mutex_lock(&g_mu); g_aim_t = now_s(); pthread_mutex_unlock(&g_mu); }
    const float *w = r ? wr : l ? wl : NULL;   // 瞄准拖拽优先（朝攻击方向），其次移动方向 | aim drag first (face the attack), then the move direction
    if (w) { atomic_store(&g_want_x1000, (int)lroundf(w[0] * 1000)); atomic_store(&g_want_z1000, (int)lroundf(w[1] * 1000)); }
    atomic_store(&g_want_on, w != NULL);
}

// 瞄准环（原神式那样在 3D 画面里瞄）：拖技能键瞄准时，在瞄准方向上人物脚前 12 处的地面上平放一个环（前端用游戏自己的金环图 touchart/dm_native_circle.png），
// 透视投到画面上。out = 5 × 5 个网格点（各 x、y，0..1，y 向下），按行排：第 0 行是环的远边（瞄准方向那头），每行从左到右——就是 Android drawBitmapMesh
// 要的顺序，图贴上去在透视下也是平躺的；返回 25，瞄准没在进行（0.15 秒内没更新）、镜头没开或有点在镜头背后返回 0。
// 环半径 2.4 世界单位（原版画面宽约 98）；方向就是发给游戏的那个（dm_cam_remap_stick 换算后的右摇杆）。原先画的是一支大白箭头，用户嫌它大而扎眼（OnePlus 13）
// Aim ring (aiming in the 3D picture, as in Genshin): while a skill button is dragged to aim, a ring lies flat on the ground 12 ahead of the character's feet
// along the aim direction (the front end uses the game's own gold ring art, touchart/dm_native_circle.png), projected onto the perspective picture. out =
// 5 x 5 grid points (x, y each, 0..1, y down) row by row: row 0 is the ring's far edge (the aim direction's end), each row from left to right -- the order
// Android's drawBitmapMesh wants, so the image lies flat in perspective too; returns 25, or 0 when no aim is in progress (no update within 0.15 s), the camera
// is off, or a point is behind the camera. Ring radius 2.4 world units (the original picture is about 98 wide); the direction is the one sent to the game
// (the right stick after dm_cam_remap_stick). It used to be a big white arrow, which the user found big and glaring (OnePlus 13)
int dm_cam_aim(float out[50]) {
    cam_t c; if (!cam_get(&c)) return 0;
    pthread_mutex_lock(&g_mu); double t = g_aim_t; pthread_mutex_unlock(&g_mu);
    if (!(t > 0 && now_s() - t < 0.15)) return 0;
    float ax = atomic_load(&g_aim_x1000) / 1000.0f, az = atomic_load(&g_aim_z1000) / 1000.0f, l = hypotf(ax, az); if (l < 1e-3f) return 0; ax /= l; az /= l;
    const float rx = -az, rz = ax, cx = c.pl[0] + ax * 12, cz = c.pl[1] + az * 12, r = 2.4f;
    for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++) {
        const float u = (j / 4.0f - 0.5f) * 2 * r, v = (0.5f - i / 4.0f) * 2 * r;
        const float w[3] = { cx + rx * u + ax * v, c.py + 0.15f, cz + rz * u + az * v };
        if (!proj(c.vpp, w, &out[2 * (i * 5 + j)], &out[2 * (i * 5 + j) + 1])) return 0; }
    return 25;
}

// 点单位：tap = 触点（透视画面归一化），feet = 这个单位脚底在原版画面上的点（归一化）。out = 触点「搬到这个单位那一层深度」后在原版画面上的点。返回 0 = 镜头没开或这个单位在镜头背后
// Tapping a unit: tap = touch point (normalized perspective picture), feet = this unit's feet on the original picture (normalized). out = the touch point "moved to this unit's depth layer", on the original picture. Returns 0 = camera off or this unit is behind the camera
int dm_cam_tap_for_unit(float tx, float ty, float fx, float fy, float *ox, float *oy) {
    cam_t c; if (!cam_get(&c)) return 0;
    float F[3], o[3], d[3]; if (!ortho_ground(&c, fx, fy, F) || !ray(&c, tx, ty, o, d)) return 0;
    float nx = d[0], nz = d[2], nl = hypotf(nx, nz); if (nl < 1e-9f) return 0; nx /= nl; nz /= nl;   // 竖直面：过脚底、法向 = 射线的水平方向 | vertical plane: through the feet, normal = the ray's horizontal direction
    float den = d[0] * nx + d[2] * nz; if (fabsf(den) < 1e-9f) return 0; float t = ((F[0] - o[0]) * nx + (F[2] - o[2]) * nz) / den; if (t <= 0) return 0;
    float Q[3] = { o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2] }; return proj(c.vpo, Q, ox, oy);
}
// 反查（测试量具 tap_unit 用）：原版画面上一个单位的脚底 / 头顶点 → 透视画面上它身体中间的点，落在玩家真看见它的地方
// Reverse lookup (for the tap_unit test tool): a unit's feet / head points on the original picture -> the middle of its body on the perspective picture, where the player actually sees it
int dm_cam_unit_to_screen(float fx, float fy, float hx, float hy, float *ox, float *oy) {
    cam_t c; if (!cam_get(&c)) return 0;
    float F[3]; if (!ortho_ground(&c, fx, fy, F)) return 0;
    // 头顶：原版正交视线过头顶点，取这条线上离「过脚底的竖直线」最近的那点的高度 | head: the original orthographic sight line through the head point; take the height of its point nearest to the vertical line through the feet
    float x, y; n2c(hx, hy, &x, &y); float a[4] = { x, y, 0, 1 }, b[4] = { x, y, 1, 1 }, p[4], q[4]; xf(c.ivpo, a, p); xf(c.ivpo, b, q);
    float P[3] = { p[0] / p[3], p[1] / p[3], p[2] / p[3] }, D[3] = { q[0] / q[3] - P[0], q[1] / q[3] - P[1], q[2] / q[3] - P[2] };
    float dh = D[0] * D[0] + D[2] * D[2]; float s = dh > 1e-12f ? ((F[0] - P[0]) * D[0] + (F[2] - P[2]) * D[2]) / dh : 0; float h = P[1] + s * D[1]; if (!(h > 0 && h < 50)) h = 0;
    float M[3] = { F[0], h / 2, F[2] }; return proj(c.vpp, M, ox, oy);
}
