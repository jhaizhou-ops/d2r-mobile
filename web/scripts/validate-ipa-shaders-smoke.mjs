#!/usr/bin/env node
// Authored synthetic envelope only: not executable AIR and not evidence of successful compilation.
// 仅使用自编合成容器；不是可执行 AIR，也不证明着色器编译成功。
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { writeZip } from "../zip-writer.mjs";
import { writeZip64 } from "../zip64-writer.mjs";
import { validateIpaShaderFile } from "./validate-ipa-shaders.mjs";

const encoder = new TextEncoder();
function integer(value, width) {
  const bytes = new Uint8Array(width),
    view = new DataView(bytes.buffer);
  if (width === 8) view.setBigUint64(0, BigInt(value), true);
  else if (width === 4) view.setUint32(0, value, true);
  else view.setUint16(0, value, true);
  return bytes;
}
function concat(...parts) {
  const bytes = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    bytes.set(part, offset);
    offset += part.length;
  }
  return bytes;
}
function fixture() {
  const bitcode = new Uint8Array([0x42, 0x43, 0xc0, 0xde, 0, 0, 0, 0]);
  const tag = (name, bytes) => concat(encoder.encode(name), integer(bytes.length, 2), bytes);
  const body = concat(
    tag("NAME", encoder.encode("main0\0")),
    tag("TYPE", new Uint8Array([1])),
    tag("HASH", new Uint8Array(createHash("sha256").update(bitcode).digest())),
    tag("MDSZ", integer(bitcode.length, 8)),
    tag("OFFT", new Uint8Array(24)),
    tag("VERS", concat(integer(2, 2), integer(6, 2), integer(3, 2), integer(2, 2))),
    encoder.encode("ENDT"),
  );
  const functions = concat(integer(1, 4), integer(body.length + 4, 4), body);
  const metadata = concat(integer(4, 4), encoder.encode("ENDT"));
  const header = new Uint8Array(88),
    view = new DataView(header.buffer);
  header.set(encoder.encode("MTLB"));
  view.setUint16(4, 1, true);
  view.setUint16(6, 2, true);
  view.setUint16(8, 7, true);
  header[11] = 0x82;
  view.setUint16(12, 18, true);
  let offset = 88;
  for (const [index, part] of [functions, metadata, metadata, bitcode].entries()) {
    view.setBigUint64(24 + index * 16, BigInt(offset), true);
    view.setBigUint64(32 + index * 16, BigInt(part.length - (index === 0 ? 4 : 0)), true);
    offset += part.length;
  }
  view.setBigUint64(16, BigInt(offset), true);
  return concat(header, functions, metadata, metadata, bitcode);
}
const directory = await mkdtemp(join(tmpdir(), "ipa-shader-smoke-"));
try {
  const binary = fixture(),
    malformed = binary.slice();
  malformed[malformed.length - 1] ^= 1;
  for (const [label, bytes, zip64] of [
    ["compiled32", binary, false],
    ["compiled64", binary, true],
    [
      "source",
      encoder.encode("#include <metal_stdlib>\nfragment float4 main0() { return 1; }\n"),
      false,
    ],
    ["malformed", malformed, false],
  ]) {
    const pieces = [],
      entries = [
        ["Payload/Fixture.app/Info.plist", encoder.encode("authored smoke fixture")],
        ["Payload/Fixture.app/msl/b-fixture/0000000000000001_b.metallib", bytes],
      ].map(([path, data]) => ({
        path,
        size: data.length,
        async *chunks() {
          yield data;
        },
      }));
    const sink = {
      async write(data) {
        pieces.push(data.slice());
      },
      async close() {},
      async abort() {
        pieces.length = 0;
      },
    };
    if (zip64) await writeZip64(entries, sink, { maxTotalBytes: 1024 * 1024 });
    else await writeZip(entries, sink);
    const filename = join(directory, `${label}.ipa`);
    await writeFile(filename, concat(...pieces));
    const report = await validateIpaShaderFile(filename, { expectedCount: 1 });
    assert.equal(report.ok, label.startsWith("compiled"));
    assert.equal(report.gameLibraries.total, 1);
    if (label.startsWith("compiled")) {
      assert.equal(report.gameLibraries.compiled, 1);
      assert.equal((await validateIpaShaderFile(filename, { expectedCount: 2 })).ok, false);
    } else assert.equal(report.gameLibraries[label === "source" ? "sourceText" : "invalid"], 1);
  }
  console.log(
    "IPA shader utility smoke passed: ZIP32/ZIP64, source rejection, malformed digest, optional count. Synthetic envelopes only.",
  );
} finally {
  await rm(directory, { recursive: true, force: true });
}
