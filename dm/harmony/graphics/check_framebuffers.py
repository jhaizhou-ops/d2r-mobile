"""Production framebuffer planner checks against independent NVN mip intersections."""
import argparse
import ctypes as C
import hashlib
import itertools
import json
from pathlib import Path


class Attachment(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('active', 'width', 'height', 'levels', 'layers', 'mip', 'mips', 'layer', 'layer_count', 'samples')]


class Extent(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('width', 'height', 'layers', 'samples')]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--library', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    lib = C.CDLL(str(args.library.resolve()))
    lib.xbg_framebuffer_plan.argtypes = [C.POINTER(Attachment), C.c_uint32, C.c_uint32, C.c_uint32, C.POINTER(Extent), C.POINTER(C.c_uint32)]
    counts = {'intersection_orders_mips_samples': 0, 'invalid_rejections': 0, 'single_attachment_clear_scope': 0}
    def run(items, expected=None, error=0, bad=0xffffffff, limit=(16384, 16384)):
        array = (Attachment * len(items))(*items)
        out = Extent(0xa5a5, 0xb6b6, 0xc7c7, 0xd8d8); before = bytes(out)
        invalid = C.c_uint32()
        result = lib.xbg_framebuffer_plan(array, len(items), *limit, C.byref(out), C.byref(invalid))
        assert (result, invalid.value) == (error, bad), (result, invalid.value, error, bad)
        if error:
            assert bytes(out) == before
            counts['invalid_rejections'] += 1
        else:
            assert (out.width, out.height, out.layers, out.samples) == expected
        return out
    def a(w=800, h=450, mip=0, layer=0, samples=1):
        return Attachment(1, w, h, 10, 6, mip, 1, layer, 1, samples)
    for sample, mip, dims in itertools.product((1, 2, 4, 8), (0, 1, 2, 7, 9),
                                                [((800, 450), (400, 225), (1024, 128)), ((1, 1), (3, 5), (17, 9)), ((1920, 1080), (800, 600), (640, 2048))]):
        for order in itertools.permutations(dims):
            items = [a(w, h, mip, i, sample) for i, (w, h) in enumerate(order)]
            expected = (min(max(1, w // (2 ** mip)) for w, _ in order),
                        min(max(1, h // (2 ** mip)) for _, h in order), 1, sample)
            run(items, expected)
            # Empty color slots cannot reduce the framebuffer to zero or supply
            # an unrelated sample count/view. Poison proves they are ignored.
            sparse = [Attachment() for _ in range(9)]
            sparse[0], sparse[5], sparse[8] = items
            sparse[3] = Attachment(0, 0, 0, 0, 0, 0xffffffff, 0, 0xffffffff, 0, 128)
            run(sparse, expected)
            counts['intersection_orders_mips_samples'] += 2
    # Distinct mip levels are resolved before intersection; no forced resize.
    run([a(1600, 900, 2), a(800, 600, 0)], (400, 225, 1, 1))
    counts['intersection_orders_mips_samples'] += 1
    color, depth = a(800, 450), a(400, 225)
    run([color, depth], (400, 225, 1, 1))
    depth.active = 0
    run([color, depth], (800, 450, 1, 1))
    color.active = 0; depth.active = 1
    run([color, depth], (400, 225, 1, 1))
    counts['single_attachment_clear_scope'] = 3
    for field, value, error in [('width', 0, -1), ('height', 0, -1), ('levels', 0, -1), ('levels', 33, -1), ('layers', 0, -1),
                               ('mip', 10, -2), ('mip', 32, -2), ('mips', 0, -2), ('mips', 11, -2),
                               ('layer', 6, -2), ('layer', 0xffffffff, -2), ('layer_count', 0, -2), ('layer_count', 7, -2),
                               ('layer_count', 2, -3), ('samples', 0, -4), ('samples', 3, -4), ('samples', 16, -4)]:
        item = a(); setattr(item, field, value); run([a(), item], error=error, bad=1)
    run([a(samples=2), a(samples=4)], error=-5, bad=1)
    run([Attachment()], error=-6)
    run([a()], error=-7, limit=(799, 450))
    run([a()], error=-7, limit=(800, 449))
    run([a()] * 10, error=-1)
    run([a()], error=-1, limit=(0, 16384))
    run([a()], error=-1, limit=(16384, 0))
    # Clear participation removes the irrelevant mismatched sample attachment.
    other = a(samples=4); other.active = 0
    run([a(samples=2), other], (800, 450, 1, 2)); counts['single_attachment_clear_scope'] += 1
    report = {'status': 'PASS', 'checks': counts, 'library_sha256': hashlib.sha256(args.library.read_bytes()).hexdigest(),
              'policy': 'minimum selected mip width/height of participating attachments; original viewport unchanged; selected-only clear',
              'mixed_samples': 'explicitly rejected, no implicit resolve or forced downsample', 'GPU_validation': 'NOT_RUN'}
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
