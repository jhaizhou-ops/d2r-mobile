#include "xbg_framebuffer.h"
int xbg_framebuffer_plan(const xbg_framebuffer_attachment *a,uint32_t count,
    uint32_t max_width,uint32_t max_height,xbg_framebuffer_extent *out,uint32_t *bad){
    if(bad)*bad=UINT32_MAX;
    if(!a || !out || !count || count>9 || !max_width || !max_height)return -1;
    xbg_framebuffer_extent p={0,0,1,0};
    for(uint32_t i=0;i<count;i++)if(a[i].active){
        const xbg_framebuffer_attachment *v=a+i;if(bad)*bad=i;
        if(!v->width || !v->height || !v->levels || v->levels>32 || !v->layers)return -1;
        if(v->base_level>=v->levels || !v->level_count || v->level_count>v->levels-v->base_level ||
           v->base_layer>=v->layers || !v->layer_count || v->layer_count>v->layers-v->base_layer)return -2;
        if(v->layer_count!=1)return -3;
        if(v->samples!=1 && v->samples!=2 && v->samples!=4 && v->samples!=8)return -4;
        if(p.samples && p.samples!=v->samples)return -5;
        uint32_t w=v->width>>v->base_level,h=v->height>>v->base_level;if(!w)w=1;if(!h)h=1;
        p.width=p.width && p.width<w?p.width:w;p.height=p.height && p.height<h?p.height:h;p.samples=v->samples;
    }
    if(bad)*bad=UINT32_MAX;
    if(!p.width || !p.height)return -6;
    if(p.width>max_width || p.height>max_height)return -7;
    *out=p;return 0;
}
const char *xbg_framebuffer_error(int result){
    switch(result){case 0:return "OK";case -1:return "invalid attachment dimensions/count";
    case -2:return "attachment mip/layer view outside texture";case -3:return "layered rendering requires explicit implementation";
    case -4:return "invalid attachment sample count";case -5:return "mixed attachment sample counts require enabled support";
    case -6:return "no render attachment";case -7:return "framebuffer extent exceeds device limit";default:return "invalid framebuffer plan";}
}
