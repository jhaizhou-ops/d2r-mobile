"""Exact production NVN depth/stencil packing tests; no GPU claims."""
import argparse,ctypes as C,hashlib,json,struct
from pathlib import Path
from check_textures import Desc
from check_bc_complete import Layout

class Format(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('packed_bytes','depth_bytes','stencil_bytes')]

def fixture(fmt,w,h,z,row=0,image=0,seed=0):
    bpp={0x31:1,0x32:2,0x33:4,0x34:4,0x35:4,0x36:8}[fmt]
    row=row or w*bpp;image=image or row*h
    raw=bytearray([0xcd])*(image*z);depth=bytearray();stencil=bytearray()
    for iz in range(z):
        for y in range(h):
            for x in range(w):
                i=((iz*h+y)*w+x)+seed;at=iz*image+y*row+x*bpp
                sv=(i*73+0xa5)&255
                if fmt==0x31:pixel=bytes([sv]);stencil+=pixel
                elif fmt==0x32:pixel=struct.pack('<H',(i*997+12345)&65535);depth+=pixel
                elif fmt in (0x33,0x35):
                    dv=(0,1,0x010203,0xffffff,0x800000,0x123456,0xabcdef)[i%7]
                    pixel=struct.pack('<I',(dv<<8)|sv);depth+=struct.pack('<I',dv)
                    if fmt==0x35:stencil+=bytes([sv])
                else:
                    bits=(0,0x3f800000,0x3f000000,0x00000001,0x00800000,0x3f7fffff)[i%6]
                    pixel=struct.pack('<I',bits);depth+=pixel
                    if fmt==0x36:pixel+=bytes([sv,0x9a,0xbc,0xde]);stencil+=bytes([sv])
                raw[at:at+bpp]=pixel
    return raw,depth,stencil

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--build',required=True,type=Path);ap.add_argument('--output',required=True,type=Path);a=ap.parse_args()
    path=a.build/'windows-test/xbg_texture_test.dll';lib=C.CDLL(str(path.resolve()))
    args=[C.c_uint32]*4+[C.c_void_p,C.c_size_t,C.c_size_t,C.c_size_t,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]
    lib.xbg_depth_extract.argtypes=lib.xbg_depth_merge.argtypes=args
    lib.xbg_depth_format_info.argtypes=[C.c_uint32,C.POINTER(Format)]
    lib.nvn_tex_layout.argtypes=[C.POINTER(Desc),C.POINTER(Layout)]
    lib.nvn_deswizzle_geom.argtypes=[C.POINTER(Desc),C.c_uint,C.c_uint]+[C.POINTER(C.c_uint32)]*3+[C.POINTER(C.c_uint64)]+[C.POINTER(C.c_uint32)]*3
    lib.nvn_deswizzle.argtypes=[C.POINTER(Desc),C.c_uint,C.c_uint,C.c_void_p,C.c_void_p,C.c_size_t]
    count={'linear_extract_merge':0,'rejections':0,'block_linear_mips_layers_volumes':0};digest=hashlib.sha256()
    for fmt in range(0x31,0x37):
        info=Format();assert lib.xbg_depth_format_info(fmt,C.byref(info))==0
        for w,h,z in ((1,1,1),(5,7,3),(17,9,6)):
            for pad in (0,1):
                row=(w+3*pad)*info.packed_bytes;image=row*(h+pad)
                raw,depth,stencil=fixture(fmt,w,h,z,row,image)
                needed=(z-1)*image+(h-1)*row+w*info.packed_bytes
                source=C.create_string_buffer(b'!'+raw);dp=C.create_string_buffer(b'!'+bytes([0xe3])*(len(depth)+16));sp=C.create_string_buffer(b'!'+bytes([0xe3])*(len(stencil)+16))
                assert lib.xbg_depth_extract(fmt,w,h,z,C.byref(source,1),needed,row,image,C.byref(dp,1),len(depth),C.byref(sp,1),len(stencil))==0
                assert dp.raw[1:len(depth)+1]==depth and sp.raw[1:len(stencil)+1]==stencil
                assert dp.raw[0]==sp.raw[0]==33 and dp.raw[len(depth)+1:len(depth)+17]==sp.raw[len(stencil)+1:len(stencil)+17]==bytes([0xe3])*16
                for pn,dn,sn in [(needed-1,len(depth),len(stencil))]+([(needed,len(depth)-1,len(stencil))] if depth else [])+([(needed,len(depth),len(stencil)-1)] if stencil else []):
                    assert lib.xbg_depth_extract(fmt,w,h,z,C.byref(source,1),pn,row,image,C.byref(dp,1),dn,C.byref(sp,1),sn)!=0;count['rejections']+=1
                output=C.create_string_buffer(bytes([0x7e])*(needed+16))
                assert lib.xbg_depth_merge(fmt,w,h,z,output,needed,row,image,C.byref(dp,1),len(depth),C.byref(sp,1),len(stencil))==0
                expected=bytearray([0x7e])*(needed+16)
                used=(0,) if fmt==0x31 else (0,1) if fmt==0x32 else (1,2,3) if fmt==0x33 else (0,1,2,3,4) if fmt==0x36 else (0,1,2,3)
                for iz in range(z):
                    for y in range(h):
                        for x in range(w):
                            at=iz*image+y*row+x*info.packed_bytes
                            for byte in used:expected[at+byte]=raw[at+byte]
                assert output.raw[:needed+16]==expected,(fmt,w,h,z,pad)
                assert source.raw[1:len(raw)+1]==raw;digest.update(depth);digest.update(stencil);count['linear_extract_merge']+=1
        # Place bytes into GOB addresses independently; pass through the same
        # production deswizzle then aspect extraction sequence as get_image.
        for target,w,h,z,levels in ((1,19,13,1,5),(4,9,7,3,4),(2,9,7,3,4)):
            d=Desc(target,fmt,0,1,w,h,z,levels,0,0);layout=Layout();lib.nvn_tex_layout(C.byref(d),C.byref(layout));storage=bytearray(layout.size);parts=[]
            for layer in range(layout.layers):
                for level in range(levels):
                    row,rows,slices,base,th,td,bb=(C.c_uint32(),C.c_uint32(),C.c_uint32(),C.c_uint64(),C.c_uint32(),C.c_uint32(),C.c_uint32())
                    assert lib.nvn_deswizzle_geom(C.byref(d),level,layer,C.byref(row),C.byref(rows),C.byref(slices),C.byref(base),C.byref(th),C.byref(td),C.byref(bb))==0
                    mw,mh=max(1,w>>level),max(1,h>>level);raw,depth,stencil=fixture(fmt,mw,mh,slices.value,seed=layer*109+level*17)
                    bh,bd=1<<th.value,1<<td.value;gobs=(row.value+63)//64;block_rows=(rows.value+8*bh-1)//(8*bh)
                    for iz in range(slices.value):
                        for y in range(rows.value):
                            for x in range(row.value):
                                at=base.value+(((iz//bd)*block_rows+y//(8*bh))*gobs+x//64)*512*bh*bd
                                at+=((iz%bd)*bh+(y//8)%bh)*512+((x>>5)&1)*256+((y>>1)&3)*64+((x>>4)&1)*32+(y&1)*16+(x&15)
                                assert at<len(storage);storage[at]=raw[(iz*rows.value+y)*row.value+x]
                    parts.append((layer,level,mw,mh,slices.value,row.value,raw,depth,stencil))
            source=C.create_string_buffer(bytes(storage))
            for layer,level,mw,mh,z,row,raw,depth,stencil in parts:
                packed=C.create_string_buffer(len(raw));dp=C.create_string_buffer(len(depth));sp=C.create_string_buffer(len(stencil))
                assert lib.nvn_deswizzle(C.byref(d),level,layer,source,packed,row)==0 and packed.raw==raw
                assert lib.xbg_depth_extract(fmt,mw,mh,z,packed,len(raw),0,0,dp,len(depth),sp,len(stencil))==0
                assert dp.raw==depth and sp.raw==stencil;count['block_linear_mips_layers_volumes']+=1
        buf=C.create_string_buffer(8)
        for dims in ((0,1,1),(1,0,1),(1,1,0),(0xffffffff,0xffffffff,0xffffffff)):
            assert lib.xbg_depth_extract(fmt,*dims,buf,8,0,0,buf,8,buf,8)!=0;count['rejections']+=1
    assert lib.xbg_depth_format_info(0x30,C.byref(Format()))!=0
    result={'status':'PASS','formats':['S8','D16','D24','D32F','D24S8','D32FS8'],'checks':count,
        'layout':'NVN D24 depth bits8..31; Vulkan copy plane bits0..23; S8 separate. D32F bits unchanged.',
        'checks_include':'distinct nonzero depth/stencil; min/max/subnormal floats; misalignment/guards; row/slice padding; short input/output; overflow; packed padding preserved',
        'decoded_sha256':digest.hexdigest(),'library_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'GPU_validation':'NOT_RUN'}
    a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
if __name__=='__main__':main()
