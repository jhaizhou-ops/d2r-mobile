#!/bin/bash
# 构建、显式装机、按模式启动并拉回 Documents/<mode>.txt。用法：[DM_OUT=产物目录] [DM_NOINSTALL=1] run.sh build | run.sh install | run.sh <mode> [上限秒]
# 包缺省同时面向 iPhone 与 iPad；只要 iPhone：DM_DEVICE_FAMILY=1 ./run.sh build（设备族是构建设置，这里替你传给 xcodebuild）
# 产物与日志在 $DM_ROOT/run/ios/（不进仓）。真机按 bundle 停：--terminate-existing 只停本 App。
# Build, explicitly install, launch by mode and pull back Documents/<mode>.txt. Usage: [DM_OUT=output dir] [DM_NOINSTALL=1] run.sh build | run.sh install | run.sh <mode> [timeout seconds]
# The app targets both iPhone and iPad by default; for iPhone only: DM_DEVICE_FAMILY=1 ./run.sh build (device family is a build setting; this passes it to xcodebuild for you)
# Outputs and logs go to $DM_ROOT/run/ios/ (not in the repo). On device, stopping is by bundle: --terminate-existing stops only this App.
set -u
cd "$(dirname "$0")"
source tools/devlock.sh
source tools/asc_env.sh
source tools/local_env.sh   # 本机设备配置（不进仓，见 local.env.example）；出包不需要设备号，装机 / 启动时才核 | Local device configuration (not committed, see local.env.example); packaging does not need a device ID, it is only checked at install / launch
D=${DM_DEVICE:-}; B=${DM_BUNDLE_ID:?先在本机配置里设 DM_BUNDLE_ID（App 包名，见 dm/ios/local.env.example）}
R=${DM_ROOT:-$HOME/dm-work}/run/ios; mkdir -p "$R/out"
DD=${DM_IOS_DD:-$R/dd}
APP=$DD/Build/Products/Release-iphoneos/DmIOS.app
t(){ echo "[$(date +%T)] $*"; }
OUT=${DM_OUT:-$(bash "../tools/product.sh" path touch)}; MODS="main sdk"
# 可选：静态链接 FFmpeg，让 movie:: 走 VP8 解码并真正显示过场。默认仍为空播。
# Optional: statically link FFmpeg so movie:: decodes VP8 and displays cutscenes. The default no-picture backend stays unchanged.
MOVIE_FFMPEG=${DM_MOVIE_FFMPEG:-0}
MOVIE_PREFIX=${DM_MOVIE_FFMPEG_PREFIX:-${DM_ROOT:-$HOME/dm-work}/build/cache/dm/ffmpeg-ios/out}
MOVIE_LDFLAGS=
if [ "$MOVIE_FFMPEG" = 1 ]; then
  case "$MOVIE_PREFIX" in *' '*) t "🔴 DM_MOVIE_FFMPEG_PREFIX 不能含空格：${MOVIE_PREFIX}"; exit 1;; esac
  for library in libavformat libavcodec libswscale libavutil; do
    [ -f "$MOVIE_PREFIX/lib/$library.a" ] || { t "🔴 缺 ${MOVIE_PREFIX}/lib/$library.a；先运行 tools/build_ffmpeg_ios.sh"; exit 1; }
  done
  [ -f "$MOVIE_PREFIX/include/libavcodec/avcodec.h" ] || { t "🔴 FFmpeg 头文件不在 ${MOVIE_PREFIX}/include；重跑 tools/build_ffmpeg_ios.sh"; exit 1; }
  MOVIE_LDFLAGS="$MOVIE_PREFIX/lib/libavformat.a $MOVIE_PREFIX/lib/libavcodec.a $MOVIE_PREFIX/lib/libswscale.a $MOVIE_PREFIX/lib/libavutil.a"
  export MOVIE_FFMPEG MOVIE_PREFIX
  t "过场解码：静态 FFmpeg（${MOVIE_PREFIX}）"
else
  export MOVIE_FFMPEG=0
  unset MOVIE_PREFIX
fi
if [ "$1" = build ]; then
  # 生成：宿主源文件/头路径/框架（取自 dm/host/Makefile）+ 映像三段内嵌汇编与 -segprot + 装载表头/门表进 App 资源 dm/
  # Generate: host source files/header paths/frameworks (taken from dm/host/Makefile) + embedded assembly for the three image segments and -segprot + load table header/gate table into the App resources dm/
  mkdir -p Gen; for x in Gen/* Gen/.[!.]*; do [ -e "$x" ] && [ "$x" != Gen/assets ] && rm -rf "$x"; done; mkdir -p Gen/dm   # 每次重建 Gen，但留着 Gen/assets：原版衍生资源有自己的输入指纹与锁（tools/gen_assets.py），删了会和并行读它的功能用例撞（accept 红） | Rebuild Gen every time but keep Gen/assets: assets derived from the original game have their own input fingerprint and lock (tools/gen_assets.py); deleting them would collide with functional cases reading them in parallel (accept fails)
  # 产品包不监听任何端口：`listen(` 全库只许 main.m 日志端口一处，且监听线程只在 DM_LOG_PORT 测试开关下起
  # The product package listens on no port: across the whole codebase `listen(` is allowed only at the log port in main.m, and the listening thread only starts under the DM_LOG_PORT test switch
  nl=$(grep -rn 'listen(' Sources ../host --include='*.c' --include='*.m' | grep -v '^[^:]*:[0-9]*:\s*//' | wc -l | tr -d ' ')
  gl=$(grep -c 'if (log_port && !pthread_create(&lp, NULL, log_port_main' Sources/main.m)
  [ "$nl" = 1 ] && [ "$gl" = 1 ] || { t "🔴 端口监听核对失败：listen( 共 $nl 处（只许 1），开关守卫 $gl 处（须 1）"; exit 1; }
  # 原版衍生资源（加宽储藏箱、百宝囊框、选角面板、小地图终点、技能图集、底部 UI v4 …）不进仓库，每次从游戏文件生成（tools/gen_assets.py；dm/README.md「从你的游戏文件生成资源」）
  # Assets derived from the original game (widened stash, pouch frame, character select panel, minimap goal, skill atlas, bottom UI v4 ...) are not committed to the repository and are generated from game files every time (tools/gen_assets.py; dm/README.md 「从你的游戏文件生成资源」(Generate assets from your game files))
  python3 tools/gen_assets.py "${DM_ROMFS_SRC:-${DM_ROOT:-$HOME/dm-work}/data/switch/3.3.94012/Update/RomFS}" || { t "🔴 gen_assets 失败（见上）"; exit 1; }
  make -s -C ../host dm_vault_tab.h dm_hud_exp.h dm_bright_levels.inc ROMFS="${DM_ROMFS_SRC:-${DM_ROOT:-$HOME/dm-work}/data/switch/3.3.94012/Update/RomFS}" || { t "🔴 宿主表头生成失败（见上）"; exit 1; }
  python3 tools/gen_build.py ../host Gen > Gen/dm.xcconfig || { t "🔴 gen_build 失败"; exit 1; }
  F=$(python3 tools/gen_embed.py $OUT Gen/embed.S $MODS) || { t "🔴 gen_embed 失败"; exit 1; }
  echo "OTHER_LDFLAGS = \$(inherited) \$(DM_FRAMEWORKS) $F $MOVIE_LDFLAGS" >> Gen/dm.xcconfig
  for m in $MODS; do cp $OUT/$m.fixups.tsv $OUT/$m.gates.tsv Gen/dm/; done
  # 启动块清单（缺省开预取：宿主到产物目录 = App 内 dm/ 找 bootlist.tsv）+ main.cuts.tsv（宿主据此核清单构建号）；产物没有就不带
  # Boot block list (prefetch on by default: the host looks for bootlist.tsv in the build output directory = dm/ inside the App) + main.cuts.tsv (the host uses it to check the list's build ID); not included if the build output lacks them
  for f in bootlist.tsv main.cuts.tsv; do [ -f $OUT/$f ] && cp $OUT/$f Gen/dm/; done
  # 图形：着色器映射表（与 Mac 同一份）+ iOS 版 metallib 缓存（PC 链 iphoneos 目标，已编，覆盖映射表 6,938/6,938；APFS 克隆不占空间）
  # Graphics: shader mapping table (the same copy as on Mac) + iOS metallib cache (PC chain iphoneos target, already compiled, covers 6,938/6,938 of the mapping table; APFS clones take no space)
  cp ../gfx/nvn/shader_map.tsv Gen/dm/   # 映射表（B 路线只用来查阶段） | Mapping table (route B only uses it to look up the stage)
  cp ../gfx/nvn/pso_manifest.tsv Gen/dm/   # 预热种子清单（与映射表同目录） | Warm-up seed list (same directory as the mapping table)
  cp ../gfx/nvn/c1_full.tsv Gen/dm/   # c1 运行时下标未证有界的着色器清单（与映射表同目录；缺失则全部 c1 独占 64 KiB 并报红） | List of shaders whose c1 runtime index is not proven bounded (same directory as the mapping table; if missing, every c1 gets an exclusive 64 KiB and a failure is reported)
  # 着色器：B 路线（Switch 原版翻译）iOS 版 <键>_b.metallib + <键>_b.json，缺失回放层报红、不回退。
  # 出包：J=10 IOSOUT=<本目录> python3 ../gfx/tools/dmsh_build.py $DM_ROOT/build/cache/dm/dmsh <Mac 缓存目录>（编译口径 tools/ios_metal.py）
  # 宿主只从 DM_MSL_DIR/b-<V>/ 加载（V = dm/gfx/nvn/dmsh_chain.h 的翻译链版本），App 内放成 msl/b-<V>/，只带当前版本
  # Shaders: route B (translated from the Switch original) iOS <key>_b.metallib + <key>_b.json; if missing, the replay layer reports a failure and does not fall back.
  # To build: J=10 IOSOUT=<this directory> python3 ../gfx/tools/dmsh_build.py $DM_ROOT/build/cache/dm/dmsh <Mac cache dir> (compile settings in tools/ios_metal.py)
  # The host loads only from DM_MSL_DIR/b-<V>/ (V = the translation chain version in dm/gfx/nvn/dmsh_chain.h); inside the App it is placed as msl/b-<V>/, carrying only the current version
  MSL=${DM_MSL_IOS:-${DM_ROOT:-$HOME/dm-work}/build/ios/dmsh_b}; V=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' ../gfx/nvn/dmsh_chain.h)
  [ -n "$V" ] && [ -d "$MSL/b-$V" ] && ls "$MSL/b-$V" | grep -q "_b.metallib$" || { t "🔴 缺 iOS 版 B 着色器 $MSL/b-${V}（见上一行出包命令；V=${V}）"; exit 1; }
  mkdir -p Gen/msl && cp -cR "$MSL/b-$V" Gen/msl/
  # 资源直接下载（直装包，Sources/asset_dl.m）：地址文件列表（仓库里的 asset_pointers.txt）+ 出包时的缺省下载根地址（DM_ASSET_BASE，tools/adhoc.sh 传隧道地址；没传就不带）
  # Direct asset download (direct-install package, Sources/asset_dl.m): URL file list (asset_pointers.txt in the repository) + the default download root URL at packaging time (DM_ASSET_BASE; tools/adhoc.sh passes the tunnel URL; omitted if not passed)
  cp asset_pointers.txt Gen/dm/; if [ -n "${DM_ASSET_BASE:-}" ]; then echo "$DM_ASSET_BASE" > Gen/dm/asset_base.txt; else rm -f Gen/dm/asset_base.txt; fi   # 没给就删掉上次的：生成物只跟本次输入走（原先只写不删，同一棵树先出带地址的包、再出不带的会沿用旧地址） | if not given, delete the previous one: the generated file follows only this run's input (it used to be written but never deleted, so building a package with an address and then one without in the same tree kept the old address)
  # 老机型补充包的自动下载根地址（DM_ASTC_PACK_BASE，<根>/manifest.json + <根>/data/…；没传 = 只能玩家用「文件」拷，见 Sources/asset_dl.m）
  # Root URL for automatic download of the Legacy Device Pack (DM_ASTC_PACK_BASE, <root>/manifest.json + <root>/data/...; not passed = players can only copy it with the Files app, see Sources/asset_dl.m)
  rm -f Gen/dm/astc_pack_base.txt; [ -n "${DM_ASTC_PACK_BASE:-}" ] && echo "$DM_ASTC_PACK_BASE" > Gen/dm/astc_pack_base.txt
  rm -rf Gen/save_ref; mkdir -p Gen/save_ref; if [ -d "${DM_ROOT:-$HOME/dm-work}/data/switch/save_ref/0000000000000001/0" ]; then cp -R "${DM_ROOT:-$HOME/dm-work}/data/switch/save_ref/0000000000000001/0/." Gen/save_ref/ && chmod -R u+w Gen/save_ref; elif [ "${DM_REQUIRE_SAVES:-0}" = 1 ]; then t "🔴 正式出包要带标准存档（DM_REQUIRE_SAVES=1），但 ${DM_ROOT:-$HOME/dm-work}/data/switch/save_ref/0000000000000001/0 不在"; exit 1; fi   # 标准存档（DM_SAVE_FRESH=1 用，80 KB） | Standard save (used with DM_SAVE_FRESH=1, 80 KB)
  rm -rf Gen/save_ref_warlock; mkdir -p Gen/save_ref_warlock; if [ -d "${DM_ROOT:-$HOME/dm-work}/data/switch/save_ref_warlock/0000000000000001/0" ]; then cp -R "${DM_ROOT:-$HOME/dm-work}/data/switch/save_ref_warlock/0000000000000001/0/." Gen/save_ref_warlock/ && chmod -R u+w Gen/save_ref_warlock; elif [ "${DM_REQUIRE_SAVES:-0}" = 1 ]; then t "🔴 正式出包要带标准存档（DM_REQUIRE_SAVES=1），但 ${DM_ROOT:-$HOME/dm-work}/data/switch/save_ref_warlock/0000000000000001/0 不在"; exit 1; fi   # 术士存档（DM_SAVE_FRESH=warlock，浸泡 / cvd 回放用，92 KB） | Warlock save (DM_SAVE_FRESH=warlock, used for soak / cvd replay, 92 KB)
  # RomFS 覆盖层（底部 UI v4：Gen/assets/NativeHUD/v4 → 借用路径底图 + 手柄 HUD 布局），App 启动按 DM_ROMFS_OVERLAY 先查本层
  # 老机型补充包（DM_ASTC_PACK = 补充包目录，dm/tools/astc_pack.py 的产物）：设了才另出一套 ASTC 版覆盖层（Gen/romfs_overlay_astc*），不支持 BC 的机型用；没设（如 TestFlight）这两个目录为空
  # RomFS overlay (bottom UI v4: Gen/assets/NativeHUD/v4 → base image at the borrowed path + controller HUD layout); at App launch this layer is looked up first according to DM_ROMFS_OVERLAY
  # Legacy Device Pack (DM_ASTC_PACK = Legacy Device Pack directory, the build output of dm/tools/astc_pack.py): only when set is an additional ASTC overlay produced (Gen/romfs_overlay_astc*), used by devices that do not support BC; when unset (e.g. TestFlight) these two directories are empty
  python3 tools/gen_romfs_overlay.py "${DM_ROMFS_SRC:-${DM_ROOT:-$HOME/dm-work}/data/switch/3.3.94012/Update/RomFS}" Gen/romfs_overlay > /dev/null || { t "🔴 gen_romfs_overlay 失败"; exit 1; }
  # 游戏的配置文件只认 App 自带的这份（玩家那边的资源只核档案文件，见 host/romfs_validate.c）：每一套覆盖层里都得有，缺一套出包失败
  # The game only accepts the config file bundled with the App (on the player's side only the archive files of the resources are checked, see
  # host/romfs_validate.c): every overlay set must contain one, and packaging fails if any set lacks it
  for o in Gen/romfs_overlay Gen/romfs_overlay_variants/* Gen/romfs_overlay_astc Gen/romfs_overlay_astc_variants/*; do
    [ -d "$o/data" ] || continue; grep -q '^vfs-root = ' "$o/data/.build.config" 2>/dev/null || { t "🔴 覆盖层 $o 里没有可用的配置文件 data/.build.config"; exit 1; }
  done
  [ -f Gen/romfs_overlay/data/.build.config ] || { t "🔴 没生成覆盖层 Gen/romfs_overlay"; exit 1; }
  # 免费的开发者账号（个人团队）签不了这几项权限：DM_FREE_ACCOUNT=1 时从工程描述里去掉再生成。去掉后少的是 iCloud 存档同步、
  # 持续执行模式与更高的内存上限；游戏照常运行（地址预留已按不带扩展虚拟寻址也放得下来定，见 host/hle_svc.c）
  # 两个开关各自独立、可以叠：先按免费档去掉签不了的，再按老接口版改部署目标（公开用户多是免费账号，「免费账号 + iOS 18」要两样都生效）。都没设 = 原样生成
  # A free developer account (personal team) cannot sign these entitlements: with DM_FREE_ACCOUNT=1 they are removed from the project description before generating. What is lost is iCloud save sync,
  # sustained execution mode and the higher memory limit; the game runs as usual (the address reservation is sized to fit without extended virtual addressing as well, see host/hle_svc.c)
  # The two switches are independent and can be combined: first remove what the free tier cannot sign, then change the deployment target for the older-API build (most public users have free accounts, and "free account + iOS 18" needs both to take effect). Neither set = generate as is
  SPEC=project.yml; FREE=
  if [ "${DM_FREE_ACCOUNT:-0}" = 1 ]; then
    python3 tools/project_free.py project.yml project_free.yml || { t "🔴 DM_FREE_ACCOUNT：免费档的工程描述没生成（见上）"; exit 1; }   # 去掉签不了的六行权限与用不上的资源下载扩展 | remove the six entitlement lines it cannot sign and the asset download extension it has no use for
    SPEC=project_free.yml; FREE=free
  fi
  if [ "${DM_IOS_M3:-0}" = 1 ]; then   # 老接口版：图形后端不用 Metal 4，部署目标 iOS 18.0（tools/project_m3.py）；缺省不走这里 | older-API build: the graphics backend does not use Metal 4, deployment target iOS 18.0 (tools/project_m3.py); not taken by default
    python3 tools/project_m3.py $SPEC project_m3.yml $FREE || { rm -f project_free.yml; t "🔴 DM_IOS_M3：老接口版的工程描述没生成（见上）"; exit 1; }
    SPEC=project_m3.yml
  fi
  if [ $SPEC = project.yml ]; then
  xcodegen generate -q >/dev/null || { t "🔴 xcodegen 失败"; exit 1; }
  else
    xcodegen generate -q --spec $SPEC >/dev/null; XG=$?; rm -f project_free.yml project_m3.yml; [ $XG = 0 ] || { t "🔴 xcodegen 失败"; exit 1; }
    [ -z "$FREE" ] || t "按免费账号生成：不带 iCloud / 持续执行 / 增加内存上限 / 扩展虚拟寻址，也不带资源下载扩展（只占一个应用标识）"
    [ "${DM_IOS_M3:-0}" != 1 ] || t "按老接口版生成：部署目标 iOS 18.0、图形后端不用 Metal 4"
  fi
  [ "${DM_GEN_ONLY:-}" = 1 ] && { t "生成完成（DM_GEN_ONLY）"; exit 0; }   # tools/testflight.sh 只要生成，自己 archive | tools/testflight.sh only needs the generation step and does the archive itself
  asc_auth   # App Store Connect API 密钥（有就带上，供签名自动注册 App Group 等能力；见 tools/asc_env.sh） | App Store Connect API key (included if present, so signing can auto-register capabilities such as the App Group; see tools/asc_env.sh)
  # 开发包带上被测提交的标识（真机收数时把包里的标识读出来核对，不靠「锁内装过包」推断）：构建号 = HEAD 的提交时间戳，
  # 启动日志首行「boot: 版本 … 构建 N」原样打出来，tools/uireg/run_dev.sh 收数时核。工作区有未提交改动 = 认不出是哪个提交 → 构建号留 1，这种包不能用于放行。
  # 只影响这条开发包构建命令：正式包（tools/testflight.sh、tools/adhoc.sh）与 Mac 包（tools/smoke_mac.sh）带 DM_GEN_ONLY=1 进来，上面一行就退出了，各自的构建号不经过这里。
  # Development packages carry the identity of the commit under test (when collecting results from a real device, the identity is read out of the package
  # and checked, rather than inferred from "a package was installed while holding the lock"): build number = the commit timestamp of HEAD,
  # printed verbatim in the first line of the boot log ("boot: version ... build N") and checked by tools/uireg/run_dev.sh when collecting results.
  # Uncommitted changes in the working tree = the commit cannot be identified → the build number stays 1, and such a package cannot be used for release
  # sign-off.
  # This affects only this development package build command: release packages (tools/testflight.sh, tools/adhoc.sh) and the Mac package
  # (tools/smoke_mac.sh) come in with DM_GEN_ONLY=1 and exit on the line above, so their build numbers do not pass through here.
  BN=$(git log -1 --format=%ct 2>/dev/null); [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ] && BN=1; BN=${BN:-1}
  [ "$BN" = 1 ] && t "⚠ 工作区有未提交改动（或读不到提交）：开发包构建号 = 1，不能用于放行" || t "开发包构建号 ${BN}（提交 $(git log -1 --format=%h | cut -c1-9) 的提交时间戳）"
  # 构建目标：平常是「通用 iOS 设备」。免费账号（个人团队）名下起初没有任何设备，苹果不给生成描述文件
  # （「Your team has no devices from which to generate a provisioning profile」，真拿免费账号签时实测撞到）——
  # 所以免费档改用连着的那台手机做目标（DM_IOS_UDID），构建时 Xcode 会把它登记到团队名下。付费账号不受影响，照旧用通用目标。
  # Build destination: normally "generic iOS device". A free account (personal team) initially has no devices under its name, and Apple will not generate a provisioning profile
  # ("Your team has no devices from which to generate a provisioning profile", hit when actually signing with a free account) --
  # so the free mode uses the connected phone as the destination instead (DM_IOS_UDID), and during the build Xcode registers it under the team. Paid accounts are unaffected and keep using the generic destination.
  DEST='generic/platform=iOS'
  if [ "${DM_FREE_ACCOUNT:-0}" = 1 ]; then
    if [ -n "${DM_IOS_UDID:-}" ]; then DEST="id=$DM_IOS_UDID"; t "免费账号：用已连接的设备做构建目标（先把手机用数据线连上并解锁）"
    else t "⚠ 免费账号第一次构建要把手机连上，并在本机配置里填 DM_IOS_UDID（见 local.env.example）；没填，先按通用目标试"; fi
  fi
  xcodebuild -project DmIOS.xcodeproj -scheme DmIOS -configuration Release -destination "$DEST" \
    -derivedDataPath "$DD" -allowProvisioningUpdates ${ASC_AUTH[@]+"${ASC_AUTH[@]}"} DM_BUILD_NUMBER=$BN ${DM_DEVICE_FAMILY:+DM_DEVICE_FAMILY=$DM_DEVICE_FAMILY} build > "$R/build.log" 2>&1 || { t "🔴 构建失败"; grep -E "error:" "$R/build.log" | head -5
      grep -q 'no devices from which to generate a provisioning profile' "$R/build.log" && t "原因：这个团队名下还没有设备。把手机用数据线连上并解锁，在本机配置里填 DM_IOS_UDID、DM_FREE_ACCOUNT=1，再构建一次"; exit 1; }
  codesign -d --entitlements :- $APP 2>/dev/null | plutil -p - ; t "构建完成"
  # DM_NOINSTALL 保持兼容；build 现在本来就只编译，不触碰设备。
  # DM_NOINSTALL is kept for compatibility; build now only compiles anyway and does not touch the device.
  t "构建完成"; exit 0
fi
if [ "$1" = install ]; then
  local_env_need DM_DEVICE || exit 1
  devlock_acquire "${SMOKE_OWNER:-${ACCEPT_OWNER:-run.sh install}}"
  trap devlock_release EXIT
  xcrun devicectl device install app --device $D "$APP" > $R/install.log 2>&1 || { t "🔴 装机失败"; tail -5 $R/install.log; exit 1; }
  t "装机完成"; exit 0
fi
local_env_need DM_DEVICE || exit 1
M=$1; CAP=${2:-60}; O=$R/out/$M.txt; rm -f $O
devlock_acquire "${SMOKE_OWNER:-${ACCEPT_OWNER:-run.sh $M}}"
trap devlock_release EXIT
xcrun devicectl device process launch --device $D --terminate-existing $B -mode $M > $R/launch_$M.log 2>&1
grep -q Launched $R/launch_$M.log || { t "🔴 启动失败"; tail -8 $R/launch_$M.log; exit 1; }
pull(){ xcrun devicectl device copy from --device $D --domain-type appDataContainer --domain-identifier $B --source Documents/$M.txt --destination $O >/dev/null 2>&1; }
alive(){ xcrun devicectl device info processes --device $D 2>/dev/null | grep -q "DmIOS.app/DmIOS"; }
s=$SECONDS; sleep 3
while :; do pull; grep -q DONE $O 2>/dev/null && { t "$M 完成"; cat $O; exit 0; }
  alive || { sleep 1; pull; t "$M 进程已退出"; cat $O 2>/dev/null; exit 0; }
  [ $((SECONDS - s)) -gt $CAP ] && { t "🔴 $M 超 ${CAP}s"; cat $O 2>/dev/null; exit 1; }; sleep 3; done
