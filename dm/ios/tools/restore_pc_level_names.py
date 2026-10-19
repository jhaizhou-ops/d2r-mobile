#!/usr/bin/env python3
"""Restaura levels.json oficial en una copia nueva de un overlay PC congelado.

Acepta exclusivamente el resultado conocido de annot_world.ann_levels, calculado
con levels.txt del mismo RomFS. La base debe ser ya original. Ningún nombre se
recorta ni se limpia mediante patrones: se restaura el recurso oficial completo.
"""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile

import pc_desktop_layout as pc
import touch_layout
from annot_strings import excel
from annot_world import _groups, _number
from dm_i18n import LANGS

LOGICAL = 'data/local/lng/strings/levels.json'
ANNOTATED_LANGS = ('zhCN', 'esES', 'esMX')


def parse(raw):
    return json.loads(raw.decode('utf-8-sig'))


def source(romfs):
    entries = []
    for line in (romfs / 'data/build_manifest.psv').read_text(encoding='utf-8').splitlines():
        f = line.split('|')
        if len(f) >= 11 and f[9].split(':', 1)[-1].lower() == LOGICAL:
            entries.append(f)
    if len(entries) != 1:
        raise ValueError('Debe existir exactamente un levels.json original')
    return touch_layout._read_entry(str(romfs), entries[0]), excel(str(romfs), 'levels.txt')


def expected_annotations(rows, levels):
    fields = ('MonLvlEx', 'MonLvlEx(N)', 'MonLvlEx(H)')
    suffixes = {}
    for key, values in _groups(levels, 'LevelName', fields).items():
        if len(set(values)) != 1:
            continue
        nums = tuple(_number(dict(zip(fields, values[0])), field) for field in fields)
        if nums != (0, 0, 0):
            suffixes[key] = '[%d/%d/%d]' % nums
    expected = copy.deepcopy(rows)
    for row in expected:
        if set(LANGS) - row.keys():
            raise ValueError('Faltan idiomas oficiales en levels.json')
        suffix = suffixes.get(row['Key'])
        if suffix:
            for lang in ANNOTATED_LANGS:
                row[lang] += suffix
    return expected


def validate_rows(original, actual, levels):
    if actual != expected_annotations(original, levels):
        raise ValueError('levels.json contiene cambios ajenos a ann_levels del RomFS original')
    return {lang: sum(a[lang] != b[lang] for a, b in zip(original, actual)) for lang in ANNOTATED_LANGS}


def validate_source(original, actual, levels):
    return validate_rows(parse(original), parse(actual), levels)


def restore_generated(romfs, outdir, replacements):
    """Etapa PC previa al empaquetado; conserva las demás sustituciones intactas."""
    original, levels = source(Path(romfs))
    counts = validate_source(original, Path(replacements[LOGICAL]).read_bytes(), levels)
    official = Path(outdir) / 'pc_official_levels.json'
    official.write_bytes(original)
    return dict(replacements, **{LOGICAL: str(official)}), counts


def fingerprints(folder):
    return {str(p.relative_to(folder)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}


def verify_delta(romfs, before, after, official, expected):
    old = json.loads((before / 'pc_manifest.json').read_text())
    new = json.loads((after / 'pc_manifest.json').read_text())
    changed = {k for k in old.keys() | new.keys() if old.get(k) != new.get(k)}
    if changed != expected:
        raise ValueError('Delta de recursos inesperado: ' + repr(changed))
    for logical in old.keys() | new.keys():
        a, b = (pc.read_overlay(romfs, path, logical) for path in (before, after))
        if logical == LOGICAL:
            if b != official:
                raise ValueError('El recurso restaurado no coincide byte por byte con el original')
        elif a != b:
            raise ValueError('Cambió otro recurso CASC: ' + logical)
    if pc.read_overlay(romfs, after, LOGICAL) != official:
        raise ValueError('Nombres de zonas distintos del original')
    pc.verify(romfs, after)
    return sorted(changed)


def main(romfs, previous, output, evidence):
    romfs, previous, output, evidence = map(Path, (romfs, previous, output, evidence))
    old_notes = Path(str(previous) + '_variants') / 'item_notes'
    new_notes = Path(str(output) + '_variants') / 'item_notes'
    if output.exists() or new_notes.parent.exists():
        raise FileExistsError('El destino debe ser nuevo; no se reescriben overlays congelados')
    evidence.mkdir(parents=True, exist_ok=True)
    original, levels = source(romfs)
    if pc.read_overlay(romfs, previous, LOGICAL) != original:
        raise ValueError('La base no contiene levels.json original')
    counts = validate_source(original, pc.read_overlay(romfs, old_notes, LOGICAL), levels)
    frozen = {str(p): fingerprints(p) for p in (previous, old_notes)}
    with tempfile.TemporaryDirectory(prefix='d2r-official-levels-') as tmp:
        work = Path(tmp)
        manifest = json.loads((old_notes / 'pc_manifest.json').read_text())
        replacements = {}
        for index, logical in enumerate(manifest):
            path = work / ('resource-%04d.bin' % index)
            path.write_bytes(original if logical == LOGICAL else pc.read_overlay(romfs, old_notes, logical))
            replacements[logical] = str(path)
        if LOGICAL not in replacements:
            raise ValueError('La variante no declara el recurso de nombres anotado')
        pc.emit(romfs, new_notes, replacements, work, 'official_levels')
    for path in old_notes.iterdir():
        if path.is_file() and path.name != 'pc_manifest.json':
            shutil.copyfile(path, new_notes / path.name)
    shutil.copytree(previous, output)
    changed = verify_delta(romfs, old_notes, new_notes, original, {LOGICAL})
    verify_delta(romfs, previous, output, original, set())
    if fingerprints(output) != frozen[str(previous)]:
        raise ValueError('La base copiada no es idéntica byte por byte')
    for folder in (previous, old_notes):
        if fingerprints(folder) != frozen[str(folder)]:
            raise ValueError('Se alteró un overlay anterior')
    result = {
        'original_levels_sha256': hashlib.sha256(original).hexdigest(),
        'official_rows': len(parse(original)), 'languages': list(LANGS),
        'removed_known_suffixes': counts, 'base_changed_resources': [],
        'notes_changed_resources': changed,
        'previous_unchanged': frozen,
        'base': {'path': str(output.resolve()), 'files': fingerprints(output)},
        'item_notes': {'path': str(new_notes.resolve()), 'files': fingerprints(new_notes)},
        'tools': {name: hashlib.sha256((Path(__file__).parent / name).read_bytes()).hexdigest()
                  for name in ('restore_pc_level_names.py', 'test_restore_pc_level_names.py')},
    }
    (evidence / 'freeze.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (evidence / 'levels-original.json').write_bytes(original)
    print('Congelado:', output, new_notes, counts, flush=True)


if __name__ == '__main__':
    main(*sys.argv[1:])
