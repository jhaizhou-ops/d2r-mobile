"""Corrupt only in-memory copies of the real linked ELF; originals remain untouched."""
import hashlib
import json
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.inspect_elf import inspect

def run(elf, disassembly, output):
    raw = elf.read_bytes()
    phoff = struct.unpack_from("<Q", raw, 32)[0]
    shoff = struct.unpack_from("<Q", raw, 40)[0]
    phnum, shnum, stridx = (struct.unpack_from("<H", raw, o)[0] for o in (56, 60, 62))
    sh = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff+i*64) for i in range(shnum)]
    names = raw[sh[stridx][4]:sh[stridx][4]+sh[stridx][5]]
    def section_name(s): return names[s[0]:names.index(0, s[0])].decode()
    sections = {section_name(s): (i, s) for i, s in enumerate(sh)}
    mutations = []
    def alter(name, offset, fmt, value):
        candidate = bytearray(raw); struct.pack_into(fmt, candidate, offset, value)
        mutations.append((name, bytes(candidate)))
    alter("wrong_machine", 18, "<H", 62)
    load_index = next(i for i in range(phnum) if struct.unpack_from("<II", raw, phoff+i*56) == (1, 5))
    alter("rwx_segment", phoff+load_index*56+4, "<I", 7)
    ro_index, ro = sections[".xblr_guest_ro"]
    alter("broken_guest_ro_relative_offset", shoff+ro_index*64+16, "<Q", ro[3]+4)
    data_index, _ = sections[".xblr_guest_data"]
    alter("broken_guest_data_size", shoff+data_index*64+32, "<Q", 16)
    _, rel = sections[".rela.dyn"]
    _, text = sections[".xblr_guest_text"]
    alter("dynamic_relocation_into_guest_code", rel[4], "<Q", text[3])
    rows = []
    for name, candidate in mutations:
        try: inspect(elf, disassembly, raw_override=candidate)
        except AssertionError as exc:
            rows.append({"name": name, "rejected": True, "reason": str(exc), "sha256": hashlib.sha256(candidate).hexdigest()})
        else: raise AssertionError(f"negative control accepted: {name}")
    assert elf.read_bytes() == raw
    Path(output).write_text(json.dumps({"result": "PASS", "scope": "ELF checker negative controls; in-memory corruptions only", "cases": rows}, indent=2), encoding="utf-8")
    print(f"ELF checker: {len(rows)} negative controls rejected; original unchanged")

if __name__ == "__main__":
    run(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
