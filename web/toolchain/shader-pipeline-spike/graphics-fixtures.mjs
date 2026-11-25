// 根据仓库内的解码器自行编写，不含从游戏提取的着色器字节。
// Authored from the vendored decoder; no extracted game shader bytes.
// 使用 InstTable.cs 的前缀和 InstDecoders.cs 的 InstAld/InstAst/InstMov32i 字段；
// InstTable.cs prefixes; InstDecoders.cs InstAld/InstAst/InstMov32i fields;
// 使用 ShaderHeader.cs 的阶段位 10..13 及 header[18] 的颜色分量掩码；
// ShaderHeader.cs stage bits10..13 + header[18] color component mask;
// Decoder.cs 在每个 32 字节指令组开头跳过一个调度字。
// Decoder.cs skips one scheduling word at each 32-byte instruction-group start.
import {
  code as computeCode,
  job as computeJob,
} from "../shader-spike/wwwroot/synthetic-fixture.mjs";
const prefix = (bits) => BigInt(`0b${bits.padEnd(64, "0")}`);
const pt = 7n << 16n,
  rz = 255n,
  exit = 0xe30000000007000fn;
const ald4 = prefix("1110111111011") | (3n << 47n) | (rz << 39n) | (0x80n << 20n) | pt | (rz << 8n);
const ast4 = prefix("1110111111110") | (3n << 47n) | (rz << 39n) | (0x70n << 20n) | pt | (rz << 8n);
const mov32i = (register, bits) =>
  prefix("000000010000") | (BigInt(bits) << 20n) | pt | (15n << 12n) | BigInt(register);
function graphics(stage, instructions, colorMask = 0) {
  const words = [];
  for (let i = 0; i < instructions.length; i++) {
    if (i % 3 === 0) words.push(0n);
    words.push(instructions[i]);
  }
  const bytes = new Uint8Array(0x50 + words.length * 8),
    view = new DataView(bytes.buffer);
  // 自行编写的最小 SPH：SphType1/2、Version3；原生解码器使用阶段字段。
  // Minimal authored SPH: SphType1/2, Version3; native decoder uses stage.
  view.setUint32(0, (stage === "vs" ? 1 : 2) | (3 << 5) | ((stage === "vs" ? 1 : 5) << 10), true);
  view.setUint32(0x48, colorMask, true);
  for (let i = 0; i < words.length; i++) view.setBigUint64(0x50 + i * 8, words[i], true);
  return bytes;
}
const common = { tex: {}, img: {}, debug: true, reduced: false };
export const fixtures = [
  { name: "compute-exit", code: computeCode, job: { ...computeJob, key: "0000000000000001" } },
  {
    name: "vertex-position",
    code: graphics("vs", [ald4, ast4, exit]),
    job: { ...common, key: "0000000000000002", stage: "vs", attr: { 0: "float" }, fout: {} },
  },
  {
    name: "fragment-color",
    code: graphics(
      "ps",
      [mov32i(0, 0x3f800000), mov32i(1, 0x3f000000), mov32i(2, 0), mov32i(3, 0x3f800000), exit],
      0xf,
    ),
    job: { ...common, key: "0000000000000003", stage: "ps", attr: {}, fout: { 0: "float" } },
  },
];
