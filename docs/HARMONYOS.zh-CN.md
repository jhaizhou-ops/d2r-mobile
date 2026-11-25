# 在鸿蒙手机上玩

鸿蒙版由 [@biglawine](https://github.com/biglawine) 移植，与安卓共用宿主和 Vulkan 引擎。鸿蒙壳是不含游戏内容的预编译 HAP：玩家用自己的调试证书重签，再导入由自己游戏文件生成的运行包，不必重新编译应用。

English: [HARMONYOS.md](HARMONYOS.md)

2026-10-22，在 Mate 90 Pro Max（CMM-AL10，HarmonyOS 7.0.0.109、设备实报 API 26）完成了重签、覆盖安装、系统文件夹导入、角色选择、进入营地和保存退出。code121 修复并验证了缺少运行目录时的首次导入；0.19.4/code122 再加入局外地图请求保护，连续三次冷启动、进局、全屏地图切换和保存退出通过。最终版 0.19.6/code125 从 v2.0 源码树构建，并让自定义页选的语言在下次冷启动生效；2026-10-23 韩文与中文各完成冷启动、进局、菜单和保存退出，更换运行包后原有 27 个存档文件无变化。截图出现过约 30–60 FPS，这只是安装与导入验证，不是性能验收；先前调试版的 59.7 FPS 不能当成本壳的测量值。Pura X 尚未重跑本轮壳流程。

使用发布页里与源码版本配套的 **unsigned HAP**。若发布页尚无该资产，请等待维护者发布，或参考[开发者构建说明](../dm/harmony/README.zh-CN.md)。不要使用别人内置游戏内容或调试签名材料的测试包。

另见 [MatePad mini 实验性适配报告](devices/MATEPAD_MINI.zh-CN.md)，记录启动阻塞及个人构建的结果；不代表发布壳已兼容该设备。

## 准备

- 一台支持该 HAP 最低 API 的鸿蒙手机，启用开发者模式和 USB 调试。本轮只验证了上面的 API 26 设备；系统营销版本号不能直接当成 API 版本。
- DevEco Studio 及相应 SDK，使用其中的 Java、`hap-sign-tool.jar` 和 `hdc`。
- 华为开发者账号、自己的调试证书和私钥、包含这台手机的调试 profile。可在 DevEco Studio 的签名配置中登录并生成；手动管理的流程见[官方调试 profile 文档](https://developer.huawei.com/consumer/cn/doc/doccenter-getting-started/agc-help-debug-profile-0000002248181278)。本轮没有验证免开发者账号安装渠道。
- 自己购买的游戏文件：Switch 版 1.0.34.0（3.3.94012）的 `Update/ExeFS/{main,sdk}` 与 `Update/RomFS/data/`，见 [GAME_FILES.zh-CN.md](GAME_FILES.zh-CN.md)。
- 运行工具所需的 Python 3.9+ 与 `pillow numpy lz4 capstone unicorn`、.NET SDK、`spirv-opt`、`spirv-val`、`glslc`。`spirv-opt` 要支持 `--trim-capabilities`。已有可用环境无需重复安装。

## 1. 重签预编译壳并安装

下载 HAP 后先比对发布页 SHA-256。证书、profile 和私钥须来自你自己的签名配置，profile 的应用身份须与 HAP 相符，并登记目标设备。下例为 Windows PowerShell，路径替换成自己的。密码只在提示框输入，不写进脚本。

```powershell
$DevEco = 'C:\Program Files\Huawei\DevEco Studio'
$Java = "$DevEco\jbr\bin\java.exe"
$SignTool = "$DevEco\sdk\default\openharmony\toolchains\lib\hap-sign-tool.jar"
$Hdc = "$DevEco\sdk\default\openharmony\toolchains\hdc.exe"
$InputHap = 'C:\D2R\D2RMobile-harmony-shell-unsigned.hap'
$OutputHap = 'C:\D2R\D2RMobile-shell-signed.hap'
$Cert = 'C:\D2R\signing\developer.cer'
$Profile = 'C:\D2R\signing\device-debug.p7b'
$KeyStore = 'C:\D2R\signing\developer.p12'
$KeyAlias = '你的签名配置里的 keyAlias'
$KeyPassword = [System.Net.NetworkCredential]::new('', (Read-Host '密钥密码' -AsSecureString)).Password
$StorePassword = [System.Net.NetworkCredential]::new('', (Read-Host '密钥库密码' -AsSecureString)).Password
& $Java -jar $SignTool sign-app -mode localSign -keyAlias $KeyAlias `
  -appCertFile $Cert -profileFile $Profile -profileSigned 1 -inForm zip `
  -keystoreFile $KeyStore -signAlg SHA256withECDSA -compatibleVersion 260000026 `
  -signCode 1 -keyPwd $KeyPassword -keystorePwd $StorePassword `
  -inFile $InputHap -outFile $OutputHap
$SignResult = $LASTEXITCODE
Remove-Variable KeyPassword,StorePassword
if ($SignResult -ne 0) { throw '签名失败' }
& $Java -jar $SignTool verify-app -inFile $OutputHap `
  -outCertChain 'C:\D2R\verified-chain.cer' -outProfile 'C:\D2R\verified-profile.p7b'
if ($LASTEXITCODE -ne 0) { throw '验签失败' }
& $Hdc list targets
& $Hdc -t '<上一步显示的设备标识>' install -r $OutputHap
```

`-compatibleVersion 260000026` 对应本次 HAP 的编码值；其他版本以该 HAP/SDK 要求为准。`-signCode 1` 是本次跑通的签名参数之一。安装输出须明确出现 `install bundle successfully`，不能只看命令退出码。第一次打开时若尚未导入文件，会显示缺少资源；本轮可执行内存自检为 `ok`。

升级继续使用原来的证书与应用身份并覆盖安装。签名不同无法保证覆盖；不要为了绕过安装错误卸载有存档的旧应用。

## 2. 一条命令生成自己的导入包

在与 HAP 配套的源码根目录运行：

```powershell
$env:DM_ROOT = 'C:\D2R\work'
python -X utf8 -B dm/tools/d2rm_android.py --target ohos 'C:\D2R\Update' 'C:\D2R\import'
```

工具会检查依赖，生成 `C:\D2R\import\harmony\dm`。本轮从全新工作目录执行成功，包含 6,931 个 XGS 着色器、18 张 `ui-art` 图片、`derived.tsv` 和 `pack_info.txt`。如果工具找不到原生解码库或着色器程序，可用其提示的 `DM_DESWIZZLE_LIB`、`GLSLC` 和 `PATH` 配置已有工具。

鸿蒙的 `out` 必须由 `--target ohos` 生成，不能拿安卓的执行模块替换。生成包和原游戏文件只留在自己的电脑和手机上。

## 3. 复制并选择正确的文件夹

在手机可由系统文件管理访问的位置放置以下结构；也可在电脑打成 ZIP，传入手机后先解压：

```text
D2R Mobile/
  data/                  ← 自己游戏的 Update/RomFS/data，全量复制，保留 .build.config
  dm/                    ← 生成的 import/harmony/dm，全量复制
    out/
    shaders/
    overlay/
    ui-art/
    derived.tsv
    pack_info.txt
```

打开应用，点“导入”，在系统选择器选中 **D2R Mobile 这一层**，确认已选目录同时包含 `data` 和 `dm` 后点右上角勾号。不要选 `data`、`dm` 或再上一级的 ZIP 解压外壳目录。即使按钮只写“装着 data 的文件夹”，发布壳仍需要同层的完整 `dm`。

等待数据与运行组件复制完成，应用会在同一次启动中加载表、启动游戏。首次加载会创建缓存，可能需要等待。原导入文件夹不会被删除，存档在应用自己的沙箱中。下载 ZIP、解压目录与应用内副本会同时占用空间，本轮合计约 17.4 GB。

## 提示和升级

- `-2` / `-5`：缺少完整游戏数据或构建配置，检查 `data` 及选择层级。
- `-3` / `-4`：缺执行模块或着色器，检查完整 `dm`；0.19.4 修复了首次导入新目录失败的问题。
- `-10` / `-11`：“导入包与这版应用不配套”。用该版本工具重新生成并导入。本轮实测错误 PRODUCT 会触发 `-11`，恢复正确文件后可重新启动。
- `-12`：`derived.tsv` 缺失或不完整，重新生成完整包。

壳使用 `pack_info.txt` 的 PRODUCT/TARGET 和覆盖层指纹核对配套关系。外部导入包不需要 `pack-version.txt`；它只用于开发者调试包中内置 ZIP 的更新判定。

换版本时取配套 HAP/工具、用原签名重签覆盖，再按需要重做导入包。桌面卡片显示、系统备份恢复、长期游玩稳定性和 Pura X 快进卡顿不属于本轮已验证范围。
