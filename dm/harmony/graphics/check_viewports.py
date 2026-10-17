"""Check the production NVN viewport converter against original framebuffer math.

Uses genuine frame120 VB/UBO captures for the UI shader and independent
passthrough clip coordinates for the non-UI shader convention. No device claim.
"""
import argparse
import ctypes as C
import hashlib
import itertools
import json
import math
import struct
from pathlib import Path


class Viewport(C.Structure):
    _fields_ = [(x, C.c_float) for x in ('x', 'y', 'width', 'height', 'minimum', 'maximum')]


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def original_ui_vertex(v, m):
    r = []
    for j in range(4):
        a = f32(v[1] * m[j + 4])
        a = f32(v[0] * m[j] + a)
        a = f32(v[2] * m[j + 8] + a)
        r.append(f32(a + m[j + 12]))
    r[1] = -r[1]
    return r


def vk_point(vp, p):
    return [vp.x + (1 + p[0] / p[3]) * vp.width / 2,
            vp.y + (1 + p[1] / p[3]) * vp.height / 2,
            vp.minimum + p[2] / p[3] * (vp.maximum - vp.minimum)]


def old_host_point(vp, p):
    return [vp[0] + (1 + p[0] / p[3]) * vp[2] / 2,
            vp[1] + (1 - p[1] / p[3]) * vp[3] / 2,
            vp[4] + p[2] / p[3] * (vp[5] - vp[4])]


def area(points, indices):
    p = [points[i] for i in indices]
    return -0.5 * sum(p[i][0] * p[(i + 1) % 3][1] - p[(i + 1) % 3][0] * p[i][1] for i in range(3))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--library', required=True, type=Path)
    ap.add_argument('--capture', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    a = ap.parse_args()
    lib = C.CDLL(str(a.library.resolve()))
    fn = lib.xbg_nvn_viewport
    fn.argtypes = [C.POINTER(Viewport)] + [C.c_float] * 6
    fn.restype = C.c_int
    inputs = {}
    def read(path):
        b = path.read_bytes()
        inputs[path.name] = hashlib.sha256(b).hexdigest()
        return b
    trace = read(a.capture / 'capture.txt').decode()
    assert 'END frame=120 events=16' in trace
    records = []
    for event in range(4, 15):
        vb = a.capture / f'f120-e{event}-vertex-stream0.bin'
        cb = a.capture / f'f120-e{event}-st0-bind6-el0-buffer.bin'
        if not vb.exists() or not cb.exists():
            continue
        record = trace.split(f'EVENT frame=120 event={event} ', 1)[1].split('EVENT frame=', 1)[0]
        assert 'vs=f88d27f131d71049' in record and 'cull=2 front=1' in record
        vertices = list(struct.iter_unpack('<5f', read(vb)))
        matrix = struct.unpack_from('<16f', read(cb))
        clips = [original_ui_vertex(v, matrix) for v in vertices]
        vp = Viewport()
        assert fn(C.byref(vp), 0, 0, 1600, 900, 0, 1) == 0
        points = [vk_point(vp, p) for p in clips]
        old = [vk_point(Viewport(0, 0, 1600, 900, 0, 1), p) for p in clips]
        reference = [old_host_point((0, 0, 1600, 900, 0, 1), p) for p in clips]
        assert points == reference
        fixed_areas = [area(points, t) for t in ((0, 1, 2), (2, 1, 3))]
        old_areas = [area(old, t) for t in ((0, 1, 2), (2, 1, 3))]
        assert all(x > 0 for x in fixed_areas) and all(x < 0 for x in old_areas)
        # CW content reverses the sign. Preserve the game's front/back setting;
        # a second front-face inversion would wrongly reject these actual quads.
        assert area(points, (0, 2, 1)) < 0
        assert all(0 <= p[0] <= 1600 and 0 <= p[1] <= 900 and 0 <= p[2] <= 1 for p in points)
        records.append({'event': event, 'screen': points, 'area': fixed_areas,
                        'old_area': old_areas, 'original_host_match': True})
    assert len(records) >= 6
    # The same mapping must work without any shader-side Y negation: the actual
    # c946/69cf/646e/d5ea non-UI programs pass location0 to position directly.
    points_checked = 0
    for x, y, w, h, lo, hi in [(0, 0, 1600, 900, 0, 1), (17, 23, 400, 225, .25, .75),
                              (-50, -30, 1024, 768, 1, 0), (3, 9, 1, 1, .5, .5)]:
        vp = Viewport()
        assert fn(C.byref(vp), x, y, w, h, lo, hi) == 0
        for px, py, pz, pw in itertools.product((-1, -.5, 0, .5, 1), (-1, 0, 1), (0, .25, 1), (1, 2)):
            clip = (px * pw, py * pw, pz * pw, pw)
            got, ref = vk_point(vp, clip), old_host_point((x, y, w, h, lo, hi), clip)
            assert got == ref, (got, ref)
            points_checked += 1
    invalid = 0
    for index in range(6):
        for bad in (math.nan, math.inf, -math.inf):
            values = [0., 0., 1600., 900., 0., 1.]; values[index] = bad
            vp = Viewport(11, 22, 33, 44, .25, .75); before = bytes(vp)
            assert fn(C.byref(vp), *values) == -1 and bytes(vp) == before
            invalid += 1
    for values in [(0, 0, 0, 900, 0, 1), (0, 0, 1600, -1, 0, 1),
                   (0, 0, 1600, 900, -.1, 1), (0, 0, 1600, 900, 0, 1.1),
                   (3e38, 0, 3e38, 1, 0, 1), (0, 3e38, 1, 3e38, 0, 1)]:
        vp = Viewport(11, 22, 33, 44, .25, .75); before = bytes(vp)
        assert fn(C.byref(vp), *values) == -1 and bytes(vp) == before
        invalid += 1
    assert fn(None, 0, 0, 1600, 900, 0, 1) == -1
    result = {'status': 'PASS', 'actual_ui_quads': records, 'non_ui_clip_points': points_checked,
              'invalid_rejections': invalid + 1, 'inputs_sha256': inputs,
              'gpu_fix_validation': 'NOT_RUN',
              'limitations': 'Capture shadow layers use black tint; unchanged RGB alone does not prove culling. No all-corruption-fix claim.',
              'references': ['https://docs.vulkan.org/refpages/latest/refpages/source/VkViewport.html',
                             'https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/chapters/primsrast.adoc']}
    a.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': 'PASS', 'actual_quads': len(records), 'non_ui_clip_points': points_checked, 'invalid': invalid + 1}))


if __name__ == '__main__':
    main()
