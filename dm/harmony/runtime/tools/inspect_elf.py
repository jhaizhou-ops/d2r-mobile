"""Independent ELF64 inspection of the linked ARM64 prototype, standard library only."""
import hashlib
import json
import re
import struct
from pathlib import Path

def inspect(path, disassembly, *, raw_override=None):
    raw = Path(path).read_bytes() if raw_override is None else raw_override
    assert raw[:6] == b"\x7fELF\x02\x01", "not little-endian ELF64"
    h = struct.unpack_from("<HHIQQQIHHHHHH", raw, 16)
    typ, arch, _, _, phoff, shoff, _, _, phsize, phnum, shsize, shnum, stridx = h
    assert (typ, arch, phsize, shsize) == (3, 183, 56, 64), "not AArch64 ET_DYN"
    ph = [struct.unpack_from("<IIQQQQQQ", raw, phoff+i*phsize) for i in range(phnum)]
    sh = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff+i*shsize) for i in range(shnum)]
    name_raw = raw[sh[stridx][4]:sh[stridx][4]+sh[stridx][5]]
    def name(offset, names=name_raw):
        return names[offset:names.index(0, offset)].decode("utf-8")
    sections = {name(s[0]): s for s in sh}
    loads = [p for p in ph if p[0] == 1]
    assert all((p[1] & 3) != 3 for p in loads), "RWX PT_LOAD"
    assert all(not (p[1] & 1) for p in ph if p[0] == 0x6474e551), "executable stack"
    base = sections[".xblr_guest_text"][3]
    layout = []
    for key, offset, flags in ((".xblr_guest_text", 0, 5), (".xblr_guest_ro", 0x4000, 4), (".xblr_guest_data", 0x8000, 6)):
        s = sections[key]
        assert s[3] == base + offset and s[5] > 0, (key, "relative layout")
        if offset: assert s[5] == 32, (key, "size")
        owners = [p for p in loads if p[3] <= s[3] and s[3]+s[5] <= p[3]+p[6]]
        assert len(owners) == 1 and owners[0][1] == flags, (key, "PT_LOAD permissions")
        assert owners[0][2] % owners[0][7] == owners[0][3] % owners[0][7]
        assert owners[0][7] >= 0x4000, "16KiB compatible alignment required"
        layout.append({"section": key, "address": s[3], "offset_from_guest": offset,
                       "bytes": s[5], "load_flags": flags, "sha256": hashlib.sha256(raw[s[4]:s[4]+s[5]]).hexdigest()})
    # Adjacent LOADs with different rights must not share a 16KiB virtual page.
    for i, a in enumerate(loads):
        for b in loads[i+1:]:
            if a[1] != b[1] and a[6] and b[6]:
                assert not (a[3]//0x4000 <= (b[3]+b[6]-1)//0x4000 and b[3]//0x4000 <= (a[3]+a[6]-1)//0x4000), "shared page rights"
    symbols = {}
    exports = []
    for section_name in (".symtab", ".dynsym"):
        s = sections[section_name]; st = sh[s[6]]; strings = raw[st[4]:st[4]+st[5]]
        for off in range(s[4], s[4]+s[5], 24):
            nm, info, other, idx, value, size = struct.unpack_from("<IBBHQQ", raw, off)
            symbol = name(nm, strings)
            if section_name == ".symtab" and symbol:
                symbols[symbol] = {"value": value, "bytes": size, "section_index": idx}
            if section_name == ".dynsym" and idx and (info >> 4) in (1, 2) and (info & 15) == 2:
                exports.append(symbol)
    assert set(exports) == {"xblr_get_layout", "xblr_probe_input", "xblr_run"}, exports
    assert symbols["xblr_guest_entry"]["value"] == base
    assert base <= symbols["xblr_guest_callback"]["value"] < base+layout[0]["bytes"]
    relocations = []
    for sname, s in sections.items():
        if s[1] != 4: continue  # SHT_RELA
        for off in range(s[4], s[4]+s[5], s[9]):
            target, info, addend = struct.unpack_from("<QQq", raw, off)
            owners = [p for p in loads if p[3] <= target < p[3]+p[6]]
            assert owners and all(p[1] & 2 for p in owners), "dynamic relocation into non-writable segment"
            assert not base <= target < base+0x8020, "dynamic relocation changes embedded guest"
            relocations.append({"section": sname, "target": target, "type": info & 0xffffffff, "addend": addend})
    dynamic = sections.get(".dynamic")
    if dynamic:
        tags = [struct.unpack_from("<qQ", raw, o) for o in range(dynamic[4], dynamic[4]+dynamic[5], 16)]
        assert all(tag != 22 and not (tag == 30 and value & 4) for tag, value in tags), "TEXTREL"
    # Inspect actual executable bytes' disassembly, not source text.
    text = Path(disassembly).read_text(encoding="utf-8")
    forbidden = re.findall(r"\b(?:[xw]18|tpidr\w*|svc|msr|mrs)\b", text, flags=re.I)
    assert not forbidden, ("reserved register/system instruction", forbidden)
    assert "<xblr_host_gate>" in text and "<xblr_guest_callback>" in text
    return {"result": "PASS", "scope": "ELF structure, segment/page permissions, exports, dynamic relocations and disassembly; not execution or signing verification",
            "elf_sha256": hashlib.sha256(raw).hexdigest(), "elf_bytes": len(raw), "machine": "AArch64", "elf_type": "ET_DYN",
            "layout": layout, "exports": sorted(exports), "relocations": relocations,
            "reserved_register_or_system_instructions": forbidden,
            "symbols": {k: v for k, v in symbols.items() if k.startswith("xblr_") or k.startswith("__xblr_")}}
