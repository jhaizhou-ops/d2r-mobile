#include "air_compiler.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <regex>
static int errors=0;
static std::string run(const char *name,const char *source,bool expected){auto *r=air_compile_v2(reinterpret_cast<const uint8_t*>(source),std::strlen(source),18,AIR_LLVM_TEXT,1);bool ok=r&&(air_result_status(r)==AIR_OK);if(ok!=expected){++errors;std::fprintf(stderr,"FAIL %s: %.*s\n",name,r?(int)air_result_diagnostics_size(r):0,r?air_result_diagnostics(r):"");}std::string ir=ok?std::string(reinterpret_cast<const char*>(air_result_data(r)),air_result_size(r)):"";air_result_free(r);return ir;}
static bool convergent(const std::string &ir,const std::string &symbol) {
 std::istringstream lines(ir);std::string line;unsigned count=0;
 while(std::getline(lines,line)) if(line.find("@"+symbol+"(")!=std::string::npos && (line.find("call ")!=std::string::npos || line.find("declare ")==0 || line.find("define ")==0)) {
  std::smatch match;if(!std::regex_search(line,match,std::regex("#([0-9]+)")))return false;
  std::regex group("attributes #"+match[1].str()+" = \\{[^}]*convergent[^}]*\\}");if(!std::regex_search(ir,group))return false;++count;
 }
 return count>=2;
}
int main(){
 auto fragment=run("fragment derivatives/discard","using namespace metal; struct Out {float4 color [[color(0)]];}; fragment Out f(constant float *i [[buffer(0)]]){float d=dfdx(i[0])+dfdy(i[1]);if(d>1)discard_fragment();return Out{float4(d)};}",true);
 for(auto symbol:{"air.dfdx.f32","air.dfdy.f32","air.discard_fragment","convergent"})if(fragment.find(symbol)==std::string::npos)++errors;
 if(!convergent(fragment,"air.dfdx.f32")||!convergent(fragment,"air.dfdy.f32"))++errors;
 auto kernel=run("compute shuffle","using namespace metal; kernel void k(device float *o [[buffer(0)]],constant ushort *i [[buffer(1)]]) {o[0]=simd_shuffle(o[0],i[0]);}",true);
 if(kernel.find("air.simd_shuffle.f32")==std::string::npos||kernel.find("convergent")==std::string::npos)++errors;
 if(!convergent(kernel,"air.simd_shuffle.f32"))++errors;
 run("kernel derivative rejected","using namespace metal; kernel void k(device float *o [[buffer(0)]]){o[0]=dfdx(o[0]);}",false);
 run("kernel discard rejected","using namespace metal; kernel void k(device float *o [[buffer(0)]]){if(o[0]>0)discard_fragment();}",false);
 auto fragmentShuffle=run("fragment shuffle","using namespace metal;struct Out{float4 c[[color(0)]];};fragment Out f(constant float *i[[buffer(0)]]){return Out{float4(simd_shuffle(i[0],(ushort)0))};}",true);
 if(!convergent(fragmentShuffle,"air.simd_shuffle.f32"))++errors;
 run("vertex shuffle unestablished","using namespace metal;struct Out{float4 p[[position]];};vertex Out f(constant float *i[[buffer(0)]]){return Out{float4(simd_shuffle(i[0],(ushort)0))};}",false);
 run("helper derivative rejected","using namespace metal;__attribute__((noinline)) float helper(float x){return dfdx(x);}kernel void k(device float *o[[buffer(0)]]){o[0]=helper(o[0]);}",false);
 auto integerShuffle=run("uint shuffle preserves payload","using namespace metal; kernel void k(device uint *o[[buffer(0)]]){o[0]=simd_shuffle(o[0],(ushort)0);}",true);
 if(!convergent(integerShuffle,"air.simd_shuffle.f32") || std::regex_search(integerShuffle,std::regex("uitofp|sitofp|fptoui|fptosi|air\\.convert\\.f\\.")))++errors;
 // 两个有限浮点数包携带精确的低/高 16 位载荷。
 // Two finite float packets carry the exact low and high 16-bit payloads.
 if(integerShuffle.find("65535")==std::string::npos || integerShuffle.find("1065353216")==std::string::npos)++errors;
 run("unsupported half shuffle","using namespace metal; kernel void k(device half *o[[buffer(0)]]){o[0]=simd_shuffle(o[0],(ushort)0);}",false);
 run("user symbol rejected","float dx(float) __asm__(\"air.dfdx.f32\");",false);
 auto nan=run("NaN survives fast math","using namespace metal;kernel void k(device uint *o[[buffer(0)]],constant float *i[[buffer(1)]]){o[0]=isnan(i[0]);}",true);if(nan.find("2139095040")==std::string::npos||nan.find("2147483647")==std::string::npos)++errors;
 auto fragmentHelper=run("fragment unannotated helper", "using namespace metal;__attribute__((noinline)) float helper(float x){return dfdx(x)+dfdy(x);}struct Out{float4 c[[color(0)]];};fragment Out main0(constant float *i[[buffer(0)]]){return Out{float4(helper(i[0]))};}",true);
 if(!convergent(fragmentHelper,"_Z6helperf"))++errors;
 auto shuffleHelper=run("compute unannotated helper", "using namespace metal;__attribute__((noinline)) float helper(float x,ushort i){return simd_shuffle(x,i);}kernel void main0(device float *o[[buffer(0)]],constant ushort *i[[buffer(1)]]){o[0]=helper(o[0],i[0]);}",true);
 if(!convergent(shuffleHelper,"_Z6helperft"))++errors;
 std::printf("Control/profile suite: %s\n",errors?"FAIL":"PASS");return errors?1:0;
}
