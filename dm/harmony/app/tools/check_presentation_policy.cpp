#include "../entry/src/main/cpp/presentation_policy.h"
int main(){
    for(unsigned i=0;i<240;++i){
        if(XblSourceReadbackAllowed(false,i,true))return 1;
        if(XblSourceReadbackAllowed(true,i,false))return 2;
        if(XblSourceReadbackAllowed(true,i,true)!=(i==0||i==119))return 3;
    }
#ifdef XBL_NEGATIVE_CONTROL
    // Deliberately demand readback on ordinary startup: this must be rejected.
    if(!XblSourceReadbackAllowed(false,0,true))return 19;
#endif
    XblSwapBudget budget;
    for(unsigned i=0;i<8;++i){if(!budget.allows(960,540,4))return 4;budget.record(960,540,4);}
    if(budget.allows(960,540,4))return 5;
    XblSwapBudget bytes;
    if(bytes.allows(0,900,4)||bytes.allows(1600,0,4)||bytes.allows(1600,900,0)||bytes.allows(1600,900,9))return 6;
    if(bytes.allows(0xffffffffU,0xffffffffU,8))return 7;
    if(!bytes.allows(4096,4096,2))return 8;
    bytes.record(4096,4096,2);
    if(bytes.allows(1,1,1))return 9;
    if(XblSurfaceGeometryChanged(false,1600,900,1600,900))return 10;
    if(!XblSurfaceGeometryChanged(true,1600,900,1600,900))return 11;
    if(!XblSurfaceGeometryChanged(false,1600,900,900,1600))return 12;
    if(!XblSurfaceGeometryChanged(false,1600,900,0,0))return 13;
    if(!XblSurfaceGeometryChanged(false,0,0,1600,900))return 14;
    const auto ready=XblSelectSurfaceExtent(1600,900,1,1,4096,4096,1600,900);
    if(ready.status!=1||ready.width!=1600||ready.height!=900)return 22;
    if(XblSelectSurfaceExtent(0,0,1,1,4096,4096,1600,900).status!=0)return 23;
    if(XblSelectSurfaceExtent(1600,900,1,1,0,0,1600,900).status!=0)return 24;
    if(XblSelectSurfaceExtent(1600,900,1,1,4096,4096,0,0).status!=0)return 25;
    const auto flexible=XblSelectSurfaceExtent(0xffffffffU,0xffffffffU,64,64,4096,4096,900,1600);
    if(flexible.status!=1||flexible.width!=900||flexible.height!=1600)return 26;
    const auto clamped=XblSelectSurfaceExtent(0xffffffffU,0xffffffffU,64,64,4096,4096,1,0xffffffffffffffffULL);
    if(clamped.status!=1||clamped.width!=64||clamped.height!=4096)return 27;
    if(XblSelectSurfaceExtent(0xffffffffU,900,1,1,4096,4096,1600,900).status!=-1)return 28;
    if(XblSelectSurfaceExtent(1600,900,4097,1,4096,4096,1600,900).status!=-1)return 29;
    if(XblSelectSurfaceExtent(5000,900,1,1,4096,4096,1600,900).status!=-1)return 30;
#ifdef XBL_NEGATIVE_GEOMETRY
    if(!XblSurfaceGeometryChanged(false,1600,900,1600,900))return 20;
#endif
#ifdef XBL_NEGATIVE_ZERO
    if(XblSelectSurfaceExtent(0,0,1,1,4096,4096,1600,900).status!=1)return 21;
#endif
    return 0;
}
