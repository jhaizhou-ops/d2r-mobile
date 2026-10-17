"""Production DLL BC checks. Independent Pillow BC7 and exact BC6 integer math.

BC6 uses Pillow's independent bit-layout tables, with specification integer
arithmetic; Pillow's preview decoder drops interpolation rounding and signed
delta re-extension, and is therefore not an exact BC6H oracle.
No GPU or device is used. See third_party/bcdec/ for production provenance.
"""
import argparse
import ctypes as C
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import random
import struct
from PIL import Image, __version__ as pillow_version
from check_textures import Texture, Desc, Nvn

class Format(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('pixels','bytes_per_pixel','bytes_per_block')]
class Layout(C.Structure):
    _fields_=[('size',C.c_uint64),('layer_size',C.c_uint64)]+[(n,C.c_uint32) for n in ('align','layers','bpb','tileH','tileD')]

WEIGHTS=(0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64)
BC6_TABLES=json.loads((Path(__file__).parent/'third_party/pillow_reference/bc6_tables.json').read_text(encoding='utf-8'))

def bc6_reference(raw,signed):
    """All-mode reference independent of production bcdec parsing/tables.

    Uses separately licensed Pillow format tables, not its lossy preview math.
    Microsoft BC6H rules require +32 interpolation rounding and sign extension
    AFTER wrapped delta reconstruction. Return exact half words, without floats.
    """
    stream=int.from_bytes(raw,'little');prefix=stream&31
    if prefix&3<2:mode=prefix&3;start=2
    elif prefix&3==2:mode=2+(prefix>>2);start=5
    else:mode=10+(prefix>>2);start=5
    if mode>=14:return struct.pack('<4H',0,0,0,0x3c00)*16
    subsets,delta,pbits,precision,rb,gb,bb=BC6_TABLES['bc6_modes'][mode]
    layout=BC6_TABLES['bc6_bit_packings'][mode];ends=[0]*12
    for bit,where in enumerate(layout):ends[where//16]|=((stream>>(start+bit))&1)<<(where%16)
    at=start+len(layout);partition=(stream>>at)&((1<<pbits)-1);at+=pbits
    def sign_extend(x,bits):
        x&=(1<<bits)-1;return x-(1<<bits) if x&(1<<(bits-1)) else x
    for i in range(3):
        if signed:ends[i]=sign_extend(ends[i],precision)
    for i in range(3,subsets*6):
        if signed or delta:ends[i]=sign_extend(ends[i],(rb,gb,bb)[i%3])
        if delta:
            ends[i]=(ends[i]+ends[i%3])&((1<<precision)-1)
            if signed:ends[i]=sign_extend(ends[i],precision)
    def unquantize(x):
        if not signed:
            if precision>=15:return x
            return 0 if x==0 else 65535 if x==(1<<precision)-1 else ((x<<16)+32768)>>precision
        if precision>=16:return x
        a=abs(x);u=0 if a==0 else 32767 if a>=(1<<(precision-1))-1 else ((a<<15)+16384)>>(precision-1)
        return -u if x<0 else u
    ends=list(map(unquantize,ends));out=[]
    for pixel in range(16):
        subset=((BC6_TABLES['bc7_si2'][partition]>>pixel)&1) if subsets==2 else 0
        bits=3 if subsets==2 else 4
        if pixel==0 or (subsets==2 and pixel==BC6_TABLES['bc7_ai0'][partition]):bits-=1
        index=(stream>>at)&((1<<bits)-1);at+=bits
        weight=(0,9,18,27,37,46,55,64)[index] if subsets==2 else WEIGHTS[index]
        for channel in range(3):
            lo,hi=ends[subset*6+channel],ends[subset*6+channel+3]
            value=(lo*(64-weight)+hi*weight+32)>>6
            magnitude=(abs(value)*31)>>(5 if signed else 6)
            out.append(magnitude|(32768 if signed and value<0 else 0))
        out.append(0x3c00)
    assert at==128
    return struct.pack('<64H',*out)

def bc6_mode11(endpoints,indices,signed):
    """Independent one-region, non-delta encoder and exact expected half words.

    Format/rounding: https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc6h-format
    Both endpoints are explicit 10-bit RGB. First texel has a 3-bit index.
    """
    bits=3;at=5
    for endpoint in endpoints:
        for value in endpoint:bits|=(value&1023)<<at;at+=10
    for i,index in enumerate(indices):
        width=3 if i==0 else 4
        assert index < 1<<width
        bits|=index<<at;at+=width
    assert at==128
    def unquantized(value):
        if not signed:
            return 0 if value==0 else 65535 if value==1023 else value*64+32
        magnitude=abs(value)
        u=0 if magnitude==0 else 32767 if magnitude>=511 else magnitude*64+32
        return -u if value<0 else u
    expected=[]
    for index in indices:
        weight=WEIGHTS[index]
        for c in range(3):
            u=(unquantized(endpoints[0][c])*(64-weight)+unquantized(endpoints[1][c])*weight+32)//64
            word=(abs(u)*31)//(32 if signed else 64)
            if signed and u<0:word|=32768
            expected.append(word)
        expected.append(0x3c00)
    return bits.to_bytes(16,'little'),struct.pack('<64H',*expected)

def signed_channel(e0,e1,indices):
    endpoints=[Fraction(max(-127,x),127) for x in (e0,e1)]
    a,b=endpoints
    palette=endpoints+([(7-i)*a/7+i*b/7 for i in range(1,7)] if a>b else
        [(5-i)*a/5+i*b/5 for i in range(1,5)]+[Fraction(-1),Fraction(1)])
    def rounded(value):
        v=value*32767;mag=abs(v);n=(mag.numerator*2+mag.denominator)//(2*mag.denominator)
        return -n if v<0 else n
    values=[rounded(palette[i]) for i in indices]
    raw=bytes((e0&255,e1&255))+sum(v<<(3*i) for i,v in enumerate(indices)).to_bytes(6,'little')
    return raw,values

def fixture(fmt,seed):
    rng=random.Random(seed);base=fmt-4 if 0x45<=fmt<=0x48 else fmt
    if fmt in (0x4f,0x50):
        if fmt==0x4f:ep=((-330+seed%79,0,260),(470,-90-seed%61,-511))
        else:ep=((700+seed%199,0,18),(1023,510,100+seed%200))
        return bc6_mode11(ep,[seed%8]+[(i+seed)%16 for i in range(1,16)],fmt==0x4f)
    if fmt in (0x4d,0x4e):
        mode=seed%8;raw=bytearray(rng.randbytes(16));raw[0]=(raw[0]&~((1<<(mode+1))-1))|(1<<mode)
        raw=bytes(raw);return raw,Image.frombytes('RGBA',(4,4),raw,'bcn',(7,'BC7')).tobytes()
    if fmt in (0x4a,0x4c):
        indices=[(i+seed)%8 for i in range(16)]
        a,av=signed_channel((-128,-127,-64,0,63,127)[seed%6],(127,0,-127,63,-64,-128)[seed%6],indices)
        if fmt==0x4a:return a,struct.pack('<16h',*av)
        b,bv=signed_channel(127,-127,list(reversed(indices)))
        return a+b,struct.pack('<32h',*[v for pair in zip(av,bv) for v in pair])
    if fmt in (0x49,0x4b):
        r,g=(seed*31+17)%256,(seed*47+91)%256
        raw=bytes([r,0])+bytes(6);pixel=struct.pack('<H',r*257)
        if fmt==0x4b:raw+=bytes([g,0])+bytes(6);pixel+=struct.pack('<H',g*257)
        return raw,pixel*16
    rgb,packed=(((255,0,0),0xf800),((0,255,0),0x07e0),((0,0,255),0x001f),((255,255,0),0xffe0))[seed%4]
    raw=struct.pack('<HHI',packed,0,0);alpha=255
    if base==0x43:alpha=(seed%16)*17;raw=bytes([alpha])*8+raw
    if base==0x44:alpha=(seed*31+17)%256;raw=bytes([alpha,0])+bytes(6)+raw
    return raw,bytes(rgb+(alpha,))*16

def fill_linear(fmt,w,h,depth,row=0,image=0,seed=0):
    bb=8 if fmt in (0x41,0x42,0x45,0x46,0x49,0x4a) else 16
    bpp=8 if fmt>=0x4f else 2 if fmt in (0x49,0x4a) else 4
    nx,ny=(w+3)//4,(h+3)//4;row=row or nx*bb;image=image or row*ny
    raw=bytearray([0xda])*(image*depth);pixels=bytearray(w*h*depth*bpp)
    for z in range(depth):
        for by in range(ny):
            for bx in range(nx):
                block,decoded=fixture(fmt,seed+(z*ny+by)*nx+bx)
                offset=z*image+by*row+bx*bb;raw[offset:offset+bb]=block
                for y in range(min(4,h-by*4)):
                    for x in range(min(4,w-bx*4)):
                        dst=((z*h+by*4+y)*w+bx*4+x)*bpp;src=(y*4+x)*bpp
                        pixels[dst:dst+bpp]=decoded[src:src+bpp]
    return raw,pixels

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();path=args.build/'windows-test/xbg_texture_test.dll';dll=C.CDLL(str(path.resolve()))
    dll.xbg_decode_block.argtypes=[C.c_uint32,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]
    dll.xbg_bc_format_info.argtypes=[C.c_uint32,C.POINTER(Format)]
    dll.xbg_bc_view_compatible.argtypes=[C.c_uint32,C.c_uint32]
    dll.xbg_decode_bc_linear.argtypes=[C.c_uint32]*4+[C.c_void_p,C.c_size_t,C.c_size_t,C.c_size_t,C.c_void_p,C.c_size_t]
    dll.nvn_tex_layout.argtypes=[C.POINTER(Desc),C.POINTER(Layout)]
    dll.nvn_deswizzle_geom.argtypes=[C.POINTER(Desc),C.c_uint,C.c_uint]+[C.POINTER(C.c_uint32)]*3+[C.POINTER(C.c_uint64)]+[C.POINTER(C.c_uint32)]*3
    dll.nvn_deswizzle.argtypes=[C.POINTER(Desc),C.c_uint,C.c_uint,C.c_void_p,C.c_void_p,C.c_size_t]
    dll.xbg_nvn_texture_layout.argtypes=[C.POINTER(Nvn),C.c_size_t,C.POINTER(Texture)]
    dll.xbg_nvn_texture_decode_mip.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(Nvn),C.c_uint32,C.c_void_p,C.c_size_t]
    count={};hdr=[];all_words=hashlib.sha256()
    def record(name):count[name]=count.get(name,0)+1
    def decode(fmt,raw):
        info=Format();assert dll.xbg_bc_format_info(fmt,C.byref(info))==0
        n=info.bytes_per_pixel*16
        # Deliberately misalign both pointers; guards detect output overrun.
        src=C.create_string_buffer(b'!'+raw);out=C.create_string_buffer(b'!'+bytes([0xe3])*(n+17))
        assert dll.xbg_decode_block(fmt,C.byref(src,1),len(raw),C.byref(out,1),n)==0
        assert out.raw[0]==33 and out.raw[n+1:n+17]==bytes([0xe3])*16
        result=out.raw[1:n+1];all_words.update(result);return result
    for fmt in range(0x41,0x51):
        info=Format();assert dll.xbg_bc_format_info(fmt,C.byref(info))==0
        expected_pixels=2 if fmt in (0x45,0x46,0x47,0x48,0x4e) else {0x49:3,0x4a:5,0x4b:4,0x4c:6,0x4f:7,0x50:7}.get(fmt,1)
        assert info.pixels==expected_pixels
        for view in range(0x41,0x51):
            def linear(f):return f-4 if 0x45<=f<=0x48 else 0x4d if f==0x4e else f
            assert bool(dll.xbg_bc_view_compatible(fmt,view))==(linear(fmt)==linear(view));record('view_pairs')
        for i in range(16):
            raw,expected=fixture(fmt,i);assert decode(fmt,raw)==expected,(hex(fmt),i);record('all_format_blocks')
        raw,_=fixture(fmt,0);out=C.create_string_buffer(128)
        assert dll.xbg_decode_block(fmt,raw,len(raw)-1,out,128)!=0
        assert dll.xbg_decode_block(fmt,raw,len(raw),out,info.bytes_per_pixel*16-1)!=0;record('short_block_pairs')
    # All BC7 modes, random endpoints/partitions/indices/rotation/P-bits. Both
    # sRGB and linear preserve the encoded bytes. Pillow is independent C code.
    rng=random.Random(26001010)
    for mode in range(8):
        for i in range(128):
            raw=bytearray(rng.randbytes(16));raw[0]=(raw[0]&~((1<<(mode+1))-1))|(1<<mode);raw=bytes(raw)
            expected=Image.frombytes('RGBA',(4,4),raw,'bcn',(7,'BC7')).tobytes()
            for fmt in (0x4d,0x4e):assert decode(fmt,raw)==expected,(mode,i);record('BC7_Pillow_blocks')
    # All14 BC6H modes plus reserved encodings; all bits including signed HDR.
    prefixes=(0,1,2,6,10,14,18,22,26,30,3,7,11,15,19,23,27,31)
    for fmt in (0x4f,0x50):
        for prefix in prefixes:
            for i in range(64):
                raw=bytearray(rng.randbytes(16));mask=3 if prefix<=1 else 31;raw[0]=(raw[0]&~mask)|prefix;raw=bytes(raw)
                actual=decode(fmt,raw);expected=bc6_reference(raw,fmt==0x4f)
                assert actual==expected,(hex(fmt),prefix,i,actual.hex(),expected.hex());record('BC6H_all_mode_exact_half_blocks')
                values=struct.unpack('<64e',actual)
                assert values[3::4]==(1.0,)*16
        edges=(-512,-511,-330,-1,0,1,255,510,511) if fmt==0x4f else (0,1,2,18,510,700,1000,1022,1023)
        for a in edges:
            for b in edges:
                endpoints=((a,b,edges[2]),(b,a,edges[-2]));indices=[0]+list(range(1,16))
                raw,expected=bc6_mode11(endpoints,indices,fmt==0x4f)
                result=decode(fmt,raw);assert result==expected,(fmt,a,b);hdr.extend(struct.unpack('<64e',result));record('BC6H_exact_HDR_signed_blocks')
    assert max(hdr)==65504 and min(hdr)<=-65504 and any(0<v<2**-14 for v in hdr)
    # Both signed endpoints including -128 saturation; all selector values and
    # ordering modes. Fraction arithmetic is independent of decoder float math.
    for e0 in (-128,-127,-126,-64,-1,0,1,64,126,127):
        for e1 in (-128,-127,-126,-64,-1,0,1,64,126,127):
            a,av=signed_channel(e0,e1,list(range(8))*2)
            b,bv=signed_channel(e1,e0,list(reversed(range(8)))*2)
            assert decode(0x4a,a)==struct.pack('<16h',*av)
            assert decode(0x4c,a+b)==struct.pack('<32h',*[v for pair in zip(av,bv) for v in pair]);record('SNORM_endpoint_pairs')
    for fmt in range(0x41,0x51):
        info=Format();assert dll.xbg_bc_format_info(fmt,C.byref(info))==0
        for w,h,z in ((1,1,1),(4,4,1),(5,7,2),(9,2,3),(17,11,4)):
            for pad in (0,1):
                nx,ny=(w+3)//4,(h+3)//4;row=(nx+pad)*info.bytes_per_block;image=row*(ny+pad)
                source,expected=fill_linear(fmt,w,h,z,row,image,seed=37)
                needed=(z-1)*image+(ny-1)*row+nx*info.bytes_per_block
                src=C.create_string_buffer(bytes(source));out=C.create_string_buffer(bytes([0xe3])*(len(expected)+16))
                assert dll.xbg_decode_bc_linear(fmt,w,h,z,src,needed,row,image,out,len(expected))==0
                assert out.raw[:len(expected)]==expected and out.raw[len(expected):len(expected)+16]==bytes([0xe3])*16
                assert dll.xbg_decode_bc_linear(fmt,w,h,z,src,needed-1,row,image,out,len(expected))!=0
                assert dll.xbg_decode_bc_linear(fmt,w,h,z,src,needed,row,image,out,len(expected)-1)!=0;record('linear_mip_edge_slice_fixtures')
        # Independent byte placement into Tegra GOBs; never call nvn_swizzle.
        for target,w,h,z,levels in ((1,19,13,1,5),(4,9,7,3,4),(8,9,9,1,4),(2,9,7,3,4)):
            d=Desc(target,fmt,0,1,w,h,z,levels,0,0);layout=Layout();dll.nvn_tex_layout(C.byref(d),C.byref(layout))
            storage=bytearray(layout.size);subresources=[]
            for layer in range(layout.layers):
                for level in range(levels):
                    row,rows,slices,base,th,td,bb=(C.c_uint32(),C.c_uint32(),C.c_uint32(),C.c_uint64(),C.c_uint32(),C.c_uint32(),C.c_uint32())
                    assert dll.nvn_deswizzle_geom(C.byref(d),level,layer,C.byref(row),C.byref(rows),C.byref(slices),C.byref(base),C.byref(th),C.byref(td),C.byref(bb))==0
                    mw,mh=max(1,w>>level),max(1,h>>level)
                    source,expected=fill_linear(fmt,mw,mh,slices.value,seed=layer*109+level*17)
                    assert len(source)==row.value*rows.value*slices.value
                    bh,bd=1<<th.value,1<<td.value;gobs=(row.value+63)//64;block_rows=(rows.value+8*bh-1)//(8*bh)
                    for sz in range(slices.value):
                        for y in range(rows.value):
                            for x in range(row.value):
                                address=base.value+(((sz//bd)*block_rows+y//(8*bh))*gobs+x//64)*512*bh*bd
                                address+=((sz%bd)*bh+(y//8)%bh)*512
                                address+=((x>>5)&1)*256+((y>>1)&3)*64+((x>>4)&1)*32+(y&1)*16+(x&15)
                                assert address<len(storage);storage[address]=source[(sz*rows.value+y)*row.value+x]
                    subresources.append((layer,level,mw,mh,slices.value,row.value,source,expected))
            src=C.create_string_buffer(bytes(storage))
            for layer,level,mw,mh,depth,row,source,expected in subresources:
                blocks=C.create_string_buffer(len(source));out=C.create_string_buffer(len(expected))
                assert dll.nvn_deswizzle(C.byref(d),level,layer,src,blocks,row)==0 and blocks.raw==source
                assert dll.xbg_decode_bc_linear(fmt,mw,mh,depth,blocks,len(blocks),0,0,out,len(expected))==0
                assert out.raw==expected;record('block_linear_layer_volume_mips')
                if target==1:
                    nd=Nvn(fmt,w,h,levels,0xffffffff);tex=Texture()
                    assert dll.xbg_nvn_texture_layout(C.byref(nd),len(storage),C.byref(tex))==0
                    assert tex.pixels==info.pixels and tex.bytes_per_pixel==info.bytes_per_pixel
                    assert dll.xbg_nvn_texture_decode_mip(src,len(storage),C.byref(nd),level,out,len(expected))==0
                    assert out.raw==expected;record('raw_NVN_2D_mips')
    result={'status':'PASS','checks':count,'formats':[hex(f) for f in range(0x41,0x51)],
        'HDR_min':min(hdr),'HDR_max':max(hdr),'BC6H_output':'binary16 RGBA; no clamp; alpha=1',
        'SNORM_output':'R16/RG16_SNORM; nearest quantization, max 0.5/32767',
        'decoded_digest':all_words.hexdigest(),'library_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
        'Pillow_version':pillow_version,'BC6H_reference':'Pillow independent bit-layout tables + spec integer operations; full half words, all14 modes and4 reserved',
        'BC6H_Pillow_preview_rejected':'Missing signed delta re-extension and interpolation rounding in 12.3.0; not used as oracle',
        'GPU_validation':'NOT_RUN'}
    args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))

if __name__=='__main__':main()
