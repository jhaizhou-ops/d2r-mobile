// 运行单个验证步骤并记录执行结果；依据 web/verification 的模块与调用约定。
// Run a verification step and record its result using the module and calling contracts in web/verification.
import process from "node:process";
import { spawnSync } from "node:child_process";
const [file, cwd, seconds] = process.argv.slice(2);
const result = spawnSync("bash", ["--noprofile", "--norc", "-e", "-o", "pipefail", file], {
  cwd,
  stdio: "inherit",
  timeout: Number(seconds) > 0 ? Number(seconds) * 1000 : undefined,
  killSignal: "SIGTERM",
});
if (result.error) console.error(result.error.message);
process.exit(result.status ?? 1);
