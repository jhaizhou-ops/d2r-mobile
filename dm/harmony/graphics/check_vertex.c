#include "xbg_vertex.h"
#include <string.h>
#define INS(n,op) (((n)<<16)|(op))
/* Minimal type/interface fragments exercise production reflection. They are
 * intentionally not executable shaders and are not sent to a Vulkan driver. */
int xbg_vertex_selftest(void){
    uint32_t w[]={0x07230203,0x10000,0,100,0,
        INS(4,71),10,30,2, INS(4,71),11,30,5, INS(4,71),12,30,7,
        INS(4,71),20,30,2, // Output at same location must not shadow Input
        INS(3,22),1,32, INS(4,21),2,32,1, INS(4,21),3,32,0,
        INS(4,23),4,1,4, INS(4,23),5,2,3, INS(4,23),6,3,2,
        INS(4,32),7,1,4, INS(4,32),8,1,5, INS(4,32),9,1,6,
        INS(4,59),7,10,1, INS(4,59),8,11,1, INS(4,59),9,12,1,
        INS(4,59),7,20,3};
    const size_t n=sizeof(w)/4;uint32_t loc[]={7,2,5},classes[3]={99,99,99};
    if(xbg_vertex_input_classes(w,n,loc,3,classes) || classes[0]!=2 || classes[1]!=0 || classes[2]!=1)return 1;
    uint32_t duplicate[]={2,2};if(xbg_vertex_input_classes(w,n,duplicate,2,classes)!=-3)return 2;
    uint32_t missing=15;if(xbg_vertex_input_classes(w,n,&missing,1,classes)!=-5)return 3;
    if(xbg_vertex_input_classes(w,n-1,loc,3,classes)!=-2)return 4;
    uint32_t bad[sizeof(w)/4];memcpy(bad,w,sizeof(w));bad[5]=71;
    if(xbg_vertex_input_classes(bad,n,loc,3,classes)!=-2)return 5;
    memcpy(bad,w,sizeof(w));bad[23]=64; // float scalar width (word23)
    if(xbg_vertex_input_classes(bad,n,loc,3,classes)!=-9)return 6;
    uint32_t attrs[16],full[16];for(int i=0;i<16;i++)full[i]=1u|((uint32_t)i<<27);
    if(xbg_vertex_attributes_replace(attrs,full,16) || memcmp(attrs,full,sizeof(full)))return 7;
    if(xbg_vertex_attributes_replace(attrs,full,2) || attrs[0]!=1 || attrs[1]!=full[1])return 8;
    for(int i=2;i<16;i++)if(attrs[i])return 9;
    if(xbg_vertex_attributes_replace(attrs,attrs,1) || attrs[0]!=1 || attrs[1])return 10;
    if(xbg_vertex_attributes_replace(attrs,0,0))return 11;
    for(int i=0;i<16;i++)if(attrs[i])return 12;
    attrs[15]=123;
    if(xbg_vertex_attributes_replace(attrs,0,1)!=-1 || attrs[15]!=123 || xbg_vertex_attributes_replace(attrs,full,17)!=-1)return 13;
    uint32_t locations[16];for(int i=0;i<16;i++)locations[i]=i;
    if(xbg_vertex_zero_binding(full,locations,16)!=-1)return 14;
    full[8]=0;if(xbg_vertex_zero_binding(full,locations,16)!=8)return 15;
    // Nonconsumed streams cannot occupy a Vulkan binding.
    if(xbg_vertex_zero_binding(full,locations+1,2)!=0)return 16;
    full[1]=1u|(31u<<27);if(xbg_vertex_zero_binding(full,locations,16)!=-1)return 17;
    return 0;
}
