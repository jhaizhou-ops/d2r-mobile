#!/usr/bin/env python3
# 从 iOS 的 App 图标生成 Android 各密度启动图标（复用同一份美术，不另做一套）。
# 源：dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png（1024×1024，iOS 的 App Store 规格）
# 出：<res>/mipmap-{mdpi,hdpi,xhdpi,xxhdpi,xxxhdpi}/ic_launcher.png
#
# 为什么是"构建时生成"而不是把 PNG 提交进仓库：与仓库里其它原版衍生资源同一个惯例（见 dm/README.md
# 「从你的游戏文件生成资源」）——生成物不进版本库，只留生成脚本。图标本身是 1024² 的源图，各处密度都由它缩出来，
# 提交五份冗余 PNG 只会让 diff 变脏。
#
# Generate the Android launcher icons at every density from the iOS app icon (reuse the same artwork, not a second set).
# Source: dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png (1024x1024, the iOS App Store spec)
# Output: <res>/mipmap-{mdpi,hdpi,xhdpi,xxhdpi,xxxhdpi}/ic_launcher.png
#
# Why generate at build time instead of committing the PNGs: same convention as the other assets derived from the original
# game (see dm/README.md, "Generate assets from your game files") -- generated files stay out of the repository and only the
# generator is kept. The artwork is one 1024^2 source and every density is a downscale of it; committing five redundant PNGs
# would only make the diffs dirty.
import os
import sys

from PIL import Image, ImageDraw

# Android 各密度下启动图标的边长（48dp）：
# 48dp @ mdpi 1x = 48, hdpi 1.5x = 72, xhdpi 2x = 96, xxhdpi 3x = 144, xxxhdpi 4x = 192
# The launcher icon edge length at each Android density (48dp):
# 48dp @ mdpi 1x = 48, hdpi 1.5x = 72, xhdpi 2x = 96, xxhdpi 3x = 144, xxxhdpi 4x = 192
DENSITIES = [
    ("mdpi", 48),
    ("hdpi", 72),
    ("xhdpi", 96),
    ("xxhdpi", 144),
    ("xxxhdpi", 192),
]


def main(argv):
    if len(argv) != 3:
        sys.exit("用法：gen_icons.py <AppIcon1024.png> <res 目录>")
    src, res = argv[1], argv[2]
    if not os.path.isfile(src):
        sys.exit(f"缺源图：{src}")

    img = Image.open(src).convert("RGBA")
    if img.size != (1024, 1024):
        # 不硬要求正好 1024：只报一句，照常按 1024 的口径缩（图标是方形，边长不同也能用）
        # Do not require exactly 1024: note it and scale by the 1024 spec anyway (the icon is square, another edge works too)
        print(f"注意：源图是 {img.size[0]}x{img.size[1]}，非 1024x1024，仍按目标密度缩")

    written = 0
    for name, px in DENSITIES:
        out_dir = os.path.join(res, f"mipmap-{name}")
        os.makedirs(out_dir, exist_ok=True)
        # LANCZOS：缩小时质量最好，不会像 NEAREST/BILINEAR 那样把细密的哥特纹路缩糊
        # LANCZOS: best quality when downscaling; NEAREST/BILINEAR would smear the fine gothic detail
        img.resize((px, px), Image.LANCZOS).save(os.path.join(out_dir, "ic_launcher.png"), optimize=True)
        # 同一尺寸再出一张圆形版，给 android:roundIcon 用（见下面的 make_round）
        # A round variant at the same size goes to android:roundIcon (see make_round below)
        make_round(img, px).save(os.path.join(out_dir, "ic_launcher_round.png"), optimize=True)
        written += 1
    print(f"启动图标：{written} 档密度（方+圆各一张）→ {res}")


def make_round(img, px):
    """把方图做成圆形图标：整张源图先放大到 1/0.90，再从中心裁出 S×S，最后套圆形遮罩。四角透明，边缘无接缝。

    这里是几版试出来的结论，写下来免得以后又试一遍：
    - 直接把方图贴到黑圆盘上（logo 缩到 72%）→ 圆的中间会露出一个**方块边界**：源图的背景是近黑
      (9,6,6)，比纯黑 (0,0,0) 亮，两者交界一眼可见。这是最丑的一版，别用。
    - 直接圆裁不缩放 → 主体（四角尖顶）对角跨度约 645px > 内切圆半径 512px，尖顶被切掉。
    - 放大到 1/0.90 再裁 → 背景一直延伸到圆外，没有方块边界；内容整体缩到 90%，MOBILE 字样与
      四角尖顶都还完整。这是采用的一版（90% 是拿 86/90/94/100 四档比出来的：86% 太挤边、
      94% 开始切尖顶）。

    Turn the square artwork into a round icon: scale the whole source up to 1/0.90, crop S x S from the centre, then apply
    a circular mask. Transparent corners, no seam at the rim.

    These are the variants that were tried, recorded so nobody has to try them again:
    - Pasting the square onto a black disc (logo at 72%) shows a SQUARE SEAM through the middle of the circle: the source
      background is near-black (9,6,6) and reads clearly against pure black (0,0,0). The ugliest variant; do not use it.
    - A straight circular crop with no scaling cuts the main subject (the spires at the corners, ~645px diagonal) because it
      exceeds the inscribed circle's 512px radius.
    - Scaling to 1/0.90 before cropping lets the background run all the way out to the rim with no seam, while the content sits
      at 90% and keeps both the MOBILE lettering and the corner spires intact. This is the variant used (90% came from
      comparing 86/90/94/100: 86% crowds the edge, 94% starts clipping the spires).
    """
    S = px
    # 放大后的画布边长。用 round 而非 int，免得小尺寸下 1/0.9 被截断成整数后留出 1px 透明边。
    # Canvas edge after scaling. round, not int, so that on small sizes 1/0.9 does not truncate and leave a 1px transparent rim.
    big = max(S, int(round(S / 0.90)))
    canvas = Image.new("RGBA", (big, big), (0, 0, 0, 255))   # 纯黑垫底，只在放大后仍盖不满时可见 | solid black backing
    canvas.paste(img.resize((big, big), Image.LANCZOS), (0, 0))
    off = (big - S) // 2
    canvas = canvas.crop((off, off, off + S, off + S))

    # 圆形遮罩：用 4x 超采样画圆再缩回来，边缘才有抗锯齿（直接画椭圆边缘是硬台阶）
    # Circular mask: draw at 4x and downscale so the rim is antialiased (a directly drawn ellipse has hard steps)
    ss = 4
    m = Image.new("L", (S * ss, S * ss), 0)
    ImageDraw.Draw(m).ellipse((0, 0, S * ss - 1, S * ss - 1), fill=255)
    m = m.resize((S, S), Image.LANCZOS)

    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(canvas, (0, 0), m)
    return out


if __name__ == "__main__":
    main(sys.argv)
