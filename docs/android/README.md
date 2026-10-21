# Android 文档

> **这个目录**：Android 原生移植的设计、实施、性能、显示与上游同步记录都收在这里。
> 面向玩家 / 第一次出包的构建与操作说明仍在根目录：[README.android.zh-CN.md](../../README.android.zh-CN.md) ·
> [README.android.md](../../README.android.md)。代码长什么样，看 [code-structure.zh-CN.md](code-structure.zh-CN.md)。

## 从哪读起

| 你是… | 先读 |
|---|---|
| 第一次碰 Android 代码 | [code-structure.zh-CN.md](code-structure.zh-CN.md)，再回根目录 README.android 走一遍构建 |
| 要改和 iOS / 上游共享的文件 | [isolation-analysis.zh-CN.md](isolation-analysis.zh-CN.md) → [ui-parity/lessons.zh-CN.md](ui-parity/lessons.zh-CN.md) |
| 要调性能 | [perf/engineering-notes.zh-CN.md](perf/engineering-notes.zh-CN.md)（全过程与数字）→ [perf/lessons.zh-CN.md](perf/lessons.zh-CN.md)（别踩的坑） |
| 要动分辨率 / 屏幕适配 | [resolution-plan.zh-CN.md](resolution-plan.zh-CN.md) → [screen-adaptation.zh-CN.md](screen-adaptation.zh-CN.md) |
| 要跟上游同步 | [upstream-sync.zh-CN.md](upstream-sync.zh-CN.md)（§6 是可以照抄的步骤） |

## 文档清单

### 总览与结构

| 文档 | 内容 | 状态 |
|---|---|---|
| [code-structure.zh-CN.md](code-structure.zh-CN.md) | 分层与三条线程、`dm/android/` 每个文件干什么、`game_thread` 启动顺序、与上游共享代码的边界、改动落点 | 现行 |
| [isolation-analysis.zh-CN.md](isolation-analysis.zh-CN.md) | 怎么和上游隔开：硬性规矩（双分支 / 小钩子 / 追加区段）、函数级决策表、策略表 | 现行 |
| [upstream-sync.zh-CN.md](upstream-sync.zh-CN.md) | 每次同步上游做了什么、冲突怎么解、合并后必须跟着改什么、验证到哪一步 | 现行（每次同步更新） |

### 性能

| 文档 | 内容 | 状态 |
|---|---|---|
| [perf/engineering-notes.zh-CN.md](perf/engineering-notes.zh-CN.md) | 性能优化的全部记录：做过什么、量出来什么、结论是什么 | 现行 |
| [perf/lessons.zh-CN.md](perf/lessons.zh-CN.md) | 16 个坑，每条「发生了什么 / 为什么 / 以后定什么规矩」 | 现行 |
| [perf/upstream-checklist.zh-CN.md](perf/upstream-checklist.zh-CN.md) | 早期把上游 iPhone 性能经验逐条对照仓库的核对清单 | 历史（结论已并入 engineering-notes） |

### 分辨率与显示

| 文档 | 内容 | 状态 |
|---|---|---|
| [resolution-plan.zh-CN.md](resolution-plan.zh-CN.md) | 分辨率档 / 出图预算 / 3D 比例 / UI 画布三者的关系与实测；含逐通道覆盖率表（附十九、二十） | 现行（改动前先读附十九） |
| [screen-adaptation.zh-CN.md](screen-adaptation.zh-CN.md) | 屏幕比例不一致（超宽屏 / 折叠 / 平板）导致的错位，根因与改法 | 问题记录（未动手） |

### UI 与 iOS 对齐

| 文档 | 内容 | 状态 |
|---|---|---|
| [ui-parity/plan.zh-CN.md](ui-parity/plan.zh-CN.md) | 一比一对齐 iOS 触屏体验的分阶段方案（2026-10-17） | 历史方案 |
| [ui-parity/analysis.zh-CN.md](ui-parity/analysis.zh-CN.md) | 上游 Android 方案 vs 本仓库代码事实的对照，评审稿 | 历史 |
| [ui-parity/implementation.zh-CN.md](ui-parity/implementation.zh-CN.md) | 实施方案的细节版：接口快照、要改哪些文件、JNI、伪代码、验收 | 历史参考（路径/行号按旧代码） |
| [ui-parity/status.zh-CN.md](ui-parity/status.zh-CN.md) | 前端体验差距逐项清单 + 这一轮改了什么、还剩什么（2026-10-17） | 记录 |
| [ui-parity/lessons.zh-CN.md](ui-parity/lessons.zh-CN.md) | 对齐过程中踩的坑与定下的做事规矩（iOS 那份就是规格） | 现行 |

## 约定

- 新增 Android 文档放本目录，按主题进 `perf/`、`ui-parity/` 或直接放根下；命名 `<主题>.zh-CN.md`。
- 根目录只留玩家向的 `README.android{,.zh-CN}.md`，其余一律收进这里。
- 文档之间用相对链接；`docs/android/...` 这种仓库根相对写法只在正文里当路径引用。
- 文档里不放游戏截图（含实机运行截图）：图放 fork 的 `assets` Release，正文用绝对 URL，理由见上游 `CONTRIBUTING.md` 规则一。
- 文档里的文件路径、行号会随代码变动过时，**以代码为准**；改动相关代码时顺手更新对应文档的那一句。
