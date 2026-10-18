# 同步上游：做法与记录（最近一次 2026-10-18）

> **这份文档是什么（人话）**：每跟进一次上游（`jhaizhou-ops/d2r-mobile`）就更新一次——这次同步了什么、冲突怎么解的、合并后必须跟着改什么、哪些还没验。
> **给谁看**：下一次做同步的人（含 AI）。照着「§6 下次怎么同步」走，别从头再摸一遍。

---

## 1. 这次同步了什么

- 上游 **10 个提交**，到 `9393b00`（含 tag `v1.0.18`）。我们当时领先 89 个提交。
- 合并方式：**merge，不 rebase**（领先 89 条，rebase 要解 89 次）。合并提交 `7185abd`；合并前状态备份在分支 `sync-backup-2026-10-18`。
- 上游这批的主要内容：
  - `46cb318` 新触摸外观与操作（`DmTouchControlArt.h`、`DmTouchOverlay.m`、新美术 `ControlArt/dm_mobile_socket.png`、`dm_touch_controls.c/h` 新布局与 API、自定义页新增「换组键在上」`touch_swap_above`）；
  - `6edec84` 空技能位边缘提亮（同上那套外观）；
  - `86b1315` nvn 图像步长修复（新 op `OP_COPY_IMAGE_STRIDE` + `nvn_copy_pitch.h`）；
  - `b84657b` 加载器保留未解析弱导入；`dc53e7a` 旧 CommitSaveData 走事务提交；`098d9f5` Mac 鼠标按下延迟；
  - `21cf97b` iPhone 原生全屏与受支持游戏启动；`3af961d` 打包留签名区；`9393b00` iOS 音频与其他 App 混音；README 两条。

## 2. 两个真冲突（都很小，两边都留）

| 文件 | 上游 | 我们 | 解法 |
|---|---|---|---|
| `dm/gfx/nvn/nvn_obj.h` | 枚举加 `OP_COPY_IMAGE_STRIDE` | `ANDROID-BEGIN/END` 区段（不在同一位置） | 上游那项插在 `OP_CLEAR_TEX` 之后、我们区段之前——正是文件注释里定的规矩 |
| `dm/gfx/nvn/hle_nvn.c` | 加 `nvnCommandBufferSetCopyImageStride` | `#ifdef __ANDROID__` 的 WaitSync / FenceSync / WaitEvent | 两边都留，上游那行紧跟 SetCopyRowStride |

其余双方都改过的 8 个文件（`dm_cursor.c`、`hle.c`、`hle_svc.c`、`loader.c`、`ui_text.tsv`、`dm_i18n_tab.h`、`touch_layout.py`、`patch_nso.py`）自动合上。

## 3. 合并后必须跟着改的（编译器不会拦你，但会挂）

1. **`nvn_vk.c` 补 `OP_COPY_IMAGE_STRIDE`**：操作码 switch 的 `default:` 是 `die("unimplemented NVN command opcode")`，不补的话游戏一调新函数就当场挂。补的时候压缩 / 非压缩两条 buffer→texture 路径都按层距算了（`0 = 紧排`，与上游 `nvn_copy_pitch.h` 同语义；非紧排时逐层各发一次 `VkBufferImageCopy`，因为 Vulkan 这条命令没有层距字段）。
2. **触摸层接上游新 API**（`touch_controls_android.c`）：
   - `dm_tc_set_safe_bottom(bottom)`：共享布局自己把技能弧抬起来，**取代**了原先手写的 `raise_skill_arc_locked`（两个都留会抬两次）；
   - `dm_tc_set_swap_above(...)`：自定义页新设置项，`tick()` 里轮询、变了就重排（对齐 iOS `refreshPortalBinding`）。
3. **`dm_i18n_tab.h`**：与合并后的 `ui_text.tsv` 复核一致（重跑生成器无差异）。
4. **摇杆浮动**：`nativeFrame` 按着时给 `joy_origin_x/y`（原来恒给静止位），Java 才画得出"底盘跟手"。iOS 那边读的就是 origin。

## 4. 验证做到哪一步

| 项 | 结果 |
|---|---|
| `dm/android/build && ninja` | ✅ |
| `build_modules.sh`（规则域 `touch-run-v4`） | ✅ main 49 行切点 / 36 行门（含表头） |
| `build_apk.sh` | ✅ 出包签名 |
| `sync_surface_audit.sh` | ✅ 冲突面 **43 → 43**（持平） |
| `dm/i18n/scan_literals.py` | ✅ 461 条全在白名单 |
| `dm/ios/tests/test_touch_layout_options.py`（上游新测试，不需要游戏文件） | ✅ PASS |
| `gen_overlay.sh` | ✅ 出覆盖层（见 §5 的开关） |
| `dm/host/tests` | ⛔ macOS 专用（`malloc/malloc.h`、`TargetConditionals.h`），Linux 跑不了，用安卓构建代验 |
| 真机 | ⏳ 手机不在时做的；装包与覆盖层推送见 §7 |

## 5. 两个环境坑（跟合并无关，但会挡住构建）

1. **`native_art` 的「PNG 重编码字节」校验跟 Pillow 版本绑死**：报 `dm_native_skill_0.png 解出的 PNG 与冻结的不一致（Pillow 版本？）`。
   原件哈希（SHA-256 / MD5）仍然校验得过，说明游戏文件没问题；差的是 Pillow 重新编码 PNG 的字节。本机 Pillow 12.3.0。
   → 只想按新布局重出覆盖层时，用加法开关 **`DM_ASSETS_REUSE=1`**（`gen_assets.py` 的 `ensure()`：产物齐了就沿用，不再按指纹重跑美术）。
   根本解法是二选一：把 Pillow 固定到出冻结值那一版，或按当前工具链重出 `native_art_manifest.json`——**都不属于同步该干的事**。
2. **`gen_hud_v4` 的 PC 版 2 倍素材目录不在**（`$DM_ROOT/data/assets-src/raw`）：缺了会回落到 Switch 半分辨率图（底栏 v4 会略糊）。这次因为用了 `DM_ASSETS_REUSE=1`，没有重出，所以没有影响。

## 6. 下次怎么同步（照抄）

```bash
# 0) 工作区要干净（上游也改的文件，脏着会被 git 拒）
git status --short

# 1) 备份 + 拉 + 合（用 merge，不要 rebase）
git branch -f sync-backup-$(date +%F) HEAD
GIT_SSH_COMMAND="ssh -F $HOME/.ssh/config" git fetch upstream --prune   # 本机 /etc/ssh 那个 systemd 配置坏了，必须绕开
git merge --no-ff --no-edit upstream/main

# 2) 解冲突：只看 git status 里 UU 的那几个；规矩是「上游的行留着 + 我们的 ANDROID 区段留着」

# 3) 合并后自检（照 §3 的清单逐条看上游这版动了哪些共享 API）

# 4) 验
dm/android/tools/sync_surface_audit.sh          # 冲突面只许降/持平；涨了在提交说明里写原因
cd dm/android/build && ninja && cd -
dm/android/tools/build_modules.sh && dm/android/tools/build_apk.sh
DM_ASSETS_REUSE=1 dm/android/tools/gen_overlay.sh   # 只重出覆盖层时（见 §5-1）
python3 dm/ios/tests/test_touch_layout_options.py
python3 dm/i18n/scan_literals.py

# 5) 上机：装包 + 推覆盖层（覆盖层目录先清空，别留旧数据档）
dm/android/tools/deploy_apk.sh
adb shell rm -rf /sdcard/Android/data/org.d2r.binaryprobe/files/d2r-mobile/diagnostics/overlay/*
adb push dm/android/build/romfs-overlay/. /sdcard/Android/data/org.d2r.binaryprobe/files/d2r-mobile/diagnostics/overlay/
```

## 7. 新触摸外观的移植（2026-10-18）

上游 `46cb318` 把触摸外观整套换了（薄边嵌槽 + 浮动摇杆）。这件事**不在合并的机械范围里**，因为 `DmTouchControlArt.h` 是 CoreGraphics，安卓编不了——按方案文档 §2.1 第 11 条「逻辑参照移植」逐项翻成了 Java Canvas（`TouchControlsView.java`）：

| iOS | 安卓 |
|---|---|
| `dm_button_draw` | `buttonDraw()`（同渐变 / 同线宽 / 同 alpha） |
| `dm_socket_neutralize` | `socketDraw()` + `socketNeutral()`（Android 无 SATURATION 混合 → 去色副本 + 椭圆遮罩，等效） |
| `dm_joystick_base_draw` / `_thumb_draw` | `joystickBaseDraw()` / `joystickThumbDraw()` |
| `controlArt:` 缓存 | `controlArt()`（同 64 上限） |
| 图标阴影 | `shadowedIcon()`（离屏位图 + `setShadowLayer`） |
| 无图标退回 `lbl_*.png` | 直接贴 `hud_v4_ui/lbl_*.png` |
| 非技能格 `_circle` + 固定图标 | `circleCell()` + `fixedIcon()` |

- 新素材：`dm_mobile_socket.png` 由 `build_apk.sh` 拷进 `assets/`；`dm_native_circle/map/menu.png` 与 `sil_dm_native_skill_*.png` 本来就在。
- **还没做**：上机对图（颜色浓度、空槽边框亮度、摇杆头大小）。三个已知的等效替代（无 SATURATION 混合 / 阴影算法不同 / luminosity 近似）可能要按真机微调。
