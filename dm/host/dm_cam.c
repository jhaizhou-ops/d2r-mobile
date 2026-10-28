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
void dm_cam_set_mode(int m) { if (m < 0 || m > 2) m = 0; if (atomic_exchange(&g_mode, m) != m) LOG("镜头视角 → %s（自定义页）%s", m == 1 ? "越肩" : m == 2 ? "第一人称" : "原版", atomic_load(&g_mode_env) >= 0 && atomic_load(&g_mode_env) != 0x100 ? "；环境变量 DM_CAM 设着，以它为准" : ""); }

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
    if (dm_cam_mode() <= 0) return 0;   // 两档都换算：第一人称以前只认越肩，摇杆一直按原版方向走，转过身就左右颠倒 | both modes: first person used to be left out, so its stick kept the original directions and swapped left and right once turned around
    pthread_mutex_lock(&g_mu); int ok = g_pub_t > 0 && now_s() - g_pub_t < 0.5; if (ok) *c = g_pub; pthread_mutex_unlock(&g_mu); return ok; }

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
    if (ok) { m4mul(p, g_pub.ivpo, t); m4mul(t, g_pub.vpp, out); }
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
// 镜头的水平朝向（世界 x / z，单位向量）：小地图上画人物朝向用（front_ios.m mapHeadingSync）。镜头关着或矩阵过期返回 0
// The camera's horizontal heading (world x / z, unit vector): for the facing indicator on the minimap (front_ios.m mapHeadingSync). Returns 0 when the
// camera is off or its matrices are stale
// sx / sy = 人物脚底在原版正交画面上的位置（归一化，y 向下）：全屏地图是游戏按原版画面画的，人物十字在这里（不能用透视画面上的位置）
// sx / sy = the character's feet on the original orthographic picture (normalized, y down): the game draws the fullscreen map by the original picture, so its
// player cross is here (not at the position on the perspective picture)
int dm_cam_heading(float *fx, float *fz, float *sx, float *sy) {
    cam_t c; float o[3], d[3]; if (!cam_get(&c) || !ray(&c, 0.5f, 0.5f, o, d)) return 0;
    float l = hypotf(d[0], d[2]); if (l < 1e-6f) return 0; *fx = d[0] / l; *fz = d[2] / l;
    float P[3] = { c.pl[0], 0, c.pl[1] }; return proj(c.vpo, P, sx, sy); }

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
    cam_t c; if (max <= 0 || !cam_get(&c)) return 0;
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
// · 全屏地图（2）：游戏把地图的玩家点放在「人物脚底在原版画面上的位置」（不开镜头时地图线叠在世界上，同 dm_map_goals 的 sx / sy）。
//   这里不能像 dm_map_goals 那样调游戏的世界点 → 屏幕：镜头开着时那条路给的是透视画面上的位置（dm_uiscreen.c 的投影门），所以用画面线程发布的原版正交矩阵投脚底。
// 方向 = 镜头水平朝向（越肩档推摇杆上 = 往这边走）按原版地图的等距换算：世界方向 (x, z) 在地图上 = ((x − z)×1.6, (x + z)×0.8) 像素（同 dm_map_goals 的换算）。
// out：中心 x、y（0..1，y 向下）、箭头方向（弧度，0 = 朝上、顺时针为正，同 dm_cam_threats）、尺度（屏高的比例：小地图 = 框高，全屏地图 0.5——那里地图放大一倍）
// Heading arrow on the map: once the perspective picture has turned, the map (the original isometric one, which does not turn) no longer shows which way
// the camera looks, so this gives where to draw an arrow on the player point and which way it points. Given only while a map is open (dm_ui_map_geom):
// - the corner minimap (1): the player point is at the centre of the minimap frame (dm_cursor.c ui_map_geom_pump), and the arrow is sized by the frame height;
// - the full-screen map (2): the game puts the map's player point at "the character's feet on the original picture" (with the camera off the map lines lie
//   over the world, as dm_map_goals' sx / sy). This cannot call the game's world point -> screen as dm_map_goals does: with the camera on that path gives
//   the position on the perspective picture (dm_uiscreen.c's projection gate), so the feet are projected with the original orthographic matrix the render
//   thread publishes.
// Direction = the camera's horizontal heading (in over-the-shoulder, stick up = walk this way) through the original map's isometric mapping: world
// direction (x, z) is ((x - z) * 1.6, (x + z) * 0.8) pixels on the map (the same mapping as dm_map_goals).
// out: centre x, y (0..1, y down), arrow direction (radians, 0 = up, clockwise positive, as in dm_cam_threats), scale (fraction of the screen height:
// minimap = the frame height, full-screen map 0.5 -- the map is drawn twice as large there)
int dm_cam_map_heading(float out[4]) {
    cam_t c; float cell, mm[4], o[3], d[3]; int map;
    if (!cam_get(&c) || !(map = dm_ui_map_geom(&cell)) || !ray(&c, 0.5f, 0.5f, o, d)) return 0;
    float mx = (d[0] - d[2]) * 1.6f, my = (d[0] + d[2]) * 0.8f;
    if (hypotf(mx, my) < 1e-6f) return 0;
    if (map == 1) {
        if (!dm_ui_minimap_rect(mm) || !(mm[3] > mm[1])) return 0;
        out[0] = (mm[0] + mm[2]) / 2; out[1] = (mm[1] + mm[3]) / 2; out[3] = mm[3] - mm[1];
    } else {
        const float feet[3] = { c.pl[0], 0, c.pl[1] };
        if (!proj(c.vpo, feet, &out[0], &out[1])) return 0;
        out[3] = 0.5f;
    }
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
int dm_cam_sensor_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_CAM_SENSOR"); m = e ? atoi(e) : 0; if (m) LOG("DM_CAM_SENSOR=%d：体感转向开（1 转手机看四周 / 2 侧倾转向）", m); } return m; }
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

static cam_t g_ref_l, g_ref_r; static int g_ref_l_on, g_ref_r_on; static float g_ref_l_turn, g_ref_r_turn;
void dm_cam_remap_stick(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    cam_t c; if (!cam_get(&c)) { atomic_store(&g_want_on, 0); g_ref_l_on = g_ref_r_on = 0; return; }
    float tt = turn_total();
    if (!*lx && !*ly) g_ref_l_on = 0; else if (!g_ref_l_on || fabsf(tt - g_ref_l_turn) > 0.035f) { g_ref_l = c; g_ref_l_on = 1; g_ref_l_turn = tt; }
    if (!*rx && !*ry) g_ref_r_on = 0; else if (!g_ref_r_on || fabsf(tt - g_ref_r_turn) > 0.035f) { g_ref_r = c; g_ref_r_on = 1; g_ref_r_turn = tt; }
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
