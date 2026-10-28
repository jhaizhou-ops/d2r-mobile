# Belt drop overlap / 腰带拖放重叠

## Evidence / 证据

Player report: a potion cannot be placed in “3 的第三格” (interpreted as column 3, row 3; the exact gesture is not uniquely identifiable in the log). Build 10230825, iPad17,1, export 2026-10-26 17:19:30 -0400. Source reviewed at upstream d93985b7d086c367efa3f01ddb086b8d4e9f334c.

玩家反馈：药水放不进「3 的第三格」。暂按第 3 列第 3 格理解；日志不能唯一对应那次手势。以下摘录只证明存在错误装备兜底，不能当成已在真机复现了这个具体格子。

Diagnostic excerpt (original file line numbers; no save, character name or account information):

```text
61673: 界面命中：(1362,745) → 物品区 Belt
61675: 界面命中：(1349,628) → 拖放落点类 5
61685: sw_cursor_apply：到位 (1357,607)，放出扣住的 A
61699: sw_cursor_apply：放在装备槽上没放下（手上 id 372）→ 直接调原版放下，栏位 7
61701: 装备：手上物品 id 242 → 栏位 7（原版判定 0：0 不能放、1 放入、5 交换）已调原版放下
```

The preceding belt-fill trace identifies id 372 as mp5, and id 242 as mp5. The different IDs at retry scheduling/execution show ongoing swaps; this patch does not claim to resolve every deferred-action race.

此前的补腰带记录确认 372 和 242 都是 mp5。排队与执行时物品 ID 不同，说明当时有连续交换；本补丁不声称修复了所有延迟操作竞态。

## Source path / 代码路径

`item_walk` accepts the first item widget hit. Expanded Belt hit regions overlap body slots, already documented by `belt_row_block`. `ui_drop_kind` then centers a `slot_*` target and arms the body-location equip fallback. A beltable item aimed at an overlapping belt cell can thus be moved away from its requested position and sent down an equipment-only retry path.

`item_walk` 接受第一个命中的物品控件；展开腰带和装备位的命中区重叠。`ui_drop_kind` 把 `slot_*` 落点移到装备槽中心，并记录装备兜底栏位，因而药水可能被移出所指腰带格。

For beltable drops that initially hit `slot_*`, search only the selected panel's visible subtree for a Belt whose native hit test accepts the original lifted image position. Select that Belt, preserve the grid point through the existing normalization, and record no equipment fallback. Empty-hand taps and non-beltable drops retain their existing routing. Native item compatibility, belt capacity and occupied-cell rules remain authoritative. A throttled diagnostic records the overlapping slot and original image/finger coordinates.

只在可入腰带物品拖放到 `slot_*` 时，在已命中面板的可见子树中检查 Belt 原生命中；改选腰带后保留格子位置、不登记装备兜底。空手点击和不可入腰带物品沿用旧路径。兼容性、容量和占格仍由原版裁定。限频日志记录冲突控件及落点。

## Validation / 验证

- `make -C dm/host/tests build/t_belt_drop && dm/host/tests/build/t_belt_drop`: 407 assertions, zero failures, against production overlap selection and normalization using authored widgets. Covers all 16 cells at three viewport heights, either child order, non-beltable items, non-equipment targets, hidden ancestors/belt, out-of-bounds points and panel isolation.
- Same fixture with UBSan: 407 assertions, zero failures.
- Literal/i18n, native-write, widget-cache and mode-registration source gates: passed.
- No game program/RomFS, native run, gameplay screenshots, or iPad validation available. This is a draft pending device acceptance; synthetic hit regions do not prove the native geometry or end-to-end placement.

自动测试使用自造命中区域，证明代码分支及坐标处理；不等于真机已修好。缺少游戏程序和资源，不能运行完整宿主/iOS游戏验收。

## iPad acceptance / iPad 回归

1. Equip belts with 1, 2, 3 and 4 usable rows. Test inventory-only and stash+inventory layouts, including landscape iPad sizes.
2. With an empty column-3 row-3 cell, drag compatible potions from inventory and from another belt cell. Verify the item lands in that exact cell. Repeat all available rows/columns and both sides of the overlapping ring/belt boundary.
3. Test occupied cells, same/different potion types, rapid swaps, full belt and full inventory. Native acceptance/rejection should be preserved; no items should remain invisibly held, disappear or duplicate.
4. Collapse/expand the belt. Empty-hand ring/belt selection and equipping real equipment must still work. Dropping non-beltable items onto belt cells must not equip, auto-stow into a different cell or discard unexpectedly.
5. On failure, export immediately. Inspect `腰带拖放` coordinates, cursor arrival, client/server held IDs and any equip fallback. Confirm no potion is sent to body slot 7 after a belt-targeted drop. Verify final item locations and counts in game.

真机重点：第 3 列第 3 格及所有可用格、背包与仓库两种布局、不同腰带容量、占格交换、快速连续拖放、满背包、折叠/展开和真实装备穿戴。核对最后位置、数量及手上物品，而不只看日志。
