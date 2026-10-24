#include "air_compiler.h"
#include <cstdio>
#include <cstring>
#include <string>
struct Result {
  air_result *p;
  explicit Result(air_result *value) : p(value) {}
  Result(const Result &) = delete;
  Result &operator=(const Result &) = delete;
  ~Result() { air_result_free(p); }
};
static int failures = 0;
static void require(bool ok, const char *message) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static Result compile(const char *s, uint32_t output = AIR_METALLIB) {
  return Result(air_compile_v1(reinterpret_cast<const uint8_t *>(s), std::strlen(s), 18, output));
}
// 从样例输出的单入口库读取首个 NAME 标签。
// Read the first NAME tag from the single-entry library emitted by fixtures.
static bool hasEntryName(const air_result *r, const char *expected) {
  const auto *data = air_result_data(r);
  const auto size = air_result_size(r);
  if (!data || size < 88) return false;
  uint64_t offset = 0;
  for (unsigned i = 0; i < 8; ++i) offset |= uint64_t(data[24 + i]) << (i * 8);
  if (offset > size || size - offset < 14) return false;
  if (data[offset] != 1 || data[offset + 1] || data[offset + 2] || data[offset + 3]) return false;
  const auto tag = size_t(offset) + 8;
  if (std::memcmp(data + tag, "NAME", 4)) return false;
  const size_t length = data[tag + 4] | (size_t(data[tag + 5]) << 8);
  return length == std::strlen(expected) + 1 && length <= size - tag - 6 &&
      !std::memcmp(data + tag + 6, expected, length);
}
int main() {
  Result empty(air_compile_v1(nullptr, 0, 18, AIR_METALLIB));
  require(air_result_status(empty.p) == AIR_BAD_INPUT, "null input rejected");
  const uint8_t byte = 'x';
  Result oversized(air_compile_v1(&byte, 4 * 1024 * 1024 + 1, 18, AIR_METALLIB));
  require(air_result_status(oversized.p) == AIR_BAD_INPUT, "source cap checked before reading");
  Result unsupported(air_compile_v1(&byte, 1, 17, AIR_METALLIB));
  require(air_result_status(unsupported.p) == AIR_BAD_OPTIONS, "unsupported target rejected");
  Result outputKind(air_compile_v1(&byte, 1, 18, 2));
  require(air_result_status(outputKind.p) == AIR_BAD_OPTIONS, "unsupported output kind rejected");
  require(air_result_status(nullptr) == AIR_BAD_INPUT &&
      air_result_size(nullptr) == 0 && air_result_data(nullptr) == nullptr,
      "null result accessors are safe");
  air_result_free(nullptr);
  require(std::strlen(air_compiler_header_sha256()) == 64, "header fingerprint exposed");
  const char *shader = "#include <metal_stdlib>\nusing namespace metal; kernel void main0(device int *p [[buffer(2)]]) { int x = p[0]; p[0] = x > 3 ? x * 7 : x + 2; }";
  auto result = compile(shader);
  if (air_result_status(result.p) != AIR_OK)
    std::fprintf(stderr, "%s", air_result_diagnostics(result.p));
  require(air_result_status(result.p) == AIR_OK, "compute compilation succeeds");
  require(air_result_size(result.p) > 88, "library has payload");
  require(air_result_size(result.p) >= 4 && !std::memcmp(air_result_data(result.p), "MTLB", 4), "library header");
  require(hasEntryName(result.p, "main0"), "compute exports source entry name");
  auto ir = compile(shader, AIR_LLVM_TEXT);
  require(air_result_status(ir.p) == AIR_OK, "AIR module available");
  std::string irText(air_result_data(ir.p) ? reinterpret_cast<const char *>(air_result_data(ir.p)) : "", air_result_size(ir.p));
  require(irText.find("air.kernel") != std::string::npos, "compute AIR metadata");
  require(irText.find("mul") != std::string::npos, "source multiplication lowered");
  auto changed = compile("[[compute_kernel, kernel_dim(3)]] void main0(__attribute__((global_as)) int *p [[buffer(2)]]) { p[0] = p[0] + 91; }", AIR_LLVM_TEXT);
  std::string changedText(air_result_data(changed.p) ? reinterpret_cast<const char *>(air_result_data(changed.p)) : "", air_result_size(changed.p));
  require(air_result_status(changed.p) == AIR_OK && changedText != irText, "source-dependent IR");
  auto vertex = compile("#include <metal_stdlib>\nusing namespace metal; struct Out { float4 position [[position]]; }; vertex Out main0(constant float4 *p [[buffer(3)]]) { Out o; o.position = p[0]; o.position.x *= 2.0f; return o; }");
  require(air_result_status(vertex.p) == AIR_OK && air_result_size(vertex.p) > 88, "authored vertex shader compiles");
  if (air_result_status(vertex.p) != AIR_OK) std::fprintf(stderr, "%s", air_result_diagnostics(vertex.p));
  require(hasEntryName(vertex.p, "main0"), "vertex exports source entry name");
  auto fragment = compile("#include <metal_stdlib>\nusing namespace metal; struct Out { float4 color [[color(0)]]; }; fragment Out main0(constant float4 *p [[buffer(5)]]) { Out o; o.color = p[0].x > 0.25f ? p[0] : p[1]; return o; }");
  require(air_result_status(fragment.p) == AIR_OK && air_result_size(fragment.p) > 88, "authored fragment shader compiles");
  if (air_result_status(fragment.p) != AIR_OK) std::fprintf(stderr, "%s", air_result_diagnostics(fragment.p));
  require(hasEntryName(fragment.p, "main0"), "fragment exports source entry name");
  // 每次调用创建并销毁独立 LLVMContext；依赖运行时的操作数防止常量折叠掩盖转换降低过程。
  // Each call creates and destroys its own LLVMContext. Runtime-dependent
  // operands prevent constant folding from hiding the conversion lowering.
  const char *conversionSource = "[[compute_kernel, kernel_dim(3)]] void convert0(__attribute__((global_as)) float *f [[buffer(0)]], __attribute__((global_as)) int *i [[buffer(1)]]) { i[0] = (int)f[0]; f[1] = (float)i[1]; }";
  for (int run = 0; run < 2; ++run) {
    auto conversion = compile(conversionSource, AIR_LLVM_TEXT);
    std::string text(air_result_data(conversion.p) ? reinterpret_cast<const char *>(air_result_data(conversion.p)) : "", air_result_size(conversion.p));
    require(air_result_status(conversion.p) == AIR_OK,
        "conversion compilation succeeds in successive contexts");
    require(text.find("air.convert.s.i32.f.f32") != std::string::npos &&
        text.find("air.convert.f.f32.s.i32") != std::string::npos,
        "both contexts lower float/int conversions to AIR intrinsics");
    require(text.find("fptosi ") == std::string::npos && text.find("sitofp ") == std::string::npos,
        "unlowered native LLVM conversions do not survive");
  }
  auto unknown = compile("[[compute_kernel, kernel_dim(3), unknown_msl_attribute]] void main0() {}");
  require(air_result_status(unknown.p) == AIR_FRONTEND_FAILED, "unknown attributes fail closed");
  auto suppressedUnknown = compile("#pragma clang diagnostic ignored \"-Wunknown-attributes\"\n[[compute_kernel, kernel_dim(3), unknown_msl_attribute]] void main0() {}");
  require(air_result_status(suppressedUnknown.p) == AIR_FRONTEND_FAILED,
      "source diagnostic pragmas cannot suppress unknown semantic attributes");
  auto umbrellaSuppression = compile("#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Weverything\"\n[[compute_kernel, kernel_dim(3), unknown_msl_attribute]] void main0() {}\n#pragma clang diagnostic pop\n");
  require(air_result_status(umbrellaSuppression.p) == AIR_FRONTEND_FAILED,
      "umbrella diagnostic pragmas cannot suppress semantic attributes");
  auto ordinaryPragma = compile("#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Wunused-variable\"\n#include <metal_stdlib>\nkernel void main0(device int *p [[buffer(0)]]) { int unused = 1; p[0] += 2; }\n#pragma clang diagnostic pop\n");
  require(air_result_status(ordinaryPragma.p) == AIR_OK,
      "ordinary diagnostic pragmas remain supported");
  auto noEntry = compile("int helper(int x) { return x + 1; }");
  require(air_result_status(noEntry.p) == AIR_NO_ENTRY, "no-entry source is rejected");
  auto include = compile("#include \"/etc/passwd\"\n[[compute_kernel, kernel_dim(3)]] void main0() {}");
  require(air_result_status(include.p) == AIR_FRONTEND_FAILED, "host files inaccessible through VFS");
  auto forgedIntrinsic = compile("extern int fake(int) asm(\"air.not_a_real_intrinsic\"); [[compute_kernel, kernel_dim(3)]] void main0(__attribute__((global_as)) int *p [[buffer(0)]]) { p[0] = fake(p[0]); }");
  require(air_result_status(forgedIntrinsic.p) != AIR_OK, "source cannot inject an AIR symbol through assembly labels");
  auto invalid = compile("[[compute_kernel, kernel_dim(3)]] void main0( { }");
  require(air_result_status(invalid.p) == AIR_FRONTEND_FAILED, "syntax errors rejected");
  auto afterFailure = compile(shader);
  require(air_result_status(afterFailure.p) == AIR_OK, "fresh compilation works after failed compilation");
  std::printf("%s: in-process authored-fixture checks; not game/Apple-runtime/WASM validation\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
