// 越肩 / 第一人称镜头（实验，缺省关）：输入换算与两个线程之间的共享状态。设计与推导见 docs/over-the-shoulder-camera.zh-CN.md。
// 画面按透视镜头画（dm/gfx/nvn/nvn_mtl.m cam_build，只有 Metal 后端），游戏却只认原版正交画面上的点和方向。做法：不碰游戏，输入在喂给原版前换一次——
//   透视画面上的点 → 打一条射线与某个平面求交 → 按原版正交视图投影投回原版画面上的点。两份矩阵由画面线程每帧发布（dm_cam_publish）。
// · 摇杆 / 瞄准拖拽（dm_cam_remap_stick，取手柄处 hle_hid.c 调）：原版把摇杆转 45° 当世界方向，画面上人走的方向 = (摇杆 x, 摇杆 y / 2)（等距 2:1）。
//   把这个画面方向按参照镜头的水平朝向转成世界方向（上 = 镜头正前），再投回原版画面方向、按 2:1 换回摇杆。推上 = 朝镜头前方走。
// · 点单位（dm_world.c w_hit）：人有高度，点在身上的射线落地会落到人身后。按候选逐个换：射线交「过该单位脚底、正对射线的竖直面」，
//   交点投回原版画面，再用原版那套身体框 / 菱形 / 选择框判命中（dm_cam_tap_for_unit）。
// 开关：环境变量 DM_CAM=1 越肩、=2 第一人称（只做了画面，输入不换算）；不设 = 关。关着或后端没发布矩阵（安卓 / 鸿蒙的 Vulkan 后端）时这里全部原样返回。
// Over-the-shoulder / first-person camera (experimental, off by default): input conversion and the state shared between the two threads. Design and derivation: docs/over-the-shoulder-camera.md.
// The picture is drawn with a perspective camera (dm/gfx/nvn/nvn_mtl.m cam_build, Metal backend only), while the game only understands points and directions on the original orthographic picture. Approach: leave the game alone and convert input once before it reaches the game --
//   a point on the perspective picture -> cast a ray and intersect it with a plane -> project back onto the original picture with the original orthographic view-projection. Both matrix sets are published every frame by the render thread (dm_cam_publish).
// - Sticks / aim drag (dm_cam_remap_stick, called at the controller poll in hle_hid.c): the game turns the stick by 45° into a world direction; on screen the character walks along (stick x, stick y / 2) (isometric 2:1).
//   That screen direction is rotated into a world direction by the reference camera's horizontal heading (up = straight ahead of the camera), projected back to an original-picture direction and turned back into a stick value via 2:1. Pushing up = walk towards where the camera looks.
// - Tapping a unit (dm_world.c w_hit): units have height, so a ray through a body lands on the ground behind them. Each candidate is handled on its own: intersect the ray with the vertical plane through that unit's feet facing the ray,
//   project the hit point back onto the original picture, then test it with the original body box / footprint diamond / selection box (dm_cam_tap_for_unit).
// Switch: environment variable DM_CAM=1 over-the-shoulder, =2 first person (picture only, input is not converted); unset = off. When off, or when the backend publishes no matrices (the Vulkan backend on Android / HarmonyOS), everything here returns unchanged.
#include "dm.h"
#include "dm_uiscreen.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int dm_cam_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_CAM"); m = e ? atoi(e) : 0; if (m) LOG("DM_CAM=%d：镜头实验开（1 越肩 / 2 第一人称）", m); } return m; }

// 画面线程发布的矩阵（行向量约定：裁剪 = [x y z 1] × M）：原版正交视图投影与逆、透视视图投影与逆、人物落地点 x / z；超过 0.5 秒没更新就当镜头没开
// Matrices published by the render thread (row-vector convention: clip = [x y z 1] * M): original orthographic view-projection and inverse, perspective view-projection and inverse, the character's ground point x / z; not refreshed for 0.5 s = camera treated as off
typedef struct { float vpo[16], ivpo[16], vpp[16], ivpp[16], pl[2]; } cam_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static cam_t g_pub; static double g_pub_t;
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
void dm_cam_publish(const float vpo[16], const float ivpo[16], const float vpp[16], const float ivpp[16], float px, float pz) {
    pthread_mutex_lock(&g_mu); memcpy(g_pub.vpo, vpo, 64); memcpy(g_pub.ivpo, ivpo, 64); memcpy(g_pub.vpp, vpp, 64); memcpy(g_pub.ivpp, ivpp, 64);
    g_pub.pl[0] = px; g_pub.pl[1] = pz; g_pub_t = now_s(); pthread_mutex_unlock(&g_mu); }

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
        *dist = g_clear[i] < g_clear[j] ? g_clear[i] : g_clear[j]; *px = g_clear_p[0]; *pz = g_clear_p[1]; }
    pthread_mutex_unlock(&g_mu); return ok; }
static int cam_get(cam_t *c) {
    if (dm_cam_mode() != 1) return 0;
    pthread_mutex_lock(&g_mu); int ok = g_pub_t > 0 && now_s() - g_pub_t < 0.5; if (ok) *c = g_pub; pthread_mutex_unlock(&g_mu); return ok; }

static void xf(const float *M, const float v[4], float o[4]) { for (int j = 0; j < 4; j++) o[j] = v[0] * M[j] + v[1] * M[4 + j] + v[2] * M[8 + j] + v[3] * M[12 + j]; }

// 跟着世界走的界面字（名字牌、血条、地上物品名、飘字）也按透视画面放（dm_uiscreen.c 在游戏的「世界点 → 屏幕」main+0x1237c60 入口换镜头副本）。
// 那个函数算 裁剪 = 投影（镜头 +0x80）× 视图 × 点，并且除以 w，所以透视矩阵也对。副本的投影换成 P' = P × 逆(原版视图投影) × 透视视图投影：
// 按行向量写，视图 × P' = 视图 × 投影 × 逆(视图 × 投影) × 透视 = 透视视图投影。p = 镜头自己的投影（与常量块 +208 相同），排布与这里的矩阵一致。
// 越肩镜头开着并且画面线程 0.5 秒内发布过矩阵才返回 1；点按与测试量具的单位位置也走这个函数，所以那边拿到的也是透视画面上的坐标
// UI text that follows the world (nameplates, health bars, ground item names, floating text) is placed on the perspective picture too (dm_uiscreen.c swaps a
// copy of the camera at the entry of the game's "world point -> screen" main+0x1237c60). That function computes clip = projection (camera +0x80) x view x point
// and divides by w, so a perspective matrix works. The copy's projection becomes P' = P x inverse(original view-projection) x perspective view-projection:
// in row-vector terms, view x P' = view x projection x inverse(view x projection) x perspective = perspective view-projection. p = the camera's own projection
// (the same as the constant block's +208), laid out like the matrices here. Returns 1 only while the over-the-shoulder camera is on and the render thread
// published matrices within 0.5 s; taps and the test tool's unit positions also go through that function, so they get perspective picture coordinates too
static void m4mul(const float *a, const float *b, float *o) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += (double)a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = (float)s; } }
int dm_cam_label_matrix(const float p[16], float out[16]) {
    cam_t c; if (!cam_get(&c)) return 0;
    float t[16]; m4mul(p, c.ivpo, t); m4mul(t, c.vpp, out); return 1; }
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

// 镜头跟随要的朝向（世界 xz，单位向量 × 1000）：瞄准拖拽 > 移动摇杆；由 dm_cam_remap_stick 记下，nvn_mtl.m cam_build 每帧取
// Heading the camera follow wants (world xz, unit vector * 1000): aim drag > move stick; recorded by dm_cam_remap_stick, read every frame by nvn_mtl.m cam_build
static _Atomic int g_want_x1000, g_want_z1000, g_want_on;
int dm_cam_want_dir(float *wx, float *wz) { if (!atomic_load(&g_want_on)) return 0; *wx = atomic_load(&g_want_x1000) / 1000.0f; *wz = atomic_load(&g_want_z1000) / 1000.0f; return 1; }

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
static cam_t g_ref_l, g_ref_r; static int g_ref_l_on, g_ref_r_on;
void dm_cam_remap_stick(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    cam_t c; if (!cam_get(&c)) { atomic_store(&g_want_on, 0); g_ref_l_on = g_ref_r_on = 0; return; }
    if (!*lx && !*ly) g_ref_l_on = 0; else if (!g_ref_l_on) { g_ref_l = c; g_ref_l_on = 1; }
    if (!*rx && !*ry) g_ref_r_on = 0; else if (!g_ref_r_on) { g_ref_r = c; g_ref_r_on = 1; }
    cam_t cl = c, cr = c; memcpy(cl.vpp, g_ref_l.vpp, 64); memcpy(cl.ivpp, g_ref_l.ivpp, 64); memcpy(cr.vpp, g_ref_r.vpp, 64); memcpy(cr.ivpp, g_ref_r.ivpp, 64);   // 只借参照镜头的朝向；原版正交与人物位置用当前的 | borrow only the reference camera's heading; the original orthographic matrices and character position are current
    float wl[2], wr[2]; int l = g_ref_l_on && remap_one(&cl, lx, ly, wl), r = g_ref_r_on && remap_one(&cr, rx, ry, wr);
    const float *w = r ? wr : l ? wl : NULL;   // 瞄准拖拽优先（朝攻击方向），其次移动方向 | aim drag first (face the attack), then the move direction
    if (w) { atomic_store(&g_want_x1000, (int)lroundf(w[0] * 1000)); atomic_store(&g_want_z1000, (int)lroundf(w[1] * 1000)); }
    atomic_store(&g_want_on, w != NULL);
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
