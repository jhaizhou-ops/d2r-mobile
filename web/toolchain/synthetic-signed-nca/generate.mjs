// 生成仅供测试的合成签名 NCA 输入；依据 web/toolchain/synthetic-signed-nca 的模块与调用约定。
// Generate synthetic signed NCA inputs solely for tests using the module and calling contracts in web/toolchain/synthetic-signed-nca.
import process from "node:process";
import { Buffer } from "node:buffer";
/** 仅使用自行编写的简单内容；临时签名私钥始终留在内存中。 */
/**
 * 仅为自编玩具内容，临时私有签名密钥不离开内存。
 * Authored toy content only. The ephemeral private signing key never leaves memory. */
import { generateKeyPairSync, createHash, sign, constants, verify } from "node:crypto";
import { mkdirSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
const out = resolve(process.argv[2]);
mkdirSync(out, { recursive: true });
const sha = (x) => createHash("sha256").update(x).digest();
const align = (n, a = 512) => Math.ceil(n / a) * a;
const { privateKey, publicKey } = generateKeyPairSync("rsa", {
  modulusLength: 2048,
  publicExponent: 65537,
});
const modulus = Buffer.from(publicKey.export({ format: "jwk" }).n, "base64url");
if (modulus.length !== 256) throw Error("Unexpected RSA modulus");
writeFileSync(`${out}/test-root.bin`, modulus);
function pfs(files) {
  const names = Buffer.from(files.map(([name]) => `${name}\0`).join(""));
  const header = Buffer.alloc(16 + files.length * 24 + names.length);
  header.write("PFS0");
  header.writeUInt32LE(files.length, 4);
  header.writeUInt32LE(names.length, 8);
  let offset = 0,
    nameOffset = 0;
  files.forEach(([name, bytes], i) => {
    const p = 16 + i * 24;
    header.writeBigUInt64LE(BigInt(offset), p);
    header.writeBigUInt64LE(BigInt(bytes.length), p + 8);
    header.writeUInt32LE(nameOffset, p + 16);
    offset += bytes.length;
    nameOffset += Buffer.byteLength(name) + 1;
  });
  names.copy(header, 16 + files.length * 24);
  return Buffer.concat([header, ...files.map(([, bytes]) => bytes)]);
}
function pfsSection(files) {
  const data = pfs(files),
    hashes = [];
  for (let i = 0; i < data.length; i += 64) hashes.push(sha(data.subarray(i, i + 64)));
  const table = Buffer.concat(hashes),
    offset = align(table.length);
  const bytes = Buffer.alloc(align(offset + data.length));
  table.copy(bytes);
  data.copy(bytes, offset);
  const fs = Buffer.alloc(512);
  fs[2] = 1;
  fs[3] = 2;
  fs[4] = 1;
  sha(table).copy(fs, 8);
  fs.writeUInt32LE(64, 40);
  fs.writeUInt32LE(2, 44);
  fs.writeBigUInt64LE(BigInt(table.length), 56);
  fs.writeBigUInt64LE(BigInt(offset), 64);
  fs.writeBigUInt64LE(BigInt(data.length), 72);
  return { fs, bytes, payloadOffset: offset + data.length - files.at(-1)[1].length };
}
// 使用真实 RomFS 树布局，仅包含自行编写的问候语，不含游戏资源。
// One real RomFS tree layout containing only an authored greeting, not game assets.
const greeting = Buffer.from("Synthetic authenticated RomFS greeting.\n");
const romfs = Buffer.alloc(256 + greeting.length);
[80, 80, 4, 84, 24, 108, 4, 112, 44, 256].forEach((v, i) =>
  romfs.writeBigUInt64LE(BigInt(v), i * 8),
);
[88, 92, 100, 116, 136].forEach((p) => romfs.writeUInt32LE(0xffffffff, p));
romfs.writeBigUInt64LE(BigInt(greeting.length), 128);
romfs.writeUInt32LE(9, 140);
romfs.write("hello.txt", 144);
greeting.copy(romfs, 256);
function romfsSection() {
  // 每层仅有一个 512 字节块，使用独立的 Node SHA256 层级校验。
  // Single 512-byte block per level, independent Node SHA256 hierarchy.
  const levels = Array.from({ length: 6 }, () => Buffer.alloc(512));
  romfs.copy(levels[5]);
  for (let i = 4; i >= 0; i--) sha(levels[i + 1]).copy(levels[i]);
  const fs = Buffer.alloc(512);
  fs[3] = 3;
  fs[4] = 1;
  fs.write("IVFC", 8);
  fs.writeUInt32LE(0x20000, 12);
  fs.writeUInt32LE(32, 16);
  fs.writeUInt32LE(7, 20);
  levels.forEach((_, i) => {
    const p = 24 + i * 24;
    fs.writeBigUInt64LE(BigInt(i * 512), p);
    fs.writeBigUInt64LE(BigInt(i === 5 ? romfs.length : 32), p + 8);
    fs.writeUInt32LE(9, p + 16);
  });
  sha(levels[0]).copy(fs, 8 + 0xc0);
  return { fs, bytes: Buffer.concat(levels), payloadOffset: 5 * 512 + 256 };
}
function nca(type, sections) {
  const header = Buffer.alloc(0xc00);
  header.write("NCA3", 0x200);
  header[0x205] = type;
  header.writeBigUInt64LE(0x0100000000001000n, 0x210);
  let offset = header.length;
  sections.forEach(({ fs, bytes }, i) => {
    header.writeUInt32LE(offset / 512, 0x240 + i * 16);
    offset += bytes.length;
    header.writeUInt32LE(offset / 512, 0x244 + i * 16);
    sha(fs).copy(header, 0x280 + i * 32);
    fs.copy(header, 0x400 + i * 512);
  });
  header.writeBigUInt64LE(BigInt(offset), 0x208);
  const signature = sign("sha256", header.subarray(0x200, 0x400), {
    key: privateKey,
    padding: constants.RSA_PKCS1_PSS_PADDING,
    saltLength: 32,
  });
  if (
    !verify(
      "sha256",
      header.subarray(0x200, 0x400),
      { key: publicKey, padding: constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 },
      signature,
    )
  )
    throw Error("Signing failed");
  signature.copy(header);
  return Buffer.concat([header, ...sections.map((s) => s.bytes)]);
}
const main = Buffer.from("NSO0 Authored toy executable marker; not executable game code.");
const npdm = Buffer.from("META Authored toy metadata marker; not a complete NPDM.");
const exefs = pfsSection([
    ["main", main],
    ["main.npdm", npdm],
  ]),
  rom = romfsSection();
const program = nca(0, [exefs, rom]);
const cnmt = Buffer.alloc(136);
cnmt.writeBigUInt64LE(0x0100000000001000n);
cnmt[12] = 128;
cnmt.writeUInt16LE(16, 14);
cnmt.writeUInt16LE(1, 16);
cnmt.writeBigUInt64LE(0x0100000000001800n, 32);
sha(program).copy(cnmt, 48);
sha(program).copy(cnmt, 80, 0, 16);
cnmt.writeUInt32LE(program.length, 96);
cnmt[102] = 1;
sha(cnmt.subarray(0, 104)).copy(cnmt, 104);
const metaSection = pfsSection([["Application_0100000000001000.cnmt", cnmt]]),
  meta = nca(1, [metaSection]);
const programName = `${sha(program).subarray(0, 16).toString("hex")}.nca`;
const metaName = `${sha(meta).subarray(0, 16).toString("hex")}.cnmt.nca`;
const files = {
  "program.nca": program,
  "meta.nca": meta,
  "synthetic.nsp": pfs([
    [metaName, meta],
    [programName, program],
  ]),
  "expected.cnmt": cnmt,
  "expected.main": main,
  "expected.npdm": npdm,
  "expected.romfs": romfs,
};
for (const [name, bytes] of Object.entries(files)) writeFileSync(`${out}/${name}`, bytes);
writeFileSync(
  `${out}/manifest.json`,
  JSON.stringify(
    {
      programName,
      metaName,
      programHash: sha(program).toString("hex"),
      metaHash: sha(meta).toString("hex"),
      mutations: {
        signature: 0,
        signedHeader: 0x210,
        sectionHeader: 0x408,
        hashTable: 0xc00,
        pfsPayload: 0xc00 + exefs.payloadOffset,
        romfsPayload: 0xc00 + exefs.bytes.length + rom.payloadOffset,
      },
    },
    null,
    2,
  ),
);
console.log("Generated synthetic signed fixtures; no real game data or keys.");
