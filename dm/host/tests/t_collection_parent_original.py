"""Clonación original y orden padre→binder→TextBox de la colección.

Ejecuta b7a36c, b79a7c, b79f54 y el despacho de callback ad29e0.
Metadata sin propiedades y fábricas/adaptadores mínimos aíslan el protocolo
de Widget; no simula una colección completa ni acredita renderizado GUI.
"""
import hashlib
from pathlib import Path
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

image = Path(sys.argv[1]).read_bytes()
assert hashlib.sha256(image).hexdigest() == '6843ab6909f96e1558b4b73b0fa807f3873409120cf3e1ef8a86dbebb275ee27'
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(0, 0x3400000)
u.mem_write(0, image)
u.mem_map(0x10000000, 0x100000)
u.mem_map(0x20000000, 0x100000)
STOP, META, VT, MVT = 0x3000000, 0x10001000, 0x10002000, 0x10003000
GETMETA, FACTORY, COPY, LAYOUT, BINDER = range(0x3000100, 0x3000240, 0x40)
checks, serial, made, factory_return, observed = 0, 0, [], [], []


def check(value):
    global checks
    checks += 1
    assert value, 'Comprobación %d' % checks


def wr(p, value, fmt='Q'):
    u.mem_write(p, struct.pack('<' + fmt, value))


def rd(p, fmt='Q'):
    return struct.unpack('<' + fmt, u.mem_read(p, struct.calcsize(fmt)))[0]


def reg(n):
    return u.reg_read(UC_ARM64_REG_X0 + n)


def ret():
    u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))


def cstr(p):
    return bytes(u.mem_read(p, 100)).split(b'\0', 1)[0]


def provision(p):
    # Capacidad disponible evita sustituir el algoritmo nativo de attach por
    # un mock de lista. El constructor inicializa +30/+50 por sí mismo.
    wr(p, VT)
    wr(p + 0x58, p + 0xc00)
    wr(p + 0x68, 16)


def hook(uc, address, size, data):
    global serial
    if address in (STOP, 0xad2a20, 0xb04cac):
        uc.emu_stop()
    elif address == GETMETA:
        uc.reg_write(UC_ARM64_REG_X0, META)
        ret()
    elif address == FACTORY:
        serial += 1
        new = 0x10010000 + serial * 0x1000
        factory_return.append(new)
        made.append(new)
        # Factory ABI (metadata, nombre, padre) → constructor Widget real.
        uc.reg_write(UC_ARM64_REG_X0, new)
        uc.reg_write(UC_ARM64_REG_PC, 0xb79a7c)
    elif address == 0xb7a3bc:
        new = factory_return.pop()
        provision(new)
        uc.reg_write(UC_ARM64_REG_X0, new)
    elif address in (COPY, LAYOUT):
        ret()
    elif address == 0x1b76070:
        uc.reg_write(UC_ARM64_REG_X0, len(cstr(reg(0))))
        ret()
    elif address == 0x1b76040:
        uc.mem_write(reg(0), bytes(uc.mem_read(reg(1), reg(2))))
        ret()
    elif address == BINDER:
        bound_row = reg(1)
        observed.append(bound_row)
        check(rd(bound_row + 0x30) == parent)
        check(rd(parent + 0x58) and rd(rd(parent + 0x58)) == bound_row)
        check(rd(bound_row + 0x60) == 1)
        text = rd(rd(bound_row + 0x58))
        check(rd(text + 0x30) == bound_row)
        check(cstr(rd(text + 8)) == b'ItemSlotText')
        ret()
    elif address == 0x136e5c:
        # Asignación string es un destino doblado; la llamada y TextBox x0
        # siguientes son las instrucciones originales de ChronicleItemListItem.
        wr(reg(0), reg(1))
        wr(reg(0) + 8, reg(2))
        ret()
    elif address == 0xadc434:
        text = reg(0)
        check(rd(text + 0x30) == row)
        check(rd(row + 0x30) == parent)
        check(cstr(rd(text + 8)) == b'ItemSlotText')
        check(cstr(rd(text + 0x88)) == b'[mp]Guantes pesados [normal]')
        ret()


u.hook_add(UC_HOOK_CODE, hook)
wr(META, MVT)
wr(MVT + 0x10, FACTORY)
wr(VT + 0x60, GETMETA)
wr(VT + 0x30, COPY)
wr(VT + 0x10, LAYOUT)
parent, proto, child = 0x10004000, 0x10006000, 0x10008000
for p, name in ((proto, b'ListItem0'), (child, b'ItemSlotText')):
    provision(p)
    wr(p + 8, p + 0x20)
    u.mem_write(p + 0x20, name + b'\0')
provision(parent)
wr(proto + 0x60, 1)
wr(proto + 0xc00, child)


def run(pc):
    u.reg_write(UC_ARM64_REG_SP, 0x200f0000)
    u.reg_write(UC_ARM64_REG_LR, STOP)
    u.emu_start(pc, STOP, count=30000)


u.reg_write(UC_ARM64_REG_X0, parent)
u.reg_write(UC_ARM64_REG_X1, proto)
u.reg_write(UC_ARM64_REG_X2, 0)
run(0xb7a36c)
row = reg(0)
check(len(made) == 2)
check(rd(row + 0x30) == parent)
check(rd(parent + 0x60) == 1)
check(rd(parent + 0xc00) == row)
text = rd(row + 0xc00)
check(rd(text + 0x30) == row)
check(rd(row + 0x50, 'B') == rd(text + 0x50, 'B') == 1)
check(u.reg_read(UC_ARM64_REG_SP) == 0x200f0000)

# Callback del recycler ejecutado sobre la fila clonada, sin inyectar +30.
listing, callback = 0x10040000, 0x10041000
wr(listing + 0x170, callback | 1)
wr(callback, callback + 0x100)
wr(callback + 0x110, BINDER)
wr(listing + 0x1c8, listing + 0x300)
wr(listing + 0x300, row)
wr(listing + 0x1e8, 1, 'I')
u.reg_write(UC_ARM64_REG_X19, listing)
u.reg_write(UC_ARM64_REG_X20, listing + 0x170)
u.reg_write(UC_ARM64_REG_X22, 0)
u.reg_write(UC_ARM64_REG_X23, 0)
run(0xad29e0)
check(observed == [row])

# Entrada real de asignación final del nombre; no se prueba shaping/render.
source = 0x10042000
u.mem_write(source, b'[mp]Guantes pesados [normal]\0')
u.reg_write(UC_ARM64_REG_X21, text)
u.reg_write(UC_ARM64_REG_X22, source)
run(0xb04c7c)
check(u.reg_read(UC_ARM64_REG_PC) == 0xb04cac)
print('COLECCIÓN PADRES ARM %d comprobaciones, 0 fallos' % checks)
