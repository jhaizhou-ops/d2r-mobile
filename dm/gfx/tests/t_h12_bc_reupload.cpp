#include "../vulkan/engine/xbg_bc_reupload.h"
#include "../vulkan/engine/xbg_texture.h"
#include <cassert>
#include <cstdio>
#include <random>

static void compare_decode(uint32_t format, uint32_t width, uint32_t height) {
    xbg_bc_format info{};
    assert(!xbg_bc_format_info(format,&info));
    std::mt19937 random(format*97+width+height);
    size_t blocks=((width+3)/4)*((height+3)/4);
    size_t bb=info.bytes_per_block,bpp=info.bytes_per_pixel;
    std::vector<uint8_t> old(blocks*bb), baseline(width*height*bpp);
    for(auto &v:old)v=uint8_t(random());
    auto full=[&](const auto &src,auto &dst){
        return xbg_decode_bc_linear(format,width,height,1,src.data(),src.size(),0,0,dst.data(),dst.size());
    };
    assert(!full(old,baseline));
    auto output=baseline, next=old;
    for(size_t changed=0;changed<blocks;++changed){
        const size_t index=(blocks-1-changed)*bb;
        next[index+bb-1]^=0x7f;
        size_t calls=0;
        auto decode=[&](const uint8_t *src,uint8_t *dst,size_t n){
            ++calls;return xbg_decode_block(format,src,bb,dst,n)==0;
        };
        assert(h12bc::patch_blocks(width,height,bpp,bb,old.data(),next.data(),output.data(),decode));
        assert(calls==1);assert(!full(next,baseline));assert(output==baseline);
        old=next;calls=0;
        assert(h12bc::patch_blocks(width,height,bpp,bb,old.data(),next.data(),output.data(),decode));
        assert(!calls);
    }
}
int main(){
    assert(h12bc::can_skip(true,true,true,11,11));
    assert(!h12bc::can_skip(false,true,true,11,11));
    assert(!h12bc::can_skip(true,false,true,11,11)); // full overwrite after GPU write
    assert(!h12bc::can_skip(true,true,false,11,11));
    assert(!h12bc::can_skip(true,true,true,11,12));
    assert(!h12bc::can_skip(true,true,true,0,0));
    assert(!h12bc::burst_active(1,0));
    assert(h12bc::burst_active(1+h12bc::idle_ns-1,1));
    assert(!h12bc::burst_active(1+h12bc::idle_ns,1));
    h12bc::Histories histories;
    auto admit=[&](uint64_t id,unsigned width=256,size_t source=65536){
        return histories.acquire(id,true,source,0x50,width,256,6,4,8,16);
    };
    assert(!admit(1));assert(!admit(1));auto *img=admit(1);
    assert(img && img->surfaces.size()==24);
    img->surfaces[0].source={42};assert(admit(1)->surfaces[0].source[0]==42);
    assert(admit(1,128)->surfaces[0].source.empty());
    for(unsigned i=2;i<24;i++){assert(!admit(i));assert(!admit(i));assert(admit(i));}
    assert(admit(1)->surfaces[0].source.empty()); // LRU eviction
    assert(!admit(23,256,(2U<<20)+1));
    for(unsigned fmt=0x41;fmt<=0x50;++fmt){
        compare_decode(fmt,1,1);compare_decode(fmt,7,9);compare_decode(fmt,32,16);
    }
    h12bc::Scratch scratch;
    h12bc::UploadBuffer buf(&scratch);buf.resize(16);buf.data()[12]=77;buf.resize(100);assert(buf.data()[12]==77);
    h12bc::UploadBuffer next(&scratch);assert(next.size()==0);
    h12bc::UploadBuffer local(nullptr);local.resize(3);assert(!local.data()[0]);
    std::puts("PASS: GPU overwrite/hash guard, all 16 BC formats, edge/mip extents, no-op and changed blocks, LRU, burst expiry, scratch and baseline");
}
