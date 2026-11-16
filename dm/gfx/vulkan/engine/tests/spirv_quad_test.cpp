// 真C++改写器回归与仓外XGS审计入口：合成SPIR-V，不链接Vulkan、不启动游戏。
// Real C++ rewriter regression and external XGS audit entry: synthetic SPIR-V, no Vulkan linkage or game launch.
#include "../xbg_spirv_quad.h"
#include <algorithm>
#include <array>
#include <cassert>
#ifdef NDEBUG
#error "Quad regression tests require active assertions; compile with -UNDEBUG."
#endif
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace fs=std::filesystem;
using Words=std::vector<uint32_t>;
using Quad=std::array<float,4>;
enum class Mode { numeric,source_bits,source_bits_copy,result_bits,result_bits_copy,integer_use,memory_use,branch,scope,xor4,index,existing_coord,source_bits_use,source_integer_use,source_memory_use };
static void add(Words &v,uint32_t op,std::initializer_list<uint32_t> args) {
    v.push_back((uint32_t(args.size()+1)<<16)|op);v.insert(v.end(),args.begin(),args.end());
}
static Words fixture(unsigned mask,Mode mode=Mode::numeric) {
    Words w={0x07230203,0x00010300,0,400,0};
    add(w,17,{1});add(w,17,{61});add(w,17,{65});add(w,14,{0,1});
    if(mode==Mode::existing_coord)add(w,15,{4,100,0x6e69616d,0,10,11,12,19});
    else if(mode==Mode::source_bits || mode==Mode::source_bits_copy)add(w,15,{4,100,0x6e69616d,0,10,11,12,19});
    else add(w,15,{4,100,0x6e69616d,0,10,11,12});
    add(w,16,{100,7});add(w,71,{10,11,41});add(w,71,{10,14});add(w,71,{11,30,0});add(w,71,{12,30,0});
    if(mode==Mode::existing_coord)add(w,71,{19,11,15});
    if(mode==Mode::source_bits || mode==Mode::source_bits_copy) {add(w,71,{19,30,1});add(w,71,{19,14});}
    add(w,19,{1});add(w,21,{2,32,0});add(w,22,{3,32});add(w,23,{4,3,4});add(w,20,{5});
    add(w,32,{6,1,2});add(w,32,{7,1,3});add(w,32,{8,3,3});add(w,33,{9,1});
    if(mode==Mode::existing_coord)add(w,32,{16,1,4});
    if((mode==Mode::memory_use || mode==Mode::source_memory_use))add(w,32,{16,7,3});
    add(w,59,{6,10,1});add(w,59,{7,11,1});add(w,59,{8,12,3});
    if(mode==Mode::existing_coord)add(w,59,{16,19,1});
    if(mode==Mode::source_bits || mode==Mode::source_bits_copy)add(w,59,{6,19,1});
    add(w,43,{2,13,mode==Mode::scope?2u:3u});add(w,43,{2,14,mode==Mode::xor4?4u:mask});add(w,41,{5,20});
    add(w,54,{1,100,0,9});add(w,248,{101});
    if((mode==Mode::memory_use || mode==Mode::source_memory_use))add(w,59,{16,19,7});
    add(w,61,{2,102,10});add(w,198,{2,103,102,14});
    if(mode==Mode::source_bits || mode==Mode::source_bits_copy) {
        add(w,61,{2,116,19});add(w,124,{3,mode==Mode::source_bits?104u:117u,116});
        if(mode==Mode::source_bits_copy)add(w,83,{3,104,117});
    } else add(w,61,{3,104,11});
    if(mode==Mode::branch) {add(w,247,{111,0});add(w,250,{20,112,113});add(w,248,{112});}
    add(w,345,{3,105,13,104,mode==Mode::index?102u:103u});
    add(w,62,{12,105});
    if(mode==Mode::source_bits_use)add(w,124,{2,118,104});
    if(mode==Mode::source_integer_use)add(w,109,{2,118,104});
    if(mode==Mode::result_bits)add(w,124,{2,118,105});
    if(mode==Mode::result_bits_copy) {add(w,83,{3,117,105});add(w,124,{2,118,117});}
    if(mode==Mode::integer_use)add(w,109,{2,118,105});
    if((mode==Mode::memory_use || mode==Mode::source_memory_use))add(w,62,{19,mode==Mode::source_memory_use?104u:105u});
    if(mode==Mode::branch) {add(w,249,{111});add(w,248,{113});add(w,249,{111});add(w,248,{111});}
    add(w,253,{});add(w,56,{});return w;
}

// 在四个活跃lane上执行真实输出指令，检查对角项和非有限回退，不模拟GPU辅助调用。
// Execute the actual emitted instructions over four active lanes to check diagonals and nonfinite fallback, without pretending to simulate GPU helpers.
static Quad evaluate(const Words &w,const Quad &input) {
    std::map<uint32_t,Quad> f;
    std::map<uint32_t,std::array<uint32_t,4>> u;
    std::map<uint32_t,std::array<bool,4>> b;
    std::map<uint32_t,std::array<Quad,4>> vectors;
    Quad result={};
    for(size_t at=5;at<w.size();) {
        auto *p=w.data()+at;unsigned wc=p[0]>>16,op=p[0]&65535;
        if(op==43 && p[1]==2)u[p[2]].fill(p[3]);
        if(op==61) {
            if(p[1]==2)u[p[2]]={0,1,2,3};
            else if(p[1]==3)f[p[2]]=input;
            else for(unsigned lane=0;lane<4;++lane)vectors[p[2]][lane]={float(lane&1)+.5f,float(lane>>1)+.5f,0,1};
        }
        if(op==198 || op==199)for(unsigned lane=0;lane<4;++lane)u[p[2]][lane]=op==198?(u[p[3]][lane]^u[p[4]][lane]):(u[p[3]][lane]&u[p[4]][lane]);
        if(op==171)for(unsigned lane=0;lane<4;++lane)b[p[2]][lane]=u[p[3]][lane]!=u[p[4]][lane];
        if(op==345)for(unsigned lane=0;lane<4;++lane)f[p[2]][lane]=f[p[4]][u[p[5]][lane]&3];
        if(op==81)for(unsigned lane=0;lane<4;++lane)f[p[2]][lane]=vectors[p[3]][lane][p[4]];
        if(op==109)for(unsigned lane=0;lane<4;++lane)u[p[2]][lane]=uint32_t(f[p[3]][lane]);
        if(op==210) {auto q=f[p[3]];f[p[2]]={q[1]-q[0],q[1]-q[0],q[3]-q[2],q[3]-q[2]};}
        if(op==211) {auto q=f[p[3]];f[p[2]]={q[2]-q[0],q[3]-q[1],q[2]-q[0],q[3]-q[1]};}
        if(op==127)for(unsigned lane=0;lane<4;++lane)f[p[2]][lane]=-f[p[3]][lane];
        if(op==129)for(unsigned lane=0;lane<4;++lane)f[p[2]][lane]=f[p[3]][lane]+f[p[4]][lane];
        if(op==156 || op==157)for(unsigned lane=0;lane<4;++lane)b[p[2]][lane]=op==156?std::isnan(f[p[3]][lane]):std::isinf(f[p[3]][lane]);
        if(op==166)for(unsigned lane=0;lane<4;++lane)b[p[2]][lane]=b[p[3]][lane]||b[p[4]][lane];
        if(op==169)for(unsigned lane=0;lane<4;++lane)f[p[2]][lane]=b[p[3]][lane]?f[p[4]][lane]:f[p[5]][lane];
        if(op==62 && p[1]==12)result=f[p[2]];
        at+=wc;
    }
    return result;
}
// 辅助函数调用形式的合成模块：Ryujinx ShuffleXor 辅助函数（同 D2R 着色器库），值以整数位经 Function 变量传入，结果按位还原。
// A synthetic module of the helper-call form: the translator's ShuffleXor helper (as in D2R's library), the value passed as
// integer bits through Function variables, the result cast back.
enum class Call { plain,uniform,divergent,kill,integer_source,integer_use };
static Words call_fixture(uint32_t xor_value,uint32_t mask,Call mode=Call::plain) {
    Words w={0x07230203,0x00010300,0,300,0};
    add(w,17,{1});add(w,17,{61});add(w,17,{65});add(w,14,{0,1});add(w,15,{4,100,0x6e69616d,0,10,11,12});add(w,16,{100,7});
    add(w,71,{10,11,41});add(w,71,{10,14});add(w,71,{11,30,0});add(w,71,{12,30,0});
    add(w,71,{34,2});add(w,72,{34,0,35,0});add(w,71,{37,34,0});add(w,71,{37,33,0});
    add(w,19,{1});add(w,21,{2,32,0});add(w,22,{3,32});add(w,23,{4,3,4});add(w,20,{5});
    add(w,32,{6,1,2});add(w,32,{7,1,3});add(w,32,{8,3,3});add(w,33,{9,1});
    add(w,21,{30,32,1});add(w,32,{31,7,30});add(w,33,{32,30,31,31,31,31});add(w,32,{33,7,5});
    add(w,30,{34,3});add(w,32,{35,2,34});add(w,32,{36,2,3});
    add(w,59,{6,10,1});add(w,59,{7,11,1});add(w,59,{8,12,3});add(w,59,{35,37,2});
    add(w,43,{2,13,3});add(w,43,{30,40,31});add(w,43,{30,41,8});add(w,43,{30,42,0xffffffffu});add(w,43,{30,43,0});
    add(w,43,{30,44,xor_value});add(w,43,{30,45,mask});add(w,43,{3,46,0x3f000000u});add(w,43,{3,48,0});
    add(w,54,{1,100,0,9});add(w,248,{101});
    for(uint32_t v:{50u,51u,52u,53u,54u})add(w,59,{31,v,7});
    add(w,59,{33,55,7});
    add(w,61,{3,60,11});add(w,mode==Call::integer_source?110u:124u,{30,61,60});
    add(w,62,{50,61});add(w,62,{51,44});add(w,62,{52,45});
    auto call=[&]{add(w,57,{30,70,200,50,51,52,53});add(w,62,{54,70});add(w,61,{30,71,54});add(w,mode==Call::integer_use?111u:124u,{3,72,71});add(w,62,{12,72});};
    if(mode==Call::uniform || mode==Call::divergent) {
        if(mode==Call::uniform) {add(w,65,{36,80,37,43});add(w,61,{3,81,80});add(w,186,{5,82,81,48});}
        else add(w,186,{5,82,60,48});
        add(w,247,{91,0});add(w,250,{82,90,91});add(w,248,{90});call();add(w,249,{91});add(w,248,{91});
    } else if(mode==Call::kill) {
        add(w,184,{5,83,60,46});add(w,62,{55,83});add(w,61,{5,84,55});add(w,247,{93,0});add(w,250,{84,92,93});add(w,248,{92});add(w,252,{});
        add(w,248,{93});add(w,61,{5,85,55});add(w,247,{95,0});add(w,250,{85,94,95});add(w,248,{94});add(w,249,{95});add(w,248,{95});call();
    } else call();
    add(w,253,{});add(w,56,{});
    add(w,54,{30,200,0,32});for(uint32_t v:{201u,202u,203u,204u})add(w,55,{31,v});add(w,248,{205});
    add(w,61,{30,210,203});add(w,199,{30,211,210,40});add(w,194,{30,212,210,41});add(w,199,{30,213,212,40});
    add(w,61,{2,214,10});add(w,124,{30,215,214});add(w,199,{30,216,215,213});add(w,200,{30,217,213});
    add(w,199,{30,218,211,217});add(w,197,{30,219,218,216});add(w,61,{30,220,202});add(w,198,{30,221,215,220});
    add(w,124,{2,222,221});add(w,124,{2,223,219});add(w,178,{5,224,222,223});add(w,169,{30,225,224,42,43});add(w,62,{204,225});
    add(w,61,{30,226,201});add(w,124,{3,227,226});add(w,345,{3,228,13,227,222});add(w,124,{30,229,228});add(w,169,{30,230,224,229,226});
    add(w,254,{230});add(w,56,{});
    return w;
}
// 在一个四元组上执行改写后的主函数：原调用按 32 道线程束的语义取 lane^x 的位，导数与非有限回退照输出指令计算。
// Run the rewritten entry function over one quad: the original call takes lane^x's bits (its 32-lane meaning), and the
// derivatives and the nonfinite fallback follow the emitted instructions.
static Quad evaluate_call(const Words &w,const Quad &input,uint32_t xor_value) {
    std::map<uint32_t,std::array<uint32_t,4>> u,memory;std::map<uint32_t,std::array<bool,4>> b;std::map<uint32_t,std::array<Quad,4>> vectors;
    auto f=[](uint32_t x){float r;std::memcpy(&r,&x,4);return r;};auto bits=[](float x){uint32_t r;std::memcpy(&r,&x,4);return r;};
    Quad result={};bool in_main=false;
    for(size_t at=5;at<w.size();) {
        auto *p=w.data()+at;unsigned wc=p[0]>>16,op=p[0]&65535;at+=wc;
        if(op==54)in_main=p[2]==100;
        if(op==43)u[p[2]].fill(p[3]);
        if(!in_main)continue;
        if(op==61) {
            if(p[3]==11)for(unsigned l=0;l<4;++l)u[p[2]][l]=bits(input[l]);
            else if(p[3]==10)u[p[2]]={0,1,2,3};
            else if(p[1]==4)for(unsigned l=0;l<4;++l)vectors[p[2]][l]={float(l&1)+.5f,float(l>>1)+.5f,0,1};
            else u[p[2]]=memory[p[3]];
        }
        if(op==62) {if(p[1]==12)for(unsigned l=0;l<4;++l)result[l]=f(u[p[2]][l]);else memory[p[1]]=u[p[2]];}
        if(op==124)u[p[2]]=u[p[3]];
        if(op==57)for(unsigned l=0;l<4;++l)u[p[2]][l]=memory[p[4]][l^xor_value];
        if(op==81)for(unsigned l=0;l<4;++l)u[p[2]][l]=bits(vectors[p[3]][l][p[4]]);
        if(op==109)for(unsigned l=0;l<4;++l)u[p[2]][l]=uint32_t(f(u[p[3]][l]));
        if(op==110)for(unsigned l=0;l<4;++l)u[p[2]][l]=uint32_t(int32_t(f(u[p[3]][l])));
        if(op==111)for(unsigned l=0;l<4;++l)u[p[2]][l]=bits(float(int32_t(u[p[3]][l])));
        if(op==83)u[p[2]]=u[p[3]];
        if(op==199)for(unsigned l=0;l<4;++l)u[p[2]][l]=u[p[3]][l]&u[p[4]][l];
        if(op==171)for(unsigned l=0;l<4;++l)b[p[2]][l]=u[p[3]][l]!=u[p[4]][l];
        if(op==210 || op==211) {Quad q;for(unsigned l=0;l<4;++l)q[l]=f(u[p[3]][l]);
            Quad d=op==210?Quad{q[1]-q[0],q[1]-q[0],q[3]-q[2],q[3]-q[2]}:Quad{q[2]-q[0],q[3]-q[1],q[2]-q[0],q[3]-q[1]};
            for(unsigned l=0;l<4;++l)u[p[2]][l]=bits(d[l]);}
        if(op==127)for(unsigned l=0;l<4;++l)u[p[2]][l]=bits(-f(u[p[3]][l]));
        if(op==129)for(unsigned l=0;l<4;++l)u[p[2]][l]=bits(f(u[p[3]][l])+f(u[p[4]][l]));
        if(op==156 || op==157)for(unsigned l=0;l<4;++l)b[p[2]][l]=op==156?std::isnan(f(u[p[3]][l])):std::isinf(f(u[p[3]][l]));
        if(op==166)for(unsigned l=0;l<4;++l)b[p[2]][l]=b[p[3]][l]||b[p[4]][l];
        if(op==169)for(unsigned l=0;l<4;++l)u[p[2]][l]=b[p[3]][l]?u[p[4]][l]:u[p[5]][l];
    }
    return result;
}
static void save(const fs::path &path,const Words &words) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char *>(words.data()),std::streamsize(words.size()*4));
    if(!file)throw std::runtime_error("cannot write synthetic SPIR-V");
}
static void run_tests(const fs::path &output={}) {
    unsigned cases=0;
    if(!output.empty())fs::create_directories(output);
    for(unsigned mask=1;mask<=3;++mask) {
        auto original=fixture(mask);Words rewritten;xbg_spirv_quad_stats stats;
        assert(xbg_spirv_quad_derivatives(original.data(),original.size(),rewritten,&stats)==1);
        assert(stats.candidates==1 && stats.rewritten==1 && stats.guarded_nonfinite==1);
        Quad values={1,4,7,20};auto actual=evaluate(rewritten,values);
        for(unsigned lane=0;lane<4;++lane)assert(actual[lane]==values[lane^mask]);
        if(mask==3) {
            size_t horizontal=0,vertical=0;uint32_t dy_source=0;
            for(size_t at=5;at<rewritten.size();at+=rewritten[at]>>16) {
                unsigned op=rewritten[at]&65535;
                if(op==210)horizontal=at;if(op==211) {vertical=at;dy_source=rewritten[at+3];}
            }
            assert(horizontal && vertical>horizontal && dy_source!=104);
        }
        Quad exceptional={INFINITY,2,NAN,-INFINITY};auto fallback=evaluate(rewritten,exceptional);
        for(unsigned lane=0;lane<4;++lane)assert(std::isnan(exceptional[lane^mask])?std::isnan(fallback[lane]):fallback[lane]==exceptional[lane^mask]);
        if(!output.empty()) {save(output/(("xor"+std::to_string(mask))+"-before.spv"),original);save(output/(("xor"+std::to_string(mask))+"-after.spv"),rewritten);}
        ++cases;
    }
    struct Rejection {Mode mode;xbg_quad_skip reason;const char *name;};
    for(auto test:{Rejection{Mode::source_bits,xbg_quad_skip::bit_pattern,"source_bits"},
        {Mode::source_bits_copy,xbg_quad_skip::bit_pattern,"transitive_source_bits"},
        {Mode::result_bits,xbg_quad_skip::bit_pattern,"result_bits"},
        {Mode::result_bits_copy,xbg_quad_skip::bit_pattern,"transitive_result_bits"},
        {Mode::integer_use,xbg_quad_skip::integer_flow,"integer_use"},
        {Mode::memory_use,xbg_quad_skip::memory_flow,"private_memory"},
        {Mode::source_bits_use,xbg_quad_skip::bit_pattern,"source_downstream_bits"},
        {Mode::source_integer_use,xbg_quad_skip::integer_flow,"source_downstream_integer"},
        {Mode::source_memory_use,xbg_quad_skip::memory_flow,"source_downstream_private_memory"},
        {Mode::branch,xbg_quad_skip::control_flow,"conditional_branch"},
        {Mode::scope,xbg_quad_skip::lane_index,"wrong_scope"},
        {Mode::xor4,xbg_quad_skip::lane_index,"cross_quad"},
        {Mode::index,xbg_quad_skip::lane_index,"unproven_index"}}) {
        auto original=fixture(3,test.mode);Words rewritten={1,2,3};xbg_spirv_quad_stats stats;
        assert(!xbg_spirv_quad_derivatives(original.data(),original.size(),rewritten,&stats));
        assert(rewritten.empty() && stats.candidates==1 && stats.skipped[size_t(test.reason)]==1);
        if(!output.empty())save(output/(std::string(test.name)+"-unchanged.spv"),original);
        ++cases;
    }
    // 辅助函数调用形式：直线、统一分支、kill 之后均改写；分叉、整数来源或用途、跨四元组、clamp 不全都保留原样。
    // Helper-call form: rewritten in straight-line code, under a uniform branch and after a kill; left alone under a divergent
    // branch, for integer sources or uses, across quads, and with a clamp that does not cover the quad.
    for(uint32_t x=1;x<=3;++x)for(Call mode:{Call::plain,Call::uniform,Call::kill}) {
        auto original=call_fixture(x,0x1c03,mode);Words rewritten;xbg_spirv_quad_stats stats;
        assert(xbg_spirv_quad_derivatives(original.data(),original.size(),rewritten,&stats)==1);
        assert(stats.calls==1 && stats.calls_rewritten==1 && stats.rewritten==0 && stats.skipped[size_t(xbg_quad_skip::control_flow)]==1);
        for(size_t at=5;at<rewritten.size();at+=rewritten[at]>>16)assert((rewritten[at]&65535)!=57 || rewritten[at+3]!=200);
        if(mode==Call::plain) {
            Quad values={1,4,7,20};auto actual=evaluate_call(rewritten,values,x);
            for(unsigned lane=0;lane<4;++lane)assert(actual[lane]==values[lane^x]);
            // 无 shuffle 回退：自身有限的 lane 照取邻居，含非有限邻居（对角分两步，还要水平与竖直邻居有限）；否则得 NaN。
            // No shuffle fallback: a lane whose own value is finite gets its neighbour's value, nonfinite ones included (the
            // diagonal takes two steps, so its horizontal and vertical neighbours must be finite too); any other lane gets NaN.
            for(Quad exceptional:{Quad{INFINITY,2,NAN,-INFINITY},Quad{3,INFINITY,5,NAN},Quad{-INFINITY,INFINITY,6,7}}) {
                auto actual=evaluate_call(rewritten,exceptional,x);
                for(unsigned lane=0;lane<4;++lane) {
                    bool exact=std::isfinite(exceptional[lane]) && (x!=3 || (std::isfinite(exceptional[lane^1]) && std::isfinite(exceptional[lane^2])));
                    if(!exact)assert(std::isnan(actual[lane]));
                    else assert(std::isnan(exceptional[lane^x])?std::isnan(actual[lane]):actual[lane]==exceptional[lane^x]);
                }
            }
        }
        if(!output.empty()) {std::string name="call-xor"+std::to_string(x)+(mode==Call::plain?"":mode==Call::uniform?"-uniform":"-kill");
            save(output/(name+"-before.spv"),original);save(output/(name+"-after.spv"),rewritten);}
        ++cases;
    }
    struct CallRejection {uint32_t x,mask;Call mode;xbg_quad_call_skip reason;const char *name;};
    for(auto test:{CallRejection{1,0x1c03,Call::divergent,xbg_quad_call_skip::control_flow,"call_divergent"},
        {1,0x1c03,Call::integer_source,xbg_quad_call_skip::source,"call_integer_source"},
        {1,0x1c03,Call::integer_use,xbg_quad_call_skip::use,"call_integer_use"},
        {4,0x1c03,Call::plain,xbg_quad_call_skip::helper,"call_cross_quad"},
        {1,0x1c02,Call::plain,xbg_quad_call_skip::helper,"call_partial_clamp"}}) {
        auto original=call_fixture(test.x,test.mask,test.mode);Words rewritten={1,2,3};xbg_spirv_quad_stats stats;
        assert(!xbg_spirv_quad_derivatives(original.data(),original.size(),rewritten,&stats));
        assert(rewritten.empty() && stats.calls==1 && stats.call_skipped[size_t(test.reason)]==1);
        if(!output.empty())save(output/(std::string(test.name)+"-unchanged.spv"),original);
        ++cases;
    }
    // 放宽模式（片元阶段不支持子组的驱动，#243 #155）：已证明的调用与缺省模式逐字相同；结果另有按位用途的按浮点改写，值来自
    // 浮点转整数的转回浮点取邻居再转整数，两者都得到邻居的精确值；分叉控制流里的调用仍然不改。
    // Relaxed mode (drivers without fragment subgroups, #243 #155): proven calls come out word for word as in the default
    // mode; a call whose result has bit uses is rewritten as floats, and one whose value comes from a float-to-integer
    // conversion goes back to float, takes the neighbour and converts again, both giving the neighbour's exact value; a call
    // under divergent control flow is still left alone.
    auto has_op=[](const Words &w,uint32_t wanted,uint32_t operand,int at){
        for(size_t i=5;i<w.size();i+=w[i]>>16)if((w[i]&65535)==wanted && (at<0 || (int(w[i]>>16)>at && w[i+size_t(at)]==operand)))return true;return false;};
    auto group_ops=[](const Words &w){size_t k=0;for(size_t i=5;i<w.size();i+=w[i]>>16)if((w[i]&65535)>=333 && (w[i]&65535)<=366)++k;return k;};
    for(uint32_t x=1;x<=3;++x) {
        auto original=call_fixture(x,0x1c03,Call::plain);Words strict,relaxed;xbg_spirv_quad_stats stats;
        assert(xbg_spirv_quad_derivatives(original.data(),original.size(),strict,&stats)==1);
        assert(xbg_spirv_quad_derivatives(original.data(),original.size(),relaxed,&stats,true)==1);
        assert(strict==relaxed && stats.calls_relaxed_float==0 && stats.calls_integer==0);++cases;
        Quad values={1,4,-7,20};
        auto bit_use=call_fixture(x,0x1c03,Call::integer_use);
        assert(!xbg_spirv_quad_derivatives(bit_use.data(),bit_use.size(),relaxed,&stats));
        assert(xbg_spirv_quad_derivatives(bit_use.data(),bit_use.size(),relaxed,&stats,true)==1 && stats.calls_relaxed_float==1);
        auto actual=evaluate_call(relaxed,values,x);
        for(unsigned lane=0;lane<4;++lane) {uint32_t b;std::memcpy(&b,&values[lane^x],4);assert(actual[lane]==float(int32_t(b)));}
        if(!output.empty())save(output/("relaxed-use-xor"+std::to_string(x)+".spv"),relaxed);
        ++cases;
        auto integer=call_fixture(x,0x1c03,Call::integer_source);
        assert(!xbg_spirv_quad_derivatives(integer.data(),integer.size(),relaxed,&stats));
        assert(xbg_spirv_quad_derivatives(integer.data(),integer.size(),relaxed,&stats,true)==1 && stats.calls_integer==1);
        actual=evaluate_call(relaxed,values,x);
        for(unsigned lane=0;lane<4;++lane) {uint32_t b;std::memcpy(&b,&actual[lane],4);assert(b==uint32_t(int32_t(values[lane^x])));}
        if(!output.empty())save(output/("relaxed-integer-xor"+std::to_string(x)+".spv"),relaxed);
        ++cases;
    }
    {auto divergent=call_fixture(1,0x1c03,Call::divergent);Words relaxed;xbg_spirv_quad_stats stats;
        assert(!xbg_spirv_quad_derivatives(divergent.data(),divergent.size(),relaxed,&stats,true));
        assert(stats.call_skipped[size_t(xbg_quad_call_skip::control_flow)]==1);++cases;}
    // 清理：改写后没人调用的辅助函数（200）、不再读取的子组内置变量（10，连同注解和入口点接口里的条目）和 GroupNonUniform
    // 能力都去掉，结果不变；辅助函数还被调用时什么都不动。
    // Clean-up: the helper nobody calls after the rewrite (200), the subgroup built-in variable nothing reads any more (10,
    // with its annotations and its entry in the entry point's interface) and the GroupNonUniform capabilities go, and the
    // results stay the same; while the helper is still called nothing changes.
    for(uint32_t x=1;x<=3;++x)for(Call mode:{Call::plain,Call::integer_source}) {
        auto original=call_fixture(x,0x1c03,mode);Words rewritten,stripped;xbg_spirv_quad_stats stats;
        assert(xbg_spirv_quad_derivatives(original.data(),original.size(),rewritten,&stats,true)==1);
        assert(xbg_spirv_strip_subgroup(rewritten.data(),rewritten.size(),stripped)==4);
        assert(!group_ops(stripped) && !has_op(stripped,54,200,2) && !has_op(stripped,59,10,2) && !has_op(stripped,71,10,1));
        assert(!has_op(stripped,17,61,1) && !has_op(stripped,17,65,1) && has_op(stripped,17,51,1));
        for(size_t i=5;i<stripped.size();i+=stripped[i]>>16)if((stripped[i]&65535)==15)for(size_t j=5;j<(stripped[i]>>16);++j)assert(stripped[i+j]!=10);
        Quad values={1,4,7,20};assert(evaluate_call(stripped,values,x)==evaluate_call(rewritten,values,x));
        if(!output.empty())save(output/("stripped-"+std::string(mode==Call::plain?"plain":"integer")+"-xor"+std::to_string(x)+".spv"),stripped);
        ++cases;
    }
    {auto divergent=call_fixture(1,0x1c03,Call::divergent);Words stripped={1};
        assert(!xbg_spirv_strip_subgroup(divergent.data(),divergent.size(),stripped) && stripped.empty());++cases;}
    auto existing=fixture(3,Mode::existing_coord);Words out;xbg_spirv_quad_stats stats;
    assert(xbg_spirv_quad_derivatives(existing.data(),existing.size(),out,&stats)==1);++cases;
    auto invalid=fixture(3);invalid[5]=0;assert(!xbg_spirv_quad_derivatives(invalid.data(),invalid.size(),out,&stats));assert(out.empty());++cases;
    assert(!xbg_spirv_quad_derivatives(nullptr,0,out,&stats));assert(stats.skipped[size_t(xbg_quad_skip::malformed)]==1);++cases;
    auto ordinary=fixture(1);assert(xbg_spirv_quad_derivatives(ordinary.data(),ordinary.size(),out)==1);++cases;
    auto rounded=evaluate(out,{1e20f,1,1e20f,1});
    assert(rounded[0]!=1);++cases;
    std::cout<<"PASS quad_rewriter cases="<<cases<<"; active-lane arithmetic only; finite_rounding_limit_confirmed=1; device_helpers=NOT_TESTED\n";
}
static void audit(const fs::path &directory,const fs::path &output={}) {
    size_t modules=0,total=0;std::vector<fs::path> files;
    for(const auto &entry:fs::directory_iterator(directory))if(entry.path().extension()==".xgs")files.push_back(entry.path());
    std::sort(files.begin(),files.end());if(!output.empty())fs::create_directories(output);
    for(const auto &path:files) {
        std::ifstream file(path,std::ios::binary);std::vector<char> raw((std::istreambuf_iterator<char>(file)),{});
        if(raw.size()<32)throw std::runtime_error("short XGS");
        uint32_t h[8];std::memcpy(h,raw.data(),32);size_t offset=32+size_t(h[3])*32+size_t(h[4])*4+h[5];
        if(h[0]!=0x31534758 || h[1]!=1 || h[6]%4 || offset>raw.size() || h[6]!=raw.size()-offset)throw std::runtime_error("invalid XGS extent");
        Words before(h[6]/4),after;std::memcpy(before.data(),raw.data()+offset,h[6]);xbg_spirv_quad_stats stats;
        total+=xbg_spirv_quad_derivatives(before.data(),before.size(),after,&stats);++modules;
        std::cout<<"{\"shader\":\""<<path.filename().string()<<"\",\"stage\":"<<h[2]<<",\"candidates\":"<<stats.candidates<<",\"rewritten\":"<<stats.rewritten<<",\"skipped\":{";
        bool first=true;
        for(size_t i=0;i<size_t(xbg_quad_skip::count);++i)if(stats.skipped[i]) {if(!first)std::cout<<',';first=false;std::cout<<'"'<<xbg_spirv_quad_skip_name(xbg_quad_skip(i))<<"\":"<<stats.skipped[i];}
        std::cout<<"},\"calls\":"<<stats.calls<<",\"calls_rewritten\":"<<stats.calls_rewritten<<",\"call_skipped\":{";
        first=true;
        for(size_t i=0;i<size_t(xbg_quad_call_skip::count);++i)if(stats.call_skipped[i]) {if(!first)std::cout<<',';first=false;std::cout<<'"'<<xbg_spirv_quad_call_skip_name(xbg_quad_call_skip(i))<<"\":"<<stats.call_skipped[i];}
        std::cout<<"}}\n";
        if(!output.empty() && stats.candidates)save(output/(path.stem().string()+".spv"),after.empty()?before:after);
    }
    std::cerr<<"AUDIT modules="<<modules<<" rewritten="<<total<<"\n";
}
int main(int argc,char **argv) {
    try {
        if(argc>=3 && std::string(argv[1])=="--audit")audit(argv[2],argc>=4?fs::path(argv[3]):fs::path{});
        else if(argc==3 && std::string(argv[1])=="--emit-fixtures")run_tests(argv[2]);
        else if(argc==1)run_tests();
        else {std::cerr<<"usage: quad_test [--emit-fixtures dir | --audit xgs-dir [spv-dir]]\n";return 2;}
        return 0;
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
