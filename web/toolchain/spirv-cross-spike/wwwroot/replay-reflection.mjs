// 直接按语义移植 dm/gfx/tools/dmsh_build.py 的反射辅助函数。
// 仅根据 MSL 与转换器提供的资源信息推导字段；不会虚构 chain/source/control 键，
// 也不会将 MSL 编译成 metallib。
// Direct semantic port of dm/gfx/tools/dmsh_build.py reflection helpers.
// This only derives fields from MSL and translator resource facts. It never
// invents chain/source/control keys or turns MSL into a compiled metallib.
const matches = (s, r) => [...s.matchAll(r)];
const TEX = 0x168,
  SAMP = 0x568,
  IMG = 0x120;
function decodeTex(name) {
  const m = /^[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)/.exec(name);
  if (!m) return null;
  const slots = m[1] ? Number(m[1]) : -1,
    h = parseInt(m[2], 16);
  const tb = slots >= 0 ? slots & 0xffff : 2;
  const sb = slots >= 0 && slots >>> 16 ? (slots >>> 16) - 1 : tb;
  const type = Math.floor(h / 0x10000000),
    words = [h & 0x3fff];
  if (type) words.push((h >>> 14) & 0x3fff);
  const slot = (w, base, cb) =>
    cb === 2 && w * 4 >= base && (w * 4 - base) % 8 === 0 ? (w * 4 - base) / 8 : null;
  const first = (base, cb, predicate) =>
    words
      .filter(predicate)
      .map((w) => slot(w, base, cb))
      .find((x) => x !== null) ?? null;
  if (/^[a-z]+_i/.test(name))
    return [first(IMG, tb, (w) => IMG <= w * 4 && w * 4 < TEX), null, type];
  return [first(TEX, tb, (w) => w * 4 < SAMP), first(SAMP, sb, (w) => w * 4 >= SAMP), type];
}
export function bindings(stage, msl, resources) {
  if (!Array.isArray(resources?.sbufs))
    throw new TypeError("Translator resource facts must include sbufs");
  const body = /struct spvDescriptorSetBuffer0\s*\{([\s\S]*?)\};/.exec(msl)?.[1] ?? "";
  const result = [];
  for (const m of matches(body, /^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];/gm)) {
    const ty = m[1].trim(),
      name = m[2],
      e = { msl_name: name, msl_type: ty, arg_id: Number(m[3]) };
    let n;
    if (name === "support_buffer") e.kind = "support";
    else if ((n = /^[a-z]+_c(\d+)$/.exec(name))) {
      const bank = Number(n[1]);
      Object.assign(
        e,
        bank >= 3 && bank <= 16
          ? { kind: "cbuf", reg: bank - 3 }
          : { kind: bank === 1 ? "c1" : "drvcb", bank },
      );
    } else if ((n = /^[a-z]+_s(\d+)$/.exec(name)))
      Object.assign(e, { kind: "sbuf", index: Number(n[1]) });
    else if (name.endsWith("Smplr")) {
      const decoded = decodeTex(name.slice(0, -5));
      if (!decoded)
        throw new Error("Unrecognized sampler name (native reflection would reject it)");
      const [t, s, type] = decoded;
      Object.assign(e, type ? { kind: "samp", reg: s } : { kind: "samp_tex", reg: t });
    } else {
      const d = decodeTex(name);
      Object.assign(
        e,
        d
          ? {
              kind: /^[a-z]+_i/.test(name) ? "img" : ty.includes("texture_buffer") ? "tbuf" : "tex",
              reg: d[0],
            }
          : { kind: "?" },
      );
    }
    result.push(e);
  }
  const base = { vs: 0x110, cs: 0x310, ps: 0x510 }[stage];
  const sorted = [...resources.sbufs].sort((a, b) => a.binding - b.binding);
  for (const e of result)
    if (e.kind === "sbuf") {
      const b = sorted[e.index],
        off = b?.cb_slot === 0 ? b.cb_offset * 4 : null;
      if (stage === "cs" && off !== null && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 === 0)
        Object.assign(e, { kind: "ubo_ptr", reg: (off - 0x210) / 16 });
      else
        e.reg =
          off !== null &&
          base !== undefined &&
          off - base >= 0 &&
          off - base < 0x100 &&
          (off - base) % 16 === 0
            ? (off - base) / 16
            : null;
      e.rw = !!(b && b.flags & 2);
    }
  return result;
}
export function driverReads(src) {
  const out = {};
  for (const bank of [0, 2]) {
    const offsets = new Set();
    let dynamic = false;
    for (const m of matches(src, new RegExp(`fp_c${bank}\\)\\._m0\\[([^\\]]+)\\]`, "g"))) {
      const idx = m[1].replace(/u+$/, "");
      if (/^\d+$/.test(idx)) offsets.add(Number(idx) * 16);
      else dynamic = true;
    }
    if (offsets.size || dynamic)
      out[bank] = { vec4_offsets: [...offsets].sort((a, b) => a - b), dynamic };
  }
  return out;
}
export function unfillable(reads) {
  const fill = {
      0: [
        [0x110, 0x210],
        [0x310, 0x410],
        [0x510, 0x610],
      ],
      2: [
        [TEX, SAMP],
        [SAMP, 0x968],
      ],
    },
    bad = [];
  for (const [bank, d] of Object.entries(reads)) {
    if (d.dynamic) bad.push(`c${bank} 动态下标`);
    for (const o of d.vec4_offsets)
      if (!fill[bank].some(([a, b]) => a <= o && o < b)) bad.push(`c${bank}+0x${o.toString(16)}`);
  }
  return bad;
}
export function rasterFacts(stage, src) {
  const out = /struct main0_out\s*\{([\s\S]*?)\};/.exec(src)?.[1] ?? "";
  if (stage === "ps")
    return {
      discard: Number(src.includes("discard_fragment")),
      sample_mask: Number(out.includes("sample_mask")),
      colors: [...new Set(matches(out, /\[\[color\((\d+)\)\]\]/g).map((m) => Number(m[1])))].sort(
        (a, b) => a - b,
      ),
    };
  if (stage !== "vs") return {};
  const b = /\bvertex\s+\w+\s+\w+\s*\([\s\S]*?\)\s*\{([\s\S]*)\}\s*$/.exec(src)?.[1];
  if (!b || /\b(if|for|while|switch|do)\b|clip_distance|ClipDistance/.test(b))
    return { pos_attr: -1 };
  const attr = Object.fromEntries(
    matches(src, /(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]/g).map((m) => [m[2], Number(m[3])]),
  );
  const vars = Object.fromEntries(
    matches(b, /float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);/g).map((m) => [m[1], [m[2], m[3]]]),
  );
  const last = Object.fromEntries(
    matches(b, /out\.gl_Position\.([xyzw])\s*=\s*([^;]+);/g).map((m) => [m[1], m[2].trim()]),
  );
  const m0 = vars[last.x];
  if (
    !m0 ||
    !(m0[0] in attr) ||
    [..."xyz"].some((c) => vars[last[c]]?.[0] !== m0[0] || vars[last[c]]?.[1] !== c)
  )
    return { pos_attr: -1 };
  const w =
    last.w === "1.0"
      ? "one"
      : vars[last.w]?.[0] === m0[0] && vars[last.w]?.[1] === "w"
        ? "attr"
        : null;
  if (!w) return { pos_attr: -1 };
  const locn = Object.fromEntries(
    matches(out, /(m_\d+) \[\[user\(locn(\d+)\)\]\]/g).map((m) => [m[1], Number(m[2])]),
  );
  const olast = Object.fromEntries(
    matches(b, /out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);/g).map((m) => [
      `${m[1]}.${m[2]}`,
      m[3].trim(),
    ]),
  );
  const uv = {},
    affine = {};
  function fm(v) {
    const escaped = v.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const m = new RegExp(
      `float ${escaped} = fma\\((_[\\w]+), \\(\\*spvDescriptorSet0\\.vp_c(\\d+)\\)\\._m0\\[(\\d+)\\]\\.([xyzw]), \\(\\*spvDescriptorSet0\\.vp_c(\\d+)\\)\\._m0\\[(\\d+)\\]\\.([xyzw])\\);`,
    ).exec(b);
    if (!m || !vars[m[1]]) return null;
    return [
      vars[m[1]],
      [Number(m[2]) - 3, Number(m[3]), "xyzw".indexOf(m[4])],
      [Number(m[5]) - 3, Number(m[6]), "xyzw".indexOf(m[7])],
    ];
  }
  for (const [mem, k] of Object.entries(locn)) {
    const sx = vars[olast[`${mem}.x`]],
      sy = vars[olast[`${mem}.y`]];
    if (sx && sy && sx[0] === sy[0] && sx[0] in attr && sx[1] === "x" && sy[1] === "y") {
      uv[k] = attr[sx[0]];
      continue;
    }
    const fx = fm(olast[`${mem}.x`] ?? ""),
      fy = fm(olast[`${mem}.y`] ?? "");
    if (
      fx &&
      fy &&
      fx[0][0] === fy[0][0] &&
      fx[0][0] in attr &&
      fx[0][1] === "x" &&
      fy[0][1] === "y" &&
      Math.min(fx[1][0], fx[2][0], fy[1][0], fy[2][0]) >= 0
    )
      affine[k] = { attr: attr[fx[0][0]], sx: fx[1], ox: fx[2], sy: fy[1], oy: fy[2] };
  }
  return { pos_attr: attr[m0[0]], pos_w: w, uv_from_attr: uv, uv_affine: affine };
}
export function replayFields(stage, msl, resources) {
  const entry = /(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(/.exec(msl);
  const reads = driverReads(msl);
  return {
    entry: entry?.[2] ?? "main0",
    msl_entry_kind: entry?.[1] ?? null,
    drv_reads: reads,
    drv_unfillable: unfillable(reads),
    bindings: bindings(stage, msl, resources),
    vertex_inputs: matches(msl, /(\w+)\s+\w+\s*\[\[attribute\((\d+)\)\]\]/g).map((m) => ({
      location: Number(m[2]),
      msl_type: m[1],
    })),
    raster: rasterFacts(stage, msl),
  };
}
