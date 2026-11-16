// PR14 a90826a的有界修订：只改已证明lane XOR 1..3、直线主函数、无整数/位用途的32位浮点。
// XOR3先求水平邻居n1，再取dFdyFine(n1)，避免原补丁遗漏混合二阶项。
// 非有限值保留原shuffle回退；有限数值仍受IEEE舍入影响，须另验截图，不宣称位精确。
// Bounded correction of PR14 a90826a: require proven lane XOR 1..3, a straight-line entry function, and numeric float32 data without bit/integer uses.
// XOR3 first takes horizontal neighbour n1, then dFdyFine(n1), preserving the mixed second-order term missing from the original patch.
// Keep the original shuffle as a nonfinite fallback; IEEE rounding remains for finite values and requires separate screenshot validation, not a bit-exact claim.
// 规范依据 | Specification: https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html
#include "xbg_spirv_quad.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
enum : uint32_t {
    ExtInst=12, EntryPoint=15, ExecutionMode=16, Capability=17, TypeVoid=19, TypeBool=20, TypeInt=21,
    TypeFloat=22, TypeVector=23, TypeMatrix=24, TypePointer=32, Constant=43, ConstantComposite=44,
    Function=54, FunctionEnd=56, FunctionCall=57, Variable=59, Load=61, Store=62, AccessChain=65,
    InBoundsAccessChain=66, Decorate=71, VectorShuffle=79, CompositeConstruct=80, CompositeExtract=81,
    CompositeInsert=82, CopyObject=83, ConvertFToU=109, Bitcast=124, FNegate=127, FAdd=129,
    IsNan=156, IsInf=157, LogicalOr=166, Select=169, INotEqual=171, BitwiseXor=198,
    BitwiseAnd=199, DPdxFine=210, DPdyFine=211, Label=248, Branch=249, BranchConditional=250,
    Switch=251, Kill=252, Return=253, ReturnValue=254, Unreachable=255, Shuffle=345,
    Input=1, Output=3, Uniform=2, PushConstant=9, BuiltIn=11, FragCoord=15, Lane=41, Fragment=4,
    FunctionParameter=55, IAdd=128, ISub=130, LogicalEqual=164, LogicalNotEqual=165, LogicalAnd=167, LogicalNot=168,
    IEqual=170, ShiftRightLogical=194, ShiftRightArithmetic=195, ShiftLeftLogical=196, BitwiseOr=197, Not=200,
    LoopMerge=246, SelectionMerge=247, TerminateInvocation=4416, UniformConstant=0, FunctionStorage=7, Block=2,
    ConvertFToS=110, ConvertSToF=111, ConvertUToF=112
};
struct Instruction { size_t offset; uint32_t op,wc,type=0,id=0,fn=0; };
struct Type { uint32_t op=0,a=0,b=0; };
uint32_t word(uint32_t op,uint32_t wc) { return op|(wc<<16); }
struct Module {
    const uint32_t *w; size_t n; uint32_t bound,entry_fn=0,entry_count=0,lane_var=0,coord_var=0;
    uint32_t t_float=0,t_uint=0,t_bool=0,t_vec4=0,t_ptr4=0;
    size_t entry_offset=0,first_type=0,first_func=0,first_cap=0;
    bool fragment=true,derivatives=false,origin_upper=false,valid=true;
    std::vector<Instruction> instructions;
    std::vector<Type> types;
    std::vector<size_t> definitions;
    std::vector<std::vector<size_t>> uses;
    std::vector<uint32_t> unsafe_functions;
    std::vector<uint32_t> glsl_sets;
    explicit Module(const uint32_t *p,size_t count):w(p),n(count),bound(p[3]),types(bound),definitions(bound,SIZE_MAX),uses(bound) {}
    const Instruction *def(uint32_t id) const { return id<bound && definitions[id]!=SIZE_MAX?&instructions[definitions[id]]:nullptr; }
    bool float_type(uint32_t id,unsigned depth=0) const {
        if(!id || id>=bound || depth>32)return false;
        const Type &t=types[id];
        return t.op==TypeFloat || ((t.op==TypeVector || t.op==TypeMatrix) && float_type(t.a,depth+1));
    }
    bool int_type(uint32_t id,unsigned depth=0) const {
        if(!id || id>=bound || depth>32)return false;
        const Type &t=types[id];return t.op==TypeInt || (t.op==TypeVector && int_type(t.a,depth+1));
    }
    bool constant(uint32_t id,uint32_t &value) const {
        const auto *i=def(id);
        if(!i || i->op!=Constant || i->wc!=4 || !int_type(i->type))return false;
        value=w[i->offset+3];return true;
    }
    uint32_t pointer_root(uint32_t id,unsigned depth=0) const {
        if(depth>32)return 0;
        const auto *i=def(id);if(!i)return 0;
        if(i->op==Variable)return id;
        if((i->op==AccessChain || i->op==InBoundsAccessChain || i->op==CopyObject) && i->wc>=4)
            return pointer_root(w[i->offset+3],depth+1);
        return 0;
    }
    uint32_t storage(uint32_t ptr) const {
        const auto *i=def(pointer_root(ptr));return i && i->wc>=4?w[i->offset+3]:UINT32_MAX;
    }
    bool lane_value(uint32_t id,unsigned depth=0) const {
        if(depth>16)return false;
        const auto *i=def(id);if(!i)return false;
        const auto *p=w+i->offset;
        if(i->op==Load && i->wc==4 && p[3]==lane_var)return true;
        return (i->op==Bitcast || i->op==CopyObject) && i->wc==4 && int_type(i->type) && lane_value(p[3],depth+1);
    }
};

// 只列实际ID操作数；未知指令宁可把字面量当ID多跳过，也不漏过整数消费者。
// Enumerate actual ID operands where known; unknown instructions may over-classify literals as IDs to avoid missing integer consumers.
std::vector<uint32_t> operands(const Module &m,const Instruction &i) {
    const auto *p=m.w+i.offset;std::vector<uint32_t> ids;
    auto tail=[&](unsigned first){for(unsigned j=first;j<i.wc;++j)ids.push_back(p[j]);};
    if(i.op==Constant || i.op==Variable || i.op==Label || i.op==Function || i.op==FunctionEnd)return ids;
    if(i.op==VectorShuffle) { if(i.wc>=5)ids={p[3],p[4]}; }
    else if(i.op==CompositeExtract || i.op==Load || i.op==CopyObject || i.op==Bitcast) { if(i.wc>=4)ids={p[3]}; }
    else if(i.op==CompositeInsert) { if(i.wc>=5)ids={p[3],p[4]}; }
    else if(i.op==ExtInst)tail(5);
    else if(i.op==Store) { if(i.wc>=3)ids={p[1],p[2]}; }
    else if(i.op==BranchConditional) { if(i.wc>=2)ids={p[1]}; }
    else if(i.op==Switch) { if(i.wc>=2)ids={p[1]}; }
    else if(i.op==ReturnValue) { if(i.wc>=2)ids={p[1]}; }
    else if(i.op==Branch || i.op==Return || i.op==Kill || i.op==Unreachable || i.op==246 || i.op==247)return ids;
    else if(i.op==245) { for(unsigned j=3;j+1<i.wc;j+=2)ids.push_back(p[j]); }
    else if(i.type)tail(3);
    else if(i.fn)tail(1);
    return ids;
}
bool parse(Module &m) {
    uint32_t fn=0;
    for(size_t off=5;off<m.n;) {
        uint32_t wc=m.w[off]>>16,op=m.w[off]&65535;
        if(!wc || wc>m.n-off)return false;
        const auto *p=m.w+off;
        if(op>=19 && op<=39) {
            if(wc<2 || !p[1] || p[1]>=m.bound)return false;
            if(!m.first_type)m.first_type=off;
            auto &t=m.types[p[1]];t.op=op;if(wc>=3)t.a=p[2];if(wc>=4)t.b=p[3];
            if(op==TypeFloat && wc==3 && p[2]==32)m.t_float=p[1];
            if(op==TypeInt && wc==4 && p[2]==32 && !p[3])m.t_uint=p[1];
            if(op==TypeBool && wc==2)m.t_bool=p[1];
        }
        if(op==Function) { if(wc!=5)return false;fn=p[2];if(!m.first_func)m.first_func=off; }
        m.instructions.push_back({off,op,wc,0,0,fn});
        if(op==FunctionEnd)fn=0;
        if(op==Capability) {if(wc!=2)return false;if(!m.first_cap)m.first_cap=off;if(p[1]==51)m.derivatives=true;}
        if(op==EntryPoint) {if(wc<4)return false;++m.entry_count;m.fragment&=p[1]==Fragment;m.entry_fn=p[2];m.entry_offset=off;}
        if(op==ExecutionMode && wc>=3 && p[2]==7)m.origin_upper=true;
        if(op==Decorate && wc>=4 && p[2]==BuiltIn) {
            if(p[3]==Lane) {if(m.lane_var)return false;m.lane_var=p[1];}
            if(p[3]==FragCoord) {if(m.coord_var)return false;m.coord_var=p[1];}
        }
        if(op==11 && wc>=5 && std::memcmp(p+2,"GLSL.std.450",12)==0)m.glsl_sets.push_back(p[1]);
        if(fn && (op==FunctionCall || op==BranchConditional || op==Switch || op==Kill || op==Unreachable || op==246 || op==247 || op==5380 || op==4416))
            m.unsafe_functions.push_back(fn);
        off+=wc;
    }
    for(size_t index=0;index<m.instructions.size();++index) {
        auto &i=m.instructions[index];const auto *p=m.w+i.offset;
        if((i.fn || (i.op>=41 && i.op<=52) || i.op==Variable || i.op==1) && i.wc>=3 && p[1]<m.bound && m.types[p[1]].op && !(i.op>=19 && i.op<=39)) {
            if(!p[2] || p[2]>=m.bound || m.definitions[p[2]]!=SIZE_MAX)return false;
            i.type=p[1];i.id=p[2];m.definitions[i.id]=index;
        }
    }
    for(size_t index=0;index<m.instructions.size();++index)
        for(uint32_t id:operands(m,m.instructions[index]))if(id<m.bound)m.uses[id].push_back(index);
    for(uint32_t id=1;id<m.bound;++id) {
        const auto &t=m.types[id];
        if(t.op==TypeVector && t.a==m.t_float && t.b==4)m.t_vec4=id;
    }
    for(uint32_t id=1;id<m.bound;++id)if(m.types[id].op==TypePointer && m.types[id].a==Input && m.types[id].b==m.t_vec4)m.t_ptr4=id;
    return true;
}
bool numeric_op(uint32_t op) {
    return op==FNegate || op==129 || op==131 || op==133 || op==136 || op==140 || op==141 ||
        (op>=142 && op<=148) || op==115 || op==116 || op==VectorShuffle || op==CompositeConstruct ||
        op==CompositeExtract || op==CompositeInsert || op==CopyObject || op==Select ||
        (op>=207 && op<=215) || op==Shuffle;
}
bool numeric_ext(const Module &m,const Instruction &i) {
    if(i.op!=ExtInst || i.wc<6)return false;
    const auto *p=m.w+i.offset;uint32_t op=p[4];
    bool set=std::find(m.glsl_sets.begin(),m.glsl_sets.end(),p[3])!=m.glsl_sets.end();
    return set && ((op>=1 && op<=34 && op!=5 && op!=7) || (op>=37 && op<=50) || op==53 || (op>=66 && op<=72) || (op>=79 && op<=81));
}
bool source_safe(const Module &m,uint32_t id,std::vector<uint8_t> &seen,xbg_quad_skip &reason,unsigned depth=0) {
    if(depth>256 || !id || id>=m.bound) {reason=xbg_quad_skip::unknown_source;return false;}
    if(seen[id]==2)return true;
    if(seen[id]==1) {reason=xbg_quad_skip::unknown_source;return false;}
    const auto *i=m.def(id);if(!i) {reason=xbg_quad_skip::unknown_source;return false;}
    if(i->op==Bitcast) {reason=xbg_quad_skip::bit_pattern;return false;}
    if(m.int_type(i->type)) {reason=xbg_quad_skip::integer_flow;return false;}
    if(!m.float_type(i->type)) {reason=xbg_quad_skip::unknown_source;return false;}
    if(i->op==Constant)return true;
    const auto *p=m.w+i->offset;
    if(i->op==Load) {
        uint32_t storage=m.storage(p[3]);
        if(storage==Input || storage==Uniform || storage==PushConstant)return true;
        reason=xbg_quad_skip::memory_flow;return false;
    }
    if(i->op>=87 && i->op<=98)return true;
    if(!numeric_op(i->op) && !numeric_ext(m,*i) && i->op!=ConstantComposite) {reason=xbg_quad_skip::unknown_source;return false;}
    seen[id]=1;
    std::vector<uint32_t> deps;
    if(i->op==Select && i->wc==6)deps={p[4],p[5]};
    else if(i->op==Shuffle && i->wc==6)deps={p[4]};
    else deps=operands(m,*i);
    for(uint32_t dep:deps)if(!source_safe(m,dep,seen,reason,depth+1))return false;
    seen[id]=2;return true;
}
bool uses_safe(const Module &m,uint32_t id,std::vector<uint8_t> &seen,xbg_quad_skip &reason,unsigned depth=0) {
    if(depth>256) {reason=xbg_quad_skip::unknown_use;return false;}
    if(seen[id])return true;seen[id]=1;
    for(size_t index:m.uses[id]) {
        const auto &i=m.instructions[index];const auto *p=m.w+i.offset;
        if(i.op==Bitcast) {reason=xbg_quad_skip::bit_pattern;return false;}
        if(m.int_type(i.type)) {reason=xbg_quad_skip::integer_flow;return false;}
        if(i.op==Store) {
            bool readback=false;
            uint32_t root=m.pointer_root(p[1]);
            for(const auto &other:m.instructions)if(other.op==Load && other.wc>=4 && m.pointer_root(m.w[other.offset+3])==root)readback=true;
            if(i.wc==3 && p[2]==id && m.storage(p[1])==Output && !readback)continue;
            reason=xbg_quad_skip::memory_flow;return false;
        }
        if(i.op==IsNan || i.op==IsInf || (i.op>=180 && i.op<=191))continue;
        if(!i.id || !m.float_type(i.type) || (!numeric_op(i.op) && !numeric_ext(m,i))) {reason=xbg_quad_skip::unknown_use;return false;}
        if(!uses_safe(m,i.id,seen,reason,depth+1))return false;
    }
    return true;
}

// Ryujinx 把每条 SHFL 译成一个直线辅助函数（值以整数位、下标与 SHFL 的 c 掩码经 Function 变量传入），主函数在分支里调用它。
// D2R 着色器库的 278 处调用都传异或 1 或 2 与掩码 0x1c03，即 2x2 四元组内的左右或上下交换。满足以下三条时，在调用处改写：
// 1. 对 32 道线程束的每条 lane 执行辅助函数，返回值都是 lane^x（x 为 1..3，各 lane 相同）上的那个参数值；
// 2. 调用处于主函数的动态统一控制流：分支条件只来自常量、uniform / push constant 读取和纯运算；另一侧只有 kill 的分支
//    不算分叉，kill 之后再读同一谓词，对存活调用是同一个值；
// 3. 交换值的位只来自浮点运算，结果的位只流向浮点用途，途经 Function 变量、32 位 bitcast、复制与 select，不经整数运算。
// 调用整个换掉（见下文发射处为何不留 shuffle 回退）。子组为 64 道时原调用的 clamp 检查按 32 道线程束写，32–63 号 lane
// 拿回自己的值；导数形式没有这个问题。
// The translator emits every SHFL through a straight-line helper function that the entry function calls, passing the value
// (as integer bits), the lane index or xor and SHFL's c mask through Function variables. All 278 calls in D2R's shader
// library pass xor 1 or 2 with mask 0x1c03: a horizontal or vertical swap inside the 2x2 quad. A call is rewritten at its
// call site when:
// 1. running the helper for each lane of a 32-lane warp returns, for every lane, the parameter's value from lane^x (the
//    same x, 1..3, for all lanes);
// 2. the call is in dynamically uniform control flow of the entry function: branch conditions come only from constants,
//    uniform / push-constant loads and pure arithmetic; a branch whose other side only kills does not diverge, and reading
//    its predicate again after the kill gives the surviving invocations one value;
// 3. the swapped value's bits come only from float producers and the result's bits reach only float consumers, through
//    Function variables, 32-bit bitcasts, copies and selects, never through an integer operation.
// The call is replaced whole (see the emitter for why there is no shuffle fallback). With a 64-lane subgroup the original
// call's clamp check, written for 32-lane warps, gives lanes 32-63 their own value back; the derivative form does not.
using Bits=std::vector<uint64_t>;
inline bool bit(const Bits &b,size_t i){return b[i>>6]>>(i&63)&1;}
inline void set_bit(Bits &b,size_t i){b[i>>6]|=uint64_t(1)<<(i&63);}
struct Cfg {
    std::vector<size_t> first,last;std::vector<std::vector<int>> succ,pred;std::vector<int> block_of,of_label;std::vector<uint8_t> kill_only;bool valid=true;
};
Cfg build_cfg(const Module &m,uint32_t fn) {
    Cfg c;c.block_of.assign(m.instructions.size(),-1);c.of_label.assign(m.bound,-1);
    for(size_t k=0;k<m.instructions.size();++k) {
        const auto &i=m.instructions[k];if(i.fn!=fn || i.op==FunctionEnd)continue;
        if(i.op==Label) {c.of_label[m.w[i.offset+1]]=int(c.first.size());c.first.push_back(k);c.last.push_back(k);}
        if(!c.first.empty()) {c.block_of[k]=int(c.first.size()-1);c.last.back()=k;}
    }
    size_t n=c.first.size();c.succ.resize(n);c.pred.resize(n);c.kill_only.resize(n);
    for(size_t b=0;b<n;++b) {
        const auto &t=m.instructions[c.last[b]];const auto *p=m.w+t.offset;std::vector<uint32_t> targets;
        if(t.op==Branch && t.wc==2)targets={p[1]};
        else if(t.op==BranchConditional && t.wc>=4)targets={p[2],p[3]};
        else if(t.op==Switch && t.wc>=3) {
            const auto *s=m.def(p[1]);if(!s || m.types[s->type].op!=TypeInt || m.types[s->type].a!=32)c.valid=false;
            targets.push_back(p[2]);for(unsigned j=4;j<t.wc;j+=2)targets.push_back(p[j]);
        } else if(!(t.op==Return || t.op==ReturnValue || t.op==Kill || t.op==Unreachable || t.op==TerminateInvocation))c.valid=false;
        for(uint32_t l:targets) {
            if(l>=m.bound || c.of_label[l]<0) {c.valid=false;continue;}
            c.succ[b].push_back(c.of_label[l]);c.pred[size_t(c.of_label[l])].push_back(int(b));
        }
        c.kill_only[b]=c.last[b]==c.first[b]+1 && (t.op==Kill || t.op==TerminateInvocation);
    }
    return c;
}
// 支配与后支配集合（逐位迭代）；后支配时略去只有 kill 的块，并以一个虚拟出口收尾。
// Dominator and post-dominator sets by bitset iteration; post-dominance skips kill-only blocks and ends at a virtual exit.
std::vector<Bits> dominators(const Cfg &c) {
    size_t n=c.first.size(),words=(n+63)/64;std::vector<Bits> d(n,Bits(words,~uint64_t(0)));
    if(!n)return d;
    d[0].assign(words,0);set_bit(d[0],0);
    for(bool changed=true;changed;) {
        changed=false;
        for(size_t b=1;b<n;++b) {
            Bits next(words,c.pred[b].empty()?0:~uint64_t(0));
            for(int p:c.pred[b])for(size_t w=0;w<words;++w)next[w]&=d[size_t(p)][w];
            set_bit(next,b);if(next!=d[b]) {d[b]=next;changed=true;}
        }
    }
    return d;
}
std::vector<Bits> post_dominators(const Cfg &c) {
    size_t n=c.first.size(),exit=n,words=(n+64)/64;std::vector<Bits> d(n+1,Bits(words,~uint64_t(0)));
    d[exit].assign(words,0);set_bit(d[exit],exit);
    for(bool changed=true;changed;) {
        changed=false;
        for(size_t b=n;b-->0;) {
            if(c.kill_only[b])continue;
            Bits next(words,~uint64_t(0));bool any=false;
            for(int s:c.succ[b]) {if(c.kill_only[size_t(s)])continue;any=true;for(size_t w=0;w<words;++w)next[w]&=d[size_t(s)][w];}
            if(!any)next=d[exit];
            set_bit(next,b);if(next!=d[b]) {d[b]=next;changed=true;}
        }
    }
    return d;
}
// 主函数的 Function 变量：直接的存与取、作为调用实参的位置、是否经访问链。
// The entry function's Function variables: their direct stores and loads, the calls they are arguments of, and whether an
// access chain reaches them.
struct Var {std::vector<size_t> stores,loads;std::vector<std::pair<size_t,unsigned>> calls;bool local=false,chained=false;};
std::vector<Var> local_vars(const Module &m,uint32_t fn) {
    std::vector<Var> v(m.bound);
    for(const auto &i:m.instructions)if(i.fn==fn && i.op==Variable && i.wc>=4 && m.w[i.offset+3]==FunctionStorage)v[i.id].local=true;
    for(size_t k=0;k<m.instructions.size();++k) {
        const auto &i=m.instructions[k];if(i.fn!=fn)continue;const auto *p=m.w+i.offset;
        auto root_of=[&](uint32_t ptr)->Var*{uint32_t r=m.pointer_root(ptr);if(!r || !v[r].local)return nullptr;if(r!=ptr)v[r].chained=true;return &v[r];};
        if(i.op==Store && i.wc>=3) {if(Var *r=root_of(p[1]))r->stores.push_back(k);}
        else if(i.op==Load && i.wc>=4) {if(Var *r=root_of(p[3]))r->loads.push_back(k);}
        else if(i.op==FunctionCall) {for(unsigned j=4;j<i.wc;++j)if(Var *r=root_of(p[j]))r->calls.push_back({k,j-4});}
        else if((i.op==AccessChain || i.op==InBoundsAccessChain) && i.wc>=4) {uint32_t r=m.pointer_root(p[3]);if(r && v[r].local)v[r].chained=true;}
    }
    return v;
}
// 纯运算：结果只取决于操作数（含 GLSL.std.450 扩展指令，插值函数除外）。
// Pure operations: the result depends only on the operands (GLSL.std.450 included, except the interpolation functions).
bool pure_op(const Module &m,const Instruction &i) {
    uint32_t op=i.op;
    if(op==ExtInst)return i.wc>=5 && std::find(m.glsl_sets.begin(),m.glsl_sets.end(),m.w[i.offset+3])!=m.glsl_sets.end() && !(m.w[i.offset+4]>=76 && m.w[i.offset+4]<=78);
    return (op>=79 && op<=83) || (op>=109 && op<=124) || (op>=126 && op<=152) || (op>=154 && op<=157) || (op>=164 && op<=191) || (op>=194 && op<=205);
}
// 动态统一性：自乐观起点不动点迭代，变量、块、值三者互相约束，单调收敛。
// Dynamic uniformity: a fixed point from an optimistic start in which variables, blocks and values constrain each other;
// it only ever removes uniformity, so it converges.
struct Uniformity {
    const Module &m;const Cfg &c;const std::vector<Var> &vars;std::vector<Bits> dom,pdom;
    std::vector<std::vector<int>> dep;std::vector<uint8_t> var_ok,block_ok;std::vector<int8_t> memo;
    std::vector<std::pair<uint32_t,int>> kill_predicates;   // (variable, surviving block): its loads there see one value
    Uniformity(const Module &mod,const Cfg &cfg,const std::vector<Var> &v):m(mod),c(cfg),vars(v),dom(dominators(cfg)),pdom(post_dominators(cfg)) {
        size_t n=c.first.size();dep.resize(n);
        for(size_t a=0;a<n;++a) {
            if(c.kill_only[a])continue;
            std::vector<int> s;for(int t:c.succ[a])if(!c.kill_only[size_t(t)])s.push_back(t);
            if(s.size()<2)continue;
            Bits reach(pdom[0].size(),0);for(int t:s)for(size_t w=0;w<reach.size();++w)reach[w]|=pdom[size_t(t)][w];
            for(size_t b=0;b<n;++b)if(bit(reach,b) && (b==a || !bit(pdom[a],b)))dep[b].push_back(int(a));
        }
        find_kill_predicates();
        var_ok.assign(m.bound,1);block_ok.assign(n,1);
        for(bool changed=true;changed;) {
            changed=false;memo.assign(m.bound,0);
            std::vector<uint8_t> blocks(n,1);
            for(size_t b=0;b<n;++b)for(int a:dep[b]) {
                const auto &t=m.instructions[c.last[size_t(a)]];
                if(!block_ok[size_t(a)] || !(t.op==BranchConditional || t.op==Switch) || !value(m.w[t.offset+1])) {blocks[b]=0;break;}
            }
            for(uint32_t id=1;id<m.bound;++id) {
                if(!vars[id].local || !var_ok[id])continue;
                bool ok=vars[id].calls.empty();
                for(size_t k:vars[id].stores) {
                    const auto *p=m.w+m.instructions[k].offset;
                    if(!ok || !value(p[2]) || !blocks[size_t(c.block_of[k])] || !chain(p[1]))ok=false;
                }
                if(!ok) {var_ok[id]=0;changed=true;}
            }
            for(size_t b=0;b<n;++b)if(block_ok[b] && !blocks[b]) {block_ok[b]=0;changed=true;}
        }
    }
    // kill 分支：另一侧只有 kill；谓词是只存一次的 Function 变量，存它的块支配该分支，且存活一侧只有这一个前驱。
    // A kill branch: its other side only kills; the predicate is a Function variable stored once, in a block dominating the
    // branch, and the surviving side has the branch as its only predecessor.
    void find_kill_predicates() {
        for(size_t a=0;a<c.first.size();++a) {
            const auto &t=m.instructions[c.last[a]];if(t.op!=BranchConditional || c.succ[a].size()!=2)continue;
            int yes=c.succ[a][0],no=c.succ[a][1];if(bool(c.kill_only[size_t(yes)])==bool(c.kill_only[size_t(no)]))continue;
            int survivor=c.kill_only[size_t(yes)]?no:yes;
            const auto *load=m.def(m.w[t.offset+1]);if(!load || load->op!=Load || load->wc!=4)continue;
            uint32_t v=m.w[load->offset+3];if(v>=m.bound || !vars[v].local || vars[v].chained || !vars[v].calls.empty() || vars[v].stores.size()!=1)continue;
            size_t store=vars[v].stores[0];int sb=c.block_of[store];
            bool before=sb==int(a)?store<size_t(load-&m.instructions[0]):bit(dom[a],size_t(sb));
            if(!before || c.pred[size_t(survivor)].size()!=1)continue;
            kill_predicates.push_back({v,survivor});
        }
    }
    bool chain(uint32_t ptr,unsigned depth=0) {
        const auto *i=m.def(ptr);if(!i || depth>32)return false;
        if(i->op==Variable)return true;
        if(i->op==CopyObject && i->wc==4)return chain(m.w[i->offset+3],depth+1);
        if((i->op==AccessChain || i->op==InBoundsAccessChain) && i->wc>=4) {
            for(unsigned j=4;j<i->wc;++j)if(!value(m.w[i->offset+j]))return false;
            return chain(m.w[i->offset+3],depth+1);
        }
        return false;
    }
    std::vector<uint8_t> blocks_decorated;
    bool block_decorated(uint32_t type) {
        if(blocks_decorated.empty()) {
            blocks_decorated.assign(m.bound,0);
            for(const auto &i:m.instructions)if(i.op==Decorate && i.wc>=3 && m.w[i.offset+1]<m.bound && m.w[i.offset+2]==Block)blocks_decorated[m.w[i.offset+1]]=1;
        }
        for(unsigned depth=0;type && type<m.bound && depth<8;++depth) {
            if(blocks_decorated[type])return true;
            if(m.types[type].op!=28 && m.types[type].op!=29)return false;
            type=m.types[type].a;
        }
        return false;
    }
    bool value(uint32_t id,unsigned depth=0) {
        if(!id || id>=m.bound)return false;
        if(memo[id]==1)return true;
        if(memo[id]==2 || memo[id]==3 || depth>512)return false;
        memo[id]=3;bool r=false;const auto *i=m.def(id);
        if(i) {
            const auto *p=m.w+i->offset;
            if(i->op==Constant || i->op==ConstantComposite || i->op==41 || i->op==42 || i->op==46)r=true;
            else if(i->op==Load && i->wc>=4) {
                uint32_t root=m.pointer_root(p[3]);const auto *v=m.def(root);
                uint32_t storage=v && v->wc>=4?m.w[v->offset+3]:UINT32_MAX;
                if(storage==Uniform)r=block_decorated(m.types[v->type].b) && chain(p[3]);
                else if(storage==PushConstant || storage==UniformConstant)r=chain(p[3]);
                else if(root && vars[root].local) {
                    int lb=c.block_of[size_t(i-&m.instructions[0])];
                    for(const auto &kp:kill_predicates)if(kp.first==root && lb>=0 && bit(dom[size_t(lb)],size_t(kp.second)))r=true;
                    if(!r)r=var_ok[root] && chain(p[3]);
                }
            } else if(pure_op(m,*i)) {
                r=true;
                if(i->op==ExtInst) {for(unsigned j=5;j<i->wc;++j)if(!value(p[j],depth+1)) {r=false;break;}}
                else for(uint32_t o:operands(m,*i))if(!value(o,depth+1)) {r=false;break;}
            }
        }
        memo[id]=r?1:2;return r;
    }
};
// 辅助函数的逐 lane 求值：只建模直线代码里的整数位运算、比较、select、Function 变量与参数指针的存取和一条子组 shuffle。
// Per-lane evaluation of a helper: models only straight-line integer bit operations, compares, select, loads and stores of
// Function variables and parameter pointers, and one subgroup shuffle.
bool scalar32(const Module &m,uint32_t type) {return type && type<m.bound && m.types[type].a==32 && (m.types[type].op==TypeInt || m.types[type].op==TypeFloat);}
struct LaneValue {uint8_t kind=0;uint32_t a=0,b=0;};   // 0 unknown; 1 bits a; 2 the contents of parameter a; 3 parameter a's value from lane b
struct Helper {std::vector<uint32_t> params;std::vector<uint8_t> written;size_t first=0,last=0;bool straight=false;};
Helper helper_body(const Module &m,uint32_t fn) {
    Helper h;bool seen=false;unsigned labels=0;h.straight=true;
    for(size_t k=0;k<m.instructions.size();++k) {
        const auto &i=m.instructions[k];if(i.fn!=fn)continue;
        if(!seen) {h.first=k;seen=true;}
        h.last=k;
        if(i.op==FunctionParameter)h.params.push_back(i.id);
        if(i.op==Label)++labels;
        if(i.op==Branch || i.op==BranchConditional || i.op==Switch || i.op==Kill || i.op==FunctionCall || i.op==LoopMerge || i.op==SelectionMerge || i.op==245)h.straight=false;
    }
    h.written.assign(h.params.size(),0);
    for(size_t k=h.first;seen && k<=h.last;++k) {
        const auto &i=m.instructions[k];if(i.op!=Store || i.wc<3)continue;
        for(size_t j=0;j<h.params.size();++j)if(m.pointer_root(m.w[i.offset+1])==h.params[j] || m.w[i.offset+1]==h.params[j])h.written[j]=1;
    }
    h.straight=h.straight && seen && labels==1;return h;
}
LaneValue run_helper(const Module &m,const Helper &h,const std::vector<LaneValue> &args,uint32_t lane) {
    std::vector<std::pair<uint32_t,LaneValue>> env,memory;
    auto get=[&](std::vector<std::pair<uint32_t,LaneValue>> &table,uint32_t id)->LaneValue*{for(auto &e:table)if(e.first==id)return &e.second;return nullptr;};
    auto put=[&](std::vector<std::pair<uint32_t,LaneValue>> &table,uint32_t id,LaneValue v){if(auto *e=get(table,id))*e=v;else table.push_back({id,v});};
    for(size_t j=0;j<h.params.size() && j<args.size();++j)put(memory,h.params[j],args[j]);
    auto val=[&](uint32_t id)->LaneValue{if(auto *e=get(env,id))return *e;uint32_t c=0;if(m.constant(id,c))return {1,c,0};
        const auto *d=m.def(id);if(d && (d->op==41 || d->op==42) && m.types[d->type].op==TypeBool)return {1,d->op==41?1u:0u,0};return {};};
    for(size_t k=h.first;k<=h.last;++k) {
        const auto &i=m.instructions[k];const auto *p=m.w+i.offset;
        if(i.op==Function || i.op==FunctionParameter || i.op==Label || i.op==FunctionEnd)continue;
        if(i.op==Variable) {put(memory,i.id,{});continue;}
        if(i.op==Load && i.wc==4) {
            if(p[3]==m.lane_var) {put(env,i.id,{1,lane,0});continue;}
            auto *cell=get(memory,p[3]);if(!cell)return {};put(env,i.id,*cell);continue;
        }
        if(i.op==Store && i.wc==3) {auto *cell=get(memory,p[1]);if(!cell)return {};*cell=val(p[2]);continue;}
        if(i.op==ReturnValue && i.wc==2)return val(p[1]);
        if((i.op==Bitcast || i.op==CopyObject) && i.wc==4) {
            const auto *src=m.def(p[3]);if(!scalar32(m,i.type) || !src || !scalar32(m,src->type))return {};
            put(env,i.id,val(p[3]));continue;
        }
        if(i.op==Shuffle && i.wc==6) {
            uint32_t scope=0;LaneValue v=val(p[4]),index=val(p[5]);
            if(!m.constant(p[3],scope) || scope!=3 || v.kind!=2 || index.kind!=1)return {};
            put(env,i.id,{3,v.a,index.a});continue;
        }
        if(i.op==Select && i.wc==6) {LaneValue cnd=val(p[3]);if(cnd.kind!=1)return {};put(env,i.id,cnd.a?val(p[4]):val(p[5]));continue;}
        if(i.wc==5 || (i.wc==4 && (i.op==Not || i.op==LogicalNot))) {
            LaneValue x=val(p[3]),y=i.wc==5?val(p[4]):LaneValue{1,0,0};if(x.kind!=1 || y.kind!=1)return {};
            uint32_t a=x.a,b=y.a,r;int32_t sa=int32_t(a),sb=int32_t(b);
            switch(i.op) {
                case BitwiseAnd:r=a&b;break;case BitwiseOr:r=a|b;break;case BitwiseXor:r=a^b;break;case Not:r=~a;break;
                case ShiftRightLogical:r=a>>(b&31);break;case ShiftRightArithmetic:r=uint32_t(sa>>(b&31));break;case ShiftLeftLogical:r=a<<(b&31);break;
                case IAdd:r=a+b;break;case ISub:r=a-b;break;
                case IEqual:r=a==b;break;case INotEqual:r=a!=b;break;case 172:r=a>b;break;case 173:r=sa>sb;break;case 174:r=a>=b;break;case 175:r=sa>=sb;break;
                case 176:r=a<b;break;case 177:r=sa<sb;break;case 178:r=a<=b;break;case 179:r=sa<=sb;break;
                case LogicalEqual:r=(a!=0)==(b!=0);break;case LogicalNotEqual:r=(a!=0)!=(b!=0);break;case LogicalOr:r=a||b;break;case LogicalAnd:r=a&&b;break;case LogicalNot:r=!a;break;
                default:return {};
            }
            put(env,i.id,{1,r,0});continue;
        }
        return {};
    }
    return {};
}
// 调用对 32 道线程束的每条 lane 都返回参数 value 在 lane^x 上的值时，给出 x（1..3），否则 0。
// The x (1..3) when the call returns, for every lane of a 32-lane warp, parameter `value`'s value from lane^x; else 0.
uint32_t quad_xor(const Module &m,const Helper &h,const std::vector<LaneValue> &args,unsigned &value) {
    uint32_t x=0;
    for(uint32_t lane=0;lane<32;++lane) {
        LaneValue r=run_helper(m,h,args,lane);
        if(r.kind!=3 || (r.b^lane)<1 || (r.b^lane)>3 || (lane && ((r.b^lane)!=x || r.a!=value)))return 0;
        x=r.b^lane;value=r.a;
    }
    return x;
}
// 位的来去：只经过搬运（Function 变量、32 位 bitcast、复制、select 的数据臂、被改写调用的值参数）时，来源须是浮点产生者，
// 去处须是浮点用途。
// Where bits come from and go: through moves only (Function variables, 32-bit bitcasts, copies, select data arms, the value
// parameter of a rewritten call), the sources must be float producers and the consumers float uses.
struct BitFlow {
    const Module &m;const std::vector<Var> &vars;const std::vector<uint32_t> &call_value;   // per call instruction: value argument position + 1, 0 if not a rewritten form
    xbg_quad_skip reason=xbg_quad_skip::unknown_source;
    bool output_readback(uint32_t root) const {for(const auto &o:m.instructions)if(o.op==Load && o.wc>=4 && m.pointer_root(m.w[o.offset+3])==root)return true;return false;}
    bool source(uint32_t start) {
        std::vector<uint8_t> seen(m.bound),seen_var(m.bound);std::vector<uint32_t> work={start};
        while(!work.empty()) {
            uint32_t id=work.back();work.pop_back();if(!id || id>=m.bound) {reason=xbg_quad_skip::unknown_source;return false;}
            if(seen[id])continue;seen[id]=1;
            const auto *i=m.def(id);if(!i) {reason=xbg_quad_skip::unknown_source;return false;}
            const auto *p=m.w+i->offset;
            if(i->op==Constant) {
                if(m.float_type(i->type))continue;
                if(scalar32(m,i->type) && i->wc==4 && ((p[3]>>23)&255)!=255)continue;   // integer bits that are a finite float
                reason=xbg_quad_skip::integer_flow;return false;
            }
            if(i->op==Bitcast || i->op==CopyObject) {
                const auto *s=m.def(p[3]);if(!scalar32(m,i->type) || !s || !scalar32(m,s->type)) {reason=xbg_quad_skip::bit_pattern;return false;}
                work.push_back(p[3]);continue;
            }
            if(i->op==Select && i->wc==6) {work.push_back(p[4]);work.push_back(p[5]);continue;}
            if(i->op==Load && i->wc>=4) {
                uint32_t root=m.pointer_root(p[3]);
                if(root && vars[root].local && root==p[3]) {
                    for(auto call:vars[root].calls)if(call_value[call.first]!=call.second+1) {reason=xbg_quad_skip::memory_flow;return false;}
                    if(!seen_var[root]) {seen_var[root]=1;for(size_t k:vars[root].stores)work.push_back(m.w[m.instructions[k].offset+2]);}
                    continue;
                }
                uint32_t storage=m.storage(p[3]);
                if((storage==Input || storage==Uniform || storage==PushConstant) && m.float_type(i->type))continue;
                reason=xbg_quad_skip::memory_flow;return false;
            }
            if(i->op>=87 && i->op<=98 && m.float_type(i->type))continue;
            if(i->op==FunctionCall) {
                size_t k=size_t(i-&m.instructions[0]);
                if(call_value[k]) {uint32_t arg=p[4+call_value[k]-1];if(!seen_var[arg]) {seen_var[arg]=1;for(size_t s:vars[arg].stores)work.push_back(m.w[m.instructions[s].offset+2]);}continue;}
                reason=xbg_quad_skip::unknown_source;return false;
            }
            if(m.float_type(i->type) && (numeric_op(i->op) || numeric_ext(m,*i))) {for(uint32_t o:operands(m,*i))work.push_back(o);continue;}
            reason=m.int_type(i->type)?xbg_quad_skip::integer_flow:xbg_quad_skip::unknown_source;return false;
        }
        return true;
    }
    // 放宽模式：值是否只来自浮点转整数（经 Function 变量、CopyObject、32 位整数间的 Bitcast、Select 和整数常量）。
    // 1 = 有符号（OpConvertFToS），2 = 无符号（OpConvertFToU），0 = 别的来源或两种都有。
    // Relaxed mode: whether a value comes only from float-to-integer conversions (through Function variables, CopyObject,
    // Bitcast between 32-bit integers, Select and integer constants). 1 = signed (OpConvertFToS), 2 = unsigned
    // (OpConvertFToU), 0 = any other source, or both kinds.
    int integer_source(uint32_t start) {
        std::vector<uint8_t> seen(m.bound),seen_var(m.bound);std::vector<uint32_t> work={start};bool sign=false,unsign=false;
        while(!work.empty()) {
            uint32_t id=work.back();work.pop_back();if(!id || id>=m.bound)return 0;
            if(seen[id])continue;seen[id]=1;
            const auto *i=m.def(id);if(!i || !scalar32(m,i->type) || !m.int_type(i->type))return 0;
            const auto *p=m.w+i->offset;
            if(i->op==Constant)continue;
            if(i->op==ConvertFToS && i->wc==4) {sign=true;continue;}
            if(i->op==ConvertFToU && i->wc==4) {unsign=true;continue;}
            if(i->op==Bitcast || i->op==CopyObject) {work.push_back(p[3]);continue;}
            if(i->op==Select && i->wc==6) {work.push_back(p[4]);work.push_back(p[5]);continue;}
            if(i->op==Load && i->wc>=4) {
                uint32_t root=m.pointer_root(p[3]);
                if(!root || !vars[root].local || root!=p[3])return 0;
                for(auto call:vars[root].calls)if(call_value[call.first]!=call.second+1)return 0;
                if(!seen_var[root]) {seen_var[root]=1;for(size_t k:vars[root].stores)work.push_back(m.w[m.instructions[k].offset+2]);}
                continue;
            }
            return 0;
        }
        return sign==unsign?0:sign?1:2;
    }
    // 放宽模式：值的直接来源（越过 Bitcast、CopyObject、Select 和 Function 变量）是否都是浮点运算、浮点常量或浮点读取。
    // 比 source() 宽：不追溯浮点运算的操作数，也不管读取来自哪种存储。
    // Relaxed mode: whether the value's immediate producers (past Bitcast, CopyObject, Select and Function variables) are all
    // float arithmetic, float constants or float loads. Wider than source(): the operands of float arithmetic are not traced,
    // nor the storage a load reads.
    bool float_producers(uint32_t start) {
        std::vector<uint8_t> seen(m.bound),seen_var(m.bound);std::vector<uint32_t> work={start};
        while(!work.empty()) {
            uint32_t id=work.back();work.pop_back();if(!id || id>=m.bound)return false;
            if(seen[id])continue;seen[id]=1;
            const auto *i=m.def(id);if(!i || !scalar32(m,i->type))return false;
            const auto *p=m.w+i->offset;
            if(i->op==Bitcast || i->op==CopyObject) {work.push_back(p[3]);continue;}
            if(i->op==Select && i->wc==6) {work.push_back(p[4]);work.push_back(p[5]);continue;}
            if(i->op==Load && i->wc>=4) {
                uint32_t root=m.pointer_root(p[3]);
                if(root && vars[root].local && root==p[3]) {
                    for(auto call:vars[root].calls)if(call_value[call.first]!=call.second+1)return false;
                    if(!seen_var[root]) {seen_var[root]=1;for(size_t k:vars[root].stores)work.push_back(m.w[m.instructions[k].offset+2]);}
                    continue;
                }
                if(m.float_type(i->type))continue;
                return false;
            }
            if(m.float_type(i->type) && (i->op==Constant || numeric_op(i->op) || numeric_ext(m,*i)))continue;
            return false;
        }
        return true;
    }
    bool uses(uint32_t start) {
        std::vector<uint8_t> seen(m.bound),seen_var(m.bound);std::vector<uint32_t> work={start};
        while(!work.empty()) {
            uint32_t id=work.back();work.pop_back();if(seen[id])continue;seen[id]=1;
            for(size_t index:m.uses[id]) {
                const auto &i=m.instructions[index];const auto *p=m.w+i.offset;
                if(i.op==Store) {
                    if(i.wc!=3 || p[2]!=id) {reason=xbg_quad_skip::memory_flow;return false;}
                    uint32_t root=m.pointer_root(p[1]);
                    if(root && vars[root].local && root==p[1]) {
                        for(auto call:vars[root].calls) {
                            if(call_value[call.first]!=call.second+1) {reason=xbg_quad_skip::memory_flow;return false;}
                            work.push_back(m.instructions[call.first].id);
                        }
                        if(!seen_var[root]) {seen_var[root]=1;for(size_t k:vars[root].loads)work.push_back(m.instructions[k].id);}
                        continue;
                    }
                    if(m.storage(p[1])==Output && !output_readback(root))continue;
                    reason=xbg_quad_skip::memory_flow;return false;
                }
                if(i.op==Bitcast || i.op==CopyObject) {if(!scalar32(m,i.type)) {reason=xbg_quad_skip::bit_pattern;return false;}work.push_back(i.id);continue;}
                if(i.op==Select) {if(i.wc!=6 || p[3]==id) {reason=xbg_quad_skip::unknown_use;return false;}work.push_back(i.id);continue;}
                if(i.op==IsNan || i.op==IsInf || (i.op>=180 && i.op<=191))continue;
                if(i.op>=87 && i.op<=94) {const auto *d=m.def(id);if(d && m.float_type(d->type))continue;reason=xbg_quad_skip::integer_flow;return false;}
                if(i.id && m.float_type(i.type) && (numeric_op(i.op) || numeric_ext(m,i)))continue;
                reason=m.int_type(i.type)?xbg_quad_skip::integer_flow:xbg_quad_skip::unknown_use;return false;
            }
        }
        return true;
    }
};
// kind：0 已证明是浮点数值；1 放宽后按浮点（结果另有按位用途，或值的直接来源是浮点运算 / 读取）；2 / 3 值来自有符号 /
// 无符号的浮点转整数（放宽）。
// kind: 0 proven float values; 1 relaxed, as floats (the result also has bit uses, or the value's immediate producers are
// float arithmetic or loads); 2 / 3 the value comes from a signed / unsigned float-to-integer conversion (relaxed).
struct CallPlan {size_t index;uint32_t value_var,mask;uint8_t kind=0;};
std::vector<CallPlan> plan_calls(const Module &m,bool contract,xbg_spirv_quad_stats &s,bool relaxed) {
    std::vector<CallPlan> plans;std::vector<uint32_t> helper_fns;
    for(const auto &i:m.instructions)if(i.op==Shuffle && i.fn && i.fn!=m.entry_fn && std::find(helper_fns.begin(),helper_fns.end(),i.fn)==helper_fns.end())helper_fns.push_back(i.fn);
    if(helper_fns.empty())return plans;
    auto skip=[&](xbg_quad_call_skip reason){++s.call_skipped[static_cast<size_t>(reason)];};
    std::vector<Helper> helpers;for(uint32_t f:helper_fns)helpers.push_back(helper_body(m,f));
    auto vars=local_vars(m,m.entry_fn);std::vector<uint32_t> call_value(m.instructions.size(),0);
    struct Candidate {size_t index;unsigned value;uint32_t x;};std::vector<Candidate> candidates;
    for(size_t k=0;k<m.instructions.size();++k) {
        const auto &i=m.instructions[k];if(i.op!=FunctionCall || i.fn!=m.entry_fn || i.wc<4)continue;
        const auto *p=m.w+i.offset;size_t h=std::find(helper_fns.begin(),helper_fns.end(),p[3])-helper_fns.begin();
        if(h==helper_fns.size())continue;
        ++s.calls;
        if(!contract) {skip(xbg_quad_call_skip::module_contract);continue;}
        const Helper &helper=helpers[h];
        if(!helper.straight || helper.params.size()!=i.wc-4) {skip(xbg_quad_call_skip::helper);continue;}
        // 实参须是主函数自己的 Function 变量；所有存入都是同一个 32 位常量的，按常量求值，其余当作符号。
        // Arguments must be the entry function's own Function variables; one whose stores all store the same 32-bit constant
        // is evaluated as that constant, any other as a symbol.
        std::vector<LaneValue> args;bool plain=true;
        for(unsigned j=0;j+4<i.wc;++j) {
            uint32_t arg=p[4+j];if(arg>=m.bound || !vars[arg].local || vars[arg].chained) {plain=false;break;}
            bool constant=!vars[arg].stores.empty();uint32_t value=0;
            for(size_t n=0;n<vars[arg].stores.size() && constant;++n) {uint32_t c=0;if(!m.constant(m.w[m.instructions[vars[arg].stores[n]].offset+2],c) || (n && c!=value))constant=false;value=c;}
            args.push_back(constant?LaneValue{1,value,0}:LaneValue{2,j,0});
        }
        if(!plain) {skip(xbg_quad_call_skip::arguments);continue;}
        unsigned value=0;uint32_t x=quad_xor(m,helper,args,value);
        if(!x || args[value].kind!=2 || helper.written[value]) {skip(xbg_quad_call_skip::helper);continue;}
        // 辅助函数写出的实参（D2R 里是 valid 标志）主函数不得再读。
        // The entry function must not read an argument the helper writes (D2R's valid flag).
        bool unread=true;for(size_t j=0;j<helper.written.size();++j)if(helper.written[j] && !vars[p[4+j]].loads.empty())unread=false;
        if(!unread) {skip(xbg_quad_call_skip::arguments);continue;}
        call_value[k]=value+1;candidates.push_back({k,value,x});
    }
    if(candidates.empty())return plans;
    Cfg cfg=build_cfg(m,m.entry_fn);
    if(!cfg.valid) {for(size_t n=0;n<candidates.size();++n)skip(xbg_quad_call_skip::control_flow);return plans;}
    Uniformity uniform(m,cfg,vars);BitFlow flow{m,vars,call_value};
    for(const auto &c:candidates) {
        const auto &i=m.instructions[c.index];const auto *p=m.w+i.offset;int b=cfg.block_of[c.index];
        if(b<0 || !uniform.block_ok[size_t(b)]) {skip(xbg_quad_call_skip::control_flow);continue;}
        uint32_t var=p[4+c.value];const auto *v=m.def(var);
        if(!scalar32(m,i.type) || !v || !scalar32(m,m.types[v->type].b)) {skip(xbg_quad_call_skip::source);continue;}
        bool numeric=!vars[var].stores.empty();
        for(size_t k:vars[var].stores)if(!flow.source(m.w[m.instructions[k].offset+2])) {numeric=false;break;}
        uint8_t kind=0;
        if(!numeric) {
            if(!relaxed || vars[var].stores.empty()) {skip(xbg_quad_call_skip::source);continue;}
            int integer=m.int_type(i.type)?3:0;
            for(size_t k:vars[var].stores) {if(!integer)break;int one=flow.integer_source(m.w[m.instructions[k].offset+2]);if(!one || (integer!=3 && integer!=one))integer=0;else integer=one;}
            bool floats=true;
            for(size_t k:vars[var].stores)if(!flow.float_producers(m.w[m.instructions[k].offset+2])) {floats=false;break;}
            if(integer==1 || integer==2)kind=uint8_t(integer+1);
            else if(floats)kind=1;
            else {skip(xbg_quad_call_skip::source);continue;}
        }
        if(!kind && !flow.uses(i.id)) {if(!relaxed) {skip(xbg_quad_call_skip::use);continue;}kind=1;}
        plans.push_back({c.index,var,c.x,kind});
    }
    return plans;
}
}

const char *xbg_spirv_quad_skip_name(xbg_quad_skip reason) {
    static const char *const names[]={"malformed","module_contract","non_float","lane_index","control_flow","bit_pattern","integer_flow","unknown_source","unknown_use","memory_flow"};
    auto index=static_cast<size_t>(reason);return index<static_cast<size_t>(xbg_quad_skip::count)?names[index]:"invalid_reason";
}
const char *xbg_spirv_quad_call_skip_name(xbg_quad_call_skip reason) {
    static const char *const names[]={"module_contract","helper","arguments","control_flow","source","use"};
    auto index=static_cast<size_t>(reason);return index<static_cast<size_t>(xbg_quad_call_skip::count)?names[index]:"invalid_reason";
}

size_t xbg_spirv_quad_derivatives(const uint32_t *in,size_t n,std::vector<uint32_t> &out,xbg_spirv_quad_stats *stats,bool relaxed) {
    out.clear();xbg_spirv_quad_stats local;auto &s=stats?*stats:local;s={};
    auto skip=[&](xbg_quad_skip reason){++s.skipped[static_cast<size_t>(reason)];};
    if(!in || n<5 || in[0]!=0x07230203 || !in[3] || in[3]>(1u<<22) || n>(1u<<24)) {skip(xbg_quad_skip::malformed);return 0;}
    Module m(in,n);if(!parse(m)) {skip(xbg_quad_skip::malformed);return 0;}
    struct Plan {size_t index;uint32_t mask;};std::vector<Plan> plans;
    bool contract=m.entry_count==1 && m.fragment && m.origin_upper && m.first_type && m.first_func && m.first_cap && m.t_float && m.t_uint && m.lane_var;
    const auto *lane=m.def(m.lane_var);
    contract=contract && lane && lane->op==Variable && lane->type<m.bound && m.types[lane->type].op==TypePointer &&
        m.types[lane->type].a==Input && m.int_type(m.types[lane->type].b) && m.types[m.types[lane->type].b].a==32;
    if(m.coord_var) {
        const auto *coord=m.def(m.coord_var);
        contract=contract && coord && coord->op==Variable && coord->type<m.bound && m.types[coord->type].op==TypePointer &&
            m.types[coord->type].a==Input && m.types[coord->type].b==m.t_vec4;
    }
    for(size_t index=0;index<m.instructions.size();++index) {
        const auto &i=m.instructions[index];if(i.op!=Shuffle)continue;++s.candidates;
        const auto *p=in+i.offset;
        if(i.wc!=6) {skip(xbg_quad_skip::malformed);continue;}
        if(i.type!=m.t_float) {skip(xbg_quad_skip::non_float);continue;}
        if(!contract) {skip(xbg_quad_skip::module_contract);continue;}
        if(i.fn!=m.entry_fn || std::find(m.unsafe_functions.begin(),m.unsafe_functions.end(),i.fn)!=m.unsafe_functions.end()) {skip(xbg_quad_skip::control_flow);continue;}
        xbg_quad_skip reason=xbg_quad_skip::unknown_source;std::vector<uint8_t> visited(m.bound);
        if(!source_safe(m,p[4],visited,reason)) {skip(reason);continue;}
        // 来源同时被当作位模式/整数/私有存储使用时也不能推定为纯数值。
        // A source also consumed as bits, integers or private storage is not proven purely numeric.
        std::fill(visited.begin(),visited.end(),0);
        if(!uses_safe(m,p[4],visited,reason)) {skip(reason);continue;}
        std::fill(visited.begin(),visited.end(),0);
        if(!uses_safe(m,p[2],visited,reason)) {skip(reason);continue;}
        uint32_t scope=0,mask=0;const auto *idx=m.def(p[5]);bool index_ok=false;
        if(m.constant(p[3],scope) && scope==3 && idx && idx->op==BitwiseXor && idx->wc==5) {
            const auto *q=in+idx->offset;
            index_ok=(m.lane_value(q[3]) && m.constant(q[4],mask)) || (m.lane_value(q[4]) && m.constant(q[3],mask));
        }
        if(!index_ok || mask<1 || mask>3) {skip(xbg_quad_skip::lane_index);continue;}
        plans.push_back({index,mask});
    }
    auto calls=plan_calls(m,contract,s,relaxed);
    if(plans.empty() && calls.empty())return 0;
    uint32_t bound=m.bound;auto fresh=[&](){return bound++;};
    std::vector<uint32_t> declarations,decorations;
    if(!m.t_bool) {m.t_bool=fresh();declarations.insert(declarations.end(),{word(TypeBool,2),m.t_bool});}
    uint32_t one=fresh(),zero=fresh();
    declarations.insert(declarations.end(),{word(Constant,4),m.t_uint,one,1,word(Constant,4),m.t_uint,zero,0});
    bool new_coord=!m.coord_var;
    if(new_coord) {
        if(!m.t_vec4) {m.t_vec4=fresh();declarations.insert(declarations.end(),{word(TypeVector,4),m.t_vec4,m.t_float,4});}
        if(!m.t_ptr4) {m.t_ptr4=fresh();declarations.insert(declarations.end(),{word(TypePointer,4),m.t_ptr4,Input,m.t_vec4});}
        m.coord_var=fresh();declarations.insert(declarations.end(),{word(Variable,4),m.t_ptr4,m.coord_var,Input});
        decorations.insert(decorations.end(),{word(Decorate,4),m.coord_var,BuiltIn,FragCoord});
    }
    if(bound+(plans.size()+calls.size())*64>0x3fffff) {skip(xbg_quad_skip::malformed);return 0;}
    out.insert(out.end(),in,in+5);size_t next=0,next_call=0;
    auto emit=[&](std::initializer_list<uint32_t> words){out.insert(out.end(),words.begin(),words.end());};
    auto nonfinite=[&](uint32_t value) {
        uint32_t nan=fresh(),inf=fresh(),bad=fresh();
        emit({word(IsNan,4),m.t_bool,nan,value});emit({word(IsInf,4),m.t_bool,inf,value});emit({word(LogicalOr,5),m.t_bool,bad,nan,inf});return bad;
    };
    // 由精细导数求四元组邻居：mask 第 0 位取水平邻居，第 1 位再取其竖直邻居；bad 汇总各步的非有限值。
    // The quad neighbour from fine derivatives: bit 0 of mask takes the horizontal neighbour, bit 1 then that one's vertical
    // neighbour; bad collects the nonfinite values of every step.
    auto neighbours=[&](uint32_t value,uint32_t mask,uint32_t coord,uint32_t *bad) {
        auto merge_bad=[&](uint32_t v){if(!bad)return;uint32_t other=nonfinite(v),combined=fresh();emit({word(LogicalOr,5),m.t_bool,combined,*bad,other});*bad=combined;};
        auto neighbour=[&](uint32_t source,uint32_t axis) {
            uint32_t derivative=fresh(),component=fresh(),integer=fresh(),parity=fresh(),odd=fresh(),negative=fresh(),signed_delta=fresh(),result=fresh();
            emit({word(axis?DPdyFine:DPdxFine,4),m.t_float,derivative,source});
            emit({word(CompositeExtract,5),m.t_float,component,coord,axis});
            emit({word(ConvertFToU,4),m.t_uint,integer,component});
            emit({word(BitwiseAnd,5),m.t_uint,parity,integer,one});
            emit({word(INotEqual,5),m.t_bool,odd,parity,zero});
            emit({word(FNegate,4),m.t_float,negative,derivative});
            emit({word(Select,6),m.t_float,signed_delta,odd,negative,derivative});
            emit({word(FAdd,5),m.t_float,result,source,signed_delta});
            merge_bad(derivative);merge_bad(result);return result;
        };
        if(mask&1)value=neighbour(value,0);
        if(mask&2)value=neighbour(value,1);
        return value;
    };
    for(size_t index=0;index<m.instructions.size();++index) {
        const auto &i=m.instructions[index];const auto *p=in+i.offset;
        if(i.offset==m.first_cap && !m.derivatives)emit({word(Capability,2),51});
        if(i.offset==m.first_type)out.insert(out.end(),decorations.begin(),decorations.end());
        if(i.offset==m.first_func)out.insert(out.end(),declarations.begin(),declarations.end());
        if(i.offset==m.entry_offset && new_coord) {out.push_back(word(EntryPoint,i.wc+1));out.insert(out.end(),p+1,p+i.wc);out.push_back(m.coord_var);continue;}
        if(next<plans.size() && plans[next].index==index) {
            uint32_t mask=plans[next++].mask,coord=fresh();
            if(relaxed) {
                // 放宽模式不留 shuffle 回退：这个阶段根本不能用 shuffle。
                // Relaxed mode keeps no shuffle fallback: this stage cannot use shuffles at all.
                emit({word(Load,4),m.t_vec4,coord,m.coord_var});
                emit({word(CopyObject,4),p[1],p[2],neighbours(p[4],mask,coord,nullptr)});
                ++s.rewritten;
            } else {
                uint32_t original=fresh();
                emit({word(Shuffle,6),p[1],original,p[3],p[4],p[5]});
                emit({word(Load,4),m.t_vec4,coord,m.coord_var});
                uint32_t bad=nonfinite(p[4]),value=neighbours(p[4],mask,coord,&bad);
                emit({word(Select,6),m.t_float,p[2],bad,original,value});
                ++s.rewritten;++s.guarded_nonfinite;
            }
        } else if(next_call<calls.size() && calls[next_call].index==index) {
            // 调用整个换成导数求出的邻居，按位还原成调用的结果类型；不留 shuffle 回退。回退的 shuffle 正是在辅助调用上读到
            // 垃圾的那一步：Redmi K50 Ultra 罪恶洞穴打斗中，留着它黑方块照旧，GPU 18.3 ms；换掉后无黑方块，15.2 ms。原调用在
            // 64 道子组的 32–63 号 lane 上本来也只返回自己的值。自身为非有限值的 lane 得 NaN，自身有限时邻居（含非有限）照取。
            // The call is replaced by the neighbour from derivatives, cast back to the call's result type, with no shuffle
            // fallback. The fallback's shuffle is the very step that reads garbage in helper invocations: in a Den of Evil fight
            // on the Redmi K50 Ultra the black squares stayed with it (GPU 18.3 ms) and went without it (15.2 ms). The original
            // call also returns a lane's own value on lanes 32-63 of a 64-lane subgroup. A lane whose own value is nonfinite gets
            // NaN; a lane whose own value is finite gets its neighbour's, nonfinite or not.
            const auto &plan=calls[next_call++];uint32_t bits=fresh(),coord=fresh(),pointee=m.types[m.def(plan.value_var)->type].b;
            emit({word(Load,4),pointee,bits,plan.value_var});
            uint32_t value=bits;
            // 整数值（放宽）：按数值转成浮点，取邻居后再转回；|v| < 2^24 时导数和加减都是精确的整数运算。
            // Integer values (relaxed): converted to float by value and back after taking the neighbour; for |v| < 2^24 the
            // derivative and the addition are exact integer arithmetic.
            if(plan.kind>=2) {value=fresh();emit({word(plan.kind==2?ConvertSToF:ConvertUToF,4),m.t_float,value,bits});}
            else if(pointee!=m.t_float) {value=fresh();emit({word(Bitcast,4),m.t_float,value,bits});}
            emit({word(Load,4),m.t_vec4,coord,m.coord_var});
            value=neighbours(value,plan.mask,coord,nullptr);
            if(plan.kind>=2)emit({word(plan.kind==2?ConvertFToS:ConvertFToU,4),p[1],p[2],value});
            else emit({word(p[1]!=m.t_float?Bitcast:CopyObject,4),p[1],p[2],value});
            ++s.calls_rewritten;if(plan.kind==1)++s.calls_relaxed_float;if(plan.kind>=2)++s.calls_integer;
        } else out.insert(out.end(),p,p+i.wc);
    }
    out[3]=bound;return s.rewritten+s.calls_rewritten;
}
size_t xbg_spirv_quad_derivatives(const uint32_t *in,size_t n,std::vector<uint32_t> &out,xbg_spirv_quad_stats *stats) {
    return xbg_spirv_quad_derivatives(in,n,out,stats,false);
}
size_t xbg_spirv_quad_derivatives(const uint32_t *in,size_t n,std::vector<uint32_t> &out) {
    return xbg_spirv_quad_derivatives(in,n,out,nullptr,false);
}
size_t xbg_spirv_strip_subgroup(const uint32_t *in,size_t n,std::vector<uint32_t> &out) {
    out.clear();
    if(!in || n<5 || in[0]!=0x07230203 || !in[3] || in[3]>(1u<<22) || n>(1u<<24))return 0;
    const uint32_t bound=in[3];
    struct Ins {size_t off;uint32_t op,wc,fn;};std::vector<Ins> ins;
    std::vector<uint8_t> is_type(bound),is_function(bound),reachable(bound),removed_id(bound),subgroup_var(bound),drop_var(bound);
    std::vector<uint32_t> entries;std::vector<std::pair<uint32_t,uint32_t>> edges;
    uint32_t fn=0;
    for(size_t off=5;off<n;) {
        uint32_t wc=in[off]>>16,op=in[off]&65535;if(!wc || wc>n-off)return 0;
        const uint32_t *p=in+off;
        // 装饰组会把注解间接挂到 id 上，这里不跟踪，遇到就不动这个模块。
        // Decoration groups attach annotations to ids indirectly; they are not tracked, so such a module is left alone.
        if(op==73 || op==74 || op==75)return 0;
        if(op==Function) {if(wc!=5 || !p[2] || p[2]>=bound)return 0;fn=p[2];is_function[fn]=1;}
        ins.push_back({off,op,wc,fn});
        if(op==FunctionEnd)fn=0;
        if(op>=19 && op<=39 && wc>=2 && p[1]<bound)is_type[p[1]]=1;
        if(op==EntryPoint && wc>=3 && p[2]<bound)entries.push_back(p[2]);
        if(op==FunctionCall && fn && wc>=4 && p[3]<bound)edges.push_back({fn,p[3]});
        if(op==Decorate && wc>=4 && p[1]<bound && p[2]==BuiltIn && (p[3]==36 || p[3]==Lane || (p[3]>=4416 && p[3]<=4420)))subgroup_var[p[1]]=1;
        off+=wc;
    }
    if(fn)return 0;
    // 入口点能调到的函数。| Functions the entry points can reach.
    std::vector<uint32_t> work=entries;
    while(!work.empty()) {uint32_t f=work.back();work.pop_back();if(reachable[f])continue;reachable[f]=1;for(const auto &e:edges)if(e.first==f && !reachable[e.second])work.push_back(e.second);}
    size_t removed=0;
    for(uint32_t id=1;id<bound;++id)if(is_function[id] && !reachable[id])++removed;
    // 被删函数里定义的 id（OpLabel 的 id 在第 1 个字，带结果类型的指令在第 2 个字）。
    // Ids defined inside removed functions (OpLabel's id is word 1; an instruction with a result type has it in word 2).
    for(const auto &i:ins) {
        if(!i.fn || reachable[i.fn])continue;const uint32_t *p=in+i.off;
        if(i.op==Label && i.wc>=2 && p[1]<bound)removed_id[p[1]]=1;
        else if(i.wc>=3 && p[1]<bound && is_type[p[1]] && p[2]<bound)removed_id[p[2]]=1;
    }
    // 剩下的子组指令，和剩下代码里对子组内置变量的指针用法。
    // Subgroup instructions that remain, and pointer uses of subgroup built-in variables in the remaining code.
    size_t group_ops=0;std::vector<uint8_t> used(bound);
    for(const auto &i:ins) {
        if(i.fn && !reachable[i.fn])continue;const uint32_t *p=in+i.off;
        if(i.op>=333 && i.op<=366)++group_ops;
        auto use=[&](uint32_t id){if(id<bound)used[id]=1;};
        if((i.op==Load || i.op==AccessChain || i.op==InBoundsAccessChain || i.op==67 || i.op==70 || i.op==CopyObject) && i.wc>=4)use(p[3]);
        if((i.op==Store || i.op==63) && i.wc>=3) {use(p[1]);use(p[2]);}
        if(i.op==FunctionCall)for(uint32_t j=4;j<i.wc;++j)use(p[j]);
    }
    bool any_var_left=false;
    for(uint32_t id=1;id<bound;++id)if(subgroup_var[id]) {if(used[id])any_var_left=true;else {drop_var[id]=1;++removed;}}
    const bool drop_caps=!group_ops && !any_var_left;
    for(const auto &i:ins)if(drop_caps && i.op==Capability && i.wc==2 && in[i.off+1]>=61 && in[i.off+1]<=68)++removed;
    if(!removed)return 0;
    out.insert(out.end(),in,in+5);
    for(const auto &i:ins) {
        const uint32_t *p=in+i.off;
        if(i.fn && !reachable[i.fn])continue;
        if(drop_caps && i.op==Capability && i.wc==2 && p[1]>=61 && p[1]<=68)continue;
        // 注解与名字：目标是被删的 id 或变量时一起删（OpName、OpDecorate、OpDecorateId、OpDecorateString）。
        // Annotations and names go with a removed id or variable (OpName, OpDecorate, OpDecorateId, OpDecorateString).
        if((i.op==5 || i.op==Decorate || i.op==332 || i.op==5632) && i.wc>=2 && p[1]<bound && (removed_id[p[1]] || drop_var[p[1]]))continue;
        if(i.op==Variable && !i.fn && i.wc>=3 && p[2]<bound && drop_var[p[2]])continue;
        if(i.op==EntryPoint) {
            // 名字是以 0 结尾的字符串，占到含 0 字节的那个字为止；其后是接口变量。
            // The name is a nul-terminated string ending in the word that holds a zero byte; the interface variables follow.
            uint32_t j=3;while(j<i.wc) {uint32_t v=p[j++];if(!(v&0xff) || !(v&0xff00) || !(v&0xff0000) || !(v&0xff000000))break;}
            std::vector<uint32_t> kept(p,p+j);for(;j<i.wc;++j)if(p[j]>=bound || !drop_var[p[j]])kept.push_back(p[j]);
            kept[0]=word(EntryPoint,uint32_t(kept.size()));out.insert(out.end(),kept.begin(),kept.end());continue;
        }
        out.insert(out.end(),p,p+i.wc);
    }
    return removed;
}
