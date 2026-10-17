#pragma once
#include <stdint.h>
class XblScreenTouch {
    int finger=-1;
public:
    using Send=int(*)(uint32_t,float,float);
    void cancelIfActive(Send send){if(finger>=0)cancel(send);}
    void cancel(Send send){finger=-1;send(3,0,0);}
    int event(unsigned phase,int id,float x,float y,uint64_t width,uint64_t height,Send send){
        if(phase==3){cancel(send);return 0;}
        if(phase>3||!width||!height||!(x>=0&&x<=width&&y>=0&&y<=height)){
            if(id==finger)cancel(send);return 40;
        }
        if(phase==0){
            if(finger>=0)return 47;
            const int rc=send(0,x/width,y/height);if(!rc)finger=id;return rc;
        }
        if(id!=finger)return 47;
        const int rc=send(phase,x/width,y/height);
        if(rc)cancel(send);
        if(phase==2)finger=-1;
        return rc;
    }
};
