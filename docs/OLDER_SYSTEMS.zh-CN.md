# 老系统：iOS 18 – 25 与 macOS 15

[English](OLDER_SYSTEMS.md) | **简体中文**

默认构建要求 iOS 26 / macOS 26：图形部分基于这些系统才存在的图形接口（Metal 4）来构建。尚未升级到 26 的设备会使用“旧接口版”——同一份源码、同一个游戏、相同的功能，只是图形层切换成兼容旧系统的实现。这样可以在较老系统上运行，同时保持游戏内容不变。

## 我该用哪一种

先看系统，再看芯片。这两件事互相独立，可以组合使用（各设备的完整信息见首页的「[支持哪些设备](../README.zh-CN.md#支持哪些设备)」）：

| 你的设备 | 系统为 iOS 26 及以上 | 系统为 iOS 18 – 25 |
|---|---|---|
| **A17 Pro 及更新**（iPhone 15 Pro 起、iPhone 16 起） | 默认构建，不需要额外处理 | 旧接口版 |
| **A16 及更早**（iPhone 14 系列、iPhone 15 / 15 Plus 及更早） | 默认构建 + 老机型贴图包 | 旧接口版 + 老机型贴图包 |

- **系统**决定使用哪种构建（默认 / 旧接口版）。
- **芯片**决定是否还需要额外做一份「老机型贴图包」：A16 及更早的 GPU 无法识别游戏贴图所使用的格式，具体做法见 [LEGACY_DEVICES.zh-CN.md](LEGACY_DEVICES.zh-CN.md)。
- Mac 只看系统：macOS 26 及以上使用默认构建，macOS 15 – 25 使用旧接口版；Apple 芯片 Mac 都不需要贴图包。

## 怎么构建旧接口版

### iPhone / iPad，使用 Mac 和 Xcode

准备步骤不变（`./setup.sh`），构建时只需再加一个开关：

```bash
cd dm/ios
DM_IOS_M3=1 ./run.sh build
```

它会把部署目标改为 iOS 18.0，并将图形部分切换到旧接口。免费 Apple ID 依然可用：`DM_FREE_ACCOUNT=1` 可以和它一起设置。A16 及更早的机型，还需要按照 [LEGACY_DEVICES.zh-CN.md](LEGACY_DEVICES.zh-CN.md) 的说明再加上老机型贴图包。

### iPhone / iPad，不使用 Xcode

发布页里有两套壳，按你的系统选择对应那一套；其余步骤与 [NO_XCODE.zh-CN.md](NO_XCODE.zh-CN.md) 完全相同���

| 壳 | 适用对象 |
|---|---|
| `D2RMobile-shell-build-input.zip` | iOS 26 及以上 |
| `D2RMobile-shell-ios18-build-input.zip` | iOS 18 – 25 |

打包工具能识别两套壳，命令不变。A16 及更早的机型同样需要额外加入 `D2RM_ASTC_PACK`（见 NO_XCODE 中的「老机型」一节）——贴图包只和芯片有关，两套壳共用同一套贴图包方案。

### Mac

```bash
source ~/dm-work/build/cache/dm/venv/bin/activate
DM_MAC_M3=1 dm/tools/mac_app.sh        # 产出 ~/dm-work/out/macapp/D2R Mobile M3.app
```

它会按 macOS 15 重新编译一份着色器库（大约 3 分钟，首次构建时才需要），并将结果放到输出目录中。旧接口版 App 名称为「D2R Mobile M3」，存档和日志目录也会单独分开，因此不会和默认构建混在一起。

## 和默认构建有什么不同

- 画面、玩法、存档格式都相同。
- 图形部分每次绘制时，指令提交量会增加约 20%–30%，因此同样的画面会多消耗一些 CPU 时间；我们测到的帧时间差异，落在测量噪声范围内。
- 不包含「资源下载扩展」（它仅服务 Apple 托管的资源包，而公开构建本来就不会使用）。

## 实际情况

macOS 15：已有内测玩家在 Mac mini M4（macOS 15.7）上实际运行过旧接口版。iOS 18：已有社区开发者在自己的分支上使用同样的方法把游戏跑到了 iOS 18 上；这一版仓库中仍未合并这些改动，但它们证明了方案可行。遇到问题请提交 Issue，并附上机型、系统版本和诊断日志（暂停菜单 →「分享诊断日志」）。
