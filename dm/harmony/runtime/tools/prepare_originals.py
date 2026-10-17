"""Verify actual NSOs and matching r32 metadata; stage decoded images OUTSIDE Git.
No execution, no Apple patching, no dumping program data or embedded credentials.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

PINNED = {
    "main": "55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e",
    "sdk": "1c9f198a91c0bcfee5cf8f5b53fb941895e91334dfb766ce79fc68576797f017",
}

def sha(data): return hashlib.sha256(data).hexdigest()

def lz4_block(data, expected):
    """Bounded raw LZ4 block decode; independent NSO SHA checks every result."""
    out = bytearray(); pos = 0
    def length(n):
        nonlocal pos
        if n == 15:
            while True:
                if pos >= len(data): raise ValueError("truncated LZ4 length")
                b = data[pos]; pos += 1; n += b
                if b != 255: break
        return n
    while pos < len(data):
        token = data[pos]; pos += 1
        count = length(token >> 4)
        if pos + count > len(data) or len(out) + count > expected: raise ValueError("LZ4 literals out of bounds")
        out.extend(data[pos:pos+count]); pos += count
        if pos == len(data): break
        if pos + 2 > len(data): raise ValueError("truncated LZ4 offset")
        distance = int.from_bytes(data[pos:pos+2], "little"); pos += 2
        if not distance or distance > len(out): raise ValueError("LZ4 invalid match offset")
        count = length(token & 15) + 4
        if len(out) + count > expected: raise ValueError("LZ4 match out of bounds")
        pattern = bytes(out[-distance:])
        out.extend((pattern * ((count + distance - 1) // distance))[:count])
    if len(out) != expected: raise ValueError("LZ4 decompressed size mismatch")
    return out

def decode(path):
    raw = path.read_bytes()
    if len(raw) < 256 or raw[:4] != b"NSO0": raise ValueError("invalid NSO header")
    flags = struct.unpack_from("<I", raw, 12)[0]
    if flags != 63: raise ValueError("expected compressed and hashed known 94012 NSO")
    segs = []
    for i in range(3):
        file_off, mem_off, size = struct.unpack_from("<III", raw, 0x10+16*i)
        stored = struct.unpack_from("<I", raw, 0x60+4*i)[0]
        if size > 128*1024*1024 or file_off+stored > len(raw): raise ValueError("segment out of bounds")
        decoded = lz4_block(raw[file_off:file_off+stored], size)
        digest = sha(decoded)
        if digest != raw[0xa0+32*i:0xc0+32*i].hex(): raise ValueError("NSO segment SHA mismatch")
        segs.append({"kind": ("text", "ro", "data")[i], "file_offset": file_off, "offset": mem_off,
                     "bytes": size, "compressed_bytes": stored, "sha256": digest, "decoded": decoded})
    bss = struct.unpack_from("<I", raw, 0x3c)[0]
    size = segs[2]["offset"]+segs[2]["bytes"]+bss
    if size > 256*1024*1024: raise ValueError("image too large")
    image = bytearray(size)
    for s in segs: image[s["offset"]:s["offset"]+s["bytes"]] = s.pop("decoded")
    return image, {"file_bytes":len(raw),"sha256":sha(raw),"build_id":raw[0x40:0x60].hex(),
                   "flags":flags,"bss_bytes":bss,"image_bytes":size,"image_sha256":sha(image),"segments":segs}
