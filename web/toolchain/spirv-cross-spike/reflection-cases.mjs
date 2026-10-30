// 自行编写的类 MSL 字符串用于独立覆盖重放解析器分支，不依赖
// Authored MSL-like strings to cover replay parser branches independently of
// 小型已编译 SPIR-V 样本；不声称这些字符串可编译为 Metal。
// the tiny compiled SPIR-V fixtures. These are not claimed to compile as Metal.
const resources = {
  sbufs: [
    { binding: 4, cb_slot: 0, cb_offset: 0x210 / 4, flags: 2 },
    { binding: 7, cb_slot: 0, cb_offset: 0x320 / 4, flags: 0 },
    { binding: 9, cb_slot: 2, cb_offset: 0, flags: 2 },
  ],
};
const argNames = [
  "support_buffer",
  "fp_c0",
  "fp_c1",
  "fp_c2",
  "fp_c3",
  "fp_c16",
  "fp_c17",
  "fp_s0",
  "fp_s1",
  "fp_s2",
  "fp_s9",
  "fp_t_tcb_5A",
  "fp_t_tcb_5ASmplr",
  "fp_i_tcb_48",
  "fp_t_cb2_5A",
  "unknown",
];
const descriptor = `struct spvDescriptorSetBuffer0 {\n${argNames.map((name, i) => `  ${name === "fp_t_cb2_5A" ? "texture_buffer<float>" : "constant float*"} ${name} [[id(${i})]];`).join("\n")}\n};\n`;
const driver =
  "(*spvDescriptorSet0.fp_c0)._m0[17u]; (*spvDescriptorSet0.fp_c0)._m0[0]; (*spvDescriptorSet0.fp_c2)._m0[index]; (*spvDescriptorSet0.fp_c2)._m0[90u];";
const vs = `struct main0_in { float4 a [[attribute(0)]]; float2 uv [[attribute(3)]]; };
struct main0_out { float2 m_0 [[user(locn0)]]; float2 m_1 [[user(locn1)]]; };
vertex main0_out main0(main0_in in) {
 float _x = in.a.x; float _y = in.a.y; float _z = in.a.z; float _w = in.a.w;
 float _u = in.uv.x; float _v = in.uv.y;
 float _fx = fma(_u, (*spvDescriptorSet0.vp_c3)._m0[1].x, (*spvDescriptorSet0.vp_c4)._m0[2].y);
 float _fy = fma(_v, (*spvDescriptorSet0.vp_c3)._m0[1].z, (*spvDescriptorSet0.vp_c4)._m0[2].w);
 out.gl_Position.x = _x; out.gl_Position.y = _y; out.gl_Position.z = _z; out.gl_Position.w = 1.0;
 out.m_0.x = _u; out.m_0.y = _v; out.m_1.x = _fx; out.m_1.y = _fy;
 return out;
}`;
export const cases = [
  ...["cs", "ps", "vs", "hs"].map((stage) => ({ stage, resources, msl: descriptor + driver })),
  {
    stage: "ps",
    resources,
    msl: "struct main0_out { float4 c [[color(3)]]; float4 d [[color(0)]]; uint mask [[sample_mask]]; }; fragment main0_out main0() { discard_fragment(); }",
  },
  ...[
    vs,
    vs.replace("= 1.0;", "= _w;"),
    vs.replace("return out;", "if (true) return out;"),
    vs.replace("out.gl_Position.z = _z", "out.gl_Position.z = _x"),
  ].map((msl) => ({ stage: "vs", resources, msl })),
];
