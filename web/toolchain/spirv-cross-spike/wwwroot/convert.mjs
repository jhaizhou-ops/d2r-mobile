// 浏览器/Node 字节边界；不使用外部进程、不上传数据，也不包含游戏资源。
// Browser/Node byte boundary. No external processes, uploads or game assets.
import { replayFields } from "./replay-reflection.mjs";
import { MSL_ARGUMENTS, CROSS_COMMIT, validateSpirv } from "./spirv-contract.mjs";
export { MSL_ARGUMENTS, CROSS_COMMIT, validateSpirv } from "./spirv-contract.mjs";
async function sha256(bytes) {
  return [...new Uint8Array(await globalThis.crypto.subtle.digest("SHA-256", bytes))]
    .map((x) => x.toString(16).padStart(2, "0"))
    .join("");
}
// 每次请求使用新实例，隔离全局命令行状态与虚拟文件。
// A fresh instance isolates global CLI state and virtual files between requests.
async function invoke(factory, input, args, output) {
  const errors = [];
  const module = await factory({
    noInitialRun: true,
    print() {},
    printErr(text) {
      errors.push(text);
    },
  });
  try {
    module.FS.writeFile("/input.spv", input);
    const status = module.callMain([...args, "/input.spv", "--output", output]);
    if (status !== 0) throw new Error(`SPIRV-Cross exited ${status}: ${errors.join("\n")}`);
    return module.FS.readFile(output, { encoding: "utf8" }).replace(/\r\n/g, "\n");
  } finally {
    for (const path of ["/input.spv", output]) {
      try {
        module.FS.unlink(path);
      } catch {
        /* 编译器失败后文件可能不存在。May not exist after compiler failure. */
      }
    }
  }
}
export async function convertSpirv(input, { factory, replayContext } = {}) {
  const bytes = validateSpirv(input);
  factory ??= (await import("./spirv-cross.mjs")).default;
  const msl = await invoke(factory, bytes, MSL_ARGUMENTS, "/output.metal");
  // 这是上游 SPIR-V 反射信息，并非游戏回放 .json 的结构。
  // This is upstream SPIR-V reflection, NOT the game's replay .json schema.
  const reflectionText = await invoke(factory, bytes, ["--reflect", "json"], "/reflection.json");
  const reflection = JSON.parse(reflectionText);
  const mslBytes = new TextEncoder().encode(msl);
  return {
    schema: "spirv-cross-browser-stage-v1",
    msl,
    reflection,
    ...(replayContext
      ? { replay: replayFields(replayContext.stage, msl, replayContext.resources) }
      : {}),
    provenance: {
      spirvCrossCommit: CROSS_COMMIT,
      mslArguments: [...MSL_ARGUMENTS],
      inputSha256: await sha256(bytes),
      mslSha256: await sha256(mslBytes),
      reflectionSha256: await sha256(new TextEncoder().encode(reflectionText)),
      nativeChainClaimed: false,
      metalCompiled: false,
    },
  };
}
