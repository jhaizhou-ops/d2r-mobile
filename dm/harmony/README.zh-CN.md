# 鸿蒙源码移植

English: [README.md](README.md)。

这是现有鸿蒙试验的独立源码归集，游戏基线为 **3.3.94012-r32**，没有把当前 `dm/host` 直接接入鸿蒙。旧 `sw_*` 触控/服务 ABI、NVN 对象布局保留在本目录；没有 ABI 审核前不要换成当前 `dm_*` 定义。PR 基线为 `17845634d71341cca1debf43ad79ac25c6a3a97a`。

- `runtime/`：受控 HLE 服务 C、ARM64 门、加载器、改写工具与此前离线测试。17 个当前服务文件都是源码输入，不再由旧生成器覆盖。
- `graphics/`：Vulkan/NVN 后端，含历史提交 `334f556` 之后最新本地描述符改动；依赖指向独立 `compat-r32/gfx/` 快照。
- `app/`：ArkTS/NAPI 源码模板，版本 **0.12.0 / 61**，无签名产品与普通 SVG 开发图标。不含原签包工程配置和游戏美术。
- `compat-r32/`：仅旧 NVN 接口/布局代码及本改写器使用的五条现有离线启动剪口，属于接口事实，不是游戏程序副本。它新增了 `docs/DERIVED.md` 所述地址/接口的一个平台实现，仍需与维护方讨论。
- `tools/`：准备仓外执行映像、生成本地表输入、组装仓外无签名 DevEco 工程。

不附带原版/改写后游戏映像、原版或转换后游戏着色器、美术、技能表、经验表、存档、密钥、证书、编译库或安装包。`gpu_assets.h`、`xbg_clear_spv.h` 仅含本项目独立编写的合成探针/清屏着色器，保留对应源码/生成器。预置共享仓库存档字节数组已剔除；云同步原有可选 include 缺省走原游戏正常空仓库行为。

## 本地准备

使用自己合法持有、版本匹配的 `main`、`sdk`、RomFS。需要官方 OHOS ARM64 native SDK（兼容版本 **26.0.0**）、DevEco Studio、装有 `capstone` 及上游素材生成依赖的 Python、附许可证的 **Opus 1.5.2** OHOS ARM64 编译产物。SDK/API 26.0.0 是构建配置，不等于宣称手机鸿蒙 7 系统版本就是该 API。

以下命令从仓库根运行。`$WORK`、`$NSO_MAIN`、`$NSO_SDK`、`$ROMFS`、`$NATIVE_SDK`、`$OPUS_LIB`、`$OPUS_INCLUDE` 为本地路径，`$WORK` 使用仓库外新的 ASCII 目录；参数中不放认证信息。

```sh
python dm/harmony/tools/prepare_execution.py \
  --main "$NSO_MAIN" --sdk "$NSO_SDK" --output "$WORK/execution" \
  --native-sdk "$NATIVE_SDK" --opus-library "$OPUS_LIB" --opus-include "$OPUS_INCLUDE"
```

省略后三个选项时只生成。工具核原 NSO 与解压段 SHA，在仓外生成映像/汇编，逐处核原指令后应用历史宽屏重定位、512 MB 显存预算、1024 阴影尺寸剪口，再按选项构建 `xblr_execution`。刻意不执行旧服务生成器，保留 Claude 改过的 audio、codec、fs、oe、svc、swkbd、thread 七项 HLE，不链接当前 `dm/host`。不要使用 `python -O`，历史改写器使用 assert 作版本守卫。**本轮没有执行这条重组后的完整游戏生成/构建链。**

通过已有上游原版素材生成器在仓外生成技能索引和 PNG，保留旧鸿蒙 ABI 名称：

```sh
python dm/harmony/tools/generate_native_art.py --romfs "$ROMFS" --output "$WORK/native-art"
```

入口复用 `dm/ios/tools/native_art/generate.py` 及其原件指纹核验，只改输出符号/资源前缀，图标选择算法沿用鸿蒙版本。当前上游 native-art 清单与历史鸿蒙清单不同，再生后的绑定覆盖和图像别名需要真机重验。Windows 仍需准备上游 RomFS/解码环境；未归集历史私有 Windows shim。生成失败不能把旧生成表复制进 Git 绕过。

用已准备的上游素材工具，从自己的 RomFS 导出 `global/excel/experience.txt`，必须含 100 行 `Level`/`Amazon` 数据。然后组装新仓外 DevEco 工程：

```sh
python dm/harmony/tools/prepare_app.py --output "$WORK/app" \
  --runtime "$WORK/execution/build/libxblr_execution.so" \
  --native-art-c "$WORK/native-art/sw_touch_native_art.c" \
  --experience-txt "$WORK/experience.txt"
```

`prepare_app.py` 插入本地生成的经验模块/技能索引，复制仓外运行库，生成本机 CMake 路径配置，不改源码树。用 DevEco Studio 打开 `$WORK/app`、解析鸿蒙依赖，构建 default 或 release 产品；仅在这个仓外工程用本地官方账号/profile 配签名。仓内两个产品均无签名。release 的包名为 `com.xiao.boluo`，实际使用自己获授权的包身份/profile。

游戏数据、转换后的 SPIR-V/着色器元数据、触屏覆盖层是独立的用户运行输入，模板保留数据导入界面。本次**没有**提供历史 ASTC/触屏覆盖层、原 Maxwell→SPIR-V 全链的一键转换部署工具。需用匹配的上游本地工具重建 overlay，把再生美术目录的 `sw_native_*` 别名保留到 overlay 的 `touchart/`，通过已有本地导入流程提供。这些生成工具链和覆盖层版本需求仍是集成缺项，不能宣称已端到端复现。历史原版 logo/真实二维诊断探针禁用，游戏图形仍启用。

## 本轮验证与贡献缺项

本轮实际用官方 OHOS Clang **15.0.4**、兼容 SDK **26.0.0**、ARM64、Release 编过合成 `xblr_runtime`、集成对象及完整 `xbg_link_check` 共享库；图形检查带 `--no-undefined`。**未编真实执行映像、Opus 服务、NAPI/ArkTS，未打 HAP/App 包，未操作设备。** 五项纯 Node 输入/导入/布局检查通过；保留的保亮检查在第 12 行已有断言失败，未改的历史源树同样失败。新经验生成器接受合成完整表、拒绝短表，源码树覆写保护检查通过；Python 语法和新入口帮助检查通过。

上游 i18n 表检查通过（371 行）。本 Windows 工作树中，未修改基线的字面量门已报 460 项：扫描路径是反斜杠，白名单是正斜杠；归集后结果相同。只归一化路径分隔符后调用该门可通过。此门不扫描新增鸿蒙目录。鸿蒙旧界面文案**尚未接入 `dm/i18n/ui_text.tsv`**，许多历史注释尚非双语；这两项贡献要求仍需补齐。没有用旧截图证明重组后的模板或中性图标效果。新增接口记录还可能需要维护方 issue 讨论。

以下仅历史交接记录，不是本轮验证：C142 在 Pura X 凉机进村 26–30 fps，在 Mate 90 进入选角页 30 fps；C149 在 Pura X 验过技能绑定；长时间发热后约 21 fps。背包拖放/丢弃、免费回城、经验 HUD 仍未完成。本轮核了源码来源和包内运行库 SHA，但**未重跑历史设备测量**，它们不能证明新准备工具链。历史 App 运行库/build2 的 SHA256 为 `2cfe57b9b25030b63e4c30f13958e596dc6db3fe8200736a1967676b8a615e39`，该库不入库。

代码沿用仓库许可，保留 NVN 原版权声明与 `graphics/third_party/` 许可/来源。本快照可供审阅平台实现；完整再生、打包、本地化、截图和真机缺项如上。
