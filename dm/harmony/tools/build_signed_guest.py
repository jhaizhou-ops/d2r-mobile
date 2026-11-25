"""Build HAP-signable ELF carriers from offline OHOS module images.

All original bytes and relative addresses are retained. Executable segments are
read-only; the key page, relocations and mutable data occupy a separate RW segment.
Run this before HAP signing. Generated binaries contain private game-derived code
and must never be committed or distributed as the public empty shell.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from datetime import datetime
from pathlib import Path
PAGE = 0x4000

def run(name, args):
    args = [str(a) for a in args]
    p = subprocess.run(args, cwd=WORK, text=True, encoding='utf-8', errors='replace', capture_output=True)
    (EVIDENCE / (name + '.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
    (EVIDENCE / (name + '.json')).write_text(json.dumps(dict(arguments=args, cwd=str(WORK), exit_code=p.returncode, time=datetime.now().astimezone().isoformat()), ensure_ascii=False, indent=2), encoding='utf-8')
    if p.returncode:
        raise RuntimeError(name + ': ' + p.stdout + p.stderr)
    return p.stdout


def main():
    global WORK, EVIDENCE, SDK
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--sdk-native', required=True, type=Path)
    args = parser.parse_args()
    inputs = args.input.resolve()
    WORK = EVIDENCE = args.output.resolve()
    SDK = args.sdk_native.resolve()
    WORK.mkdir(parents=True, exist_ok=True)
    reports = []
    for module in ('main', 'sdk'):
        raw_path = inputs / (module + '.patched.bin')
        raw = raw_path.read_bytes()
        layout = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'^#\s+(\w+)\s+([0-9a-fA-Fx]+)', (inputs / (module + '.fixups.tsv')).read_text(), re.M)}
        x0 = (layout['TEXT_END'] + PAGE - 1) & -PAGE
        po = layout['PATCH_OFF']
        end = (len(raw) + PAGE - 1) & -PAGE
        assert po % PAGE == 0 and x0 <= layout['KEY_PAGE_OFF']
        assert layout['KEY_PAGE_OFF'] + layout['KEY_PAGE_SIZE'] <= po
        assert layout['TARGET'] == 2
        asm = []
        for section, flags, lo, hi in [('x0', 'ax', 0, x0), ('w', 'aw', x0, po), ('x1', 'ax', po, end)]:
            asm += [f'.section .guest.{section},"{flags}",@progbits', '.balign 16384']
            if lo == 0:
                asm += ['.global dm_guest_image', '.type dm_guest_image,@object', 'dm_guest_image:']
            n = min(hi, len(raw)) - lo
            asm += [f'.incbin "{raw_path.as_posix()}", {lo}, {n}']
            if hi - lo > n:
                asm += [f'.zero {hi - lo - n}']
        asm += [f'.size dm_guest_image,{end}', '.section .rodata.guest.meta,"a",@progbits', '.balign 8', '.global dm_guest_descriptor', '.type dm_guest_descriptor,@object', 'dm_guest_descriptor:']
        values = [0x444D5349474E3031, 1, len(raw), end, layout['TEXT_END'], po, layout['KEY_PAGE_OFF'], layout['KEY_PAGE_SIZE'], layout['RO_OFF'], layout['DATA_OFF']]
        asm += [f'.quad {v}' for v in values]
        asm += ['.byte ' + ','.join(str(b) for b in hashlib.sha256(raw).digest() + hashlib.sha256((inputs / (module + '.fixups.tsv')).read_bytes()).digest()), '.size dm_guest_descriptor,.-dm_guest_descriptor', '.section .note.GNU-stack,"",@progbits']
        asm_path = WORK / (module + '.S')
        asm_path.write_text('\n'.join(asm) + '\n', encoding='utf-8')
        script = '''PHDRS {
  phdr PT_PHDR PHDRS FLAGS(4);
  meta PT_LOAD FILEHDR PHDRS FLAGS(4);
  x0 PT_LOAD FLAGS(5);
  w PT_LOAD FLAGS(6);
  x1 PT_LOAD FLAGS(5);
  dynamic PT_DYNAMIC FLAGS(4);
  stack PT_GNU_STACK FLAGS(6);
}
SECTIONS {
  . = SIZEOF_HEADERS;
  .note.gnu.build-id : { *(.note.gnu.build-id) } :meta
  .hash : { *(.hash) } :meta
  .gnu.hash : { *(.gnu.hash) } :meta
  .dynsym : { *(.dynsym) } :meta
  .dynstr : { *(.dynstr) } :meta
  .guest.meta : { KEEP(*(.rodata.guest.meta)) } :meta
  .dynamic : { *(.dynamic) } :meta :dynamic
  . = ALIGN(0x4000);
  .guest.x0 : { KEEP(*(.guest.x0)) } :x0
  .guest.w : { KEEP(*(.guest.w)) } :w
  .guest.x1 : { KEEP(*(.guest.x1)) } :x1
  /DISCARD/ : { *(.comment) *(.note.GNU-stack) }
}
'''
        script += f'ASSERT(ADDR(.guest.w) == ADDR(.guest.x0) + {x0}, "data offset changed")\n'
        script += f'ASSERT(ADDR(.guest.x1) == ADDR(.guest.x0) + {po}, "patch offset changed")\n'
        script_path = WORK / (module + '.ld')
        script_path.write_text(script, encoding='utf-8')
        obj = WORK / (module + '.o')
        lib = WORK / ('libdm_guest_' + module + '.so')
        run(module + '-assemble', [SDK / 'llvm/bin/clang.exe', '--target=aarch64-linux-ohos', '-c', asm_path, '-o', obj])
        run(module + '-link', [SDK / 'llvm/bin/ld.lld.exe', '-shared', '--build-id=sha1', '--hash-style=both', '-z', 'max-page-size=16384', '-T', script_path, '-soname', lib.name, obj, '-o', lib])
        run(module + '-readelf', [SDK / 'llvm/bin/llvm-readelf.exe', '-h', '-l', '-d', '-s', lib])
        elf = lib.read_bytes()
        eh = struct.unpack_from('<16sHHIQQQIHHHHHH', elf)
        assert eh[1:4] == (3, 183, 1)
        loads = []
        for i in range(eh[10]):
            ph = struct.unpack_from('<IIQQQQQQ', elf, eh[5] + i * eh[9])
            if ph[0] == 1:
                loads.append(ph)
                assert not (ph[1] & 2 and ph[1] & 1), 'Writable executable segment'
        assert [p[1] for p in loads] == [4, 5, 6, 5]
        image_segments = loads[1:]
        base = image_segments[0][3]
        reconstructed = bytearray(end)
        for p in image_segments:
            offset = p[3] - base
            reconstructed[offset:offset + p[5]] = elf[p[2]:p[2] + p[5]]
        assert bytes(reconstructed[:len(raw)]) == raw
        assert not any(reconstructed[len(raw):])
        assert [p[3] - base for p in image_segments] == [0, x0, po]
        reports.append(dict(module=module, bytes=len(raw), raw_sha256=hashlib.sha256(raw).hexdigest(), library=str(lib), library_sha256=hashlib.sha256(elf).hexdigest(), original_relative_offsets_preserved=True, original_bytes_preserved=True, writable_executable_segments=0, flags=[p[1] for p in loads], device_execution_verified=False))
    (EVIDENCE / 'signed-guest-libraries.json').write_text(json.dumps(reports, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(reports, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
