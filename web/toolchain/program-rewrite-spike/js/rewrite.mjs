// 应用已校验规则并生成改写后的 NSO；依据 ../../../program-release-catalog.mjs、./binary.mjs 的模块与调用约定。
// Apply validated rules and produce rewritten NSO output using the module and calling contracts in ../../../program-release-catalog.mjs、./binary.mjs.
import {
  PROGRAM_REWRITE_LIMITS,
  PROGRAM_REWRITER_SOURCE,
  findRegisteredProgramRelease,
} from "../../../program-release-catalog.mjs";
import {
  check,
  createNativeBindings,
  hash,
  hex,
  hexAddress,
  loadNso,
  put32,
  range,
  u32,
  view,
} from "./binary.mjs";
import { B, has18, num } from "./arm64.mjs";
import { createRelocations, findWindows } from "./relocations.mjs";
import { SOURCE_SHA256 } from "./rule-data.mjs";
import { prepareRules, finishRules } from "./rules.mjs";
import { buildOutputs } from "./outputs.mjs";

export const KEY_INDEX = Object.freeze({
  K18: 0,
  KTLS: 1,
  KTLR: 2,
  KGATE: 3,
  KRES0: 4,
  KRES1: 5,
  KASPECT: 6,
  KASPECTUI: 7,
});
const sorted = (values) => [...values].sort((a, b) => a - b);
const isBranch = (mnemonic) =>
  ["b", "bl", "cbz", "cbnz", "tbz", "tbnz"].includes(mnemonic) || mnemonic.startsWith("b.");

function functionStarts(image, textEnd) {
  const mod0 = u32(image, 4);
  range(image, mod0, 20);
  check(u32(image, mod0) === 0x30444f4d, "Invalid MOD0 header");
  const eh = mod0 + view(image).getInt32(mod0 + 16, true);
  range(image, eh, 4);
  const readEncoded = (offset, encoding) => {
    const format = encoding & 0xf,
      application = encoding & 0x70;
    check(format === 3 || format === 0xb || format === 4, "Unsupported eh_frame_hdr encoding");
    const size = format === 4 ? 8 : 4;
    range(image, offset, size);
    let value;
    if (format === 4) value = view(image).getBigUint64(offset, true);
    else value = BigInt(format === 0xb ? view(image).getInt32(offset, true) : u32(image, offset));
    if (application === 0x10) value += BigInt(offset);
    if (application === 0x30) value += BigInt(eh);
    check(
      value >= BigInt(Number.MIN_SAFE_INTEGER) && value <= BigInt(Number.MAX_SAFE_INTEGER),
      "eh_frame_hdr value exceeds safe integer range",
    );
    return [Number(value), size];
  };
  const [, pointerEncoding, countEncoding, tableEncoding] = image.subarray(eh, eh + 4);
  let offset = eh + 4;
  const [, pointerSize] = readEncoded(offset, pointerEncoding);
  offset += pointerSize;
  const [count, countSize] = readEncoded(offset, countEncoding);
  offset += countSize;
  check(
    count >= 0 && count <= Math.floor((image.length - offset) / 8),
    "Invalid eh_frame_hdr entry count",
  );
  const starts = new Set();
  for (let i = 0; i < count; i++) {
    const [address, size] = readEncoded(offset, tableEncoding);
    offset += size;
    const [, secondSize] = readEncoded(offset, tableEncoding);
    offset += secondSize;
    if (address < textEnd) starts.add(address);
  }
  return sorted(starts);
}

function scan(context) {
  const { image, textEnd, bindings } = context;
  const starts = functionStarts(image, textEnd),
    kinds = new Map();
  for (let address = 0; address < textEnd; address += 4) {
    const word = u32(image, address),
      masked = (word & 0xffffffe0) >>> 0;
    if ((word & 0xffe0001f) >>> 0 === 0xd4000001)
      kinds.set(address, ["svc", (word >>> 5) & 0xffff]);
    else if (masked === 0xd53bd040) kinds.set(address, ["tpidr_r", null]);
    else if (masked === 0xd51bd040) kinds.set(address, ["tpidr_w", null]);
    else if (masked === 0xd53bd060) kinds.set(address, ["tpidrro_r", null]);
    else if (masked === 0xd53be020) kinds.set(address, ["cntpct", word & 31]);
    else if (masked === 0xd53be040) kinds.set(address, ["cntvct", word & 31]);
    else if ([0, 5, 10, 16].some((position) => ((word >>> position) & 31) === 18)) {
      const ins = bindings.decodeMaybe(word, address);
      if (ins && has18(ins)) kinds.set(address, ["x18", null]);
    }
  }
  const functionOf = (address) => {
    let low = 0,
      high = starts.length;
    while (low < high) {
      const middle = Math.floor((low + high) / 2);
      if (starts[middle] <= address) low = middle + 1;
      else high = middle;
    }
    const index = low - 1;
    return index >= 0 ? [starts[index], starts[index + 1] ?? textEnd] : [address, address + 4];
  };
  const funcs = new Map();
  for (const [address, [kind]] of kinds)
    if (kind === "x18") {
      const pair = functionOf(address);
      funcs.set(pair[0], pair);
    }
  const done = new Set(),
    windows = [],
    jumps = new Map();
  for (const [from, to] of [...funcs.values()].sort(([a], [b]) => a - b)) {
    const instructions = bindings.instructions(image, from, to),
      segments = findWindows(instructions);
    for (const segment of segments) {
      windows.push(segment);
      for (const ins of segment) done.add(ins.address);
    }
    for (let index = 0; index < instructions.length; index++) {
      const ins = instructions[index];
      if (ins.mnemonic !== "br" || ins.op_str !== "x18") continue;
      let base = null,
        width = null;
      const nearby = instructions.slice(Math.max(0, index - 8), index).reverse();
      for (const previous of nearby)
        if (previous.mnemonic === "adr" && previous.op_str.startsWith("x18,")) {
          base = previous.operands[1].imm;
          break;
        }
      for (const previous of nearby) {
        if (
          previous.mnemonic !== "add" ||
          !previous.op_str.startsWith("x18, x18,") ||
          !previous.op_str.includes("lsl #2")
        )
          continue;
        const register = num(previous.operands[2].reg);
        for (const load of nearby) {
          if (["ldrb", "ldrsb"].includes(load.mnemonic) && num(load.operands[0].reg) === register)
            width = 256;
          if (["ldrh", "ldrsh"].includes(load.mnemonic) && num(load.operands[0].reg) === register)
            width = 65536;
          if (width) break;
        }
        break;
      }
      check(base && width, `jump table ${ins.address.toString(16)}`);
      jumps.set(ins.address, [base, width]);
    }
    const inWindow = new Set(segments.flatMap((segment) => segment.map((ins) => ins.address)));
    const heads = new Set(segments.map((segment) => segment[0].address));
    for (const ins of instructions)
      if (isBranch(ins.mnemonic)) {
        const target = ins.operands.at(-1).imm;
        check(
          !inWindow.has(target) || heads.has(target),
          `branch into window ${ins.address.toString(16)}`,
        );
      }
  }
  return { starts, kinds, done, windows, jumps };
}

/**
 * 供明确开发用途的底层移植，交付调用方必须使用下方可信工厂。
 * Low-level port for explicit development use. Shipping callers must use the trusted factory below. */
export async function rewriteNso({
  bytes,
  moduleName = "main",
  ruleset = "native",
  privateGates = true,
  native,
  bindings = null,
  bootlist = null,
}) {
  check(SOURCE_SHA256 === PROGRAM_REWRITER_SOURCE, "Program source pin mismatch");
  check(
    typeof moduleName === "string" && /^[A-Za-z0-9_-]+$/.test(moduleName),
    "Invalid module name",
  );
  bindings ??= createNativeBindings(native);
  const context = { ...loadNso(bytes, bindings.decompress), moduleName, bindings, stats: {} };
  const rules = await prepareRules({ ...context, ruleset, privateGates });
  Object.assign(context, scan(context));
  const r = createRelocations(context, bindings, KEY_INDEX);
  context.relocator = r;
  const putBranch = (at, to) => put32(context.image, at, B(at, to));
  const stat = (kind) => {
    context.stats[kind] = (context.stats[kind] ?? 0) + 1;
  };
  for (const segment of context.windows) {
    const [start, low] = r.siteWindow(segment);
    stat("window");
    putBranch(low, start);
  }
  for (const address of sorted(context.kinds.keys())) {
    if (context.done.has(address)) continue;
    const [kind, arg] = context.kinds.get(address),
      ins = bindings.decode(u32(context.image, address), address);
    let start;
    if (kind === "x18") {
      const mnemonic = ins.mnemonic;
      if (mnemonic === "br") {
        start = r.siteBr(ins, ...context.jumps.get(address));
        stat("br");
      } else if (["cbz", "cbnz", "tbz", "tbnz"].includes(mnemonic)) {
        start = r.siteCond(ins);
        stat("cond");
      } else if (["adr", "adrp"].includes(mnemonic)) {
        start = r.sitePcrel(ins);
        stat("pcrel");
      } else if (
        ["blr", "ret", "b", "bl"].includes(mnemonic) ||
        (mnemonic === "ldr" && !ins.op_str.includes("["))
      )
        throw new Error(`${address.toString(16)} ${mnemonic} ${ins.op_str}`);
      else {
        start = r.sitePlain(ins);
        stat("plain");
      }
    } else {
      if (kind === "tpidr_r") start = r.siteSysregRead(ins, "KTLS");
      else if (kind === "tpidrro_r") start = r.siteSysregRead(ins, "KTLR");
      else if (kind === "tpidr_w") start = r.siteTpidrWrite(ins);
      else if (["svc", "cntpct", "cntvct"].includes(kind)) start = r.siteGate(ins, kind, arg);
      else throw new Error(`Unknown rewrite kind at ${hexAddress(address)}`);
      stat(kind);
    }
    putBranch(address, start);
  }
  finishRules(context, rules);
  const files = await buildOutputs(context, rules, KEY_INDEX, bootlist);
  return { files, stats: context.stats };
}

/**
 * 可直接替换的候选适配器；验证等价性前，生产 Python 路径保持独立。
 * Drop-in candidate adapter. The production Python path remains separate until equivalence is verified. */
export function createJavaScriptProgramRewriter(initialize) {
  if (typeof initialize !== "function") throw new TypeError("Missing trusted runtime initializer");
  return async function rewriteProgram({ bytes, caseName, releaseId, moduleName = "main" }) {
    check(
      bytes instanceof Uint8Array &&
        bytes.length >= 256 &&
        bytes.length <= PROGRAM_REWRITE_LIMITS.moduleInputBytes,
      "Invalid bounded program input",
    );
    const inputHash = await hash(bytes);
    let registered, release;
    if (releaseId !== undefined) {
      release = findRegisteredProgramRelease(releaseId);
      registered = release?.modules.find((entry) => entry.name === moduleName);
      check(
        release && caseName === undefined && registered && registered.inputSha256 === inputHash,
        "Unsupported registered program input",
      );
      check(
        !registered.buildId || hex(bytes.subarray(0x40, 0x50)) === registered.buildId,
        "Registered build ID mismatch",
      );
    } else check(bytes.length <= 1024 * 1024, "Invalid bounded synthetic input");
    const runtime = await initialize(),
      { manifest } = runtime;
    check(
      manifest?.sourceSha256 === SOURCE_SHA256 &&
        PROGRAM_REWRITER_SOURCE === SOURCE_SHA256 &&
        (!release || release.sourceSha256 === SOURCE_SHA256),
      "Program source pin mismatch",
    );
    const fixture = manifest.cases.find(
      (entry) => entry.name === caseName && (entry.moduleName ?? "main") === moduleName,
    );
    check(
      registered || (fixture && inputHash === fixture.sha256),
      "Unsupported input: only exact registered release hashes or generated synthetic fixtures are enabled",
    );
    const { files } = await rewriteNso({
      bytes,
      moduleName,
      ruleset: registered ? release.ruleset : fixture.ruleset,
      privateGates: true,
      native: runtime.native,
      bindings: runtime.bindings ?? null,
    });
    return {
      status: registered ? "repository-registered-unverified" : "synthetic-only",
      realGameVerified: false,
      files,
    };
  };
}
