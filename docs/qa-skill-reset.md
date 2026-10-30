# 免费技能点重置 / Free skill reset

触屏技能树底栏提供「重置」，再选择「确认重置」或「取消」。免费、不限制次数。确认后关闭技能树，让服务端恢复执行。只返还本职业已投入的硬点，包含秘境奖励投入的点；属性点、金币、道具和任务重置次数不变。未投入点数时不调用重置。

The touch skill tree footer offers Reset Skills, followed by Confirm Reset or Cancel. Resets are free and have no use limit. Confirmation closes the tree to resume server ticks. Only invested class skill points are refunded, including invested Rift rewards. Attributes, gold, items and quest reset uses are unchanged. An empty allocation does not invoke the reset.

原版存档的未用技能点只有 8 位。总数超过 255 或职业表异常时，执行前拒绝，不截断、不丢点。仅单机可用。确认绑定游戏与玩家，关闭页面、读档、取消时撤销。

The original save has eight bits for unspent skill points. Totals over 255 or invalid class data are rejected before mutation, without truncating points. Available only in single player. Confirmation is bound to the game/player and is revoked on closing, loading or canceling.

## 实现依据 / Native path

3.3.94012 原映像 `main+0x7c0f90(game, player)` 是技能单独重置：按版本表 `+0x1208` 的职业技能数量、`+0x1220` 的步长和 `+0x1228` 的技能号数组遍历；调用 `0x7c11b0(player, skill, 0, 1)`、`0x63aaa0` 更新客户端；`0x5bc5a0` 返还 stat 5；随后调用原版清理路径。已静态核对本地 Guest framework 的对应指令，Mac 游戏内验证见下文。

In the 3.3.94012 original image, `main+0x7c0f90(game, player)` resets skills alone: it traverses the version table's class skill counts at `+0x1208`, stride at `+0x1220` and skill IDs at `+0x1228`; calls `0x7c11b0(player, skill, 0, 1)` and `0x63aaa0` to update skills and the client; refunds stat 5 through `0x5bc5a0`; then runs native cleanup. Corresponding instructions in the local Guest framework were inspected statically; see Mac runtime verification below.

## 验证 / Verification

- `make -C dm/host/tests build/t_skill_reset && dm/host/tests/build/t_skill_reset`: 容量、255 边界、非法点数、取消、重复确认、换角色、关页、重复重置 / capacity, 255 boundary, invalid points, cancel, repeated confirmation, player change, closed tree and repeated resets.
- `python3 dm/ios/tools/dm_i18n.py --check`、`python3 dm/i18n/scan_literals.py`: 本地化 / localization.
- 原版技能树布局实际生成并通过 JSON 检查 / actual skill tree layout generated and JSON validated.

待游戏内验证 / Pending in-game checks:

1. 各职业、手机宽高比的按钮位置和触摸命中 / button layout and touch hits across classes and aspect ratios.
2. 加点 → 重置 → 再加点 → 再重置；取消和关页不改点 / allocate, reset, allocate again, reset again; cancel/close does not mutate points.
3. 装备加技能、被动、光环、召唤、主副手技能栏刷新 / equipment skill bonuses, passives, auras, summons and both weapon-set bindings refresh.
4. 保存退出再进入，点数一致，属性 / 金币 / 道具不变 / save and rejoin preserves refunded points and unchanged attributes/gold/items.
5. 秘境奖励投入的点正常返还；超出 255 时整笔拒绝 / invested Rift points refund correctly; totals exceeding 255 are rejected atomically.

## Mac 实测（2026-10-30） / Mac runtime validation

使用当前源码编译的 Mac host、r44 MacPrepared 原生运行资源和独立临时存档。预编译着色器的工具锁与当前仓库不同；用资源包的工具锁对当前着色器源码重新计算得到相同的 `7554df5942ff`，测试 host 使用此版本，仓库生成头已还原。

- 10 级野蛮人：未用点数 10 → 剑支配（126）投入 2 点 → 剩余 8。
- 实际点击重置、取消：仍为 8；再次点击重置、确认：原生函数返回 10（预期 10）。
- 保存退出后，关闭程序再启动，不设置 `DM_TEST_STAT`：首拍读取未用点数 10、等级 10，已学硬点列表中没有技能 126。
- 全程四项基础属性为 30 / 10 / 20 / 25，金币为 0，重置前后不变。
- 修复实测发现的底栏遮挡：按钮下移至 y=1460，确认说明、剩余技能点与按钮均清晰可见。

Current-source Mac host with r44 prepared runtime and an isolated temporary save. The prepared tool lock reproduces shader chain `7554df5942ff` using the current shader sources; the validation host used that chain and the repository's generated header was restored. A level-10 Barbarian invested two points in Sword Mastery (126), leaving eight. Clicking Cancel preserved eight; confirming refunded ten. After saving, exiting, and restarting without initial-stat injection, the first tick showed ten unspent points and no learned skill 126. Attributes and gold were unchanged. Runtime screenshots exposed a footer overlap, fixed by moving the buttons to y=1460.

运行记录与截图 / Local evidence: `/tmp/d2r-reset-validation/run5.log`, `/tmp/d2r-reset-validation/reload.log`, `/tmp/d2r-reset-validation/confirm.png`.

仍待验证：其他职业、手机不同宽高比、装备技能加成 / 光环 / 召唤 / 主副手绑定刷新、秘境奖励点与容量边界的游戏内表现。容量边界已通过纯逻辑测试。上述 Mac 实测不替代 iPhone 真机测试。

Still pending: other classes, phone aspect ratios, equipment skill bonuses / auras / summons / weapon-set bindings, and in-game Rift reward and capacity-boundary scenarios. Capacity boundaries have pure-logic test coverage. Mac runtime validation does not replace iPhone device testing.

## iPhone 17 分辨率复测 / Resolution retest

2026-10-30：入口按钮简化为「重置」。按 Apple 官方规格（https://www.apple.com.cn/iphone-17/specs/），以 `DM_GAME_RES=2622x1206` 在 Mac host 上运行，输出 PNG 实际尺寸也是 2622 × 1206。技能树、剩余点数、说明和按钮均无遮挡；实际点击「重置 → 取消 → 重置 → 确认重置」成功，取消后保持 8 点，确认后恢复 10 点，属性 / 金币不变，保存退出成功。日志 `/tmp/d2r-reset-validation/iphone17.log`，截图 `iphone17-skills.png` 和 `iphone17-confirm.png` 同目录。此项验证分辨率与点击布局，不模拟 iOS 安全区、圆角或灵动岛。

The entry label is now 「重置」. Mac host rendered at the iPhone 17's 2622 × 1206 landscape resolution; PNG dimensions matched. Text and buttons did not overlap. Actual clicks exercised request, cancel, request and confirm: eight points remained after cancellation and ten after confirmation, with unchanged attributes/gold. Save-and-exit completed. This validates resolution/layout, not iOS safe areas, rounded corners or Dynamic Island.
