"""Exercise the compiled production C decoder; independent Pillow DDS oracle for BC1/3.
Creates only bounded evidence/artifacts outside the source tree. No device access.
"""
import argparse
import ctypes as C
import hashlib
import io
import json
from pathlib import Path
import struct
from PIL import Image

class Texture(C.Structure):
    _fields_=[(name,C.c_uint32) for name in ('width','height','levels','source_code','pixels','bytes_per_pixel',
        'block_linear','block_height_log2','nvn_format','header_bytes')]+[('offsets',C.c_uint64*16),('sizes',C.c_uint32*16)]
class Desc(C.Structure):
    _fields_=[(n,C.c_int) for n in ('target','format','flags','samples')]+[(n,C.c_uint32) for n in ('width','height','depth','levels','stride')]+[('pkg_block_h',C.c_int)]
class Nvn(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('format','width','height','levels','block_height_log2')]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library',type=Path,required=True)
    p.add_argument('--assets',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    lib=C.CDLL(str(a.library.resolve()))
    lib.xbg_texture_parse.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(Texture)]
    lib.xbg_texture_mip_bytes.argtypes=[C.POINTER(Texture),C.c_uint32];lib.xbg_texture_mip_bytes.restype=C.c_size_t
    lib.xbg_texture_decode_mip.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(Texture),C.c_uint32,C.c_void_p,C.c_size_t]
    lib.xbg_decode_block.argtypes=[C.c_uint32,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]
    lib.xbg_nvn_texture_decode_mip.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(Nvn),C.c_uint32,C.c_void_p,C.c_size_t]
    lib.xbg_nvn_texture_layout.argtypes=[C.POINTER(Nvn),C.c_size_t,C.POINTER(Texture)]
    lib.nvn_deswizzle.argtypes=[C.POINTER(Desc),C.c_uint,C.c_uint,C.c_void_p,C.c_void_p,C.c_size_t]
    checks=[]
    def check(name,condition):
        checks.append({'name':name,'passed':bool(condition)})
        if not condition:raise AssertionError(name)
    def block(fmt,raw,n=64):
        dst=C.create_string_buffer(n);check(f'block_{fmt:02x}_accepted',lib.xbg_decode_block(fmt,raw,len(raw),dst,n)==0);return dst.raw
    check('NVN_sampler_blend_interleaved_vertices_and_bounds',lib.xbg_nvn_mapping_selftest()==0)
    selector3=struct.pack('<HHI',0,65535,0xffffffff)
    check('BC1_RGBA_transparent_selector',block(0x42,selector3)==bytes(64))
    check('BC1_RGB_opaque_black_selector',block(0x41,selector3)==bytes([0,0,0,255])*16)
    # BC3 RGB is always 4-color even for ascending endpoints; alpha selector7 is 1.
    bc3=bytes([0,255])+((1<<48)-1).to_bytes(6,'little')+selector3
    check('BC3_ascending_endpoints_force_four_color_alpha',block(0x44,bc3)==bytes([170,170,170,255])*16)
    bc3zero=bytes([0,255])+sum(6<<(3*i) for i in range(16)).to_bytes(6,'little')+selector3
    check('BC3_alpha_selector6_zero',all(v==0 for v in block(0x44,bc3zero)[3::4]))
    # Exact endpoint and rational-interpolation expectations, independently written.
    selectors=sum((i%8)<<(3*i) for i in range(16))
    bc4=bytes([255,0])+selectors.to_bytes(6,'little')
    values=struct.unpack('<16H',block(0x49,bc4,32))
    check('BC4_R16_all_selectors',list(values[:8])==[65535,0,56173,46811,37449,28086,18724,9362])
    bc5=bc4+bytes([0,255])+selectors.to_bytes(6,'little')
    values=struct.unpack('<32H',block(0x4b,bc5,64))
    check('BC5_RG16_preserves_channels',list(values[0:16:2])==[65535,0,56173,46811,37449,28086,18724,9362] and list(values[1:16:2])==[0,65535,13107,26214,39321,52428,0,65535])
    check('short_block_rejected',lib.xbg_decode_block(0x44,bc3,15,C.create_string_buffer(64),64)!=0)
    report={'library_sha256':hashlib.sha256(a.library.read_bytes()).hexdigest(),'checks':checks,'assets':[]}
    for record in json.loads((a.assets/'manifest.json').read_text(encoding='utf-8'))['files']:
        path=a.assets/record['filename'];data=path.read_bytes();t=Texture()
        check(path.name+'_parse',lib.xbg_texture_parse(data,len(data),C.byref(t))==0)
        all_mips=[]
        for level in range(t.levels):
            n=lib.xbg_texture_mip_bytes(C.byref(t),level);out=C.create_string_buffer(n)
            check(path.name+f'_mip{level}',lib.xbg_texture_decode_mip(data,len(data),C.byref(t),level,out,n)==0)
            all_mips.append(out.raw)
            if t.block_linear:
                nd=Nvn(t.nvn_format,t.width,t.height,t.levels,t.block_height_log2);raw=data[t.header_bytes:];raw_out=C.create_string_buffer(n)
                check(path.name+f'_raw_NVN_mip{level}',lib.xbg_nvn_texture_decode_mip(raw,len(raw),C.byref(nd),level,raw_out,n)==0 and raw_out.raw==out.raw)
        if t.block_linear:
            check(path.name+'_raw_NVN_short_storage',lib.xbg_nvn_texture_layout(C.byref(nd),len(raw)-1,C.byref(Texture()))!=0)
        check(path.name+'_truncation_rejected',lib.xbg_texture_parse(data,len(data)-1,C.byref(Texture()))!=0)
        check(path.name+'_bad_mip_rejected',lib.xbg_texture_decode_mip(data,len(data),C.byref(t),32,C.create_string_buffer(1),1)!=0)
        changed=Texture.from_buffer_copy(t);changed.width+=1
        check(path.name+'_stale_descriptor_rejected',lib.xbg_texture_decode_mip(data,len(data),C.byref(changed),0,C.create_string_buffer(len(all_mips[0])),len(all_mips[0]))!=0)
        item={**record,'width':t.width,'height':t.height,'levels':t.levels,'pixel_format':t.pixels,
              'decoded_mip_sha256':[hashlib.sha256(m).hexdigest() for m in all_mips]}
        (a.output/(path.stem+'.mip0.bin')).write_bytes(all_mips[0])
        if t.pixels in (1,2):
            rgba=all_mips[0]
            if t.nvn_format in (0x42,0x46,0x44,0x48):
                bpb=8 if t.nvn_format in (0x42,0x46) else 16;pitch=(t.width+3)//4*bpb
                linear=C.create_string_buffer(pitch*((t.height+3)//4));d=Desc(1,t.nvn_format,0,0,t.width,t.height,1,t.levels,0,t.block_height_log2+1)
                check(path.name+'_baseline_deswizzle',lib.nvn_deswizzle(C.byref(d),0,0,data[t.header_bytes:],linear,pitch)==0)
                fourcc=b'DXT1' if bpb==8 else b'DXT5'
                header=[124,0x81007,t.height,t.width,len(linear),0,1]+[0]*11+[32,4,struct.unpack('<I',fourcc)[0],0,0,0,0,0]+[0x1000,0,0,0,0]
                reference=Image.open(io.BytesIO(b'DDS '+struct.pack('<31I',*header)+linear.raw)).convert('RGBA').tobytes()
                differences=[abs(x-y) for x,y in zip(rgba,reference)]
                item['pillow_max_error']=max(differences);item['pillow_equal_bytes']=sum(d==0 for d in differences);item['compared_bytes']=len(rgba)
                check(path.name+'_independent_DDS_decode_RGBA',max(differences)<=1)
                # Alpha equality is assessed separately; no RGB-only acceptance.
                item['alpha_max_error']=max(differences[3::4]);item['alpha_range']=[min(rgba[3::4]),max(rgba[3::4])]
            Image.frombytes('RGBA',(t.width,t.height),rgba).save(a.output/(path.stem+'.png'))
        report['assets'].append(item)
    report['passed']=all(c['passed'] for c in checks);report['checks_count']=len(checks)
    (a.output/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'passed':report['passed'],'checks':len(checks),'assets':len(report['assets'])}))

if __name__=='__main__':main()
