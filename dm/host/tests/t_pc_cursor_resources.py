"""Contrato ARM de recursos de cursor: aro, guante y objeto sostenido separados.

Ejecuta inicialización y carga originales; el cargador de sprites y el destino
de dibujo son dobles. No acredita el renderizado de la aplicación instalada.
"""
from pathlib import Path
import hashlib
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

image = Path(sys.argv[1]).read_bytes()
assert hashlib.sha256(image).hexdigest() == "6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27"
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(0, 0x3400000)
u.mem_write(0, image)
u.mem_map(0x10000000, 0x100000)
u.mem_map(0x20000000, 0x100000)
stop = 0x3000000
root, item_handle, item_image = 0x10001000, 0x10003000, 0x10004000
loads, draws = [], []
stop_after_loads = False
checks = 0


def check(value):
    global checks
    checks += 1
    assert value, "Comprobación %d" % checks


def wr(address, value, fmt="Q"):
    u.mem_write(address, struct.pack("<" + fmt, value))


def rd(address, fmt="Q"):
    return struct.unpack("<" + fmt, u.mem_read(address, struct.calcsize(fmt)))[0]


def cstr(address):
    return bytes(u.mem_read(address, 200)).split(b"\0", 1)[0].decode()


def ret():
    u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))


def hook(uc, address, size, data):
    if address == stop or (stop_after_loads and address == 0x2aa864):
        uc.emu_stop()
    elif address == 0x250:  # Registro de destructores; no interviene en rutas.
        ret()
    elif address == 0xad75f0:  # Cargador de recursos, único doble de carga.
        destination = uc.reg_read(UC_ARM64_REG_X0)
        path = cstr(uc.reg_read(UC_ARM64_REG_X1))
        loads.append((destination, path))
        wr(destination, 0x10010000 + len(loads) * 0x1000)
        ret()
    elif address == 0x390b38:
        uc.reg_write(UC_ARM64_REG_S0, 0x3f800000)
        ret()
    elif address == 0x45bd30:  # Color del objeto: no selecciona su imagen.
        ret()
    elif address == 0xad7764:
        draws.append(uc.reg_read(UC_ARM64_REG_X0))
        ret()


u.hook_add(UC_HOOK_CODE, hook)


def run(address):
    u.reg_write(UC_ARM64_REG_SP, 0x200f0000)
    u.reg_write(UC_ARM64_REG_LR, stop)
    u.emu_start(address, stop, count=10000)


run(0x2aa370)
expected = ["CURSOR/ohand", "CURSOR/orotate", "CURSOR/ppress", "CURSOR/protate",
            "CURSOR/buysell", "Controller/Cursor/Idle", "Controller/Cursor/Hold",
            "Controller/Cursor/Held"]
for index, name in enumerate(expected):
    check(cstr(rd(0x2d12828 + index * 0x18)) == name)
stop_after_loads = True
run(0x2aa7d0)
check([path for _, path in loads] == expected)
for index, (destination, _) in enumerate(loads):
    check(destination == 0x2d12828 + index * 0x18 + 8)
check(rd(0x2d12820) == 0)

# El recurso del objeto sostenido es independiente, incluso sin imágenes de aro.
wr(0x2f05458, root)
wr(root + 0xb9, 1, "B")
wr(0x2d12920, 0x10005000)
wr(0x2d12820, item_handle)
wr(item_handle, item_image)
wr(item_image + 0xc0, 32, "I")
wr(item_image + 0xd0, 64, "I")
for missing in (False, True):
    if missing:
        for index in (5, 6, 7):
            wr(0x2d12828 + index * 0x18 + 8, 0)
    draws.clear()
    u.reg_write(UC_ARM64_REG_X0, (400 << 32) | 300)
    run(0x2ab480)
    check(draws == [item_handle])
    check(rd(0x2d12820) == item_handle)
    check(u.reg_read(UC_ARM64_REG_SP) == 0x200f0000)
print("PC CURSOR ARM %d comprobaciones, 0 fallos" % checks, flush=True)

if len(sys.argv) == 4:
    import json

    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "ios/tools"))
    import pc_desktop_layout as pc

    romfs, overlay = map(Path, sys.argv[2:])
    manifest = json.loads((overlay / "pc_manifest.json").read_text())
    rings = {"data/hd/global/ui/controller/cursor/%s.sprite" % name
             for name in ("idle", "hold", "held")}
    actual = {name for name in manifest if "/ui/controller/cursor/" in name}
    check(actual == rings)
    check((overlay / "pc_desktop").is_file())
    check(not (overlay / "ctrl_touch").exists())
    originals = {}
    for line in (romfs / "data/build_manifest.psv").read_text().splitlines():
        fields = line.split("|")
        if len(fields) >= 11:
            logical = fields[9].lower().split(":", 1)[-1]
            if logical in rings or "/ui/cursor/" in logical:
                originals[logical] = pc.touch_layout._read_entry(str(romfs), fields)
    for logical in rings:
        original = originals[logical]
        replaced = pc.read_overlay(romfs, overlay, logical)
        check(replaced[:4] == b"SpA1")
        check(replaced[:0x28] == original[:0x28])
        check(len(replaced) == len(original))
        check(any(original[0x28:]))
        check(not any(replaced[0x28:]))
    gloves = {name for name in originals if "/ui/cursor/" in name}
    check(bool(gloves))
    for logical in gloves:
        check(logical not in manifest)
        check(pc.read_overlay(romfs, overlay, logical) == originals[logical])
    print("PC CURSOR ARM + CASC %d comprobaciones, 0 fallos" % checks)
