#pragma once
inline bool XblSurfaceGeometryChanged(bool identityChanged,unsigned long long oldWidth,unsigned long long oldHeight,
    unsigned long long newWidth,unsigned long long newHeight){
    return identityChanged||oldWidth!=newWidth||oldHeight!=newHeight;
}
struct XblSurfaceExtentPlan { int status; unsigned width,height; };
inline XblSurfaceExtentPlan XblSelectSurfaceExtent(unsigned currentWidth,unsigned currentHeight,
    unsigned minWidth,unsigned minHeight,unsigned maxWidth,unsigned maxHeight,
    unsigned long long nativeWidth,unsigned long long nativeHeight){
    if(!currentWidth||!currentHeight||!maxWidth||!maxHeight||!nativeWidth||!nativeHeight)return {0,0,0};
    if(minWidth>maxWidth||minHeight>maxHeight)return {-1,0,0};
    if(currentWidth==0xffffffffU||currentHeight==0xffffffffU){
        if(currentWidth!=0xffffffffU||currentHeight!=0xffffffffU)return {-1,0,0};
        const unsigned w=nativeWidth<minWidth?minWidth:(nativeWidth>maxWidth?maxWidth:static_cast<unsigned>(nativeWidth));
        const unsigned h=nativeHeight<minHeight?minHeight:(nativeHeight>maxHeight?maxHeight:static_cast<unsigned>(nativeHeight));
        return w&&h?XblSurfaceExtentPlan{1,w,h}:XblSurfaceExtentPlan{0,0,0};
    }
    if(currentWidth<minWidth||currentWidth>maxWidth||currentHeight<minHeight||currentHeight>maxHeight)return {-1,0,0};
    return {1,currentWidth,currentHeight};
}
inline bool XblSourceReadbackAllowed(bool diagnostics,unsigned long long frame,bool rgbaFormat){
    return diagnostics&&(frame==0||frame==119)&&rgbaFormat;
}
struct XblSwapBudget {
    unsigned generations=0;
    // Image byte estimates are lower bounds, not actual driver allocation limits.
    unsigned long long imageByteLowerBound=0;
    bool allows(unsigned width,unsigned height,unsigned count) const {
        if(generations>=8||!width||!height||!count||count>8)return false;
        const unsigned long long pixels=static_cast<unsigned long long>(width)*height;
        const unsigned long long limit=128ULL*1024*1024;
        return pixels<=limit/4/count && imageByteLowerBound<=limit-pixels*4*count;
    }
    bool release(unsigned long long bytes){
        if(!generations||bytes>imageByteLowerBound)return false;
        --generations;imageByteLowerBound-=bytes;return true;
    }
    void record(unsigned width,unsigned height,unsigned count){
        ++generations;imageByteLowerBound+=static_cast<unsigned long long>(width)*height*4*count;
    }
};

// Source blits are in logical upright orientation; no pre-rotation is applied.
// Only advertise a transform actually applied to the swapchain image content.
inline unsigned XblUnrotatedPresentTransform(unsigned supported) {
    return (supported & 1u) ? 1u : 0u; // VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
}

// The acquire fence must have succeeded before acquired() is queried.
// Reacquiring the first successfully presented image proves that presentation
// complete; a submit fence or queue-idle alone does not prove WSI completion.
struct XblPresentCompletion {
    unsigned firstIndex=~0u;
    void reset(){firstIndex=~0u;}
    void presented(unsigned index){if(firstIndex==~0u)firstIndex=index;}
    bool acquired(unsigned index,bool fenceCompleted) const {return fenceCompleted&&firstIndex!=~0u&&firstIndex==index;}
};

struct XblRetiredSwapProof {
    unsigned long long surface=0,completeAfter=0;
    void replacement(unsigned long long nextSurface,unsigned long long nextSwap){
        if(surface==nextSurface)completeAfter=nextSwap;
    }
    bool ready(unsigned long long currentSwap) const {return completeAfter!=0&&completeAfter==currentSwap;}
};
