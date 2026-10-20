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

from PIL import Image

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
        written += 1
    print(f"启动图标：{written} 档密度 → {res}")


if __name__ == "__main__":
    main(sys.argv)
