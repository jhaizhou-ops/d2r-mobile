#version 450
// 替换游戏的 3D 放大着色器 ps_upsample_p（键 52aac1f2306b8fc5）：AMD FidelityFX FSR 1 的 EASU（12 点、按边缘方向的 Lanczos 近似），
// 再做 RCAS 式的限幅锐化（单遍：锐化的十字邻域取在源分辨率上，不是 FSR 原版第二遍的输出分辨率）。接口与原着色器逐项一致：
//   binding 22 = 常量块 c7：c[0].xy 把画面坐标换成源纹理坐标，c[1].xy 一个源像素，c[1].z 原版锐化强度，c[2] 源区域的夹取范围；
//   binding 10 = 3D 低分辨率画面；binding 16 = 原版逐点乘的系数（取 .r）；location 2 = 画面坐标；输出 alpha 恒 1。
// Replacement for the game's 3D upscale shader ps_upsample_p (key 52aac1f2306b8fc5): AMD FidelityFX FSR 1 EASU (12 taps,
// edge-directed Lanczos approximation), then RCAS-style limited sharpening (single pass: the sharpening cross is taken at source
// resolution, not at output resolution as in FSR's separate second pass). The interface matches the original item for item:
//   binding 22 = constant block c7: c[0].xy maps screen coordinates to source texture coordinates, c[1].xy one source texel,
//   c[1].z the original sharpening strength, c[2] the clamp range of the source region;
//   binding 10 = low-resolution 3D picture; binding 16 = the original per-pixel multiplier (.r); location 2 = screen coordinates; alpha is 1.
// The EASU and RCAS math below is adapted from AMD FidelityFX Super Resolution 1.0 (ffx_fsr1.h):
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice
// shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#ifndef RCAS
#define RCAS 1          // 0 = EASU only
#endif
#define SHARP 0.87      // RCAS sharpness = exp2(-0.2 stops)
layout(set = 0, binding = 22, std140) uniform C7 { vec4 c[4096]; } cb;
layout(set = 0, binding = 10) uniform sampler2D src;
layout(set = 0, binding = 16) uniform sampler2D mulTex;
layout(location = 2) in vec4 vUv;
layout(location = 0) out vec4 oColor;

ivec2 lo, hi;
vec3 tap(ivec2 p) { return texelFetch(src, clamp(p, lo, hi), 0).rgb; }
float luma(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC, cb_ = lC - lB;
    float lenX = max(abs(dc), abs(cb_)); lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;
    float dirX = lD - lB; dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0); lenX *= lenX; len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = max(abs(ec), abs(ca)); lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;
    float dirY = lE - lA; dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0); lenY *= lenY; len += lenY * w;
}
void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0, wA = lob * d2 - 1.0;
    wB *= wB; wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA; aC += c * w; aW += w;
}

void main() {
    vec2 size = vec2(textureSize(src, 0));
    vec2 p = min(vUv.xy * cb.c[0].xy, cb.c[2].zw);
    p = max(p, cb.c[2].xy);
    lo = ivec2(floor(cb.c[2].xy * size)); hi = max(lo, ivec2(ceil(cb.c[2].zw * size)) - 1);
    vec2 pp = p * size - 0.5; vec2 fp = floor(pp); pp -= fp; ivec2 f0 = ivec2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = tap(f0 + ivec2(0, -1)), c = tap(f0 + ivec2(1, -1));
    vec3 e = tap(f0 + ivec2(-1, 0)), f = tap(f0), g = tap(f0 + ivec2(1, 0)), h = tap(f0 + ivec2(2, 0));
    vec3 i = tap(f0 + ivec2(-1, 1)), j = tap(f0 + ivec2(0, 1)), k = tap(f0 + ivec2(1, 1)), l = tap(f0 + ivec2(2, 1));
    vec3 n = tap(f0 + ivec2(0, 2)), o = tap(f0 + ivec2(1, 2));
    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);
    vec2 dir = vec2(0.0); float len = 0.0;
    easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir; float dirR = dir2.x + dir2.y; bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : inversesqrt(dirR); dir.x = zro ? 1.0 : dir.x; dir *= dirR;
    len = len * 0.5; len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len, clp = 1.0 / lob;
    vec3 mn4 = min(min(f, g), min(j, k)), mx4 = max(max(f, g), max(j, k));
    vec3 aC = vec3(0.0); float aW = 0.0;
    easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    vec3 col = min(mx4, max(mn4, aC / aW));
#if RCAS
    {
        // RCAS：十字邻域取离采样点最近的源像素四周（源分辨率），按 FSR 的限幅公式求负瓣，不出光晕
        // RCAS: the cross is the four source texels around the nearest source texel (source resolution); FSR's limiter gives the negative lobe, no halos
        ivec2 q = f0 + ivec2(greaterThanEqual(pp, vec2(0.5)));
        vec3 rb = tap(q + ivec2(0, -1)), rd = tap(q + ivec2(-1, 0)), rf = tap(q + ivec2(1, 0)), rh = tap(q + ivec2(0, 1));
        vec3 mn = min(min(rb, rd), min(rf, rh)), mx = max(max(rb, rd), max(rf, rh));
        vec3 hitMin = min(mn, col) / (4.0 * max(mx, vec3(1.0 / 65536.0)));
        vec3 hitMax = (1.0 - max(mx, col)) / min(4.0 * mn - 4.0, vec3(-1.0 / 65536.0));
        vec3 lobeRGB = max(-hitMin, hitMax);
        float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * SHARP;
        col = (lobe * (rb + rd + rf + rh) + col) / (4.0 * lobe + 1.0);
    }
#endif
    oColor = vec4(col * texture(mulTex, vUv.xy).r, 1.0);
}
