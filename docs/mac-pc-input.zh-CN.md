# Mac PC 输入模式

生成 Mac 覆盖层或安装包时，设置 `DM_OVERLAY_PC=1` 可启用可选的桌面模式。默认手柄路径和 iOS 路径保留现有布局。此模式优先使用已有的原版 PC 布局、原版键盘绑定记录和原版动作分派器；鼠标点击不会被转换成触屏点按。

## 集成前提

按要求，此源码分片中的 `dm/PRODUCT` 仍为 r44。**旧的 r44 缓存产物不包含这些钩子。** 维护者必须使用当前检出版本重新生成改写产物，再与新的宿主配套使用。请保留现有安装，并使用新的私有工作区：

```sh
DM_ROOT="/path/to/new-private-workspace" \
DM_NSO_DIR="/path/to/own/ExeFS" \
DM_PY="/path/to/prepared/venv/bin/python" \
  bash dm/tools/product.sh build
```

这条命令是集成步骤，并不表示此源码分片已经重新生成产物或运行过游戏。后续构建应继续使用同一个新 `DM_ROOT`。游戏资源、着色器库以及现有的其他环境准备工作仍然必需；本变更不提供任何游戏文件。

非 PC 模式下合并鼠标悬停事件的路径来自 PR #95。准备此分片时，该 PR 尚未合并。集成 #95 后应保留这一行为：旧路径中的操作 3 表示悬停，鼠标右键按下则使用显式鼠标 API。PC 模式下，指针事件在有序队列中保留精确的浮点坐标。共享的小型悬停辅助函数归功于 #95，并非本分片新增的悬停功能。

## 审阅索引

| 边界 | 实现 |
| --- | --- |
| AppKit 事件、修饰键和文本焦点 | `dm/host/front_mac.m`、`dm_controls_mac.[hm]` |
| 有序指针事件、取消和旧路径悬停 | `dm/host/touch_mac.[ch]`、`dm_cursor.c` |
| 原版 69 个动作的绑定匹配器、分派器和按住状态 | `dm/host/dm_cursor.c` |
| 点击移动、目标选择和快速施法 | `dm/host/dm_world.c` |
| PC 布局回退和就绪状态 | `dm/host/dm_padmode.c`、`dm/ios/tools/pc_desktop_layout.py` |
| 带保护检查的客体调用点和资源参数 | `dm/tools/patch_nso.py`、`dm/host/hle*`、`dm.h` |

AppKit 回退方案提供技能键 1–6 和药水键 QWER。PC 模式生效时，以原版游戏的绑定记录为准，由游戏自身的控制设置页处理重新绑定。原地不动、鼠标副绑定、滚轮绑定、快速施法选择和松开事件均使用原生动作路径。失去焦点、切换模式和取消队列时，会先使按住的输入失效，避免其他上下文继续消费这些输入。

可选的物品模块属于另一个分片。其头文件和源码存在时，指针泵会发布客体线程快照，内容包括根节点身份、UI 代次、取消纪元、精确坐标、修饰键和就绪状态。物品事件在原生根节点处理器调用前后处理。输入分片无需该模块也可构建。这里共享的 A3 网格门提供仅处理技能的弱实现；物品模块会覆盖它，并在自身的网格处理器之前调用技能处理器。移植两个分片时，A3 只注册一次。

创建角色时，将 PC 的 `Create` 消息转换为原版 Switch 的 `TryCreateCharacter` 操作，让原生名称校验和复制流程照常执行。这不会改变授权检查。桌面生成器保留原版角色布局，以及语言和配置档的尺寸参数；不会添加私有的职业控件或西班牙语表格列。

光标 PNG 由 `gen_mac_cursors.py` 从调用者自有的 RomFS 中提取，并在设置 `DM_OVERLAY_PC=1` 时放入私有 Mac 应用包。链接模式会在 `paths.txt` 中记录外部 `cursors` 目录，使游戏派生图像保留在应用包之外。仓库不提交光标图像。开发宿主可用 `DM_MAC_CURSOR_DIR` 指定已提取的目录。光标热点独立于事件坐标转换。

## 检查

使用已经准备好、包含 Unicorn、Pillow 和项目 NSO 依赖的环境：

```sh
make -C dm/host/tests pc-input \
  DM_ROOT="/path/to/prepared-private-workspace" \
  PY="/path/to/prepared/venv/bin/python"
python3 dm/ios/tests/test_mac_framework_boundary.py
python3 dm/ios/tools/dm_i18n.py --check
python3 dm/host/tests/native_write_scan.py --selftest dm/host
python3 dm/host/tests/widget_cache_scan.py --selftest dm/host
python3 dm/host/tests/mode_scan.py dm
```

`pc-input` 覆盖队列精度、顺序和取消，原生绑定表，鼠标与快速施法判定，技能悬停，控制设置切换，AppKit 持久化与本地化，以及 #95 的旧路径悬停。ARM 检查使用调用者自有的原版可执行文件和显式提供的测试依赖，不运行游戏、GPU 或服务器。覆盖层测试打包合成资源，并验证从 CASC 读回的结果。框架测试确认生成的 iOS 链接输入中不包含 Carbon/Cocoa。支持通过 `B` 指定输出目录。

未改动的字面量扫描器存在基线差异：系统 Python 3.9 会报告两条已有的 f-string 诊断；Python 3.12 及以上版本会报告成功，因为这些版本的分词器会暴露当前扫描器未检查的片段。较新 Python 上的成功结果并不表示 f-string 已得到完整检查。

完整的改写产物回归、重新启动游戏、Mac 上的视觉行为以及实体 iPhone 冒烟测试，仍需由维护者在集成时检查。提取此分片期间，未修改或启动已验收的已安装应用。这些源码测试夹具不宣称已证明通用兼容性或通过视觉验收。
