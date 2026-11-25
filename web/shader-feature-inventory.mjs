// 仅统计生成的 MSL 词法特征，不证明编译、语义或 iOS 兼容性。
// Inventory generated MSL lexically; this does not prove compilation, semantics or iOS compatibility.
const STAGES = ["vs", "ps", "cs", "hs", "ds"];
const ATTRIBUTES = [
  "buffer",
  "id",
  "texture",
  "sampler",
  "stage_in",
  "attribute",
  "user",
  "position",
  "color",
  "depth",
  "sample_id",
  "sample_mask",
  "vertex_id",
  "instance_id",
  "base_vertex",
  "base_instance",
  "front_facing",
  "point_size",
  "point_coord",
  "clip_distance",
  "flat",
  "center_perspective",
  "center_no_perspective",
  "centroid_perspective",
  "centroid_no_perspective",
  "sample_perspective",
  "sample_no_perspective",
  "function_constant",
  "patch",
  "patch_id",
  "position_in_patch",
  "patch_control_point",
  "thread_position_in_grid",
  "thread_position_in_threadgroup",
  "thread_index_in_threadgroup",
  "threadgroup_position_in_grid",
  "threads_per_threadgroup",
  "threadgroups_per_grid",
];
const TEXTURES = [
  "texture1d",
  "texture1d_array",
  "texture2d",
  "texture2d_array",
  "texture2d_ms",
  "texture2d_ms_array",
  "texture3d",
  "texturecube",
  "texturecube_array",
  "texture_buffer",
  "depth2d",
  "depth2d_array",
  "depth2d_ms",
  "depth2d_ms_array",
  "depthcube",
  "depthcube_array",
];
const FEATURES = {
  threadgroupMemory: /\bthreadgroup\b/,
  barriers: /\b(?:threadgroup_barrier|simdgroup_barrier)\s*\(/,
  atomics: /\batomic_\w+\b/,
  simdgroup: /\b(?:simd_\w+|simdgroup_\w+)\b/,
  derivatives: /\b(?:dfdx|dfdy|fwidth)\s*\(/,
  discard: /\bdiscard_fragment\s*\(/,
  textureSampling: /\.\s*(?:sample|sample_compare)\s*\(/,
  textureGather: /\.\s*(?:gather|gather_compare)\s*\(/,
  textureReadWrite: /\.\s*(?:read|write)\s*\(/,
  textureQueries: /\.\s*get_(?:width|height|depth|array_size|num_mip_levels|num_samples)\s*\(/,
  templates: /\btemplate\s*</,
  loops: /\b(?:for|while|do)\b/,
  switches: /\bswitch\s*\(/,
  preprocessor: /^\s*#/m,
};
const MAX_SOURCE_UNITS = 8 * 1024 * 1024;
const MAX_RECORDS = 100_000;
function fail(code) {
  const error = new Error(code);
  error.code = code;
  throw error;
}
function counts(keys) {
  return Object.fromEntries(keys.map((key) => [key, 0]));
}

// 注释及字符串不能伪造特征；保留空格边界，不返回用户源码。
// Comments and literals cannot spoof features; keep token boundaries and never return user source.
function lexicalCode(source) {
  const parts = [];
  let start = 0;
  for (let i = 0; i < source.length;) {
    let end = i;
    if (source.startsWith("//", i)) {
      end = source.indexOf("\n", i + 2);
      if (end < 0) end = source.length;
    } else if (source.startsWith("/*", i)) {
      end = source.indexOf("*/", i + 2);
      if (end < 0) fail("MSL_INVENTORY_UNTERMINATED_COMMENT");
      end += 2;
    } else if (source[i] === '"' || source[i] === "'") {
      const quote = source[i];
      if (quote === '"' && source[i - 1] === "R") {
        const open = source.indexOf("(", i + 1);
        if (open < 0 || open - i > 17) fail("MSL_INVENTORY_INVALID_RAW_LITERAL");
        const delimiter = source.slice(i + 1, open);
        if (/[\s()\\]/.test(delimiter)) fail("MSL_INVENTORY_INVALID_RAW_LITERAL");
        const close = source.indexOf(`)${delimiter}"`, open + 1);
        if (close < 0) fail("MSL_INVENTORY_UNTERMINATED_LITERAL");
        end = close + delimiter.length + 2;
      } else {
        end = i + 1;
        while (end < source.length && source[end] !== quote) {
          if (source[end] === "\\") end++;
          end++;
        }
        if (end >= source.length) fail("MSL_INVENTORY_UNTERMINATED_LITERAL");
        end++;
      }
    }
    if (end > i) {
      if (parts.length >= 131072) fail("MSL_INVENTORY_TOKEN_LIMIT");
      parts.push(source.slice(start, i), source.slice(i, end).replace(/[^\r\n]/g, " "));
      i = end;
      start = end;
    } else i++;
  }
  parts.push(source.slice(start));
  return parts.join("");
}

/**
 * 逐个接收真实生成源码，只保存有界汇总，不保存源码、入口名称或文件名。
 * Accept real generated sources one at a time; retain bounded totals, not source, entry names or filenames.
 * Counts include inactive preprocessor branches. They are lexical coverage requirements, not compiler validation.
 */
export function createShaderFeatureInventory() {
  let state = {
    schema: 1,
    analysis: "lexical-only",
    records: 0,
    stages: counts(STAGES),
    variants: { base: 0, readableTile: 0 },
    attributes: counts(ATTRIBUTES),
    unknownAttributeOccurrences: 0,
    textureTypeOccurrences: counts(TEXTURES),
    featureShaderCounts: counts(Object.keys(FEATURES)),
    functionConstants: { numericAttributeOccurrences: 0, symbolicAttributeOccurrences: 0 },
  };
  return {
    add(source, { stage, variant = "base" } = {}) {
      if (
        typeof source !== "string" ||
        !STAGES.includes(stage) ||
        !["base", "readableTile"].includes(variant)
      )
        fail("MSL_INVENTORY_INVALID_INPUT");
      if (source.length > MAX_SOURCE_UNITS || state.records >= MAX_RECORDS)
        fail("MSL_INVENTORY_LIMIT");
      // 失败不能留下半条记录。 / Failure must not leave a partially counted record.
      const code = lexicalCode(source.replace(/\\\r?\n/g, ""));
      const next = structuredClone(state);
      next.records++;
      next.stages[stage]++;
      next.variants[variant]++;
      for (const [key, pattern] of Object.entries(FEATURES))
        if (pattern.test(code)) next.featureShaderCounts[key]++;
      for (const type of TEXTURES) {
        const pattern = new RegExp(`\\b${type}\\s*<`, "g");
        while (pattern.exec(code)) next.textureTypeOccurrences[type]++;
      }
      for (const block of code.matchAll(/\[\[([\s\S]*?)\]\]/g)) {
        // 参数内部逗号不开始新属性。 / Commas inside arguments do not start a new attribute.
        const attributes = [];
        let depth = 0,
          start = 0;
        for (let i = 0; i <= block[1].length; i++) {
          const ch = block[1][i];
          if (ch === "(") depth++;
          else if (ch === ")") depth--;
          if (depth < 0) fail("MSL_INVENTORY_INVALID_ATTRIBUTE");
          if (i === block[1].length || (ch === "," && depth === 0)) {
            attributes.push(block[1].slice(start, i));
            start = i + 1;
          }
        }
        if (depth !== 0) fail("MSL_INVENTORY_INVALID_ATTRIBUTE");
        for (const attribute of attributes) {
          const name = /^\s*([a-zA-Z_]\w*)\b/.exec(attribute)?.[1];
          if (!name || !Object.hasOwn(next.attributes, name)) next.unknownAttributeOccurrences++;
          else next.attributes[name]++;
          if (name === "function_constant") {
            const expression = /^\s*function_constant\s*\(([^()]*)\)\s*$/
              .exec(attribute)?.[1]
              .trim();
            if (expression !== undefined && /^(?:\d+|0[xX][\da-fA-F]+)[uU]?$/.test(expression))
              next.functionConstants.numericAttributeOccurrences++;
            else next.functionConstants.symbolicAttributeOccurrences++;
          }
        }
      }
      state = next;
    },
    snapshot() {
      return structuredClone(state);
    },
  };
}
