// 直接测试生产的 24 位定点深度 <-> 浮点互转（没有 D24 格式的设备用浮点替身，#250）：全部 2^24 个值往返无损、
// 取最近的 float、严格递增，以及浮点转回定点的边界和最近取整。只做整数和浮点运算，不需要 Vulkan 或 GPU。
// Test the production 24-bit unorm depth <-> float conversions (the float stand-in on devices without D24 formats, #250) directly:
// all 2^24 values round-trip exactly, map to the nearest float and increase strictly, plus the float -> unorm edges and nearest
// rounding. Integer and float work only, no Vulkan or GPU.
#include "../xbg_depth.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned long checks,failures;
#define CHECK(x) do {++checks;if(!(x)){if(++failures<20)printf("FAIL line %d: %s\n",__LINE__,#x);}} while(0)

static float as_float(uint32_t bits){float f;memcpy(&f,&bits,4);return f;}
static uint32_t as_bits(float f){uint32_t b;memcpy(&b,&f,4);return b;}

int main(void){
    const double M=16777215.0;
    float previous=-1.0f;
    for(uint32_t v=0;v<=0xffffffu;v++){
        uint32_t bits=xbg_depth_unorm24_to_float(v);float f=as_float(bits);
        // 往返无损 | exact round trip
        CHECK(xbg_depth_float_to_unorm24(bits)==v);
        // 严格递增 | strictly increasing
        CHECK(f>previous);previous=f;
        // 最近的 float：f*M 在 double 里精确，和两侧相邻的 float 比较到 v 的距离。
        // Nearest float: f*M is exact in double; compare the distance to v with both neighbouring floats.
        double here=fabs((double)f*M-v);
        if(v)CHECK(here<=fabs((double)nextafterf(f,-1.0f)*M-v));
        if(v<0xffffffu)CHECK(here<=fabs((double)nextafterf(f,2.0f)*M-v));
    }
    // 端点与高 8 位忽略 | end points, and the top 8 bits are ignored
    CHECK(xbg_depth_unorm24_to_float(0)==0u);
    CHECK(xbg_depth_unorm24_to_float(0xffffffu)==as_bits(1.0f));
    CHECK(xbg_depth_unorm24_to_float(0xff000005u)==xbg_depth_unorm24_to_float(5));
    // 浮点 -> 定点的边界 | float -> unorm edges
    CHECK(xbg_depth_float_to_unorm24(as_bits(NAN))==0);
    CHECK(xbg_depth_float_to_unorm24(as_bits(-NAN))==0);
    CHECK(xbg_depth_float_to_unorm24(as_bits(-0.0f))==0);
    CHECK(xbg_depth_float_to_unorm24(as_bits(-1.0f))==0);
    CHECK(xbg_depth_float_to_unorm24(as_bits(-INFINITY))==0);
    CHECK(xbg_depth_float_to_unorm24(as_bits(1e-45f))==0);
    CHECK(xbg_depth_float_to_unorm24(as_bits(1.0f))==0xffffffu);
    CHECK(xbg_depth_float_to_unorm24(as_bits(1.5f))==0xffffffu);
    CHECK(xbg_depth_float_to_unorm24(as_bits(INFINITY))==0xffffffu);
    CHECK(xbg_depth_float_to_unorm24(as_bits(nextafterf(1.0f,0.0f)))==0xfffffeu);
    CHECK(xbg_depth_float_to_unorm24(as_bits(0.5f))==8388608u);
    // 任意 [0,1] 浮点（GPU 写的深度）取最近的定点值：每隔 4099 个位模式取一个。
    // Any float in [0,1] (depth the GPU wrote) gives the nearest unorm value: every 4099th bit pattern.
    for(uint32_t b=0;b<=as_bits(1.0f);b+=4099){
        uint32_t k=xbg_depth_float_to_unorm24(b);double x=(double)as_float(b)*M;
        CHECK(k<=0xffffffu);CHECK(fabs(x-k)<=0.5);
    }
    printf("depth24_float_test: %lu checks, %lu failures\n",checks,failures);
    return failures?1:0;
}
