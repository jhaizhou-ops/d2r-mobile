#!/usr/bin/env python3
"""Switch D2R RomFS 存储最小提取器（只读）。

存储布局（实测）：
  build_manifest.psv 每行 = FileId|KeyLayout|Chunk|Archive|CKey|CSize|EKey|ESize|Encoding|Path|Flags
  数据文件 = <Chunk:02d>-<Archive:08d>.data
  偏移   = EKey 末 5 字节（大端 40 位；key-layout-0 = 8 8 40 1 → EKey[9]=Chunk, EKey[10]=Archive, EKey[11:16]=偏移）
  编码   n          → 原样 CSize 字节
         z          → 一整段 zlib（无 BLTE 头）
         b:..*=..   → 标准 BLTE 容器（'BLTE' + 头长 + 0x0F + 块数 + {esize,csize,md5}×N，每块首字节 N/Z）
  校验   md5(解出内容) == CKey
用法：
  dmx.py get <Path> <out>         取单个文件（Path 用清单 Path 列，或去掉 'data:' 前缀，大小写不敏感）
  dmx.py list [glob]              列 Path\tCSize
  Minimal extractor for Switch D2R RomFS storage (read-only).
  Storage layout (measured):
    build_manifest.psv each line = FileId|KeyLayout|Chunk|Archive|CKey|CSize|EKey|ESize|Encoding|Path|Flags
    data file = <Chunk:02d>-<Archive:08d>.data
    offset   = last 5 bytes of EKey (big-endian 40 bits; key-layout-0 = 8 8 40 1 → EKey[9]=Chunk, EKey[10]=Archive, EKey[11:16]=offset)
    encoding n          → CSize bytes as is
           z          → one whole zlib segment (no BLTE header)
           b:..*=..   → standard BLTE container ('BLTE' + header length + 0x0F + block count + {esize,csize,md5}×N, first byte of each block N/Z)
    check    md5(decoded content) == CKey
  Usage:
    dmx.py get <Path> <out>         fetch a single file (Path uses the manifest's Path column, or with the 'data:' prefix removed; case-insensitive)
    dmx.py list [glob]              list Path\\tCSize
"""
import fnmatch, hashlib, os, struct, sys, zlib

ROOT = os.environ.get("DMX_ROOT", "~/Diablo II Resurrected - 提取内容/Update/RomFS/data")
_IDX = None
_FH = {}


def index():
    global _IDX
    if _IDX is None:
        _IDX = {}
        # The 3.3.94012 manifest has binary data after its textual index. Replace the
        # undecodable tail so the valid pipe-delimited rows before it remain usable.
        with open(os.path.join(ROOT, "build_manifest.psv"), encoding="utf-8", errors="replace") as f:
            next(f)
            for line in f:
                if line.startswith("#"):
                    continue
                c = line.rstrip("\n").split("|")
                if len(c) < 11:
                    continue
                _IDX[c[9].lower()] = c
    return _IDX


def lookup(path):
    idx = index()
    k = path.lower().replace("\\", "/")
    for cand in (k, "data:" + k, "data:data/" + k):
        if cand in idx:
            return idx[cand]
    raise KeyError(path)


def _fh(chunk, arch):
    key = (chunk, arch)
    if key not in _FH:
        _FH[key] = open(os.path.join(ROOT, f"{chunk:02d}-{arch:08d}.data"), "rb")
    return _FH[key]


def _block(b):
    m = b[:1]
    if m == b"N":
        return b[1:]
    if m == b"Z":
        return zlib.decompress(b[1:])
    raise ValueError(f"未知 BLTE 块模式 {m!r}")


def read_row(c):
    chunk, arch, csize, esize, enc = int(c[2]), int(c[3]), int(c[5]), int(c[7]), c[8]
    ekey = bytes.fromhex(c[6])
    off = int.from_bytes(ekey[11:16], "big")
    assert ekey[9] == chunk and ekey[10] == arch, (c[9], ekey.hex())
    f = _fh(chunk, arch)
    f.seek(off)
    raw = f.read(esize)
    if raw[:4] == b"BLTE":
        hlen = struct.unpack(">I", raw[4:8])[0]
        n = int.from_bytes(raw[9:12], "big")
        pos, out = hlen, []
        for i in range(n):
            es, cs = struct.unpack(">II", raw[12 + 24 * i:20 + 24 * i])
            out.append(_block(raw[pos:pos + es]))
            pos += es
        data = b"".join(out)
    elif enc == "n":
        data = raw
    elif enc == "z":
        data = zlib.decompress(raw)
    else:
        raise ValueError(f"未知编码 {enc}")
    if len(data) != csize or hashlib.md5(data).hexdigest() != c[4]:
        raise ValueError(f"校验失败 {c[9]} len={len(data)} csize={csize}")
    return data


def get(path):
    return read_row(lookup(path))


if __name__ == "__main__":
    if sys.argv[1] == "get":
        d = get(sys.argv[2])
        open(sys.argv[3], "wb").write(d)
        print(len(d), "bytes, md5 ok")
    elif sys.argv[1] == "list":
        pat = sys.argv[2].lower() if len(sys.argv) > 2 else "*"
        for k, c in index().items():
            if fnmatch.fnmatch(k, pat):
                print(f"{c[9]}\t{c[5]}")
