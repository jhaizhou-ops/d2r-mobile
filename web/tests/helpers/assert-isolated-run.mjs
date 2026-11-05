// 校验隔离测试的结构化 TAP 汇总，不固定用例数或依赖默认终端报告样式。
// Check isolated TAP summaries without fixing case counts or depending on terminal reporter styling.
import assert from "node:assert/strict";
export function assertIsolatedRun(result) {
  const details = (result.stdout ?? "") + (result.stderr ?? "");
  assert.equal(result.error, undefined, details);
  assert.equal(result.status, 0, details);
  const totals = {};
  for (const key of ["tests", "pass", "fail", "cancelled", "skipped", "todo"]) {
    const matches = [...result.stdout.matchAll(new RegExp(`^# ${key} (\\d+)\\r?$`, "gm"))];
    assert.equal(matches.length, 1, `Missing or ambiguous TAP ${key} summary\n${details}`);
    totals[key] = Number(matches[0][1]);
  }
  assert.ok(totals.tests > 0, "Isolated fixture must execute tests");
  assert.equal(totals.pass, totals.tests, details);
  for (const key of ["fail", "cancelled", "skipped", "todo"]) assert.equal(totals[key], 0, details);
}
