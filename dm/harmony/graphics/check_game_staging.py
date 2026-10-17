"""Run production C BC staging/command-lifetime code on CPU; no GPU claims."""
import argparse,ctypes,hashlib,json,pathlib,struct

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=pathlib.Path,required=True)
    ap.add_argument('--output',type=pathlib.Path,required=True)
    a=ap.parse_args();dll=ctypes.CDLL(str(a.build/'windows-test/xbg_texture_test.dll'))
    fn=dll.xbg_decode_bc_linear
    fn.argtypes=[ctypes.c_uint32]*4+[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_size_t,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t]
    formats=list(range(0x41,0x4a))+[0x4b];positives=negatives=0
    for fmt in formats:
        base=fmt-4 if 0x45<=fmt<=0x48 else fmt
        block_bytes=8 if base in (0x41,0x42,0x49) else 16
        bpp=2 if fmt==0x49 else 4
        for w,h,depth in ((1,1,1),(4,4,1),(5,7,2),(9,2,3)):
            for padded in (False,True):
                nx,ny=(w+3)//4,(h+3)//4
                row=(nx+int(padded))*block_bytes
                image=row*(ny+int(padded))
                source=bytearray([0xcd])*(image*depth)
                expected=bytearray(w*h*depth*bpp)
                for z in range(depth):
                    for by in range(ny):
                        for bx in range(nx):
                            index=(z*ny+by)*nx+bx
                            rv,gv=(index*31+17)%256,(index*47+91)%256
                            alpha=(index%15+1)*17
                            if fmt in (0x49,0x4b):
                                block=bytes([rv,0])+bytes(6)
                                pixel=struct.pack('<H',rv*257)
                                if fmt==0x4b:block+=bytes([gv,0])+bytes(6);pixel+=struct.pack('<H',gv*257)
                            else:
                                rgb,packed=(((255,0,0),0xf800),((0,255,0),0x07e0),((0,0,255),0x001f),((255,255,0),0xffe0))[index%4]
                                colors=struct.pack('<HHI',packed,0,0)
                                if base==0x43:block=bytes([(alpha//17)*17])*8+colors
                                elif base==0x44:block=bytes([rv,0])+bytes(6)+colors;alpha=rv
                                else:block=colors;alpha=255
                                pixel=bytes(rgb+(alpha,))
                            offset=z*image+by*row+bx*block_bytes
                            source[offset:offset+block_bytes]=block
                            for y in range(by*4,min(h,by*4+4)):
                                for x in range(bx*4,min(w,bx*4+4)):
                                    offset=((z*h+y)*w+x)*bpp;expected[offset:offset+bpp]=pixel
                needed=(depth-1)*image+(ny-1)*row+nx*block_bytes
                src=ctypes.create_string_buffer(bytes(source));out=ctypes.create_string_buffer(bytes([0xee])*(len(expected)+32))
                assert fn(fmt,w,h,depth,src,needed,row,image,out,len(expected))==0
                assert out.raw[:len(expected)]==expected,(fmt,w,h,depth,padded)
                assert out.raw[len(expected):len(expected)+32]==bytes([0xee])*32
                positives+=1
                assert fn(fmt,w,h,depth,src,needed-1,row,image,out,len(expected))!=0
                assert fn(fmt,w,h,depth,src,needed,row,image,out,len(expected)-1)!=0
                negatives+=2
    assert dll.xbg_game_abi_selftest()==0
    dll.xbg_nvn_get_proc_address.argtypes=[ctypes.c_char_p];dll.xbg_nvn_get_proc_address.restype=ctypes.c_void_p
    names=(pathlib.Path(__file__).parent/'../compat-r32/gfx/nvn/names.txt').read_text().splitlines()
    assert len(names)==523 and all(dll.xbg_nvn_get_proc_address(n.encode()) for n in names)
    assert not dll.xbg_nvn_get_proc_address(b'not_an_NVN_function')
    dll.xbg_shader_control_key.argtypes=[ctypes.c_void_p,ctypes.c_void_p]
    for i in range(27):
        raw=hashlib.shake_256(str(i).encode()).digest(0x880);src=ctypes.create_string_buffer(raw);out=ctypes.create_string_buffer(17)
        assert dll.xbg_shader_control_key(src,out)==0
        assert out.value.decode()==hashlib.sha256(raw).hexdigest()[:16]
    result={'linear_BC_positive_fixtures':positives,'truncated_source_or_output_rejections':negatives,
        'formats':[hex(f) for f in formats],'dimensions':'1x1x1,4x4x1,5x7x2,9x2x3; tight/padded rows and slices',
        'NVN_names':523,'recording_callback_jump_uniform_snapshot':True,'copy_commands_survives_source_recycle':True,
        'clone_cycle_and_bad_extent_rejected':True,'shader_control_SHA_vectors':27,'GPU_validation':'NOT_RUN'}
    a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
if __name__=='__main__':main()
