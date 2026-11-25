// 独立扫描原始 DEFLATE 边界，不生成解码输出。
// 依据 RFC 1951 的 DEFLATE 帧结构。
// Independently scans raw DEFLATE boundaries without materializing decoded output.
// Based on RFC 1951 DEFLATE framing.

// 原生运行时对于原始 DEFLATE 是否静默接受尾随字节的行为各异。
// 独立扫描 RFC 1951 帧结构，不生成解码输出。
// 事务在 1 KiB 输入片段之间仅保留不完整的头或符号。
// Native runtimes differ on whether raw DEFLATE silently accepts trailing bytes.
// Independently scan the RFC 1951 framing, without materializing decoded output.
// Transactions retain only an incomplete header/symbol across 1 KiB input slices.
export function createDeflateBoundary(expectedSize) {
  if (!Number.isSafeInteger(expectedSize) || expectedSize < 0 || expectedSize > 64 * 1024 * 1024)
    throw new Error("Invalid ZIP DEFLATE expected size");
  const need = Symbol("more input");
  let buffer = new Uint8Array(0),
    bit = 0,
    state = "header",
    final = false;
  let produced = 0,
    remaining = 0,
    literal,
    distance;
  const invalid = () => {
    throw new Error("Invalid ZIP DEFLATE structure");
  };
  const bits = (n) => {
    if (bit + n > buffer.length * 8) throw need;
    let result = 0;
    for (let i = 0; i < n; i++, bit++) result |= ((buffer[bit >>> 3] >>> (bit & 7)) & 1) << i;
    return result;
  };
  const tree = (lengths, kind) => {
    const counts = new Uint16Array(16),
      next = new Uint16Array(16),
      maps = Array.from({ length: 16 }, () => new Map());
    let max = 0,
      left = 1,
      code = 0;
    for (const n of lengths) {
      if (n > 15) invalid();
      if (n) {
        counts[n]++;
        max = Math.max(max, n);
      }
    }
    if (!max) {
      if (kind !== "distance") invalid();
      return { maps, max };
    }
    for (let n = 1; n <= 15; n++) {
      left = left * 2 - counts[n];
      if (left < 0) invalid();
    }
    if (left && (kind === "codes" || max !== 1)) invalid();
    for (let n = 1; n <= 15; n++) {
      code = (code + counts[n - 1]) << 1;
      next[n] = code;
    }
    lengths.forEach((n, symbol) => {
      if (!n) return;
      let value = next[n]++,
        reversed = 0;
      for (let i = 0; i < n; i++) {
        reversed = (reversed << 1) | (value & 1);
        value >>>= 1;
      }
      maps[n].set(reversed, symbol);
    });
    return { maps, max };
  };
  const symbol = (t) => {
    let code = 0;
    for (let n = 1; n <= t.max; n++) {
      code |= bits(1) << (n - 1);
      const result = t.maps[n].get(code);
      if (result !== undefined) return result;
    }
    invalid();
  };
  const fixedLiteral = tree(
    Array.from({ length: 288 }, (_, n) => (n <= 143 ? 8 : n <= 255 ? 9 : n <= 279 ? 7 : 8)),
    "literal",
  );
  const fixedDistance = tree(Array(32).fill(5), "distance");
  const order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
  const lengthBase = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
    163, 195, 227, 258,
  ];
  const lengthExtra = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
  ];
  const distanceBase = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
    3073, 4097, 6145, 8193, 12289, 16385, 24577,
  ];
  const distanceExtra = [
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    13,
  ];
  const add = (n) => {
    produced += n;
    if (produced > expectedSize) invalid();
  };
  return {
    push(bytes) {
      if (!(bytes instanceof Uint8Array) || bytes.length > 1024)
        throw new Error("Invalid ZIP DEFLATE input slice");
      const start = bit >>> 3;
      const joined = new Uint8Array(buffer.length - start + bytes.length);
      joined.set(buffer.subarray(start));
      joined.set(bytes, buffer.length - start);
      buffer = joined;
      bit &= 7;
      // 动态头最多几 KiB；符号最多六字节。
      // A dynamic header is at most a few KiB; symbols are at most six bytes.
      if (buffer.length > 8192) invalid();
      while (true) {
        const saved = bit;
        try {
          if (state === "done") {
            if (Math.ceil(bit / 8) !== buffer.length) invalid();
            return;
          }
          if (state === "header") {
            const last = bits(1),
              type = bits(2);
            let l, d;
            if (type === 3) invalid();
            if (type === 1) {
              l = fixedLiteral;
              d = fixedDistance;
            }
            if (type === 2) {
              const nl = bits(5) + 257,
                nd = bits(5) + 1,
                nc = bits(4) + 4;
              if (nl > 286) invalid();
              const codes = Array(19).fill(0);
              for (let i = 0; i < nc; i++) codes[order[i]] = bits(3);
              const codeTree = tree(codes, "codes"),
                lengths = [];
              while (lengths.length < nl + nd) {
                const value = symbol(codeTree);
                if (value < 16) lengths.push(value);
                else {
                  if (value === 16 && !lengths.length) invalid();
                  const repeat =
                    value === 16 ? bits(2) + 3 : value === 17 ? bits(3) + 3 : bits(7) + 11;
                  const repeated = value === 16 ? lengths.at(-1) : 0;
                  if (lengths.length + repeat > nl + nd) invalid();
                  for (let i = 0; i < repeat; i++) lengths.push(repeated);
                }
              }
              if (!lengths[256]) invalid();
              l = tree(lengths.slice(0, nl), "literal");
              d = tree(lengths.slice(nl), "distance");
            }
            if (type === 0) {
              bit = Math.ceil(bit / 8) * 8;
              const size = bits(16),
                complement = bits(16);
              if ((size ^ complement) !== 0xffff) invalid();
              remaining = size;
            }
            final = Boolean(last);
            literal = l;
            distance = d;
            state = type === 0 ? "stored" : "codes";
          } else if (state === "stored") {
            const size = Math.min(remaining, buffer.length - bit / 8);
            add(size);
            bit += size * 8;
            remaining -= size;
            if (remaining) return;
            state = final ? "done" : "header";
          } else {
            const value = symbol(literal);
            if (value < 256) add(1);
            else if (value === 256) state = final ? "done" : "header";
            else {
              if (value > 285) invalid();
              const n = value - 257,
                length = lengthBase[n] + bits(lengthExtra[n]);
              const ds = symbol(distance);
              if (ds > 29) invalid();
              const back = distanceBase[ds] + bits(distanceExtra[ds]);
              if (back > produced) invalid();
              add(length);
            }
          }
        } catch (error) {
          if (error !== need) throw error;
          bit = saved;
          return;
        }
      }
    },
    finish() {
      if (state !== "done" || produced !== expectedSize) invalid();
    },
  };
}
