// 依据 dmsh_fetch.variant 和 c1_guard 生成原生着色器配套数据。
// Generate native shader companion data based on dmsh_fetch.variant and c1_guard.
/**
 * 移植 dmsh_fetch.variant 和 c1_guard 源码；不编译着色器。
 * Source port of dmsh_fetch.variant and c1_guard; no shader compilation.
 */
export function c1NeedsFullBuffer(source) {
  const definitions = new Map(
    [...source.matchAll(/\b(?:u?int|long|short)\s+(_\d+)\s*=\s*([^;]+);/g)].map((m) => [
      m[1],
      m[2],
    ]),
  );
  function bounded(index) {
    const names = [...index.matchAll(/\b(_\d+)\b/g)];
    let current = names.length ? names.at(-1)[1] : index.trim();
    for (let i = 0; i < 8; i++) {
      const expression = definitions.get(current);
      if (expression === undefined) return false;
      if (/\bmin\s*\(|\bclamp\s*\(|&\s*(?:0x[0-9a-fA-F]+|\d+)/.test(expression)) return true;
      const next = [...expression.matchAll(/\b(_\d+)\b/g)];
      if (!next.length) return false;
      current = next.at(-1)[1];
    }
    return false;
  }
  return [...source.matchAll(/(?:fp|vp)_c1\)\._m0\[([^\]]+)\]/g)].some(
    (m) => !/^\d+$/.test(m[1].trim()) && !bounded(m[1]),
  );
}
export function readableTileVariant(msl, bindings, omOnlyOk = false) {
  const tex = new Set(
    [...msl.matchAll(/texture2d<float> (\w+) \[\[id\(\d+\)\]\];/g)].map((m) => m[1]),
  );
  const input = msl.includes("struct main0_in") ? msl.split("struct main0_in", 2)[1] : msl;
  const locn = new Map(
    [...input.split("};", 1)[0].matchAll(/float4 (m_\d+) \[\[user\(locn(\d+)\)\]\];/g)].map((m) => [
      m[1],
      Number(m[2]),
    ]),
  );
  const reg = new Map(
    bindings
      .filter((b) => b.kind === "tex" && b.msl_name && b.reg !== null && b.reg !== undefined)
      .map((b) => [b.msl_name, b.reg]),
  );
  const calls = [],
    parts = [];
  let pos = 0;
  const pat =
    /spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+)Smplr, select\((_\d+), float2\(0\.0\), isnan\(\3\)\)\)/g;
  for (const m of msl.matchAll(pat)) {
    const member = m[1],
      variable = m[3];
    if (!tex.has(member) || member !== m[2] || !reg.has(member)) continue;
    const d = new RegExp(`float2 ${variable} = float2\\((_\\d+), (_\\d+)\\);`).exec(msl);
    const a = d && new RegExp(`float ${d[1]} = in\\.(m_\\d+)\\.x;`).exec(msl);
    const b = d && new RegExp(`float ${d[2]} = in\\.(m_\\d+)\\.y;`).exec(msl);
    if (!a || !b || a[1] !== b[1] || !locn.has(a[1])) continue;
    const i = calls.length;
    if (i >= 32) break;
    const sel = Array.from({ length: 8 }, (_, k) => `FX_${i} == ${k} ? fb${k} : `).join("");
    parts.push(msl.slice(pos, m.index), `(${sel}${m[0]})`);
    pos = m.index + m[0].length;
    calls.push({ fc: i, tex_reg: reg.get(member), varying_locn: locn.get(a[1]) });
  }
  if (!calls.length && !omOnlyOk) return null;
  const conv = new Map();
  for (const c of calls) conv.set(c.tex_reg, (conv.get(c.tex_reg) || 0) + 1);
  const mixed = [
    ...new Set(
      [...reg]
        .filter(
          ([mb, r]) =>
            tex.has(mb) && msl.split(`spvDescriptorSet0.${mb}.`).length - 1 > (conv.get(r) || 0),
        )
        .map(([, r]) => r),
    ),
  ].sort((a, b) => a - b);
  let body = parts.join("") + msl.slice(pos);
  const om = /struct main0_out\s*\{(.*?)\};/s.exec(body);
  if (!om) return null;
  const outs = [...om[1].matchAll(/(\w+) (m_\d+) \[\[color\((\d+)\)\]\];/g)].map((m) => [
    m[1],
    m[2],
    Number(m[3]),
  ]);
  if (
    !outs.length ||
    outs.length > 4 ||
    om[1].replace(/\w+ m_\d+ \[\[color\(\d+\)\]\];/g, "").trim()
  )
    return null;
  const members = outs
    .map(([t, n], j) =>
      Array.from(
        { length: 8 },
        (_, k) => `    ${t} ${n}_at${k} [[color(${k}), function_constant(OM_${j}_${k})]];\n`,
      ).join(""),
    )
    .join("");
  const start = om.index + om[0].indexOf("{") + 1;
  body = body.slice(0, start) + "\n" + members + body.slice(start + om[1].length);
  const consts =
    calls.map((c) => `constant int FX_${c.fc} [[function_constant(${c.fc})]];\n`).join("") +
    Array.from(
      { length: 8 },
      (_, k) => `constant bool FB_${k} [[function_constant(${32 + k})]];\n`,
    ).join("") +
    outs
      .map((_, j) =>
        Array.from(
          { length: 8 },
          (_, k) => `constant bool OM_${j}_${k} [[function_constant(${64 + 8 * j + k})]];\n`,
        ).join(""),
      )
      .join("");
  body = body.replace("using namespace metal;\n", "using namespace metal;\n\n" + consts);
  const i0 = body.indexOf("fragment main0_out main0("),
    i1 = i0 >= 0 ? body.indexOf(")\n{", i0) : -1;
  if (i0 < 0 || i1 < 0) return null;
  body =
    body.slice(0, i1) +
    Array.from(
      { length: 8 },
      (_, k) => `, float4 fb${k} [[color(${k}), function_constant(FB_${k})]]`,
    ).join("") +
    body.slice(i1);
  const decl = outs.map(([t, n]) => `    ${t} spv_o_${n} = ${t}(0);\n`).join("");
  body = body.replace("    main0_out out = {};\n", "    main0_out out = {};\n" + decl);
  for (const [, n] of outs) body = body.split(`out.${n}`).join(`spv_o_${n}`);
  const put = outs
    .map(([, n], j) =>
      Array.from(
        { length: 8 },
        (_, k) => `    if (OM_${j}_${k}) out.${n}_at${k} = spv_o_${n};\n`,
      ).join(""),
    )
    .join("");
  if (!body.includes("    return out;\n")) return null;
  body = body.split("    return out;\n").join(put + "    return out;\n");
  return {
    msl: body,
    metadata: {
      calls,
      mixed_regs: mixed,
      outputs: outs.map(([type, , color]) => ({ color, type })),
    },
  };
}
