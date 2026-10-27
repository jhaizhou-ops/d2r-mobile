// 越肩 / 第一人称镜头的矩阵运算（纯逻辑，不碰 Vulkan、不读环境变量）：从 nvn_mtl.m 的 cam_sig / cam_build / cam_build10 搬来，算法与常数不变，
// 改成显式传入状态和参数，C 和 C++ 都能编；xbg_game.cpp 的 cam 命名空间负责快照、帧号与宿主（dm/host/dm_cam.c）的往来，测试直接调用这里。
// 设计与推导见 docs/over-the-shoulder-camera.zh-CN.md。约定同原版：行向量（裁剪 = [x y z 1] × M），反向 Z（近 = 1、远 = 0）。
// Matrix math of the over-the-shoulder / first-person camera (pure logic, no Vulkan, no environment): moved from nvn_mtl.m's cam_sig /
// cam_build / cam_build10 with the same algorithm and constants, but with state and parameters passed explicitly, so it builds as C or C++.
// The cam namespace in xbg_game.cpp handles snapshots, frame numbers and the host (dm/host/dm_cam.c); the tests call this header directly.
// Design and derivation: docs/over-the-shoulder-camera.md. Conventions as in the original: row vectors (clip = [x y z 1] * M), reversed Z (near = 1, far = 0).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

// 主相机常量块的长度与片元槽 10 场景常量块的长度（字节）
// Length of the main camera constant block and of the fragment slot 10 scene constant block (bytes)
enum { XBG_CAM_BLOCK = 608, XBG_CAM_SCENE = 1440 };

// 参数：越肩（档 1）用 dist / h / look / lookh，第一人称（档 2）用 eye / fwd / pitch；fov 是水平视角（度）
// Parameters: over-the-shoulder (mode 1) uses dist / h / look / lookh, first person (mode 2) uses eye / fwd / pitch; fov is the horizontal field of view (degrees)
typedef struct { float dist, h, look, lookh, eye, fwd, pitch, fov, tau, near_; } xbg_cam_params;
static inline xbg_cam_params xbg_cam_default_params(int mode) {
    xbg_cam_params p = { 20.0f, 13.0f, 4.0f, 3.0f, 6.0f, 2.2f, 8.0f, mode == 2 ? 75.0f : 65.0f, 0.35f, 0.3f };
    return p;
}

// 跨帧状态：朝向 (fx, fz)、上一帧人物位置、上一帧与本帧改过的视图投影、本帧镜头位置与视线
// State across frames: heading (fx, fz), the character's previous position, the previous and current modified view-projection, this frame's camera position and sight line
typedef struct { float vp_prev[16], vp_last[16], fx, fz, px, pz, eye[3], fwd[3]; int have, have_eye; uint64_t frame; double t; } xbg_cam_state;
static inline void xbg_cam_init(xbg_cam_state *s) {
    memset(s, 0, sizeof *s);
    // 朝向缺省取原版视线的水平方向（原版镜头看向 −x −z），画面一进来和原版同一个朝向
    // The heading defaults to the horizontal direction of the original sight line (the original camera looks towards -x -z), so the picture starts out facing the same way as the original
    s->fx = s->fz = -0.70711f;
    s->frame = UINT64_MAX;
}

// 主相机：+80 视图第 1 行 (0.7071, -0.3536, 0.6124)、第 2 行 (0, 0.866, 0.5)，+208 正交（第 3 列为 0、右下 1）。
// avail = 快照里有的字节（Vulkan 只绑着色器读得到的那段），full = 游戏绑定的整块长度
// Main camera: view row 1 at +80 (0.7071, -0.3536, 0.6124), row 2 (0, 0.866, 0.5), orthographic at +208 (third column 0, bottom-right 1).
// avail = the bytes present in the snapshot (Vulkan binds only the part a shader can read), full = the length of the whole block the game bound
static inline int xbg_cam_sig(const uint8_t *p, uint64_t avail, uint64_t full) {
    if (full < XBG_CAM_BLOCK || avail < 272) return 0;
    float v[7], pr[16];
    memcpy(v, p + 80, sizeof v);
    memcpy(pr, p + 208, sizeof pr);
    return fabsf(v[0] - 0.70711f) < 2e-3f && fabsf(v[1] + 0.35355f) < 2e-3f && fabsf(v[2] - 0.61237f) < 2e-3f && fabsf(v[5] - 0.86603f) < 2e-3f
        && fabsf(v[6] - 0.5f) < 2e-3f && pr[3] == 0 && pr[7] == 0 && pr[11] == 0 && fabsf(pr[15] - 1) < 1e-4f;
}
// 片元槽 10 的场景常量（1440 字节）：+16 镜头位置、+32 视线方向 (-0.612, -0.5, -0.612)、+848 线性深度换算（尺度、1/尺度、起点）
// Scene constants of fragment slot 10 (1440 bytes): +16 camera position, +32 view direction (-0.612, -0.5, -0.612), +848 linear depth conversion (scale, 1/scale, origin)
static inline int xbg_cam_sig10(const uint8_t *p, uint64_t avail, uint64_t full) {
    if (full != XBG_CAM_SCENE || avail < 44) return 0;
    float v[3];
    memcpy(v, p + 32, sizeof v);
    return fabsf(v[0] + 0.61237f) < 2e-3f && fabsf(v[1] + 0.5f) < 2e-3f && fabsf(v[2] + 0.61237f) < 2e-3f;
}

static inline void xbg_cam_m4_mul(const float *a, const float *b, float *o) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += (double)a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = (float)s; }
}
// 4×4 高斯消元求逆（双精度，选主元）；奇异返回 0
// 4x4 inverse by Gaussian elimination (double precision, partial pivoting); returns 0 when singular
static inline int xbg_cam_m4_inv(const float *m, float *o) {
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return 0;
        if (p != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o[i * 4 + j] = (float)a[i][j + 4];
    return 1;
}

// 人物落地点：原版镜头总对准人物，逆视图（+144）第 3 行 = 视图 +z 的世界方向、第 4 行 = 镜头位置，视线落到地面 y = 0 处就是人物
// The character's ground point: the original camera always aims at the character; in the inverse view (+144) row 3 = world direction of view +z
// and row 4 = camera position, and where the sight line meets the ground y = 0 is the character
static inline void xbg_cam_ground(const uint8_t *src, float *px, float *pz) {
    float iv[16];
    memcpy(iv, src + 144, sizeof iv);
    float bx = iv[8], by = iv[9], bz = iv[10], cx = iv[12], cy = iv[13], cz = iv[14], t = by != 0 ? cy / by : 0;
    *px = cx - t * bx;
    *pz = cz - t * bz;
}

// 镜头跟随，每帧一次（frame 变了才走，返回 1）。want = 宿主给的朝向（瞄准拖拽 > 移动摇杆，已是世界方向），没有就取人物位移方向，都没有就保持。
// 按墙钟一阶阻尼转角度（tau 秒；各帧率下转速一致），走最短的那一边。首帧、位移一帧超过 20（换区域 / 传送 / 过门）：朝向重置为原版视线的水平方向
// Camera follow, once per frame (only when frame changes; returns 1). want = the host's heading (aim drag > move stick, already a world direction),
// otherwise the character's displacement, otherwise keep the heading. The angle turns with a wall-clock first-order lag (tau seconds; same turn rate
// at every frame rate), taking the shorter way round. First frame, or a displacement above 20 in one frame (area change / teleport / door): the
// heading resets to the horizontal direction of the original sight line
static inline int xbg_cam_follow(xbg_cam_state *s, float px, float pz, uint64_t frame, double now, int want, float wx, float wz, float tau) {
    if (frame == s->frame) return 0;
    s->frame = frame;
    double dt = s->t ? now - s->t : 0;
    s->t = now;
    if (dt > 0.25) dt = 0.25;
    float dx = px - s->px, dz = pz - s->pz, l = sqrtf(dx * dx + dz * dz);
    if (!s->have || l >= 20) s->fx = s->fz = -0.70711f;
    else {
        if (!want && l > 0.05f) { wx = dx / l; wz = dz / l; want = 1; }
        if (want && dt > 0) {
            float a = atan2f(s->fz, s->fx), d = atan2f(wz, wx) - a;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            a += d * (float)(1 - exp(-dt / tau));
            s->fx = cosf(a);
            s->fz = sinf(a);
        }
    }
    s->px = px;
    s->pz = pz;
    s->have = 1;
    memcpy(s->vp_prev, s->vp_last, 64);
    return 1;
}

// dst 已是 src 的拷贝（至少 608 字节）：按本帧人物位置与朝向重写 +80 视图、+144 逆视图、+208 投影、+272 逆投影、+336 视图投影、+400 逆视图投影、
// +464 上一帧（改过的）视图投影，+592 深度还原改走透视分支。vp / ivp 给出新的视图投影与逆，供输入换算发布
// dst is already a copy of src (at least 608 bytes): rewrite +80 view, +144 inverse view, +208 projection, +272 inverse projection, +336 view-projection,
// +400 inverse view-projection and +464 the previous frame's (modified) view-projection for this frame's character position and heading, and switch +592
// depth reconstruction to the perspective branch. vp / ivp receive the new view-projection and its inverse, to publish for input conversion
static inline void xbg_cam_build(xbg_cam_state *s, const uint8_t *src, uint8_t *dst, int mode, const xbg_cam_params *pp, float vp[16], float ivp[16]) {
    float pr[16], px, pz;
    memcpy(pr, src + 208, sizeof pr);
    xbg_cam_ground(src, &px, &pz);
    float fx = s->fx, fz = s->fz, eye[3], at[3], fov = pp->fov;
    if (mode == 2) {
        float h = pp->eye, ah = pp->fwd, pitch = pp->pitch * 3.14159265f / 180;
        eye[0] = px + fx * ah; eye[1] = h; eye[2] = pz + fz * ah;
        at[0] = eye[0] + fx * cosf(pitch); at[1] = h - sinf(pitch); at[2] = eye[2] + fz * cosf(pitch);
    } else {
        float d = pp->dist, h = pp->h, la = pp->look, lh = pp->lookh;
        // 俯仰限制：画面上沿至少在水平线下 3°，地平线不进画面——原版俯视，没有天空，透视下地平线以上全黑。竖直半视角按本帧纵横比算，不够就把镜头抬高
        // Pitch limit: the top edge of the picture stays at least 3 degrees below the horizon, so the horizon never enters the picture -- the original is
        // top-down and has no sky, so everything above the horizon is black in perspective. The vertical half-angle follows this frame's aspect ratio; raise the camera when it falls short
        float tvh = tanf(fov * 3.14159265f / 360) * (pr[5] != 0 ? fabsf(pr[0] / pr[5]) : 9.0f / 16), hmin = lh + (d + la) * tanf(atanf(tvh) + 3 * 3.14159265f / 180);
        if (h < hmin) h = hmin;
        eye[0] = px - fx * d; eye[1] = h; eye[2] = pz - fz * d;
        at[0] = px + fx * la; at[1] = lh; at[2] = pz + fz * la;
    }
    // 视图 +z = 朝后；右 = 上(0,1,0) × 后；上 = 后 × 右
    // View +z = backwards; right = up(0,1,0) x back; up = back x right
    float b[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] }, bl = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    for (int i = 0; i < 3; i++) b[i] /= bl;
    float r[3] = { b[2], 0, -b[0] }, rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
    r[0] /= rl; r[2] /= rl;
    float u[3] = { b[1] * r[2] - b[2] * r[1], b[2] * r[0] - b[0] * r[2], b[0] * r[1] - b[1] * r[0] };
    float V[16] = { r[0], u[0], b[0], 0,  r[1], u[1], b[1], 0,  r[2], u[2], b[2], 0,
        -(eye[0] * r[0] + eye[1] * r[1] + eye[2] * r[2]), -(eye[0] * u[0] + eye[1] * u[1] + eye[2] * u[2]), -(eye[0] * b[0] + eye[1] * b[1] + eye[2] * b[2]), 1 };
    // 水平视角 fov，纵横比照原投影；反向 Z：视图 z = -n → 1，-F → 0
    // Horizontal field of view fov, aspect ratio taken from the original projection; reversed Z: view z = -n -> 1, -F -> 0
    float n = pp->near_, F = 3000, sx = 1.0f / tanf(fov * 3.14159265f / 360), sy = sx * (pr[0] != 0 ? pr[5] / pr[0] : 16.0f / 9);
    float A = n / (F - n), B = n * F / (F - n);
    float P[16] = { sx, 0, 0, 0,  0, sy, 0, 0,  0, 0, A, -1,  0, 0, B, 0 }, iV[16], iP[16];
    xbg_cam_m4_mul(V, P, vp);
    xbg_cam_m4_inv(V, iV);
    xbg_cam_m4_inv(P, iP);
    xbg_cam_m4_inv(vp, ivp);
    memcpy(dst + 80, V, 64); memcpy(dst + 144, iV, 64); memcpy(dst + 208, P, 64); memcpy(dst + 272, iP, 64); memcpy(dst + 336, vp, 64); memcpy(dst + 400, ivp, 64);
    // 上一帧填我们改过的那份，运动矢量才对
    // The previous frame gets our modified one, so motion vectors stay right
    memcpy(dst + 464, s->have && s->vp_prev[15] != 0 ? s->vp_prev : vp, 64);
    memcpy(s->vp_last, vp, 64);
    // +592 = (远, 近, ?, 正交标志 1)。着色器按标志选深度还原：正交 距离 = 远 + d·(近 − 远)；透视 距离 = x / max(d − y, ε)。本投影 d = B/距离 − A → 距离 = B / (d + A)
    // +592 = (far, near, ?, orthographic flag 1). The shaders pick depth reconstruction by the flag: orthographic distance = far + d*(near - far);
    // perspective distance = x / max(d - y, eps). For this projection d = B/distance - A -> distance = B / (d + A)
    { int32_t zero = 0; float xy[2] = { B, -A }; memcpy(dst + 592, xy, 8); memcpy(dst + 604, &zero, 4); }
    for (int i = 0; i < 3; i++) { s->eye[i] = eye[i]; s->fwd[i] = -b[i]; }
    s->have_eye = 1;
}
// 片元槽 10 的场景常量跟着改：镜头位置、视线；线性深度起点原版 550（正交镜头离地很远），透视镜头从 0 起
// The scene constants of fragment slot 10 follow: camera position, sight line; the linear depth origin is 550 in the original (the orthographic camera
// sits far above the ground) and 0 for the perspective camera
static inline void xbg_cam_build10(const xbg_cam_state *s, uint8_t *dst) {
    if (!s->have_eye) return;
    memcpy(dst + 16, s->eye, 12);
    memcpy(dst + 32, s->fwd, 12);
    float zero = 0;
    memcpy(dst + 848 + 8, &zero, 4);
}
