"""Exercise production NVN sampler conversion and original ABI defaults, no GPU."""
import argparse
import ctypes as C
import hashlib
import itertools
import json
from pathlib import Path


class Nvn(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('minf', 'magf', 's', 't', 'r', 'cmp', 'func')] + [
        (n, C.c_float) for n in ('lo', 'hi', 'bias', 'aniso')] + [('border', C.c_float * 4)]


class Limits(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('aniso', 'mirror', 'integer')] + [
        ('max_aniso', C.c_float), ('max_bias', C.c_float)]


class VkSampler(C.Structure):
    _fields_ = [('sType', C.c_uint32), ('pNext', C.c_void_p), ('flags', C.c_uint32)] + [
        (n, C.c_uint32) for n in ('mag', 'min', 'mip', 'u', 'v', 'w')] + [
        ('bias', C.c_float), ('aniso_on', C.c_uint32), ('aniso', C.c_float),
        ('cmp_on', C.c_uint32), ('cmp', C.c_uint32), ('lo', C.c_float), ('hi', C.c_float),
        ('border', C.c_uint32), ('unnormalized', C.c_uint32)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--library', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    lib = C.CDLL(str(args.library.resolve()))
    lib.xbg_sampler_from_nvn.argtypes = [C.POINTER(Nvn), C.POINTER(Limits), C.POINTER(VkSampler)]
    counts = {'filter_wrap_combinations': 0, 'canonical_borders': 0, 'compare_modes': 0,
              'feature_and_invalid_rejections': 0, 'LOD_and_anisotropy': 0, 'NVN_ABI_defaults_setters': 0}
    def defaults():
        return Nvn(4, 1, 1, 1, 1, 0, 2, -1000, 1000, 0, 1, (0, 0, 0, 0))
    lim = Limits(1, 1, 0, 16, 16)
    def convert(p, expected=0, limits=lim):
        out = VkSampler(); C.memset(C.byref(out), 0xA5, C.sizeof(out))
        before = bytes(out)
        result = lib.xbg_sampler_from_nvn(C.byref(p), C.byref(limits), C.byref(out))
        assert result == expected, (result, expected, tuple(getattr(p, n) for n in ('minf', 'magf', 's', 't', 'r')))
        if expected:
            assert bytes(out) == before, 'Failed conversion changed output'
            counts['feature_and_invalid_rejections'] += 1
        else:
            assert out.sType == 31 and out.pNext is None and out.flags == 0 and out.unnormalized == 0
        return out
    wraps = {1: 0, 3: 4, 5: 3, 6: 1, 7: 2}
    # Independent expected enum table from NVN/Vulkan headers, including all axes.
    filters = [(0, 0), (1, 0), (0, 0), (1, 0), (0, 1), (1, 1)]
    for mi, ma, (s, t, r) in itertools.product(range(6), range(2), itertools.product(wraps, repeat=3)):
        p = defaults(); p.minf, p.magf, p.s, p.t, p.r = mi, ma, s, t, r
        out = convert(p)
        assert (out.min, out.mag, out.mip) == (filters[mi][0], ma, filters[mi][1])
        assert (out.u, out.v, out.w) == (wraps[s], wraps[t], wraps[r])
        assert out.lo == 0 and out.hi == (0.25 if mi < 2 else 1000)
        counts['filter_wrap_combinations'] += 1
    for integer, (color, value) in itertools.product(range(2), [((0, 0, 0, 0), 0), ((0, 0, 0, 1), 2), ((1, 1, 1, 1), 4)]):
        p = defaults(); p.s = 5; p.border[:] = color
        out = convert(p, limits=Limits(1, 1, integer, 16, 16))
        assert out.border == value + integer
        counts['canonical_borders'] += 1
    for mode, func in itertools.product(range(2), range(1, 9)):
        p = defaults(); p.cmp, p.func = mode, func
        out = convert(p); assert (out.cmp_on, out.cmp) == (mode, func - 1)
        counts['compare_modes'] += 1
    for axis in ('s', 't', 'r'):
        for value in (0, 2, 4, 8, 0xffffffff):
            p = defaults(); setattr(p, axis, value); convert(p, -2)
        p = defaults(); setattr(p, axis, 3); convert(p, -3, Limits(1, 0, 0, 16, 16))
    for field, value, error in [('minf', 6, -1), ('magf', 2, -1), ('cmp', 2, -6), ('func', 0, -6), ('func', 9, -6),
                                ('lo', float('nan'), -4), ('hi', float('inf'), -4), ('bias', float('-inf'), -4),
                                ('bias', 17, -4), ('bias', -17, -4), ('aniso', 0, -5), ('aniso', 17, -5),
                                ('aniso', float('nan'), -5)]:
        p = defaults(); setattr(p, field, value); convert(p, error)
    p = defaults(); p.lo, p.hi = 3, 2; convert(p, -4)
    p = defaults(); p.aniso = 2; convert(p, -5, Limits(0, 1, 0, 16, 16))
    for color in [(1, 0, 0, 1), (0.25, 0.25, 0.25, 1), (0, 0, 0, float('nan'))]:
        p = defaults(); p.t = 5; p.border[:] = color; convert(p, -7)
    for lo, hi, bias, aniso in [(2, 6, .5, 16), (-1000, -2, -16, 1), (0, 0, 16, 1), (100, 1000, 0, 4)]:
        p = defaults(); p.lo, p.hi, p.bias, p.aniso = lo, hi, bias, aniso
        out = convert(p)
        assert (out.lo, out.hi, out.bias, out.aniso, out.aniso_on) == (max(0, lo), max(0, hi), bias, aniso, int(aniso > 1))
        counts['LOD_and_anisotropy'] += 1
    # Invoke production original NVN entry points, not a test reimplementation.
    lib.xbg_nvn_get_proc_address.argtypes = [C.c_char_p]; lib.xbg_nvn_get_proc_address.restype = C.c_void_p
    def fn(name, restype, *types):
        return C.CFUNCTYPE(restype, *types)(lib.xbg_nvn_get_proc_address(name.encode()))
    builder = C.create_string_buffer(256)
    fn('nvnSamplerBuilderSetDevice', None, C.c_void_p, C.c_void_p)(builder, 0x12345678)
    fn('nvnSamplerBuilderSetDefaults', None, C.c_void_p)(builder)
    assert C.c_uint64.from_buffer(builder).value == 0x12345678
    mi, ma = C.c_uint32(), C.c_uint32()
    fn('nvnSamplerBuilderGetMinMagFilter', None, C.c_void_p, C.c_void_p, C.c_void_p)(builder, C.byref(mi), C.byref(ma))
    assert (mi.value, ma.value) == (4, 1)
    s, t, r = C.c_uint32(), C.c_uint32(), C.c_uint32()
    getter = fn('nvnSamplerBuilderGetWrapMode', None, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p)
    getter(builder, C.byref(s), C.byref(t), C.byref(r)); assert (s.value, t.value, r.value) == (1, 1, 1)
    counts['NVN_ABI_defaults_setters'] += 3
    for value in range(8):
        fn('nvnSamplerBuilderSetWrapMode', None, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32)(builder, value, value, value)
        getter(builder, C.byref(s), C.byref(t), C.byref(r)); assert (s.value, t.value, r.value) == (value, value, value)
        counts['NVN_ABI_defaults_setters'] += 1
    assert lib.xbg_nvn_mapping_selftest() == 0
    report = {'status': 'PASS', 'checks': counts, 'library_sha256': hashlib.sha256(args.library.read_bytes()).hexdigest(),
              'exact_wraps': [1, 3, 5, 6, 7], 'legacy_wraps_explicitly_rejected': [0, 2, 4],
              'non_mip_maxLod': .25, 'GPU_validation': 'NOT_RUN'}
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
