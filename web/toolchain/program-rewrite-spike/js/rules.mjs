// 准备并完成程序改写规则；依据 ./binary.mjs、./arm64.mjs 的模块与调用约定。
// Prepare and finalize program-rewrite rules using the module and calling contracts in ./binary.mjs、./arm64.mjs.
import { check, hash, hex, hexAddress, put32, u32, unhex, wordHex } from "./binary.mjs";
import {
  B,
  BLR,
  CBZ,
  LDP_POST,
  LDRX,
  LDUR,
  MRS_TPIDRRO,
  NOP,
  STP_PRE,
  STUR,
  regsIn,
} from "./arm64.mjs";
import {
  CUTS,
  OBS,
  PARAMS,
  PRIVATE_BRANCHES,
  PRIVATE_OBS,
  RULE_BUILD,
  RUN_REASONS,
  RUN_WORDS,
} from "./rule-data.mjs";

/**
 * 应用预扫描裁剪，保留精确源码顺序及审计文本。
 * Apply pre-scan cuts, retaining their exact source ordering and audit text. */
export async function prepareRules({
  image,
  buildId,
  moduleName,
  ruleset = "native",
  privateGates = true,
}) {
  check(ruleset === "native" || ruleset === "touch-run-v4", `未知规则域 ${ruleset}`);
  const ruleRun = ruleset === "touch-run-v4" && moduleName === "main";
  if (ruleRun) check(buildId === RULE_BUILD, `Run规则构建号不符 ${buildId}`);
  // Python 参考对解压后的映像及其追加的 16 KiB 一起计算哈希。
  // The Python oracle hashes the decompressed image including its appended 16 KiB.
  const originalHash = ruleRun ? await hash(image) : null;
  const privateAim = ruleRun && privateGates;
  const cuts = (CUTS[buildId] ?? []).map((row) => [...row]);
  if (privateAim)
    cuts.push([
      0x2faf04,
      "fde7fe97",
      "c11000d4",
      "skill-aim-v1-private: skill ground direction call only; svc86 delegates original2b4ef8 then sampled-right/original rotation+grid; zero/nonplayer/non-skill retain original; busy/town/skill/server untouched",
      "仅宿主",
    ]);
  if (ruleRun) {
    const movzW14 = (value) => wordHex(0x52800000 | (value << 5) | 14);
    cuts.push([
      0x168a580,
      movzW14(2100),
      movzW14(1024),
      "阴影图边长 mov w14,#2100 → mov w14,#1024（档位 1）",
      "仅宿主",
    ]);
    const cbzX8 = (at, to) => wordHex(0xb4000008 | ((((to - at) >> 2) & 0x7ffff) << 5));
    const strX19 = (rt) => wordHex(0xf9000000 | ((0x178 / 8) << 10) | (19 << 5) | rt);
    cuts.push([
      0xab5c28,
      strX19(8),
      strX19(31),
      "系统通知的确认动作类别 str x8,[x19,#0x178] → str xzr（不带确认动作）",
      "仅宿主",
    ]);
    cuts.push([
      0x3d853c,
      cbzX8(0x3d853c, 0x3d8560),
      cbzX8(0x3d853c, 0x3d880c),
      "通知面板：确认动作类别为空 → 不处理 PLUS",
      "仅宿主",
    ]);
    cuts.push([
      0x3d8544,
      cbzX8(0x3d8544, 0x3d8560),
      cbzX8(0x3d8544, 0x3d880c),
      "通知面板：确认动作哈希为空 → 不处理 PLUS",
      "仅宿主",
    ]);
    cuts.push([
      0xc35fb0,
      wordHex(0xb3407d01),
      wordHex(0xf2800000 | (3000 << 5) | 1),
      "技能说明框横坐标给到屏幕外，由原回夹贴右边（不盖技能树）",
      "仅宿主",
    ]);
  }
  if (ruleRun && privateGates) {
    for (const [off, old, replacement, why] of RUN_WORDS) {
      const word = replacement ?? B(off, off === 0x369a90 ? 0x369900 : 0x6dce50);
      cuts.push([off, wordHex(old), wordHex(word), `touch-run-v4：${why}`]);
    }
  }
  for (const row of cuts) {
    if (row.length === 4) row.push("全");
    const [off, original, replacement] = row,
      old = unhex(original),
      next = unhex(replacement);
    check(
      old.length === next.length && hex(image.subarray(off, off + old.length)) === original,
      `剪口原字节不符 ${hexAddress(off)}：${hex(image.subarray(off, off + old.length))}`,
    );
    image.set(next, off);
  }
  return {
    buildId,
    ruleset,
    ruleRun,
    privateGates,
    privateAim,
    originalHash,
    cuts,
    obs: [],
    params: [],
    ruleBlocks: [],
  };
}

/**
 * 按参考顺序生成扫描后门控、参数槽及原生规则跳板。
 * Emit post-scan gates, parameter slots and native rule trampolines in oracle order. */
export function finishRules(context, rules) {
  const {
    image,
    textEnd,
    starts,
    kinds,
    windows,
    relocator: r,
    bindings: { decode },
  } = context;
  const { emit, here, emitGateCall, emitSlotAddr, siteObs } = r;
  const startsSet = new Set(starts);
  const inWindow = (off) =>
    windows.some((seg) => seg[0].address <= off && off < seg.at(-1).address + 4);
  const putBranch = (at, to) => put32(image, at, B(at, to));
  const stat = (key) => {
    context.stats[key] = (context.stats[key] ?? 0) + 1;
  };
  function privateBranch(row) {
    const [off, word, target, svc, kind, why] = row;
    check(u32(image, off) === word, `Private gate original word differs at ${hexAddress(off)}`);
    if (svc === 0x89)
      check(startsSet.has(off) && startsSet.has(target), "Rift exit is not a function entry");
    check(
      off < textEnd && !kinds.has(off) && !inWindow(off),
      `Private gate conflicts at ${hexAddress(off)}`,
    );
    const start = here();
    emit(STP_PRE(8, 31, 31, -16));
    emit(STP_PRE(29, 30, 31, -16));
    emitGateCall();
    emit(BLR(30));
    const ret = here();
    emit(LDP_POST(29, 30, 31, 16));
    emit(0x34000068);
    emit(LDP_POST(8, 31, 31, 16));
    emit(B(here(), target));
    emit(LDP_POST(8, 31, 31, 16));
    emit(word);
    emit(B(here(), off + 4));
    r.gates.push([ret, "svc", svc, off]);
    putBranch(off, start);
    stat(kind);
    rules.cuts.push([off, wordHex(word), wordHex(B(off, start)), why, "仅宿主"]);
  }
  if (rules.ruleRun && rules.privateGates) privateBranch(PRIVATE_BRANCHES[0]);
  else if (rules.ruleRun) {
    // 精确保留原私有视觉门控的占用空间，不写可执行 NOP。
    // Reserve exactly the former private visual gate footprint, not executable NOPs.
    for (let i = 0; i < 0x80 / 4; i++) emit(0);
    stat("private_gates_reserved");
  }
  rules.obs = (OBS[rules.buildId] ?? []).map((row) => [...row]);
  if (rules.ruleRun && rules.privateGates) rules.obs.push(...PRIVATE_OBS.map((row) => [...row]));
  for (const [off, oid, original] of rules.obs) {
    const word = u32(image, off),
      actual = word.toString(16).padStart(8, "0");
    check(actual === original, `观测门原指令不符 ${hexAddress(off)}：${actual}`);
    check(
      startsSet.has(off) || (oid === 6 && off === 0x43ecc8),
      `private obs point not approved ${hexAddress(off)}`,
    );
    check(
      off < textEnd && !kinds.has(off) && !inWindow(off),
      `观测门与改写点冲突 ${hexAddress(off)}`,
    );
    putBranch(off, siteObs(off, oid, word));
    stat("obs");
  }
  if (rules.ruleRun && rules.privateGates)
    for (const row of PRIVATE_BRANCHES.slice(1)) privateBranch(row);
  rules.params = (PARAMS[rules.buildId] ?? []).map((row) => [...row]);
  for (const [oa, wa, ob, wb, reg, key] of rules.params) {
    for (const [off, original] of [
      [oa, wa],
      [ob, wb],
    ]) {
      const actual = u32(image, off).toString(16).padStart(8, "0");
      check(actual === original, `参数位原指令不符 ${hexAddress(off)}：${actual}`);
      check(!kinds.has(off) && !inWindow(off), `参数位与改写点冲突 ${hexAddress(off)}`);
    }
    for (let off = oa + 4; off < ob; off += 4) {
      const ins = decode(u32(image, off), off);
      check(
        !regsIn(ins).has(reg),
        `参数位中间用到 x${reg}：${hexAddress(off)} ${ins.mnemonic} ${ins.op_str}`,
      );
    }
    const start = here(),
      tmp = reg === 16 ? 17 : 16;
    emit(STUR(tmp, 31, -16));
    emit(MRS_TPIDRRO(reg));
    emitSlotAddr(reg, tmp, key);
    emit(LDRX(reg, reg));
    emit(LDUR(tmp, 31, -16));
    emit(B(here(), oa + 4));
    putBranch(oa, start);
    put32(image, ob, NOP);
    stat("param");
  }
  if (!rules.ruleRun) return;
  const run = (off, old, body, why) => {
    check(u32(image, off) === old, `Run跳板原字不符 ${hexAddress(off)}`);
    const start = here(),
      words = body(start);
    for (const word of words) emit(word);
    const next = B(off, start);
    put32(image, off, next);
    rules.cuts.push([off, wordHex(old), wordHex(next), why, "全"]);
    rules.ruleBlocks.push([start, words.map(wordHex).join(""), why]);
  };
  run(
    0x6c6648,
    0x34000060,
    (a) => [
      0xb9400268,
      CBZ(a + 4, a + 12, 8, true),
      B(a + 8, 0x6c664c),
      CBZ(a + 12, a + 20, 0),
      B(a + 16, 0x6c664c),
      B(a + 20, 0x6c6654),
    ],
    RUN_REASONS[0],
  );
  run(
    0x6c6290,
    0x710006df,
    (a) => [
      0xb9400268,
      CBZ(a + 4, a + 12, 8, true),
      B(a + 8, 0x6c62ec),
      0x710006df,
      B(a + 16, 0x6c6294),
    ],
    RUN_REASONS[1],
  );
  run(
    0x4ac368,
    0x35000460,
    (a) => [
      0xb9400268,
      CBZ(a + 4, a + 12, 8),
      B(a + 8, a + 16),
      B(a + 12, 0x4ac3f4),
      CBZ(a + 16, a + 24, 0),
      B(a + 20, 0x4ac3f4),
      B(a + 24, 0x4ac36c),
    ],
    RUN_REASONS[2],
  );
  run(
    0x6c6580,
    0x51000849,
    (a) => [
      CBZ(a, a + 16, 8, true),
      0x51000849,
      CBZ(a + 8, a + 16, 9, true),
      0x52800062,
      0x51000849,
      B(a + 20, 0x6c6584),
    ],
    RUN_REASONS[3],
  );
}
