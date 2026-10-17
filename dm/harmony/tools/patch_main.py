"""只对仓外r32产物应用历史宽屏/显存/阴影剪口；逐处核原指令。
Apply historical widescreen/VRAM/shadow cuts only to external r32 outputs, checking each original word.
"""
import struct


def apply(generated):
    t=bytearray(open(generated / 'main.harmony.text.bin','rb').read())
    seg=bytearray(open(generated / 'main.harmony.segments.bin','rb').read())
    def word(b,o): return struct.unpack_from('<I',b,o)[0]
    def put(b,o,old,new):
        assert word(b,o)==old, f'{o:#x}: {word(b,o):08x} != {old:08x}'; struct.pack_into('<I',b,o,new)
    relocs=[]
    # (A, oldA, B, oldB, reg, symbol, 64bit)
    PARAMS=[(0xe12d8c,0xd280c813,0xe12d90,0xf2c07093,19,'xbl_kres1',1),
            (0xe12e1c,0xd280a008,0xe12e20,0xf2c05a08,8,'xbl_kres0',1),
            (0xe10d60,0xd280c818,0xe10d80,0xf2c07098,24,'xbl_kres1',1),
            (0xe10d5c,0xd280a016,0xe10d74,0xf2c05a16,22,'xbl_kres0',1),
            (0x1663ee8,0x52838e49,0x1663eec,0x72a800e9,9,'xbl_kaspect_ui',0),
            (0xb152bc,0x52838e48,0xb152c0,0x72a800e8,8,'xbl_kaspect',0),
            (0x23e888,0x52838e48,0x23e88c,0x72a800e8,8,'xbl_kaspect',0)]
    for a,wa,b,wb,r,sym,x64 in PARAMS:
        put(t,a,wa,0x90000000|r)                                   # adrp xR, sym
        put(t,b,wb,(0xF9400000 if x64 else 0xB9400000)|(r<<5)|r)   # ldr xR/wR, [xR, :lo12:sym]
        relocs.append((a,'R_AARCH64_ADR_PREL_PG_HI21',sym)); relocs.append((b,'R_AARCH64_LDST64_ABS_LO12_NC' if x64 else 'R_AARCH64_LDST32_ABS_LO12_NC',sym))
    # width masks: mov w9,#0x740; and x9,xN,x9 -> mov w9,wN; nop
    put(t,0xe10e48,0x5280e809,0x2a1903e9); put(t,0xe10e4c,0x8a090329,0xd503201f)
    put(t,0xe12e88,0x5280e809,0x2a1403e9); put(t,0xe12e8c,0x8a090289,0xd503201f)
    # UI canvas width cap 4560 -> 0xffff
    put(t,0xb1a660,0x52823a08,0x529fffe8)
    # output resolution cap 1920x1080 -> 4096x2160 (data)
    assert seg[0x211da30:0x211da38]==bytes.fromhex('8007000038040000'); seg[0x211da30:0x211da38]=bytes.fromhex('0010000070080000')
    open(generated / 'main.harmony.text.bin','wb').write(t); open(generated / 'main.harmony.segments.bin','wb').write(seg)
    s=open(generated / 'harmony_images.S',encoding='utf-8').read()
    marker='// claude widescreen relocs\n'
    if marker not in s:
        k=s.index('.global xblr_main_s0')
        s=s[:k]+marker+''.join(f'.reloc xblr_main_base+{o}, {ty}, {sym}\n' for o,ty,sym in relocs)+s[k:]
    open(generated / 'harmony_images.S','w',encoding='utf-8').write(s)

    # 512MB显存与1024阴影是当前历史候选的有界替换。 | Guarded 512MB VRAM and 1024 shadow replacements from the historical candidate.
    t=bytearray((generated / 'main.harmony.text.bin').read_bytes())
    put(t,0x8524c4,0xd2a20808,0xd2a40008)
    old=struct.unpack_from('<I',t,0x168a580)[0]
    assert old==0x5280000e|(2100<<5)
    struct.pack_into('<I',t,0x168a580,0x5280000e|(1024<<5))
    (generated / 'main.harmony.text.bin').write_bytes(t)
