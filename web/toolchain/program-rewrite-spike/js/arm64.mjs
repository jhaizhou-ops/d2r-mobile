// 编码 ARM64 指令并处理寄存器替换；依据 ./binary.mjs 的模块与调用约定。
// Encode ARM64 instructions and handle register substitution using the module and calling contracts in ./binary.mjs.
import { check } from "./binary.mjs";
export function B(from, to) {
  const off = to - from;
  check(
    Number.isSafeInteger(off) && off >= -(2 ** 27) && off < 2 ** 27 && off % 4 === 0,
    "ARM64 branch out of range",
  );
  return (0x14000000 | ((off / 4) & 0x3ffffff)) >>> 0;
}
function pairOffset(off) {
  check(
    Number.isSafeInteger(off) && off >= -512 && off <= 504 && off % 8 === 0,
    "ARM64 pair offset out of range",
  );
  return ((off / 8) & 0x7f) << 15;
}
export const STP = (t, t2, n, off) =>
  (0xa9000000 | pairOffset(off) | (t2 << 10) | (n << 5) | t) >>> 0;
export const LDP = (t, t2, n, off) =>
  (0xa9400000 | pairOffset(off) | (t2 << 10) | (n << 5) | t) >>> 0;
export const STP_PRE = (t, t2, n, off) =>
  (0xa9800000 | pairOffset(off) | (t2 << 10) | (n << 5) | t) >>> 0;
export const LDP_POST = (t, t2, n, off) =>
  (0xa8c00000 | pairOffset(off) | (t2 << 10) | (n << 5) | t) >>> 0;
export const STUR = (t, n, off) => (0xf8000000 | ((off & 0x1ff) << 12) | (n << 5) | t) >>> 0;
export const LDUR = (t, n, off) => (0xf8400000 | ((off & 0x1ff) << 12) | (n << 5) | t) >>> 0;
export const LDRX = (t, n, imm12 = 0) => (0xf9400000 | (imm12 << 10) | (n << 5) | t) >>> 0;
export const STRX = (t, n, imm12 = 0) => (0xf9000000 | (imm12 << 10) | (n << 5) | t) >>> 0;
export const MRS_TPIDRRO = (t) => (0xd53bd060 | t) >>> 0;
export function ADRP(dd, pc, target) {
  const imm = Math.floor(target / 4096) - Math.floor(pc / 4096);
  check(Number.isSafeInteger(imm) && imm >= -(2 ** 20) && imm < 2 ** 20, "ADRP out of range");
  return (0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7ffff) << 5) | dd) >>> 0;
}
export const ADDI = (dd, n, imm12, sh = 0) =>
  (0x91000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd) >>> 0;
export const ADDR = (dd, n, m) => (0x8b000000 | (m << 16) | (n << 5) | dd) >>> 0;
export const SUBI = (dd, n, imm12, sh = 0) =>
  (0xd1000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd) >>> 0;
export const BR = (n) => (0xd61f0000 | (n << 5)) >>> 0;
export const BLR = (n) => (0xd63f0000 | (n << 5)) >>> 0;
export const NOP = 0xd503201f;
export const num = (name) => (/^[xw]\d+$/.test(name ?? "") ? Number(name.slice(1)) : null);
export const has18 = (ins) => /\b[xw]18\b/.test(ins.op_str);
export function regsIn(ins) {
  const out = new Set([...ins.op_str.matchAll(/\b[xw](\d+)\b/g)].map((m) => Number(m[1])));
  if (!ins.regs_error)
    for (const r of [...ins.reads, ...ins.writes]) {
      const n = num(r);
      if (n !== null) out.add(n);
    }
  return out;
}
export function pick(avoid, count = 2) {
  const out = [];
  for (let r = 0; r < 16 && out.length < count; r++) if (!avoid.has(r)) out.push(r);
  check(out.length === count, "No scratch registers");
  return out;
}
function* combinations(items, count, offset = 0, prefix = []) {
  if (count === 0) {
    yield prefix;
    return;
  }
  for (let i = offset; i <= items.length - count; i++)
    yield* combinations(items, count - 1, i + 1, [...prefix, items[i]]);
}
export function substitute(ins, scratch, decode) {
  const want = ins.op_str.replace(/\b([xw])18\b/g, (_, kind) => `${kind}${scratch}`),
    positions = [0, 5, 10, 16].filter((p) => ((ins.word >>> p) & 31) === 18);
  for (let count = positions.length; count > 0; count--)
    for (const fields of combinations(positions, count)) {
      let word = ins.word;
      for (const p of fields) word = ((word & ~(31 << p)) | (scratch << p)) >>> 0;
      const result = decode(word, ins.address);
      if (result && result.mnemonic === ins.mnemonic && result.op_str === want) return word;
    }
  throw new Error(`Register substitution failed at ${ins.address.toString(16)}`);
}
export function spDelta(ins) {
  const m =
    /\[sp, #(-?(?:0x)?[0-9a-f]+)\]!/.exec(ins.op_str) ??
    /\[sp\], #(-?(?:0x)?[0-9a-f]+)$/.exec(ins.op_str);
  if (m) {
    const s = m[1],
      neg = s.startsWith("-");
    return (neg ? -1 : 1) * Number(neg ? s.slice(1) : s);
  }
  check(!/^(sp|wsp),/.test(ins.op_str), "Unsupported SP write");
  return 0;
}
export function CBZ(at, target, r, nonzero = false) {
  const delta = target - at;
  check(delta % 4 === 0 && delta >= -(2 ** 20) && delta < 2 ** 20, "CBZ out of range");
  return ((nonzero ? 0x35000000 : 0x34000000) | (((delta / 4) & 0x7ffff) << 5) | r) >>> 0;
}
