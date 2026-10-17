#!/usr/bin/env bash
# 打 Android 安装包：本树 Java/清单 + 本树 CMake 产物 + 本树改写模块 → 仓库内 dm/android/build/apk/。
# 全部输入输出都在本仓库（build/ 不进版本库）；游戏数据（game.romfs 等）只在部署时从数据区推送，不进包。
set -euo pipefail
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
BUILD=$SRC/dm/android/build
MODULES=${DM_MODULES:-$BUILD/modules-touch}
CMAKE=$BUILD
OUT=$BUILD/apk
SDK=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
PLATFORM_JAR=$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)/android.jar
JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}
AAPT2="$BT/aapt2"; D8="$BT/d8"; ZIPALIGN="$BT/zipalign"; APKSIGNER="$BT/apksigner"

[ -f "$CMAKE/libd2r_mobile_android.so" ] || { echo "先编 native：cd dm/android/build && ninja" >&2; exit 2; }
[ -f "$MODULES/main.patched.bin" ] || { echo "缺模块产物 $MODULES（先跑 dm/android/tools/build_modules.sh）" >&2; exit 2; }

rm -rf "$OUT"; mkdir -p "$OUT"/{classes,dex,gen,assets/modules,stage/lib/arm64-v8a}

# 1) assets: 模块 + 清单 + 底栏 v4 素材（gen_assets 产物）
python3 "$SRC/dm/android/tools/gen_module_manifest.py" "$MODULES" "$OUT/assets"
V4=${DM_HUD_V4_DIR:-$SRC/dm/ios/Gen/assets/NativeHUD/v4/hud_v4_ui}
[ -d "$V4" ] && { mkdir -p "$OUT/assets/hud_v4_ui"; cp "$V4"/* "$OUT/assets/hud_v4_ui/"; echo "hud_v4_ui: $(ls "$OUT/assets/hud_v4_ui" | wc -l) 个文件"; } || echo "警告：没有 hud_v4_ui 素材，底栏 v4 不画"

# 2) 清单 + 资源链接
"$AAPT2" link -o "$OUT/base.apk" -I "$PLATFORM_JAR" \
    --manifest "$SRC/dm/android/AndroidManifest.xml" -A "$OUT/assets" --java "$OUT/gen"

# 3) Java → dex
find "$SRC/dm/android/src" "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
"$JAVA_HOME/bin/javac" -encoding UTF-8 -nowarn -Xlint:-options -source 17 -target 17 \
    -classpath "$PLATFORM_JAR" -d "$OUT/classes" @"$OUT/sources.txt"
mapfile -t CLASSES < <(find "$OUT/classes" -name '*.class')
"$D8" --lib "$PLATFORM_JAR" --min-api 29 --output "$OUT/dex" "${CLASSES[@]}"

# 4) 组装
cp "$OUT/base.apk" "$OUT/dodola.apk"
cp "$OUT/dex/classes.dex" "$OUT/stage/classes.dex"
cp "$CMAKE/libd2r_mobile_android.so" "$OUT/stage/lib/arm64-v8a/"
python3 - "$OUT/dodola.apk" "$OUT/stage" <<'EOF'
import os, sys, zipfile
apk, stage = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    for root, _d, files in os.walk(stage):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, stage))
print('added', len(zipfile.ZipFile(apk).namelist()), 'entries total')
EOF
unzip -l "$OUT/dodola.apk" | grep -E "classes.dex|\.so|AndroidManifest|module-manifest|patched.bin" | head -15

# 5) 对齐 + 签名
"$ZIPALIGN" -f -p 4 "$OUT/dodola.apk" "$OUT/dodola.aligned.apk"
KS=${DM_KEYSTORE:-$HOME/.android/debug.keystore}
"$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/dodola.signed.apk" "$OUT/dodola.aligned.apk"
"$APKSIGNER" verify "$OUT/dodola.signed.apk" && echo "signed OK"
echo "APK: $OUT/dodola.signed.apk ($(stat -c%s "$OUT/dodola.signed.apk") bytes)"
