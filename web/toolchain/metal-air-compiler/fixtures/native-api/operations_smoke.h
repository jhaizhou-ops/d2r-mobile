// SPDX-License-Identifier: MIT
// Independently authored regression sources, not SDK or application shaders.
#ifndef D2R_AIR_OPERATIONS_SMOKE_H
#define D2R_AIR_OPERATIONS_SMOKE_H
#include <regex>
#include <sstream>

static std::string operationIR(const char *name, const std::string &source,
                               unsigned profile, unsigned expectedStatus,
                               int &failures, const char *diagnostic = nullptr) {
  auto *result = air_compile_v2(reinterpret_cast<const uint8_t *>(source.data()),
      source.size(), 18, AIR_LLVM_TEXT, profile);
  bool ok = result && air_result_status(result) == expectedStatus;
  const std::string diagnostics = result ? std::string(air_result_diagnostics(result),
      air_result_diagnostics_size(result)) : "No compilation result";
  if (diagnostic) ok = ok && diagnostics.find(diagnostic) != std::string::npos;
  if (expectedStatus != AIR_OK) ok = ok && result && air_result_size(result) == 0;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL %s: %s\n", name, diagnostics.c_str());
  }
  std::string ir;
  if (result && air_result_status(result) == AIR_OK)
    ir.assign(reinterpret_cast<const char *>(air_result_data(result)), air_result_size(result));
  air_result_free(result);
  return ir;
}

static void requireOperation(bool condition, const char *name, int &failures) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", name);
  }
}

static bool operationSignature(const std::string &ir, const std::string &symbol,
                               const char *resultType, const char *parameterType) {
  std::istringstream lines(ir);
  std::string line;
  while (std::getline(lines, line)) {
    const auto position = line.find("@" + symbol + "(");
    if (position == std::string::npos || line.find("declare ") != 0) continue;
    const auto prefix = line.substr(0, position);
    const auto arguments = line.substr(position + symbol.size() + 2);
    return std::regex_search(prefix, std::regex(std::string("\\b") + resultType + "\\s+$")) &&
        std::regex_search(arguments, std::regex(std::string("^") + parameterType + "\\b[^,()]*\\)"));
  }
  return false;
}

static bool operationConvergent(const std::string &ir, const std::string &symbol) {
  std::istringstream lines(ir);
  std::string line;
  unsigned count = 0;
  while (std::getline(lines, line)) {
    if (line.find("@" + symbol + "(") == std::string::npos ||
        (line.find("call ") == std::string::npos && line.find("declare ") != 0 &&
         line.find("define ") != 0)) continue;
    std::smatch number;
    if (!std::regex_search(line, number, std::regex("#([0-9]+)"))) return false;
    std::smatch group;
    if (!std::regex_search(ir, group,
        std::regex("attributes #" + number[1].str() + " = \\{([^}]*)\\}"))) return false;
    const auto attributes = group[1].str();
    if (attributes.find("convergent") == std::string::npos ||
        std::regex_search(attributes, std::regex("\\b(readnone|readonly|speculatable)\\b"))) return false;
    ++count;
  }
  // 调用点及其声明/定义必须同时存在。
  // Require both a call site and its declaration/definition.
  return count >= 2;
}

static int checkMissingOperations() {
  int failures = 0;
  for (const auto *name : {"sin", "cos", "ceil"}) {
    const std::string operation(name);
    const std::string body = "static_assert(__is_same(decltype(metal::fast::" + operation +
        "(0.0f)),float),\"scalar float signature\"); "
        "kernel void k(device float *o [[buffer(0)]],constant float *i [[buffer(1)]]) {o[0]=";
    const auto ir = operationIR(name, body + "metal::" + operation + "(i[0]);}",
        1, AIR_OK, failures);
    requireOperation(operationSignature(ir, "air.fast_" + operation + ".f32", "float", "float"),
        "exact scalar math AIR signature", failures);
    requireOperation(ir.find("@llvm." + operation) == std::string::npos,
        "math does not leak LLVM intrinsics", failures);
    const auto explicitFast = operationIR("explicit fast math in conservative profile",
        body + "metal::fast::" + operation + "(i[0]);}", 0, AIR_OK, failures);
    requireOperation(operationSignature(explicitFast, "air.fast_" + operation + ".f32", "float", "float"),
        "explicit fast math signature", failures);
    operationIR("standard math needs fast profile", body + "metal::" + operation + "(i[0]);}",
        0, AIR_FRONTEND_FAILED, failures);
    for (const auto *type : {"half", "float2", "int"}) {
      operationIR("unsupported scalar math type", "using namespace metal; kernel void k(device float *o [[buffer(0)]]) "
          "{o[0]=metal::fast::" + operation + "((" + type + ")1);}",
          1, AIR_FRONTEND_FAILED, failures);
    }
  }

  const auto integer = operationIR("signed and unsigned bit reversal", R"METAL(
    using namespace metal;
    static_assert(__is_same(decltype(reverse_bits(0)),int), "signed reversal result");
    static_assert(__is_same(decltype(reverse_bits(0u)),uint), "unsigned reversal result");
    kernel void k(device int *s [[buffer(0)]],device uint *u [[buffer(1)]]) {
      s[0]=reverse_bits(s[0]); u[0]=reverse_bits(u[0]);
      s[1]=reverse_bits(as_type<int>(0x80000000u));
      u[1]=reverse_bits(0xffffffffu);
    }
  )METAL", 0, AIR_OK, failures);
  requireOperation(operationSignature(integer, "air.reverse_bits.i32", "i32", "i32"),
      "exact bit reversal AIR signature", failures);
  requireOperation(!std::regex_search(integer,
      std::regex("uitofp|sitofp|fptoui|fptosi|air\\.convert\\.f\\.|llvm\\.bitreverse")),
      "integer reversal preserves bits without float or LLVM intrinsic lowering", failures);
  for (const auto *type : {"short", "ushort", "long", "ulong", "int2", "uint2"}) {
    operationIR("unsupported bit reversal width", std::string("using namespace metal; ") +
        "kernel void k(device " + type + " *o [[buffer(0)]]) {o[0]=reverse_bits(o[0]);}",
        1, AIR_FRONTEND_FAILED, failures);
  }

  for (const unsigned profile : {0u, 1u}) {
    const auto halfIR = operationIR("native Metal half semantics in both profiles", R"METAL(
      using namespace metal;
      static __attribute__((noinline)) half half_sum(half a,half b) {return a+b;}
      kernel void k(constant half *i [[buffer(0)]],constant float *f [[buffer(1)]],
          device half *o [[buffer(2)]],device half2 *v [[buffer(3)]],
          device float *w [[buffer(4)]]) {
        half values[2]={i[0],(half)f[0]};
        half result=half_sum(values[0],values[1]);
        o[0]=result;
        o[1]=0.5h;
        v[0]=half2(i[1],i[2])+half2(i[3],i[4]);
        w[0]=float(i[5]);
      }
    )METAL", profile, AIR_OK, failures);
    requireOperation(std::regex_search(halfIR, std::regex(
        "define [^\\n]*\\bhalf @[^\\n]*half_sum[^\\n]*\\(half [^,\\n]*, half [^\\n]*\\)")),
        "half helper preserves native argument and return ABI", failures);
    requireOperation(std::regex_search(halfIR, std::regex("fadd( [a-z]+)* half ")) &&
        std::regex_search(halfIR, std::regex("fadd( [a-z]+)* <2 x half> ")),
        "scalar and vector half arithmetic stay native", failures);
    requireOperation(std::regex_search(halfIR, std::regex("load half,")) &&
        std::regex_search(halfIR, std::regex("store (half|<[0-9]+ x half>) ")) &&
        std::regex_search(halfIR, std::regex("fptrunc float [^\\n]+ to half")) &&
        std::regex_search(halfIR, std::regex("fpext half [^\\n]+ to float")),
        "half memory and explicit float conversions remain typed", failures);
  }

  const auto quadFactors = operationIR("quad tessellation factor ABI", R"METAL(
    using namespace metal;
    static_assert(__is_same(decltype(MTLQuadTessellationFactorsHalf::edgeTessellationFactor),half[4]), "four half edges");
    static_assert(__is_same(decltype(MTLQuadTessellationFactorsHalf::insideTessellationFactor),half[2]), "two half interiors");
    static_assert(sizeof(MTLQuadTessellationFactorsHalf)==12, "quad factor stride");
    static_assert(alignof(MTLQuadTessellationFactorsHalf)==2, "quad factor alignment");
    static_assert(__builtin_offsetof(MTLQuadTessellationFactorsHalf,edgeTessellationFactor)==0, "edge offset");
    static_assert(__builtin_offsetof(MTLQuadTessellationFactorsHalf,insideTessellationFactor)==8, "inside offset");
    kernel void k(device MTLQuadTessellationFactorsHalf *o [[buffer(0)]],constant float *i [[buffer(1)]]) {
      for(uint n=0;n<4;++n) o[0].edgeTessellationFactor[n]=half(i[n]);
      for(uint n=0;n<2;++n) o[0].insideTessellationFactor[n]=half(i[n+4]);
    }
  )METAL", 1, AIR_OK, failures);
  requireOperation(quadFactors.find("MTLQuadTessellationFactorsHalf")!=std::string::npos &&
      quadFactors.find("type { [4 x half], [2 x half] }")!=std::string::npos,
      "quad factors retain half-array IR layout", failures);
  requireOperation(std::regex_search(quadFactors, std::regex("store (half|<[0-9]+ x half>) ")) &&
      !std::regex_search(quadFactors, std::regex("fptoui|air\\.convert\\.u\\.i16")),
      "quad factors use half stores, not numeric integer conversion", failures);

  const auto vote = operationIR("compute SIMD all", R"METAL(
    using namespace metal;
    static_assert(__is_same(decltype(simd_all(true)),bool), "boolean vote result");
    kernel void k(device uint *o [[buffer(0)]],constant int *i [[buffer(1)]]) {
      o[0]=simd_all(i[0]!=0);
    }
  )METAL", 1, AIR_OK, failures);
  requireOperation(operationSignature(vote, "air.simd_all", "i1", "i1"),
      "exact SIMD all AIR signature", failures);
  requireOperation(operationConvergent(vote, "air.simd_all"),
      "SIMD all call and declaration remain convergent and not pure", failures);
  const auto helper = operationIR("compute SIMD all through unannotated helper", R"METAL(
    using namespace metal;
    __attribute__((noinline)) bool vote_helper(bool predicate) {return simd_all(predicate);}
    kernel void k(device uint *o [[buffer(0)]],constant int *i [[buffer(1)]]) {
      if(i[1]!=0) o[0]=vote_helper(i[0]!=0);
    }
  )METAL", 1, AIR_OK, failures);
  requireOperation(operationConvergent(helper, "air.simd_all") &&
      operationConvergent(helper, "_Z11vote_helperb"),
      "unannotated SIMD helper retains convergence", failures);

  for (const auto *stage : {"vertex", "fragment"}) {
    const std::string prefix = std::string("using namespace metal; struct O {float4 value [[") +
        (std::string(stage) == "vertex" ? "position" : "color(0)") + "]];}; ";
    const std::string entry = std::string(stage) +
        " O k(constant int *i [[buffer(0)]]) {return O{float4(";
    operationIR("SIMD all rejected outside compute", prefix + entry +
        "simd_all(i[0]!=0)?1.0f:0.0f)};}", 1, AIR_INVALID_IR, failures,
        "Unsupported shader stage");
    operationIR("SIMD all helper rejected outside compute", prefix +
        "__attribute__((noinline)) bool vote_helper(bool p) {return simd_all(p);}" + entry +
        "vote_helper(i[0]!=0)?1.0f:0.0f)};}", 1, AIR_INVALID_IR, failures,
        "Unsupported shader stage");
  }

  const auto fragmentAtomic = operationIR("fragment device atomic add", R"METAL(
    using namespace metal;
    struct O {float4 color [[color(0)]];};
    fragment O k(device atomic_uint *counter [[buffer(0)]],constant uint *i [[buffer(1)]]) {
      uint previous=atomic_fetch_add_explicit(counter,i[0],memory_order_relaxed);
      return O{float4(previous==i[1]?1.0f:0.0f)};
    }
  )METAL", 1, AIR_OK, failures);
  requireOperation(fragmentAtomic.find("@air.atomic.global.add.u.i32")!=std::string::npos,
      "fragment emits device uint atomic add", failures);
  const std::string atomicFragmentPrefix = "using namespace metal; struct O {float4 color [[color(0)]];}; "
      "fragment O k(device uint *counter [[buffer(0)]],constant uint *i [[buffer(1)]]) {uint value=";
  const std::string atomicFragmentSuffix = ";return O{float4(value!=0u?1.0f:0.0f)};}";
  operationIR("fragment global min remains rejected", atomicFragmentPrefix +
      "__atomic_detail::global_min(counter,i[0],0u,2u,true)" + atomicFragmentSuffix,
      1, AIR_INVALID_IR, failures, "Unsupported shader stage");
  for (const auto *operation : {"local_add", "local_min", "local_max"}) {
    // 使用现有可信声明，将阶段检查与片元 threadgroup 存储的无关前端限制隔离。
    // Use the existing trusted declaration to isolate the stage check from
    // unrelated frontend restrictions on fragment threadgroup storage.
    operationIR("fragment local atomics remain rejected", atomicFragmentPrefix +
        "__atomic_detail::" + operation + "((threadgroup uint*)0,i[0],0u,1u,true)" +
        atomicFragmentSuffix, 1, AIR_INVALID_IR, failures, "Unsupported shader stage");
  }
  operationIR("vertex global add remains rejected", R"METAL(
    using namespace metal;
    struct O {float4 position [[position]];};
    vertex O k(device atomic_uint *counter [[buffer(0)]],constant uint *i [[buffer(1)]]) {
      uint previous=atomic_fetch_add_explicit(counter,i[0],memory_order_relaxed);
      return O{float4(previous==i[1]?1.0f:0.0f)};
    }
  )METAL", 1, AIR_INVALID_IR, failures, "Unsupported shader stage");
  operationIR("fragment barrier remains rejected", R"METAL(
    using namespace metal;
    struct O {float4 color [[color(0)]];};
    fragment O k(constant float *i [[buffer(0)]]) {
      threadgroup_barrier(mem_flags::mem_threadgroup);
      return O{float4(i[0])};
    }
  )METAL", 1, AIR_INVALID_IR, failures, "Unsupported shader stage");
  for (const auto *flags : {"1u,2u,true", "0u,1u,true", "0u,2u,false"}) {
    operationIR("fragment atomic flags remain exact", atomicFragmentPrefix +
        "__atomic_detail::global_add(counter,i[0]," + flags + ")" + atomicFragmentSuffix,
        1, AIR_INVALID_IR, failures, "Unsupported shader stage");
  }

  struct Counterfeit { const char *header; const char *declaration; };
  for (const auto &test : {
      Counterfeit{"metal_math", "float fake(float) __asm__(\"air.fast_sin.f32\");"},
      Counterfeit{"metal_math", "float fake(float) __asm__(\"air.fast_cos.f32\");"},
      Counterfeit{"metal_math", "float fake(float) __asm__(\"air.fast_ceil.f32\");"},
      Counterfeit{"metal_integer", "uint fake(uint) __asm__(\"air.reverse_bits.i32\");"},
      Counterfeit{"metal_simd", "bool fake(bool) __asm__(\"air.simd_all\") __attribute__((convergent));"}}) {
    for (const bool forgedLine : {false, true}) {
      const auto source = (forgedLine ? std::string("#line 1 \"/include/") + test.header + "\"\n" : "") +
          "using namespace metal; " + test.declaration + " kernel void k() {}";
      operationIR("counterfeit AIR label rejected", source, 1, AIR_FRONTEND_FAILED, failures,
          "assembly and assembly symbol labels are unsupported");
    }
  }
  for (const auto *source : {
      "namespace metal { namespace __air_detail { float sin(float); } }",
      "namespace metal { namespace __air_detail { float cos(float x) {return x;} } }",
      "namespace metal { namespace __air_detail { float ceil(float,...) __asm__(\"air.fast_ceil.f32\"); } }",
      "namespace metal { namespace __integer_detail { uint reverse(uint); } }",
      "namespace metal { namespace __integer_detail { int reverse(int) __asm__(\"air.reverse_bits.i32\"); } }",
      "namespace metal { namespace __simd_detail { bool all(bool); } }",
      "namespace metal { namespace __simd_detail { bool all(bool) __attribute__((const)); } }",
      "namespace metal { namespace __simd_detail { bool all(bool) __attribute__((pure)); } }",
      "#define LABEL __asm__(\"air.simd_all\")\nbool fake(bool) LABEL;"}) {
    operationIR("counterfeit declaration rejected", std::string(source) +
        "\nkernel void k() {}", 1, AIR_FRONTEND_FAILED, failures);
  }
  return failures;
}
#endif
