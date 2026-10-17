#include "xbg_vulkan.h"
/* CPU-only conversion checks, linked only into the test DLL. */
extern "C" int xbg_nvn_mapping_selftest(void) {
    xbg_draw2d d{};
    if(xbg_2d_sampler_from_nvn(&d,4,1,1,6,2,6,0.25f) || d.min_filter!=VK_FILTER_NEAREST ||
       d.mag_filter!=VK_FILTER_LINEAR || d.mipmap_mode!=VK_SAMPLER_MIPMAP_MODE_LINEAR || d.max_lod!=6 ||
       d.address_u!=VK_SAMPLER_ADDRESS_MODE_REPEAT || d.address_v!=VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT)return 1;
    if(xbg_2d_sampler_from_nvn(&d,1,0,7,7,2,6,0) || d.min_lod!=0 || d.max_lod!=0.25f)return 2;
    if(!xbg_2d_sampler_from_nvn(&d,1,0,0,7,0,0,0))return 3; /* GL_CLAMP must not silently become clamp-to-edge. */
    if(xbg_2d_blend_from_nvn(&d,1,5,6,2,6,1,1,15) || d.blend.srcColorBlendFactor!=VK_BLEND_FACTOR_SRC_ALPHA ||
        d.blend.dstColorBlendFactor!=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA || d.blend.srcAlphaBlendFactor!=VK_BLEND_FACTOR_ONE ||
        d.blend.dstAlphaBlendFactor!=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)return 4;
    if(!xbg_2d_blend_from_nvn(&d,1,0x10,2,2,1,1,1,15))return 5; /* no second color output */
    struct Vertex {float pos[2],uv[2];uint8_t color[4];} raw[]={{{0,0},{0,0},{0,0,0,0}},{{17,29},{0.25f,0.75f},{255,128,0,64}}};
    xbg_nvn_attribute a[]={{raw,sizeof(raw),0x16,0,sizeof(Vertex)},{raw,sizeof(raw),0x16,8,sizeof(Vertex)},{raw,sizeof(raw),0x25,16,sizeof(Vertex)}};
    xbg_vertex2d v{};
    if(xbg_2d_vertices_from_nvn(a,1,1,&v,sizeof(v)) || v.position[0]!=17 || v.position[1]!=29 || v.uv[0]!=0.25f ||
        v.uv[1]!=0.75f || v.color[0]!=1 || v.color[1]!=128/255.0f || v.color[2]!=0 || v.color[3]!=64/255.0f)return 6;
    if(!xbg_2d_vertices_from_nvn(a,2,1,&v,sizeof(v)))return 7;
    if(!xbg_2d_vertices_from_nvn(a,UINT32_MAX,2,&v,sizeof(v)))return 8;
    return 0;
}
