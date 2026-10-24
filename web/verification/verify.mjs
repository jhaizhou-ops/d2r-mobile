// 解析验证配置并调度所需步骤；依据 web/verification 的模块与调用约定。
// Parse verification profiles and schedule their steps using the module and calling contracts in web/verification.
import process from "node:process";
import { readFileSync, appendFileSync, mkdtempSync, mkdirSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { resolve, join } from "node:path";
import { tmpdir } from "node:os";

export const root = fileURLToPath(new URL("../../", import.meta.url));
export const profiles = JSON.parse(readFileSync(new URL("profiles.json", import.meta.url), "utf8"));
// 保留独立配置语义，仅把同类 Bash 入口集中到功能文件。
// Preserve independent profile semantics; only group related Bash entry points.
const recipeGroups = Object.freeze({
  shader: "shader-proofs",
  "offline-dotnet": "shader-proofs",
  "spirv-cross": "shader-proofs",
  "shader-pipeline": "shader-proofs",
  extraction: "input-proofs",
  "signed-nca": "input-proofs",
  program: "input-proofs",
  texture: "browser-runtime-proofs",
  "offline-wasm": "browser-runtime-proofs",
  "desktop-build": "browser-runtime-proofs",
  desktop: "browser-runtime-proofs",
  "staged-output": "storage-proofs",
  zip64: "storage-proofs",
  quick: "quick",
});
export function recipeCommand(profile) {
  validateProfile(profile);
  if (profile === "full") throw new Error("Full verification dispatches individual profiles");
  const script = resolve(root, `web/verification/recipes/${recipeGroups[profile]}.sh`);
  return profile === "quick" ? [script] : [script, profile];
}
export function validateProfile(profile) {
  if (!["quick", "full", ...Object.keys(profiles)].includes(profile)) {
    throw new Error(`Unknown verification profile: ${profile}`);
  }
  return profile;
}
export function run(profile, environment = process.env, provision = false) {
  validateProfile(profile);
  if (profile !== "quick" && environment.GITHUB_ACTIONS === "true") {
    throw new Error(
      "Heavy profiles are local verification only; GitHub verifies the browser build.",
    );
  }
  if (profile !== "quick" && environment.GITHUB_ACTIONS !== "true" && !provision) {
    throw new Error(
      "Heavy profiles install pinned SDKs/tools and may use sudo apt-get. Review the recipe and use --provision explicitly, or use manual CI.",
    );
  }
  if (profile === "full") {
    console.log(
      "Full local Linux proof set; Windows/macOS desktop coverage is excluded and must run separately.",
    );
    // 在 Linux 上不会假装已执行特定平台的桌面验证。
    // Platform-specific desktop execution is intentionally not pretended on Linux.
    for (const name of [
      "quick",
      ...Object.keys(profiles).filter((p) => !p.startsWith("desktop")),
    ]) {
      const result = run(name, environment, provision);
      if (result !== 0) return result;
    }
    return 0;
  }
  const temp = mkdtempSync(join(environment.RUNNER_TEMP || tmpdir(), `d2r-${profile}-`));
  const env = {
    ...environment,
    VERIFY_ROOT: root.replaceAll("\\", "/"),
    GITHUB_WORKSPACE: root,
    RUNNER_TEMP: temp,
    GITHUB_STEP_SUMMARY: environment.GITHUB_STEP_SUMMARY || join(temp, "summary.md"),
    GITHUB_OUTPUT: environment.GITHUB_OUTPUT || join(temp, "output"),
    GITHUB_PATH: join(temp, "path"),
    GITHUB_ENV: join(temp, "env"),
  };
  // 当产物或依赖作业使用已知路径时，保留 CI 临时根目录。
  // Preserve the CI temp root when artifacts or dependent jobs use its known paths.
  if (environment.GITHUB_ACTIONS === "true") env.RUNNER_TEMP = environment.RUNNER_TEMP;
  mkdirSync(env.RUNNER_TEMP, { recursive: true });
  const result = spawnSync("bash", recipeCommand(profile), {
    cwd: root,
    env,
    stdio: "inherit",
    timeout: (profiles[profile]?.timeout || 15) * 60_000,
    killSignal: "SIGTERM",
  });
  if (result.error) console.error(result.error.message);
  return result.status ?? 1;
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [profile = "quick", option] = process.argv.slice(2);
  validateProfile(profile);
  if (option === "--metadata") {
    const info = profiles[profile];
    if (!info) throw new Error("Metadata requires one concrete proof profile");
    const path =
      info.artifact?.path.replaceAll("$RUNNER_TEMP", process.env.RUNNER_TEMP || tmpdir()) || "";
    const values = {
      node: info.node,
      dotnet: String(info.dotnet),
      artifact: info.artifact?.name || "",
      retention: String(info.artifact?.retention || 7),
      missing: info.artifact?.missing || "warn",
      always: String(info.artifact?.always || false),
    };
    if (!process.env.GITHUB_OUTPUT) throw new Error("Metadata output requires GITHUB_OUTPUT");
    for (const [key, value] of Object.entries(values))
      appendFileSync(process.env.GITHUB_OUTPUT, `${key}=${value}\n`);
    appendFileSync(process.env.GITHUB_OUTPUT, `paths<<VERIFY_PATHS\n${path}\nVERIFY_PATHS\n`);
  } else if (option && option !== "--provision") {
    throw new Error("Unknown option");
  } else {
    process.exit(run(profile, process.env, option === "--provision"));
  }
}
