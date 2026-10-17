"""Exercise the actual input_gate.c native Windows build, never the ARM64 code."""
import ctypes as C
import json
import struct
import sys
from pathlib import Path

class Info(C.Structure):
    _fields_ = [(x, C.c_uint32) for x in (
        "abi_version", "struct_bytes", "input_kind", "role", "nso_flags", "header_only",
        "main_id_prefix_matches", "execution_allowed")]
    _fields_ += [("build_id", C.c_uint8 * 32), ("file_bytes", C.c_uint64), ("image_bytes", C.c_uint64)]

def run(dll, output):
    gate = C.CDLL(str(dll.resolve())).xblr_probe_input
    gate.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.POINTER(Info), C.c_size_t]
    gate.restype = C.c_int
    data = bytearray(0x130)
    data[:4] = b"NSO0"
    data[0x40:0x50] = bytes.fromhex("db0507f36b84200ac05f75d76ed5026f")
    for i in range(3):
        struct.pack_into("<III", data, 0x10 + 0x10 * i, 0x100 + 0x10 * i, 0x4000 * i, 0x10)
        struct.pack_into("<I", data, 0x60 + 4 * i, 0x10)
    rows = []
    def check(name, payload, expected, role=1, size=C.sizeof(Info), null_output=False):
        buf = C.create_string_buffer(bytes(payload)) if payload is not None else None
        info = Info()
        # Poison execution_allowed before every call to prove the gate clears it.
        info.execution_allowed = 0xFFFFFFFF
        rc = gate(buf, len(payload) if payload is not None else 0, role,
                  None if null_output else C.byref(info), size)
        assert rc == expected, (name, rc, expected)
        if expected != 1:
            assert info.execution_allowed == 0, (name, "execution permitted")
        rows.append({"name": name, "exit_status": rc, "execution_allowed":
                     info.execution_allowed if expected != 1 else "output unspecified on bad argument"})
        return info
    # These bytes are synthetic NSO-shaped parser fixtures, NOT original modules.
    info = check("recognized_header_still_cannot_execute", data, 3)
    assert info.input_kind == 1 and info.header_only == 1 and info.main_id_prefix_matches == 1
    check("sdk_header_still_cannot_execute", data, 3, role=2)
    for length in (0, 1, 3, 4, 15, 127, 255, 256, 0x12f):
        check(f"truncation_{length}", data[:length], 2)
    def changed(offset, value):
        result = data.copy(); struct.pack_into("<I", result, offset, value); return result
    check("wrong_magic", b"XBLR" + data[4:], 2)
    check("unknown_nso_version", changed(4, 1), 2)
    check("unknown_flags", changed(0xc, 0x40), 2)
    check("file_offset_overflow_range", changed(0x10, 0xfffffff0), 2)
    check("segment_overlaps_header", changed(0x10, 0xf0), 2)
    check("file_segments_overlap", changed(0x20, 0x100), 2)
    check("memory_segments_overlap", changed(0x24, 0), 2)
    check("memory_offset_overflow", changed(0x34, 0xfffffff8), 2)
    check("bss_overflow", changed(0x3c, 0xffffffff), 2)
    check("text_not_at_zero", changed(0x14, 4), 2)
    check("empty_segment", changed(0x28, 0), 2)
    compressed = changed(0xc, 1)
    struct.pack_into("<I", compressed, 0x60, 0)
    check("empty_compressed_stream", compressed, 2)
    check("wrong_main_build_id", changed(0x40, 0), 8)
    check("sdk_not_guessed_from_main_id", changed(0x40, 0), 3, role=2)
    check("unknown_role", data, 1, role=0)
    check("wrong_output_abi_size", data, 1, size=C.sizeof(Info)-1)
    check("null_output", data, 1, null_output=True)
    check("null_empty_input", None, 2)
    for flag in (1, 2, 4, 7, 0x38, 0x3f):
        check(f"known_flags_{flag}_still_no_execution", changed(0xc, flag), 3)
    report = {"result": "PASS", "scope": "same input_gate.c executed as Windows x64 DLL; synthetic NSO-shaped byte fixtures; no original NSO and no ARM64 execution",
              "cases": rows, "struct_bytes": C.sizeof(Info)}
    Path(output).write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"input gate: {len(rows)} cases PASS; no real NSO execution")

if __name__ == "__main__":
    run(Path(sys.argv[1]), Path(sys.argv[2]))
