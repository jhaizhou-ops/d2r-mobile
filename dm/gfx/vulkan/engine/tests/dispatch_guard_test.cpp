// 直接调用生产的派发范围检查并检查合成 SPIR-V：检查加在入口最前面，范围内的调用照常执行、范围外的立即返回，原有指令一条不改，
// 认不得的模块原样退回。
// Call the production dispatch-range check on synthetic SPIR-V: the check goes first in the entry function, an invocation
// inside the range runs the body as before and one outside returns at once, no original instruction changes, and a module
// in a form it does not know comes back untouched.
#include "../xbg_spirv_dispatch.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using Module=std::vector<uint32_t>;
using V3=std::array<uint32_t,3>;
void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
void op(Module &m,uint32_t opcode,std::initializer_list<uint32_t> args){m.push_back((uint32_t(args.size()+1)<<16)|opcode);m.insert(m.end(),args);}
enum : uint32_t {tVoid=1,tFn=2,tUint=3,tV3=4,tPtrIn=5,tPtrPriv=6,tPtrFn=7,tBool=8,tBV3=9,
    c0=10,c1=11,c2=12,c7=13,cWx=14,cWy=15,cWz=16,cWs=17,fMain=20,lEntry=21,lNext=22,gDone=30,vNum=31,vGroup=32,vLocal=33,vA=40,vB=41,
    Bound=60};
// ws：0 没有 WorkgroupSize 常量；1 由三个普通常量组成（glslang 的写法）；2 特化常量组成。
// ws: 0 no WorkgroupSize constant; 1 one made of three plain constants (as glslang writes it); 2 one made of specialization
// constants.
struct Shape{bool num=false,group=true,local=true,bools=false;uint32_t size[3]={8,4,1};int ws=0;uint32_t ws_size[3]={16,2,1};};
// 一个计算入口：两个 Function 变量（一个带初值），第一块写 gDone=1 后跳到第二块，第二块写 gDone=2 后返回。
// One compute entry point: two Function variables (one with an initializer); the first block stores gDone = 1 and branches to
// the second, which stores gDone = 2 and returns.
Module module(const Shape &s){
    Module m{0x07230203,0x00010300,0,Bound,0};
    op(m,17,{1});op(m,14,{0,1});
    {Module e{5,fMain,0x6e69616d,0};if(s.num)e.push_back(vNum);if(s.group)e.push_back(vGroup);if(s.local)e.push_back(vLocal);
     m.push_back((uint32_t(e.size()+1)<<16)|15);m.insert(m.end(),e.begin(),e.end());}
    op(m,16,{fMain,17,s.size[0],s.size[1],s.size[2]});
    if(s.num)op(m,71,{vNum,11,24});
    if(s.group)op(m,71,{vGroup,11,26});
    if(s.local)op(m,71,{vLocal,11,27});
    if(s.ws)op(m,71,{cWs,11,25});
    op(m,19,{tVoid});op(m,33,{tFn,tVoid});op(m,21,{tUint,32,0});op(m,23,{tV3,tUint,3});
    if(s.bools){op(m,20,{tBool});op(m,23,{tBV3,tBool,3});}
    op(m,32,{tPtrIn,1,tV3});op(m,32,{tPtrPriv,6,tUint});op(m,32,{tPtrFn,7,tUint});
    op(m,43,{tUint,c0,0});op(m,43,{tUint,c1,1});op(m,43,{tUint,c2,2});op(m,43,{tUint,c7,7});
    if(s.ws==1){op(m,43,{tUint,cWx,s.ws_size[0]});op(m,43,{tUint,cWy,s.ws_size[1]});op(m,43,{tUint,cWz,s.ws_size[2]});op(m,44,{tV3,cWs,cWx,cWy,cWz});}
    if(s.ws==2){op(m,50,{tUint,cWx,s.ws_size[0]});op(m,50,{tUint,cWy,s.ws_size[1]});op(m,50,{tUint,cWz,s.ws_size[2]});op(m,51,{tV3,cWs,cWx,cWy,cWz});}
    op(m,59,{tPtrPriv,gDone,6,c0});
    if(s.num)op(m,59,{tPtrIn,vNum,1});
    if(s.group)op(m,59,{tPtrIn,vGroup,1});
    if(s.local)op(m,59,{tPtrIn,vLocal,1});
    op(m,54,{tVoid,fMain,0,tFn});op(m,248,{lEntry});
    op(m,59,{tPtrFn,vA,7});op(m,59,{tPtrFn,vB,7,c7});
    op(m,62,{vA,c1});op(m,62,{gDone,c1});op(m,249,{lNext});
    op(m,248,{lNext});op(m,62,{gDone,c2});op(m,253,{});op(m,56,{});
    return m;
}
size_t count(const Module &m,uint32_t opcode){size_t c=0;for(size_t i=5;i<m.size();){uint32_t wc=m[i]>>16;if((m[i]&0xffff)==opcode)c++;i+=wc;}return c;}
size_t find(const Module &m,uint32_t opcode,size_t from=5){for(size_t i=5;i<m.size();){uint32_t wc=m[i]>>16;if(i>=from && (m[i]&0xffff)==opcode)return i;i+=wc;}return 0;}
// 内建变量：有 BuiltIn 修饰的 id。
// The variable decorated with a built-in.
uint32_t builtin_var(const Module &m,uint32_t which){uint32_t v=0;size_t c=0;for(size_t i=5;i<m.size();){uint32_t wc=m[i]>>16;if((m[i]&0xffff)==71 && wc==4 && m[i+2]==11 && m[i+3]==which){v=m[i+1];c++;}i+=wc;}require(c<=1,"a built-in is decorated twice");return v;}
// 每个结果 id 只定义一次、都小于 bound；类型不重复。
// Every result id is defined once and is below the bound; no type is declared twice.
void well_formed(const Module &m){
    std::set<uint32_t> ids;std::set<std::vector<uint32_t>> types;
    for(size_t i=5;i<m.size();){uint32_t wc=m[i]>>16,o=m[i]&0xffff;require(wc && i+wc<=m.size(),"broken instruction");
        uint32_t id=0;
        if(o==19||o==20||o==21||o==23||o==32||o==33||o==248)id=m[i+1];
        else if(o==43||o==44||o==59||o==61||o==54||o==128||o==132||o==154||o==166||o==174)id=m[i+2];
        if(id){require(id<m[3],"an id is not below the bound");require(ids.insert(id).second,"an id is defined twice");}
        if(o==20||o==21||o==23){std::vector<uint32_t> key{o};key.insert(key.end(),m.begin()+i+2,m.begin()+i+wc);require(types.insert(key).second,"a scalar or vector type is declared twice");}
        i+=wc;}
    // 用到的操作数都有定义。
    // Every operand used by the check is defined.
    for(size_t i=5;i<m.size();){uint32_t wc=m[i]>>16,o=m[i]&0xffff;
        if(o==61)require(ids.count(m[i+1]) && ids.count(m[i+3]),"a load uses an undefined id");
        if(o==174||o==166||o==128||o==132)require(ids.count(m[i+1]) && ids.count(m[i+3]) && ids.count(m[i+4]),"an operation of the check uses an undefined id");
        if(o==154)require(ids.count(m[i+1]) && ids.count(m[i+3]),"OpAny uses an undefined id");
        if(o==250)require(ids.count(m[i+1]) && ids.count(m[i+2]) && ids.count(m[i+3]),"a branch uses an undefined id");
        i+=wc;}
}
// 按给定的内建值执行入口函数，返回 gDone 最后的值（0 = 函数体没有执行）。fold_local 模拟 Adreno 830 的编译器：组内号与常量
// 尺寸的比较一律当作假（它认定组内号总小于局部尺寸）。
// Runs the entry function with the given built-in values and returns the final gDone (0: the body did not run).
// fold_local plays the Adreno 830's compiler: a comparison of the local id with a constant size is always false (it takes
// the local id to be below the local size).
uint32_t run(const Module &m,V3 group,V3 num,V3 local,bool fold_local=false){
    const uint32_t vn=builtin_var(m,24),vg=builtin_var(m,26),vl=builtin_var(m,27);
    std::map<uint32_t,uint32_t> scalar,mem;std::map<uint32_t,V3> vec;std::map<uint32_t,size_t> label;std::set<uint32_t> constant;
    size_t func=0;
    for(size_t i=5;i<m.size();){uint32_t wc=m[i]>>16,o=m[i]&0xffff;const uint32_t *a=&m[i+1];
        if(o==43)scalar[a[1]]=a[2];
        else if(o==44 && wc==6){vec[a[1]]=V3{scalar[a[2]],scalar[a[3]],scalar[a[4]]};constant.insert(a[1]);}
        else if(o==59 && a[2]==6 && wc>=5)mem[a[1]]=scalar[a[3]];
        else if(o==54)func=i;
        else if(o==248)label[a[0]]=i;
        i+=wc;}
    require(func,"no function");
    size_t i=func+5;int steps=0;
    for(;;){require(++steps<1000 && i<m.size(),"the function does not end");
        uint32_t wc=m[i]>>16,o=m[i]&0xffff;const uint32_t *a=&m[i+1];
        if(o==59 && wc>=5)mem[a[1]]=scalar[a[3]];
        else if(o==61){if(a[2]==vn)vec[a[1]]=num;else if(a[2]==vg)vec[a[1]]=group;else if(a[2]==vl)vec[a[1]]=local;else scalar[a[1]]=mem[a[2]];}
        else if(o==174){V3 r;for(int k=0;k<3;k++)r[k]=vec[a[2]][k]>=vec[a[3]][k];if(fold_local && constant.count(a[3]))r=V3{0,0,0};vec[a[1]]=r;}
        else if(o==132){V3 r;for(int k=0;k<3;k++)r[k]=vec[a[2]][k]*vec[a[3]][k];vec[a[1]]=r;}
        else if(o==128){V3 r;for(int k=0;k<3;k++)r[k]=vec[a[2]][k]+vec[a[3]][k];vec[a[1]]=r;}
        else if(o==166){V3 r;for(int k=0;k<3;k++)r[k]=vec[a[2]][k]||vec[a[3]][k];vec[a[1]]=r;}
        else if(o==154)scalar[a[1]]=vec[a[2]][0]||vec[a[2]][1]||vec[a[2]][2];
        else if(o==62)mem[a[0]]=scalar[a[1]];
        else if(o==249){require(label.count(a[0]),"branch to an unknown label");i=label[a[0]];continue;}
        else if(o==250){uint32_t t=scalar[a[0]]?a[1]:a[2];require(label.count(t),"branch to an unknown label");i=label[t];continue;}
        else if(o==253)break;
        i+=wc;}
    return mem[gDone];
}
// 检查改写结果的形状，返回新加的指令字数。
// Checks the shape of the rewritten module.
void shape_ok(const Module &in,const Module &out,const Shape &s){
    well_formed(out);
    require(out[3]>in[3],"the id bound did not grow");
    const uint32_t vn=builtin_var(out,24),vg=builtin_var(out,26),vl=builtin_var(out,27);
    require(vn && vg && vl,"a built-in variable is missing");
    require(!s.num || vn==vNum,"the existing gl_NumWorkGroups was not reused");
    require(!s.group || vg==vGroup,"the existing gl_WorkGroupID was not reused");
    require(!s.local || vl==vLocal,"the existing gl_LocalInvocationID was not reused");
    // 入口点接口表里三个内建变量各出现一次。
    // The entry point lists each of the three built-in variables once.
    size_t e=find(out,15);require(e,"no entry point");
    for(uint32_t v:{vn,vg,vl}){size_t c=0;for(uint32_t w=4;w<(out[e]>>16);w++)c+=out[e+w]==v;require(c==1,"a built-in variable is not listed once in the entry point interface");}
    require(count(out,15)==1 && count(out,16)==1 && count(out,54)==1,"entry point, execution mode or function count changed");
    // 第一块：新标号、原来的两个变量、检查、条件返回；随后是原标号与原来的指令。
    // The first block: a new label, the two original variables, the check and the conditional return; then the original
    // label and the original instructions.
    size_t f=find(out,54),fi=find(in,54);
    require(out[f+5]==((2u<<16)|248) && out[f+6]!=lEntry,"the function does not start with a new label");
    require(out[f+7]==((4u<<16)|59) && out[f+9]==vA && out[f+11]==((5u<<16)|59) && out[f+13]==vB && out[f+15]==c7,"the Function variables did not move to the new first block");
    size_t merge=find(out,247,f);require(merge && out[merge+1]==lEntry,"the selection does not merge at the original first block");
    size_t br=merge+3;require((out[br]&0xffff)==250 && out[br+3]==lEntry,"the false branch does not go to the original first block");
    size_t ret=br+4;require(out[ret]==((2u<<16)|248) && out[ret+1]==out[br+2] && out[ret+2]==((1u<<16)|253),"the true branch is not a bare return");
    size_t rest=ret+3;require(out[rest]==((2u<<16)|248) && out[rest+1]==lEntry,"the original label does not follow the return block");
    // 原来第一块里变量之后的全部指令，逐字相同。
    // Everything after the variables of the original first block, word for word.
    const size_t in_rest=fi+5+2+4+5;
    require(Module(out.begin()+rest+2,out.end())==Module(in.begin()+in_rest,in.end()),"the original body changed");
    // 函数之前：原来的指令全部保留、顺序不变。
    // Before the function: every original instruction is kept, in order.
    size_t j=5;
    for(size_t i=5;i<fi;){uint32_t wc=in[i]>>16,o=in[i]&0xffff;bool found=false;
        while(j<f){uint32_t wj=out[j]>>16;bool same=o==15?((out[j]&0xffff)==15 && Module(out.begin()+j+1,out.begin()+j+wc)==Module(in.begin()+i+1,in.begin()+i+wc)):(out[j]==in[i] && Module(out.begin()+j,out.begin()+j+wj)==Module(in.begin()+i,in.begin()+i+wc));j+=wj;if(same){found=true;break;}}
        require(found,"an original declaration is missing or out of order");i+=wc;}
    require(count(out,20)==1 && count(out,21)==1,"bool or uint is not declared exactly once");
}
size_t rejected=0;
void reject(const Module &in,const char *why){Module out{99};require(xbg_spirv_dispatch_guard(in.data(),in.size(),out)==0 && out.empty(),why);++rejected;}
}
int main(){try{
    size_t shapes=0,runs=0;
    // 内建变量与布尔类型有或没有的各种组合，三种局部尺寸。
    // Every combination of built-in variables and bool types present or absent, with three local sizes.
    const uint32_t sizes[3][3]={{8,4,1},{32,1,1},{4,4,4}};
    for(int ws=0;ws<2;ws++)for(int mask=0;mask<16;mask++)for(const auto &literal:sizes){
        Shape s;s.num=mask&1;s.group=mask&2;s.local=mask&4;s.bools=mask&8;s.ws=ws;for(int k=0;k<3;k++)s.size[k]=literal[k];
        // 有 WorkgroupSize 常量时以它为准（这里故意与 LocalSize 不同）。
        // With a WorkgroupSize constant, that constant is the local size (here it differs from LocalSize on purpose).
        const uint32_t *size=ws?s.ws_size:literal;
        Module in=module(s),out{99};
        require(xbg_spirv_dispatch_guard(in.data(),in.size(),out)==1,"a plain compute module was not guarded");
        shape_ok(in,out,s);++shapes;
        const V3 num{4,5,2};
        for(uint32_t gx=0;gx<=5;gx++)for(uint32_t gy=0;gy<=6;gy++)for(uint32_t gz=0;gz<=3;gz++)
            for(uint32_t lx:{0u,size[0]-1,size[0],size[0]+3})for(uint32_t ly:{0u,size[1]-1,size[1],size[1]+3})for(uint32_t lz:{0u,size[2]-1,size[2]}){
                const V3 g{gx,gy,gz},l{lx,ly,lz};
                const bool inside=gx<num[0] && gy<num[1] && gz<num[2] && lx<size[0] && ly<size[1] && lz<size[2];
                require(run(in,g,num,l)==2,"the original module does not run its body");
                require(run(out,g,num,l)==(inside?2u:0u),inside?"an invocation inside the range did not run the body":"an invocation outside the range ran the body");
                ++runs;
            }
        // Adreno 830 的多余调用：最后一行工作组里组内号越界（y 在 [尺寸, 2×尺寸)），或多出一行工作组；编译器删掉组内号比较后也必须返回。
        // The Adreno 830's extra invocations: local ids out of range in the last row of work groups (y in [size, 2 x size)),
        // or one more row of work groups. They must return even when the compiler has removed the local id comparison.
        for(uint32_t gx=0;gx<num[0];gx++)for(uint32_t ly=size[1];ly<2*size[1];ly++)for(uint32_t lx=0;lx<size[0];lx++){
            require(run(out,V3{gx,num[1]-1,0},num,V3{lx,ly,0},true)==0,"an out-of-range local id in the last row ran the body once the local id comparison was folded");
            require(run(out,V3{gx,num[1],0},num,V3{lx,ly-size[1],0},true)==0,"an extra row of work groups ran the body once the local id comparison was folded");
            require(run(out,V3{gx,num[1]-1,0},num,V3{lx,ly-size[1],0},true)==2,"an invocation in range did not run the body with the local id comparison folded");
            runs+=3;
        }
    }
    Shape s;Module base=module(s);
    reject(Module{},"empty input");reject(Module{0x07230203},"short input");
    Module bad=base;bad[0]=0;reject(bad,"bad magic");
    bad=base;bad[5]=0;reject(bad,"zero word count");
    bad=base;bad[5]=0xffff0011;reject(bad,"word count past the end");
    {Module o2{99};require(!xbg_spirv_dispatch_guard(nullptr,5,o2) && o2.empty(),"null input");++rejected;}
    // 不是计算入口。
    // Not a compute entry point.
    bad=base;bad[find(bad,15)+1]=4;reject(bad,"a fragment entry point");
    // 两个入口。
    // Two entry points.
    {Module two(base.begin(),base.begin()+find(base,16));op(two,15,{5,fMain,0x326e6961,0});two.insert(two.end(),base.begin()+find(base,16),base.end());reject(two,"two entry points");}
    // 没有 LocalSize；尺寸为 0；LocalSizeId；WorkgroupSize 常量。
    // No LocalSize; a zero size; LocalSizeId; a WorkgroupSize constant.
    {size_t x=find(base,16);Module none(base.begin(),base.begin()+x);none.insert(none.end(),base.begin()+x+6,base.end());reject(none,"no LocalSize");}
    bad=base;bad[find(bad,16)+4]=0;reject(bad,"a zero local size");
    {size_t x=find(base,16);Module idm(base.begin(),base.begin()+x+6);op(idm,331,{fMain,38,c1,c1,c1});idm.insert(idm.end(),base.begin()+x+6,base.end());reject(idm,"LocalSizeId");}
    {size_t x=find(base,71);Module ws(base.begin(),base.begin()+x);op(ws,71,{c7,11,25});ws.insert(ws.end(),base.begin()+x,base.end());reject(ws,"a WorkgroupSize decoration on a scalar constant");}
    {Shape spec;spec.ws=2;reject(module(spec),"a WorkgroupSize made of specialization constants");}
    {Shape twice;twice.ws=1;Module m2=module(twice);size_t x=find(m2,71);Module ws(m2.begin(),m2.begin()+x);op(ws,71,{c7,11,25});ws.insert(ws.end(),m2.begin()+x,m2.end());reject(ws,"two WorkgroupSize decorations");}
    // 入口函数不返回 void；内建变量不是 Input；函数不以 OpLabel 开头。
    // An entry function that does not return void; a built-in variable that is not an Input; a function that does not start
    // with OpLabel.
    bad=base;bad[find(bad,54)+1]=tUint;reject(bad,"a non-void entry function");
    {bad=base;size_t i=5;for(;i<bad.size();){uint32_t wc=bad[i]>>16;if((bad[i]&0xffff)==59 && bad[i+2]==vGroup){bad[i+3]=6;break;}i+=wc;}reject(bad,"a built-in variable that is not an input");}
    bad=base;bad[find(bad,54)+5]=(2u<<16)|249;reject(bad,"a function that does not start with a label");
    // 没有函数。
    // No function.
    {Module none(base.begin(),base.begin()+find(base,54));reject(none,"no function");}
    std::cout<<"PASS dispatch guard: "<<shapes<<" module shapes guarded and well formed, "<<runs<<" invocations (inside run the body, outside return at once), "<<rejected<<" unknown forms left untouched\n";return 0;
}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
