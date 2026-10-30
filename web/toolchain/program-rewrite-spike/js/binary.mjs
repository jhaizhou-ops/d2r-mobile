// 提供程序二进制的有界读取、摘要及原生绑定；依据 ../../../program-release-catalog.mjs 的模块与调用约定。
// Provide bounded program-binary access, hashing and native bindings using the module and calling contracts in ../../../program-release-catalog.mjs.
import { PROGRAM_REWRITE_LIMITS } from "../../../program-release-catalog.mjs";
export function check(value, message = "Program rewrite invariant failed") {
  if (!value) throw new Error(message);
}
export const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
export const hexAddress = (value) => `0x${value.toString(16)}`;
export function unhex(text) {
  check(typeof text === "string" && /^(?:[0-9a-f]{2})*$/i.test(text), "Invalid byte hex");
  return Uint8Array.from(text.match(/../g) ?? [], (b) => parseInt(b, 16));
}
export function wordBytes(word) {
  const bytes = new Uint8Array(4);
  new DataView(bytes.buffer).setUint32(0, word >>> 0, true);
  return bytes;
}
export const wordHex = (word) => hex(wordBytes(word));
export const hash = async (bytes) =>
  hex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));
export function view(bytes) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}
export function range(bytes, offset, size) {
  check(
    Number.isSafeInteger(offset) &&
      Number.isSafeInteger(size) &&
      offset >= 0 &&
      size >= 0 &&
      offset <= bytes.length - size,
    "Program byte range exceeded",
  );
}
export function u32(bytes, offset) {
  range(bytes, offset, 4);
  return view(bytes).getUint32(offset, true);
}
export function put32(bytes, offset, word) {
  range(bytes, offset, 4);
  view(bytes).setUint32(offset, word >>> 0, true);
}
export function assertBytes(image, offset, expected) {
  range(image, offset, expected.length);
  check(
    expected.every((b, i) => b === image[offset + i]),
    `Original bytes differ at ${hexAddress(offset)}`,
  );
}
export function loadNso(bytes, decompress) {
  check(
    bytes instanceof Uint8Array &&
      bytes.length >= 256 &&
      bytes.length <= PROGRAM_REWRITE_LIMITS.moduleInputBytes,
    "Invalid bounded NSO",
  );
  check(u32(bytes, 0) === 0x304f534e, "Invalid NSO header");
  const flags = u32(bytes, 12),
    bss = u32(bytes, 0x3c);
  const segments = [0, 1, 2].map((i) => ({
    file: u32(bytes, 0x10 + i * 16),
    memory: u32(bytes, 0x14 + i * 16),
    decoded: u32(bytes, 0x18 + i * 16),
    stored: u32(bytes, 0x60 + i * 4),
  }));
  const size = segments[2].memory + segments[2].decoded + bss;
  check(size <= PROGRAM_REWRITE_LIMITS.imageBytes, "NSO image budget exceeded");
  const image = new Uint8Array(size + 0x4000);
  for (let i = 0; i < 3; i++) {
    const s = segments[i];
    range(bytes, s.file, s.stored);
    check(
      s.decoded <= PROGRAM_REWRITE_LIMITS.segmentDecodedBytes && s.memory + s.decoded <= size,
      "NSO segment budget exceeded",
    );
    const input = bytes.subarray(s.file, s.file + s.stored);
    const raw = (flags >>> i) & 1 ? decompress(input, s.decoded) : input;
    check(raw instanceof Uint8Array && raw.length === s.decoded, "NSO segment size mismatch");
    image.set(raw, s.memory);
  }
  return {
    image,
    buildId: hex(bytes.subarray(0x40, 0x50)),
    textEnd: segments[0].decoded,
    roOff: segments[1].memory,
    dataOff: segments[2].memory,
    keyPageOff: Math.ceil(size / 0x4000) * 0x4000,
    patchOff: Math.ceil(image.length / 0x10000) * 0x10000,
  };
}
export function createNativeBindings(native) {
  function decodeMaybe(word, address) {
    check(
      Number.isSafeInteger(address) && address >= 0 && address <= 0xffffffff,
      "Instruction address out of range",
    );
    const data = JSON.parse(
      native.ccall("decode_word", "string", ["number", "number"], [word >>> 0, address]),
    );
    if (data === null) return null;
    check(!data.error && data.size === 4 && data.address === address, "Capstone decode failed");
    for (const op of data.operands) {
      const n = BigInt(op.imm);
      op.imm =
        n <= BigInt(Number.MAX_SAFE_INTEGER) && n >= BigInt(Number.MIN_SAFE_INTEGER)
          ? Number(n)
          : n;
    }
    return { ...data, word: word >>> 0 };
  }
  function decode(word, address) {
    const ins = decodeMaybe(word, address);
    check(ins !== null, "Instruction decode failed");
    return ins;
  }
  function decompress(input, size) {
    check(
      input instanceof Uint8Array &&
        Number.isSafeInteger(size) &&
        size >= 0 &&
        size <= PROGRAM_REWRITE_LIMITS.segmentDecodedBytes,
      "Invalid LZ4 bounds",
    );
    const src = native._malloc(Math.max(1, input.length)),
      dst = native._malloc(Math.max(1, size));
    try {
      check(src && dst, "WASM allocation failed");
      native.HEAPU8.set(input, src);
      check(
        native._LZ4_decompress_safe(src, dst, input.length, size) === size,
        "LZ4 output size mismatch",
      );
      return native.HEAPU8.slice(dst, dst + size);
    } finally {
      native._free(src);
      native._free(dst);
    }
  }
  function instructions(image, start, end) {
    range(image, start, end - start);
    const out = [];
    for (let pc = start; pc + 4 <= end; pc += 4) {
      const ins = decodeMaybe(u32(image, pc), pc);
      if (!ins) break;
      out.push(ins);
    }
    return out;
  }
  return { decode, decodeMaybe, decompress, instructions };
}
