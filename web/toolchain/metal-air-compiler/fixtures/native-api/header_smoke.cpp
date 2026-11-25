#include "air_compiler.h"
#include <cstring>
#include <cstdio>
#include <string>
#include <fstream>
#include <iterator>
#include "operations_smoke.h"
static bool check(const char *name,const char *source,unsigned profile,bool success,const char *symbol=nullptr) {
 auto *r=air_compile_v2(reinterpret_cast<const uint8_t*>(source),std::strlen(source),18,AIR_LLVM_TEXT,profile);
 bool ok=r && ((air_result_status(r)==AIR_OK)==success);
 if(ok&&success&&symbol) { std::string ir(reinterpret_cast<const char*>(air_result_data(r)),air_result_size(r));ok=ir.find(symbol)!=std::string::npos; }
 if(!ok) std::fprintf(stderr,"FAIL %s: %.*s\n",name,r?(int)air_result_diagnostics_size(r):0,r?air_result_diagnostics(r):"");
 air_result_free(r);return ok;
}
int main(int argc,char **argv){int failures=0;
 if(argc==3) { std::ifstream input(argv[1],std::ios::binary);std::string source((std::istreambuf_iterator<char>(input)),{});auto *r=air_compile_v2(reinterpret_cast<const uint8_t*>(source.data()),source.size(),18,AIR_METALLIB,1);if(!r||air_result_status(r)!=AIR_OK){if(r)std::fprintf(stderr,"%.*s",(int)air_result_diagnostics_size(r),air_result_diagnostics(r));air_result_free(r);return 1;}std::ofstream output(argv[2],std::ios::binary);output.write(reinterpret_cast<const char*>(air_result_data(r)),air_result_size(r));air_result_free(r);return output?0:1;}
 const char *math="#include <metal_stdlib>\n#include <simd/simd.h>\nusing namespace metal; kernel void k(device float *o [[buffer(0)]]) {o[0]=sqrt(fma(o[0],o[1],o[2]));}";
 failures+=!check("explicit fast",math,1,true,"call fast");
 failures+=!check("conservative rejects unsupported sqrt",math,0,false);
 failures+=!check("invalid profile",math,2,false);
 failures+=!check("float literal profile", "static_assert(__is_same(decltype(1.0),float), \"expected float literal\"); kernel void k(device float *o [[buffer(0)]]){o[0]=metal::fast::max(o[0],100.0);}",1,true,"air.fast_fmax.f32");
 failures+=!check("direct user label","float bad(float) __asm__(\"air.fast_sqrt.f32\");",1,false);
 failures+=!check("forged line","#line 1 \"/include/metal_math\"\nfloat bad(float) __asm__(\"air.fast_sqrt.f32\");",1,false);
 failures+=!check("user redeclaration","namespace metal { namespace __air_detail { float sqrt(float); } }",1,false);
 failures+=!check("user definition","namespace metal { namespace __air_detail { float sqrt(float x) {return x;} } }",1,false);
 failures+=!check("macro assembly label","#define LABEL __asm__(\"air.fast_sqrt.f32\")\nfloat bad(float) LABEL;",1,false);
 failures+=!check("ordinary overload","float helper(float x){return x+x;} kernel void k(device float *o [[buffer(0)]]){o[0]=helper(o[0]);}",1,true);
 for(int i=0;i<3;++i) failures+=!check("repeat",math,1,true,"air.fast_sqrt.f32");
 if(air_compiler_header_bundle_version()!=2 || std::strlen(air_compiler_header_sha256())!=64) ++failures;
 failures+=checkMissingOperations();
 std::printf("Header/profile checks: %s; bundle %s\n",failures?"FAIL":"PASS",air_compiler_header_sha256());return failures?1:0;
}
