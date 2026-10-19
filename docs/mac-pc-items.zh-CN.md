# PC 物品栏、NPC 对话与储藏箱边界

English: [mac-pc-items.md](mac-pc-items.md)

此切片将物品相关的 PC 适配与输入前端分开。不包含游戏资源、生成的改写产物、存档、西班牙语翻译、影片播放器、角色创建或键位编辑器。只针对原版 Switch 程序 **3.3.94012**；其他程序版本不通过指令守卫。

## 集成状态

代码可以在不合入 PC 输入切片的 main 分支上编译。此时弱定义 `dm_ui_pc_desktop()` 返回零：新门保留原版路径，不启用 PC 物品行为。这是**可独立编译的模块，不是功能独立的桌面移植**。

启用它需要 PC 输入/profile 工作，包括 root 优先的鼠标分派和 profile/布局 hooks。输入模块提供强定义 `dm_ui_pc_desktop()`，并发布下述 snapshot。PR95 的物理 hover 是输入集成的前提，不能替代此处的物品 hooks。可选强定义 `dm_ui_pc_skill_grid_hover(regs)` 让输入切片在物品 A3 判断之前保留技能选择器 hover；独立构建的缺省定义返回零。

宿主源码和改写器必须一起集成。本切片故意不修改 `dm/PRODUCT`。仅重新编译宿主不会给已有 r44 缓存增加新门，`setup.sh` 也可能直接复用旧缓存。维护者接受前必须显式重建选定产物并执行 `dm/verify/regress.sh --fresh`。候选使用独立 `DM_ROOT`，保留已接受的安装和缓存。

## 所有权和事件契约

`dm/host/dm_pc_items.[ch]` 负责物品策略、延迟拖放状态和七个宿主门。A3 与输入切片共享 ABI，合并时只保留一份 gate/enum/dispatch 记录。输入模块负责 AppKit 事件、指针队列、root/profile 选择及 epoch/generation 生命周期。

发布、指针分派、pump 和 UI 消息观察都只在游戏线程执行。只有 `dm_pc_items_cancel()` 可从其他线程调用：它只递增原子取消序号，不解引用 guest 指针。

1. 发布 `dm_pc_items_snapshot`：当前 root、generation、取消 epoch、原版 UI 输入像素坐标/尺寸、经典尺寸、修饰键、HD 标志及游戏/加载/busy 状态。`event_scope` 的 0 表示持续指针，1 表示当前物理事件，2 表示无效。`valid` 必须包含视口和几何检查。失焦或 root 更换后不能把旧队列事件重新标为当前事件。
2. 物理事件在原 root 分派前调用 `dm_pc_items_pointer(phase,x,y)`，phase 使用现有 `DM_UP_*` 值。非零表示消费；若消费 LUP/RUP，发布者必须清掉自己的该按钮追踪。
3. 未消费事件交原 root，再调用 `dm_pc_items_pointer_after()`。它记录拖放释放；放置会等待相同 held 物品的确认，紧接原版 F8 前再按保存的释放坐标和当前目标映射单元格。
4. 发布当前 snapshot 后调用 `dm_pc_items_pump()`。排队放置不能当成第二次鼠标点击；等待确认最多 90 次 pump。
5. 将解析后的 UI 消息传给 `dm_pc_items_message(category,action,argument)`。个人分页仅接受原版 `PanelManager:ClosePanel:TouchPersonalStashPrevious/Next` 协议，并要求个人页签零、空手和有效上下文。共享页签保留原回调。
6. 失焦、模式/root 更换和指针取消时调用 cancel。取消后的发布必须使用严格更新的 epoch 才能重新启用；重复取消或缺失 snapshot 也不能降低该屏障。root/generation/epoch 改变会作废延迟放置。

手持绘制只修改交给原绘制器的参数。HD 使用发布的输入点；经典模式核对尺寸后只调用一次原版 `23e760` 投影。模块不写原版焦点/选择控件或全局手柄模式。手持保留是被动保护，依据当前原版队列/物品栏/面板上下文；失焦或无效指针本身不能授权自动丢弃，物理操作和绘制仍要求有效 snapshot。

## 保留的原版行为

| 边界 | 行为 |
|---|---|
| ItemSlot F8 | 左键拿起/放下/交换仍由原版类处理；空格、占用格和无效目标保留原版资格判断与客户端/服务端处理 |
| 修饰键 | Ctrl 在物品栏与储藏箱/魔盒间转移、通过原版交易出售、或请求编年史入库；Shift 使用原版腰带判断/放置、批量购买、或现有佣兵药水 API |
| NPC 行 | 活跃 NPC 列表内的实际成员响应物理左键，走原版子控件分派及 `DialogMessage:ListItemClicked` 回调，不合成 ControllerAction |
| held 协调 | 活跃物品面板保留 held，避免手柄自动协调；失焦或离开视口不能自动丢弃，原队列和显式丢弃仍保留 |
| 说明与交易 | 有效物理 hover 仅用于指定物品栏/装备/银行/商店 getter 调用点；格式化、价格、购买和确认仍由原版函数负责 |
| 编年史/Vault | 自有控件保留指针捕获、拖放取消、入库归还身份及原版取出请求；PC profile 外仍选原触屏/手柄几何 |

银行/商店/魔盒/装备族共享原 ItemSlot 分派，模块按已支持的原版 F8 函数识别，不相信布局名称。它不是替代物品栏或宿主物品模拟器。

## 改写契约

七处都只在带私有门的 touch 产物启用。public/native 和 rules-only 产物不受这些新增规则影响。

| SVC | 原位置 | 判断 |
|---|---|---|
| 9d | `3ca47c` | ItemSlot 物理左键；走原 consumed 分支 |
| 9e | `3ca58c` | Shift 右键；否则保留原 virtual call 和 LR |
| 9f | `3b8648` | 手持物品的手柄协调 |
| a0 | `3db0b4` | NPC 子控件鼠标分派 |
| a1 | `abd428` | 手持绘制 XY 参数；始终执行原绘制器 |
| a2 | `3cc97c` | 局部 W9 getter 判断，不写全局 F0 |
| a3 | `abcca4` | 当前物理 Grid hover；否则完整执行原 clear/Leave |

## 布局准备

`dm/ios/tools/pc_item_layouts.py` 从玩家自己的 RomFS 生成银行/编年史、物品栏、商店和佣兵布局。`build(romfs,new_output_directory,prior=None)` 返回替换字典和统计。PC 覆盖层生成器可选择性导入并在其余替换上合并此字典，不依赖西班牙语生成器。

准备保留共享储藏箱回调，添加个人分页和第六编年史页签，排列原生 91 个高级储藏格。保留 upstream 的符文之语按钮、Pouch/入库控件、商店刷新及佣兵六个额外装备位。在本机生成六张纹理与两份 inventory 表；表扩展储藏箱行并保留已有佣兵槽位调整。合并布局后必须像 PC 输入生成器一样应用一次现有 UI 字符串表覆盖层。不提交任何生成的 JSON、纹理或数据表。这些是 HD 布局；不据此认证经典 SD 面板的重新设计。

## 验证与限制

准备好通常的工作区和自己的游戏文件后：

```sh
make -C dm/host/tests pc-items
```

该目标执行生产 C 策略、公开 snapshot/缺省链接边界、Pouch/Vault fixtures、合成布局测试和生产 AST 门测试。后者需要已准备 Python 环境中的 Unicorn。单独执行门测试时可用 `DM_MAIN_ORIGINAL` 指定本机原镜像，不会下载镜像。

隔离切片测试结果：物品边界 136、说明域 294、缺 profile 218、Pouch 14、Vault 27/36/28/23，均零失败；UBSan 重复通过。profile/技能扩展链接测试通过。合成布局 10/10；从自己的 RomFS 准备得到十二份非空替换。门测试通过物品 40、A2 12、A3 9 案例及八项 AST 守卫。独立结构验证器接受七个 stub，并拒绝 110 处单字指令变异。门测试执行生产 stub，宿主分派为明确替身，不执行原版物品域操作或 GPU 绘制器。

断开 profile 的负对照会使正向行为测试失败。epoch 屏障与 Vault 捕获身份的取消回归在修复前报红，之后变绿；也覆盖 END→发布者递增 epoch→新手势的适配器顺序。修饰键测试执行生产编排，原版 API 用测试替身；异步确认测试覆盖状态转移及幂等性，不是完整服务器交换。

在新的隔离临时工作区重新生成 touch 产物后，main/sdk/subsdk0 结构检查和完整改写等价回归通过（25,084/11,975/10,486 个等价站点，零分歧；900 个变异全部检出），原生门测试 153/0。新增物品位置属于有意改控制流的剪口，由结构验证与明确的门/策略 fixtures 检查；标准等价测试不证明它们的完整原版物品域或服务器语义。组合生成器的独立检查还确认 25 份资源（19 替换、6 新增）、16 个 @dm 引用可解析及 2,131 条原字符串不变。

字符串表及原版写入/缓存/模式扫描通过。literal scan 仍报告未改动基线中 `gen_romfs_overlay.py` 和 `drop_light.py` 的两项诊断，这两文件不属于本切片。此分支未运行完整 host/gfx/iOS 套件或合并输入后的 GUI 验收。Tooltip/A3 适配覆盖物品栏、银行和商店，不在本切片中增加对 Hireling/Cube 面板的该项适配。历史本地组合移植的 QA 不能代替此次拆分上的检查。
