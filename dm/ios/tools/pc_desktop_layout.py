"""Optional desktop input profile built from the user's original PC layouts.

OBS24/25 resolve root layouts before the original controller fallback. This
module only supplies PC profile metrics, the frontend offline argument and
transparent virtual-controller cursors. Item extensions are optional and live
in pc_item_layouts. The existing upstream Chinese/English UI table is appended
once after optional extensions; no private translations are added here.
"""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import touch_layout

HERE = Path(__file__).resolve().parent
LAY = 'data/global/ui/layouts/'
CONTROLLER_PROFILES = ('hd', 'lv', 'asian', 'consolehd', 'consolelv', 'handheldhd', 'handheldlv')


def parse(text):
    """Read original JSON comments/trailing commas without changing string contents."""
    out = []
    i = 0
    quoted = False
    while i < len(text):
        c = text[i]
        if quoted:
            out.append(c)
            if c == '\\':
                i += 1
                out.append(text[i])
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
            out.append(c)
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = len(text) if j < 0 else j
            continue
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            if j < 0:
                raise ValueError('Unterminated comment')
            out.append(' ')
            i = j + 2
            continue
        else:
            out.append(c)
        i += 1
    text = ''.join(out)
    out = []
    i = 0
    quoted = False
    while i < len(text):
        c = text[i]
        if quoted:
            out.append(c)
            if c == '\\':
                i += 1
                out.append(text[i])
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
            out.append(c)
        elif c == ',' and re.match(r'\s*[}\]]', text[i + 1:]):
            pass
        else:
            out.append(c)
        i += 1
    return json.loads(''.join(out).lstrip('\ufeff'))



def load(romfs, rel):
    return parse(touch_layout.read_layout(str(romfs), rel)[1])



def walk(widget):
    yield widget
    for child in widget.get('children', []):
        yield from walk(child)



def find(widget, name):
    matches = [w for w in walk(widget) if w.get('name') == name]
    if len(matches) != 1:
        raise ValueError('%s: expected one widget, found %d' % (name, len(matches)))
    return matches[0]



def controller_shell(original):
    """Keep the native class; PC panels already draw their own frames."""
    shell = copy.deepcopy(original)
    for name in ('TransparentBG', 'Anchor'):
        fields = find(shell, name).setdefault('fields', {})
        fields.update(fitToParent=False, fitToScreen=False,
                      rect={'x': 99999, 'y': 0, 'width': 2880, 'height': 1763})
    return shell



def profile_family(relative):
    name = Path(relative).stem
    return 'asian' if name.endswith('asian') else 'lv' if name.endswith('lv') else 'hd'



def pc_profile(romfs, family):
    """Resolve original PC metrics, retaining language and accessibility variants."""
    values = load(romfs, '_profilehd.json')
    if family != 'hd':
        child = load(romfs, '_profile' + family + '.json')
        assert child.pop('basedOn') == 'HD'
        values.update(child)
    assert 'basedOn' not in values
    return values



def desktop_profiles(romfs):
    profiles = {}
    for suffix in CONTROLLER_PROFILES:
        relative = 'controller/_profile' + suffix + '.json'
        values = load(romfs, relative)
        assert values.get('basedOn')
        # Keep platform-only values while sharing original PC layout metrics.
        values.update(copy.deepcopy(pc_profile(romfs, profile_family(relative))))
        profiles[relative] = values
    return profiles



def character_select_layout(original):
    """Supply the offline argument required by the original Switch handler."""
    page = copy.deepcopy(original)
    button = find(page, 'CreateCharacter')['fields']
    assert button['onClickMessage'] == 'FrontEndNavigation:ToCharacterCreate'
    button['onClickMessage'] += ':1'
    return page



def emit(romfs, output, replacements, workdir, tag):
    if output.exists():
        raise FileExistsError('Overlay output already exists: ' + str(output))
    manifest = {line.split('|')[9].lower().split(':', 1)[-1] for line in (romfs / 'data/build_manifest.psv').read_text().splitlines() if len(line.split('|')) >= 11}
    table = workdir / (tag + '_repl.tsv')
    table.write_text(''.join(('' if k.lstrip('+') in manifest else '+') + k.lstrip('+') + '\t' + str(v) + '\n' for k, v in replacements.items()), encoding='utf-8')
    packed = workdir / (tag + '_packed')
    subprocess.run([sys.executable, str(HERE / '../../tools/casc_overlay.py'), str(romfs), str(packed), str(table)], check=True)
    data = [p for p in (packed / 'data').glob('*.data') if not p.is_symlink()]
    assert len(data) == 1
    (output / 'data').mkdir(parents=True)
    for p in [packed / 'data/.build.config', *data]:
        shutil.copyfile(p, output / 'data' / p.name)
    (output / 'pc_desktop').write_text('Original PC layouts; native fallback when a root layout is absent.\n', encoding='utf-8')
    (output / 'pc_manifest.json').write_text(json.dumps({k.lstrip('+'): hashlib.sha256(Path(v).read_bytes()).hexdigest() for k, v in replacements.items()}, indent=1) + '\n')
    print('Overlay PC:', output, data[0].name, data[0].stat().st_size, 'bytes', flush=True)



def overlay_index(romfs, folder):
    sys.path.insert(0, str(HERE / '../../tools'))
    import casc_overlay as casc
    original = casc.load_vfs1(romfs)
    tvfs = casc.load_vfs1(folder)
    po, ps, fo, fs, co, cs = struct.unpack_from('>IIIIII', tvfs, 12)
    files = {p.decode().lower(): v for _, p, v, sub, _ in casc.walk_paths(tvfs, po, po + ps) if sub is None}
    so, sl = struct.unpack_from('>II', tvfs, 38)
    specs = tvfs[so:so + sl].split(b'\0')
    return casc, original, tvfs, files, fo, co, cs, specs, casc.read_manifest(romfs)



def read_overlay(romfs, folder, logical):
    casc, original, tvfs, files, fo, co, cs, specs, manifest = overlay_index(romfs, folder)
    if logical.lower() in files:
        at = fo + files[logical.lower()]
        offset = co + int.from_bytes(tvfs[at + 9:at + 9 + casc.offset_field_size(cs)], 'big')
    else:
        oc, size = struct.unpack_from('>II', original, 28)
        offset = co + casc.cft_position(original, oc, oc + size, manifest, logical) - oc
    key = tvfs[offset:offset + 16]
    size = struct.unpack_from('>I', tvfs, offset + 16)[0]
    spec = specs[tvfs[offset + 20]]
    path = folder / 'data' / ('%02d-%08d.data' % (key[9], key[10]))
    if not path.exists():
        path = romfs / 'data' / path.name
    with path.open('rb') as f:
        f.seek(int.from_bytes(key[11:16], 'big'))
        raw = f.read(size)
    if spec in (b'z', b'n'):
        assert hashlib.md5(raw).digest()[:9] == key[:9]
        return zlib.decompress(raw) if spec == b'z' else raw
    return casc.read_data_file(path.parent, bytes(key), size)



def build_layouts(romfs, outdir):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    changes = desktop_profiles(romfs)
    changes['characterselectpanelhd.json'] = character_select_layout(load(romfs, 'characterselectpanelhd.json'))
    changes['controller/controlleroverlayhd.json'] = controller_shell(load(romfs, 'controller/controlleroverlayhd.json'))
    result = {}
    for relative, page in changes.items():
        path = outdir / relative.replace('/', '_')
        path.write_text(json.dumps(page, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
        result[LAY + relative] = str(path)
    return result


def build_cursor_art(romfs, outdir):
    """Hide only the virtual controller rings; held items and OS cursors stay intact."""
    import romfs_art
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    result = {}
    for name in ('idle', 'hold', 'held'):
        logical = 'data/hd/global/ui/controller/cursor/' + name + '.sprite'
        raw = romfs_art.read(str(romfs), logical)
        if len(raw) < 40 or raw[:4] != b'SpA1' or struct.unpack_from('<H', raw, 4)[0] != 61 or struct.unpack_from('<I', raw, 32)[0] != len(raw) - 40:
            raise ValueError('Unsupported controller cursor: ' + logical)
        path = outdir / ('cursor_' + name + '.sprite')
        path.write_bytes(raw[:40] + bytes(len(raw) - 40))
        result[logical] = str(path)
    return result


def build_art(romfs, outdir):
    return build_cursor_art(romfs, outdir)


def build_replacements(romfs, outdir):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    result = build_layouts(romfs, outdir)
    result.update(build_art(romfs, outdir))
    if importlib.util.find_spec('pc_item_layouts') is not None:
        import pc_item_layouts
        result.update(pc_item_layouts.build(romfs, outdir / 'pc-items', result)[0])
    return result


def verify(romfs, output):
    output = Path(output)
    manifest = json.loads((output / 'pc_manifest.json').read_text())
    for logical, digest in manifest.items():
        if hashlib.sha256(read_overlay(Path(romfs), output, logical)).hexdigest() != digest:
            raise ValueError('Overlay content mismatch: ' + logical)
    if not (output / 'pc_desktop').is_file():
        raise ValueError('Missing desktop profile marker')
    print('PC overlay verified:', len(manifest), 'resources', flush=True)


def main(romfs, output):
    romfs, output = Path(romfs), Path(output)
    if not os.environ.get('DM_OVERLAY_MAC'):
        raise SystemExit('DM_OVERLAY_PC requires DM_OVERLAY_MAC=1')
    if output.exists():
        raise FileExistsError('Overlay output already exists: ' + str(output))
    with tempfile.TemporaryDirectory(prefix='pc-input-overlay-') as tmp:
        work = Path(tmp)
        replacements = build_replacements(romfs, work)
        import dm_i18n
        replacements.update(dm_i18n.build(str(romfs), str(work), replacements)[0])
        emit(romfs, output, replacements, work, 'pc')
        verify(romfs, output)


if __name__ == '__main__':
    main(*sys.argv[1:3])
