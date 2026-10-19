# 按 RT 并行 replay：代码落点草图（未实现，供下次直接开工）

实测（`edb70bd`，营地/帧）：world 23,266 ops / 1,167 draw；ui 13,138 ops / 814 draw；**切换 4 次**。

## 落点一：切分点在哪里
`execute()`（nvn_vk.c，回放主循环）里每个 `OP_SET_RT` 之后按新 RT 重新判组。
`edb70bd` 已经在同一位置算过组别，直接复用那段分类代码（`grp` 变量）。

组别切换时即为**候选切分点**；4 次/帧意味着每帧最多 5 段。

## 落点二：两套上下文
已有：`G.submission_slot_ring[2]`（双缓冲）+ `sub_begin_ctx()`。
需要新增：`VkSubmission rt_split[2]`（或复用 ring 的两个槽位给两组），并且
**命令池、描述符缓存必须按上下文创建**（`submission_pools_create()` 已做，别回到 init 里建）。

## 落点三：两套状态
`G.state` 目前是全局单份。并行组必须各自一份，否则两组互相覆盖。
草案：把 `G.state` 包成 `struct replay_state st[2]`，用 `#define state (st[g_grp])` 之类的最小侵入替换；
注意 `G.state.rt`（当前渲染目标）被大量代码直接读，替换后要确认没有跨组读取。

## 落点四：同步
每个切分点：
- 组 A 的命令缓冲 → `vkQueueSubmit` → 一个 `VkSemaphore`；
- 组 B 的第一条命令 `vkCmdWaitEvents` 或第二个 submit 的 `pWaitSemaphores` 等它。
因为切换只 4 次，每帧最多 4 对信号量，开销可忽略。

## 落点五：不改顺序的兜底
`DM_RT_PARALLEL=0`（默认）：完全走现在的单线程路径。
`=1`：启用并行。A/B 用 `tools/pf/sf_fps.py` 看 fps + 掉帧率 + 场景探针。

## 先做的验证（不需要并行就能做）
把 `execute()` 里的分类计数扩展成"每段的起始 op 索引 + 组别"，打印一帧的分段表
（预期 5 段）。**这一步零风险，且能确认切分点位置是否稳定**（每帧都一样还是有抖动）。

**2026-10-19 已完成（`3aab27e`）**：营地每帧恒 5 段、逐帧字节级相同：

```
RENDER_RT_FRAME_SEG segments=5 ops=36326
  [0]g1@1+19/0 [1]g0@20+22/0 [2]g1@42+13046/812 [3]g0@13088+23166/1163 [4]g1@36254+73/2
```

切分点：42（g1→UI 大段）、13088（→世界大段）、36254（→UI 尾段），每帧固定。
实测数据与踩坑见 `android-resolution-plan.zh-CN.md` 附十；关键一条：组别只能在 `OP_SET_RT`
重算并缓存，逐条分类要 -3%。开关 `diagnostics/rtseg.txt`（0 关 / 1 组别 / 2 分段表）。
