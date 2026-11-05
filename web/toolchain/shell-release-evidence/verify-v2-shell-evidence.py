#!/usr/bin/env python3
"""对暂存 v2 证据验证本地公开 ZIP，不执行其内容。检查字节身份、Mach-O 符号/段/函数边界、指令窗口及内存内容模式哈希；这不是控制流或设备证明。
Verify a local public ZIP against staged v2 evidence without executing its contents.

Checks byte identities, Mach-O symbol/section/function bounds, instruction windows,
and the in-memory content-mode hash. This is not a control-flow or device proof.
"""

import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile

HERE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def span(data, start, size):
    require(0 <= start <= len(data) and 0 <= size <= len(data) - start, "out-of-range data")
    return data[start : start + size]


def macho(data):
    header = struct.unpack("<8I", span(data, 0, 32))
    require(
        header[0] == 0xFEEDFACF and header[1] == 0x100000C and header[3] == 2,
        "not thin arm64 executable",
    )
    require(header[4] <= 128 and header[5] <= 65536, "load-command limits")
    end = 32 + header[5]
    span(data, 32, header[5])
    pos, sections, symbols, starts = 32, [], [], []
    symtab = function_data = text_address = None
    for _ in range(header[4]):
        cmd, size = struct.unpack("<II", span(data, pos, 8))
        require(size >= 8 and pos + size <= end, "invalid load command")
        if cmd == 0x19:
            values = struct.unpack("<II16sQQQQIIII", span(data, pos, 72))
            _, _, name, address, _, _, _, _, _, count, _ = values
            require(size == 72 + 80 * count, "invalid segment command")
            if name.rstrip(b"\0") == b"__TEXT":
                text_address = address
            for i in range(count):
                name, segment, address, length, offset, _, _, _, flags, *_ = struct.unpack(
                    "<16s16sQQ8I", span(data, pos + 72 + 80 * i, 80)
                )
                sections.append(
                    (
                        name.rstrip(b"\0"),
                        segment.rstrip(b"\0"),
                        address,
                        length,
                        offset,
                        flags,
                    )
                )
        elif cmd == 2:
            require(symtab is None and size == 24, "duplicate/invalid symbol command")
            symtab = struct.unpack("<4I", span(data, pos + 8, 16))
        elif cmd == 0x26:
            require(
                function_data is None and size == 16,
                "duplicate/invalid function command",
            )
            function_data = struct.unpack("<II", span(data, pos + 8, 8))
        pos += size
    require(
        pos == end and symtab and function_data and text_address is not None,
        "missing Mach-O metadata",
    )
    offset, count, string_offset, string_size = symtab
    require(count <= 100000, "symbol limit")
    strings = span(data, string_offset, string_size)
    for i in range(count):
        index, typ, section, _, address = struct.unpack("<IBBHQ", span(data, offset + 16 * i, 16))
        require(index < len(strings), "invalid symbol string")
        name = strings[index:].split(b"\0")[0].decode("utf8")
        if typ & 0xE0 or typ & 0x0E != 0x0E:
            continue
        require(1 <= section <= len(sections), "invalid symbol section")
        sn, sg, sa, length, off, flags = sections[section - 1]
        symbols.append(
            {
                "name": name,
                "address": address,
                "offset": off + address - sa,
                "section": sn,
                "segment": sg,
                "sectionStart": sa,
                "sectionEnd": sa + length,
                "flags": flags,
            }
        )
    address, value, shift = text_address, 0, 0
    for byte in span(data, *function_data):
        value |= (byte & 127) << shift
        if byte & 128:
            shift += 7
            require(shift <= 63, "invalid ULEB")
            continue
        if value == 0:
            break
        address += value
        starts.append(address)
        value = shift = 0
    require(shift == 0 and len(starts) == len(set(starts)), "invalid function starts")
    return symbols, starts


def find_symbol(symbols, name):
    found = [s for s in symbols if s["name"] == name]
    require(len(found) == 1, "nonunique symbol: " + name)
    symbol = found[0]
    require(
        symbol["sectionStart"] <= symbol["address"] < symbol["sectionEnd"],
        "selected symbol outside section",
    )
    return symbol


def verify_archive(archive, evidence, capacities):
    require(
        Path(archive).stat().st_size == evidence["archiveBytes"] <= 8 * 1024**2,
        "archive byte cap/size",
    )
    raw = Path(archive).read_bytes()
    require(
        len(raw) == evidence["archiveBytes"] and digest(raw) == evidence["sha256"],
        "archive identity mismatch",
    )
    require(capacities["shellSha256"] == evidence["sha256"], "capacity archive mismatch")
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        entries = z.infolist()
        require(
            len(entries) == len(evidence["entries"])
            and len({e.filename for e in entries}) == len(entries),
            "inventory size/duplicate",
        )
        expected = {e["path"]: e for e in evidence["entries"]}
        require(set(expected) == {e.filename for e in entries}, "inventory paths")
        for entry in entries:
            want = expected[entry.filename]
            require(
                entry.file_size == want["size"] and entry.external_attr >> 16 == want["mode"],
                "member metadata",
            )
            require(
                entry.file_size <= 8 * 1024**2 and digest(z.read(entry)) == want["sha256"],
                "member identity",
            )
        binary = z.read(capacities["evidence"]["executablePath"])
    require(
        digest(binary) == capacities["evidence"]["executableSha256"],
        "capacity executable mismatch",
    )
    symbols, starts = macho(binary)
    for binding in capacities["evidence"]["bindings"]:
        symbol = find_symbol(symbols, binding["name"])
        require(
            symbol["section"] == b"__text" and symbol["segment"] == b"__TEXT",
            "function section",
        )
        require(
            symbol["address"] == int(binding["virtualAddress"], 16)
            and symbol["offset"] == binding["fileOffset"],
            "function location",
        )
        require(symbol["address"] in starts, "not a function start")
        end = next((a for a in starts if a > symbol["address"]), None)
        require(
            end is not None and end - symbol["address"] == binding["functionBytes"],
            "function bounds",
        )
        body = span(binary, symbol["offset"], binding["functionBytes"])
        require(digest(body) == binding["functionSha256"], "function hash")
        for group in binding["instructions"]:
            for ins in group["window"]:
                off = ins["fileOffset"]
                require(
                    symbol["offset"] <= off < symbol["offset"] + len(body),
                    "instruction outside function",
                )
                require(span(binary, off, 4).hex() == ins["bytes"], "instruction bytes")
    content = evidence["inspection"]["contentMode"]
    symbol = find_symbol(symbols, content["symbol"])
    require(
        symbol["section"] == b"__data"
        and symbol["segment"] == b"__DATA"
        and symbol["flags"] & 255 == 0,
        "content-mode section",
    )
    require(
        symbol["address"] == int(content["address"], 16)
        and symbol["offset"] == content["fileOffset"],
        "content-mode location",
    )
    require(symbol["address"] + 4 <= symbol["sectionEnd"], "content-mode section bounds")
    off = symbol["offset"]
    require(
        span(binary, off, 4).hex() == content["originalFourBytes"] == "ffffffff",
        "content-mode initializer",
    )
    require(content["replacementFourBytes"] == "00000000", "content-mode replacement")
    require(
        digest(binary[:off] + b"\0" * 4 + binary[off + 4 :]) == content["derivedSha256"],
        "content-mode derived hash",
    )
    return {
        "archiveSha256": evidence["sha256"],
        "executableSha256": digest(binary),
        "membersVerified": len(entries),
        "functionsVerified": len(capacities["evidence"]["bindings"]),
        "contentModeHashVerifiedInMemory": True,
        "executed": False,
        "productionActivated": False,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--profile", required=True, choices=["v2.0.0", "v2.0.0-ios18"])
    args = parser.parse_args()
    evidence = json.loads((HERE / (args.profile + ".json")).read_text())
    capacities = json.loads((HERE / (args.profile + "-derived-capacities.json")).read_text())
    print(json.dumps(verify_archive(args.archive, evidence, capacities), indent=2))
