// 按固定源版本移植 patch_nso.py 的重定位代码块；仍由 Capstone 解码。
// Source-pinned ports of patch_nso.py's relocation blocks. Capstone remains the decoder.
import { check, put32 } from "./binary.mjs";
import {
  B,
  STP,
  LDP,
  STP_PRE,
  LDP_POST,
  STUR,
  LDUR,
  LDRX,
  STRX,
  MRS_TPIDRRO,
  ADRP,
  ADDI,
  ADDR,
  SUBI,
  BR,
  BLR,
  NOP,
  has18,
  regsIn,
  pick,
  substitute,
  spDelta,
} from "./arm64.mjs";
import { PROGRAM_REWRITE_LIMITS } from "../../../program-release-catalog.mjs";
const EXCL_LD = new Set(["ldxr", "ldaxr", "ldxp", "ldaxp", "ldxrb", "ldaxrb", "ldxrh", "ldaxrh"]);
const EXCL_ST = new Set(["stxr", "stlxr", "stxp", "stlxp", "stxrb", "stlxrb", "stxrh", "stlxrh"]);
const condition = (m) => ["cbz", "cbnz", "tbz", "tbnz"].includes(m) || m.startsWith("b.");
export function findWindows(instructions) {
  const out = [];
  for (let i = 0; i < instructions.length; i++) {
    if (!EXCL_LD.has(instructions[i].mnemonic)) continue;
    let j = i;
    while (j < instructions.length && !EXCL_ST.has(instructions[j].mnemonic)) j++;
    if (j >= instructions.length || j - i >= 16) continue;
    if (j + 1 < instructions.length && ["cbz", "cbnz"].includes(instructions[j + 1].mnemonic)) j++;
    const segment = instructions.slice(i, j + 1);
    if (segment.some(has18)) out.push(segment);
  }
  return out;
}
export function createRelocations(context, bindings, keyIndex) {
  const { patchOff, keyPageOff } = context;
  const { decode, decodeMaybe } = bindings;
  let buffer = new Uint8Array(65536),
    length = 0;
  const fix = [],
    gates = [];
  const here = () => patchOff + length;
  function reserveBytes(count) {
    check(
      Number.isSafeInteger(count) &&
        count >= 0 &&
        count % 4 === 0 &&
        length + count <= PROGRAM_REWRITE_LIMITS.totalOutputBytes - patchOff,
      "Patch byte budget exceeded",
    );
    if (length + count > buffer.length) {
      const next = new Uint8Array(
        Math.max(
          length + count,
          Math.min(buffer.length * 2, PROGRAM_REWRITE_LIMITS.totalOutputBytes - patchOff),
        ),
      );
      next.set(buffer.subarray(0, length));
      buffer = next;
    }
    length += count;
  }
  function emit(word, kind = null) {
    if (kind) fix.push([length, kind]);
    const at = length;
    reserveBytes(4);
    put32(buffer, at, word);
  }
  function emitKeyAddr(reg, kind) {
    check(Object.hasOwn(keyIndex, kind), "Unknown key slot");
    fix.push([length, kind]);
    emit(ADRP(reg, here(), keyPageOff));
    emit(LDRX(reg, reg, keyIndex[kind]));
  }
  function emitSlotAddr(base, keyReg, kind) {
    emitKeyAddr(keyReg, kind);
    emit(ADDR(base, base, keyReg));
  }
  function sitePlain(ins) {
    const [s, t] = pick(regsIn(ins)),
      delta = spDelta(ins),
      save = delta === 0 ? -16 : -112;
    check(delta === 0 || delta >= -96, "Unsupported SP delta");
    // 发出任何字节前验证恢复地址，不能让缩放立即数截断或回绕。
    // Validate the restore address before emitting bytes; scaled immediates must not truncate or wrap.
    const restore = LDP(s, t, 31, save - delta);
    const start = here();
    emit(STP(s, t, 31, save));
    emit(MRS_TPIDRRO(t));
    emitSlotAddr(t, s, "K18");
    emit(LDRX(s, t));
    emit(substitute(ins, s, decodeMaybe));
    emit(STRX(s, t));
    emit(restore);
    emit(B(here(), ins.address + 4));
    return start;
  }
  function siteCond(ins) {
    const word = ins.word,
      [s, t] = pick(regsIn(ins)),
      target = ins.operands.at(-1).imm,
      start = here();
    emit(STP(s, t, 31, -16));
    emit(MRS_TPIDRRO(t));
    emitSlotAddr(t, s, "K18");
    emit(LDRX(s, t));
    const m = ins.mnemonic,
      taken = here() + 12,
      off = (taken - here()) / 4;
    const nw =
      (["cbz", "cbnz"].includes(m)
        ? (word & 0xff000000) | ((off & 0x7ffff) << 5) | s
        : (word & 0xfff80000) | ((off & 0x3fff) << 5) | s) >>> 0;
    const pc = here();
    emit(nw);
    const dd = decode(nw, pc);
    check(dd.mnemonic === m && dd.operands.at(-1).imm === taken, "Conditional relocation mismatch");
    emit(LDP(s, t, 31, -16));
    emit(B(here(), ins.address + 4));
    check(here() === taken, "Conditional stub offset mismatch");
    emit(LDP(s, t, 31, -16));
    emit(B(here(), target));
    return start;
  }
  function sitePcrel(ins) {
    const target = ins.operands[1].imm,
      [s, t] = pick(new Set()),
      start = here();
    emit(STP(s, t, 31, -16));
    emit(MRS_TPIDRRO(t));
    emitSlotAddr(t, s, "K18");
    emit(ADRP(s, here(), target));
    if (ins.mnemonic === "adr") emit(ADDI(s, s, target & 0xfff));
    emit(STRX(s, t));
    emit(LDP(s, t, 31, -16));
    emit(B(here(), ins.address + 4));
    return start;
  }
  function siteBr(ins, base, count) {
    check(
      Number.isSafeInteger(base) && [256, 65536].includes(count),
      "Invalid jump table geometry",
    );
    const start = here();
    emit(STUR(16, 31, -16));
    emit(STUR(17, 31, -24));
    emit(MRS_TPIDRRO(16));
    emitSlotAddr(16, 17, "K18");
    emit(LDRX(16, 16));
    emit(LDUR(17, 31, -24));
    const padbase = start + 96,
      delta = padbase - base,
      op = delta >= 0 ? ADDI : SUBI,
      dl = Math.abs(delta),
      ops = [];
    let hi = Math.floor(dl / 4096);
    const lo = dl & 0xfff;
    while (hi) {
      const h = Math.min(hi, 0xfff);
      ops.push(op(16, 16, h, 1));
      hi -= h;
    }
    if (lo) ops.push(op(16, 16, lo, 0));
    check(ops.length <= 12, "Jump table delta too large");
    for (const word of ops) emit(word);
    emit(BR(16));
    while (here() < padbase) emit(NOP);
    const tails = padbase + 4 * count;
    for (let i = 0; i < count; i++) emit(B(here(), tails + 8 * i));
    for (let i = 0; i < count; i++) {
      emit(0xf85f0000 | (31 << 5) | 16);
      emit(B(here(), base + 4 * i));
    }
    return start;
  }
  function siteSysregRead(ins, kind) {
    const n = ins.word & 31,
      tmp = n !== 16 ? 16 : 17,
      start = here();
    emit(STUR(tmp, 31, -16));
    emit(MRS_TPIDRRO(n));
    emitSlotAddr(n, tmp, kind);
    emit(LDRX(n, n));
    emit(LDUR(tmp, 31, -16));
    emit(B(here(), ins.address + 4));
    return start;
  }
  function siteTpidrWrite(ins) {
    const n = ins.word & 31,
      [t] = pick(new Set([n]), 1),
      start = here();
    emit(0xf81f0000 | (31 << 5) | t);
    const tmp = [16, 17].find((x) => x !== n && x !== t);
    emit(STUR(tmp, 31, -24));
    emit(MRS_TPIDRRO(t));
    emitSlotAddr(t, tmp, "KTLS");
    emit(STRX(n, t));
    emit(LDUR(tmp, 31, -24));
    emit(0xf85f0000 | (31 << 5) | t);
    emit(B(here(), ins.address + 4));
    return start;
  }
  function emitGateCall() {
    emit(MRS_TPIDRRO(30));
    emitSlotAddr(30, 29, "KGATE");
    emit(LDRX(30, 30));
    emit(LDRX(29, 31));
  }
  function siteGate(ins, kind, arg) {
    const start = here();
    emit(STP_PRE(29, 30, 31, -16));
    emitGateCall();
    emit(BLR(30));
    const ret = here();
    emit(LDP_POST(29, 30, 31, 16));
    emit(B(here(), ins.address + 4));
    gates.push([ret, kind, arg, ins.address]);
    return start;
  }
  function siteObs(off, oid, word) {
    const ins = decode(word, off),
      m = ins.mnemonic;
    check(
      !["adr", "adrp", "b", "bl", "br", "blr", "ret", "cbz", "cbnz", "tbz", "tbnz"].includes(m) &&
        !m.startsWith("b.") &&
        !(m.startsWith("ldr") && !ins.op_str.includes("[")) &&
        !has18(ins),
      "Nonrelocatable observation instruction",
    );
    const start = here();
    emit(STP_PRE(29, 30, 31, -16));
    emitGateCall();
    emit(BLR(30));
    const ret = here();
    emit(LDP_POST(29, 30, 31, 16));
    emit(word);
    emit(B(here(), off + 4));
    gates.push([ret, "obs", oid, off]);
    return start;
  }
  function siteWindow(segment) {
    const used = new Set();
    for (const ins of segment) for (const r of regsIn(ins)) used.add(r);
    const [s, t] = pick(used),
      lo = segment[0].address,
      hi = segment.at(-1).address + 4,
      start = here();
    emit(STP(s, t, 31, -16));
    emit(MRS_TPIDRRO(t));
    emitSlotAddr(t, s, "K18");
    emit(LDRX(s, t));
    const copy0 = here(),
      exits = copy0 + 4 * segment.length,
      targets = [],
      words = [];
    for (let k = 0; k < segment.length; k++) {
      const ins = segment[k],
        m = ins.mnemonic;
      let word = has18(ins) ? substitute(ins, s, decodeMaybe) : ins.word;
      if (condition(m)) {
        const target = ins.operands.at(-1).imm;
        let newTarget;
        if (target >= lo && target < hi) newTarget = copy0 + target - lo;
        else {
          targets.push(target);
          newTarget = exits + 16 * targets.length;
        }
        const pc = copy0 + 4 * k,
          off = (newTarget - pc) / 4;
        word =
          (["tbz", "tbnz"].includes(m)
            ? (word & 0xfff8001f) | ((off & 0x3fff) << 5)
            : (word & 0xff00001f) | ((off & 0x7ffff) << 5)) >>> 0;
        const dd = decode(word, pc);
        check(
          dd.operands.at(-1).imm === newTarget && dd.mnemonic === m,
          "Exclusive window branch mismatch",
        );
      } else
        check(
          !["adr", "adrp", "b", "bl", "br", "blr", "ret"].includes(m) &&
            !(m === "ldr" && !ins.op_str.includes("[")),
          "Nonrelocatable exclusive window instruction",
        );
      words.push(word);
    }
    for (const word of words) emit(word);
    // 顺序执行的出口必须是第零个桩；外部出口排在其后。
    // Sequential fall-out must be stub zero; external exits follow it.
    for (const target of [hi, ...targets]) {
      emit(STRX(s, t));
      emit(LDP(s, t, 31, -16));
      emit(B(here(), target));
      emit(NOP);
    }
    return [start, lo, hi];
  }
  return {
    here,
    emit,
    emitKeyAddr,
    emitSlotAddr,
    emitGateCall,
    sitePlain,
    siteCond,
    sitePcrel,
    siteBr,
    siteSysregRead,
    siteTpidrWrite,
    siteGate,
    siteObs,
    siteWindow,
    fix,
    gates,
    patchBytes: () => buffer.slice(0, length),
    reserveBytes,
  };
}
