; Hand-authored symbolic fixtures, not captured compiler output.
; No RUN line: compiler / LLVM verifier execution is intentionally out of scope.
; Ordinary functions isolate the cast helper, not a complete AIR entry pipeline.
; The expected file shows immediate post-helper shapes modulo SSA names.
; Later full-pipeline debug cleanup and optimization may change their spelling.

define half @signed_half(i1 %b) {
entry:
  %r = select i1 %b, half -1.000000e+00, half 0.000000e+00
  ret half %r
}

define float @signed_float(i1 %b) {
entry:
  %r = select i1 %b, float -1.000000e+00, float 0.000000e+00
  ret float %r
}

define double @signed_double(i1 %b) {
entry:
  %r = select i1 %b, double -1.000000e+00, double 0.000000e+00
  ret double %r
}

define float @signed_true() {
entry:
  ret float -1.000000e+00
}

define float @signed_false() {
entry:
  ret float 0.000000e+00
}

define float @signed_poison() {
entry:
  ret float poison
}

define float @signed_undef() {
entry:
  ret float 0.000000e+00
}

define half @unsigned_half(i1 %b) {
entry:
  %r = select i1 %b, half 1.000000e+00, half 0.000000e+00
  ret half %r
}

define float @unsigned_float(i1 %b) {
entry:
  %r = select i1 %b, float 1.000000e+00, float 0.000000e+00
  ret float %r
}

define double @unsigned_double(i1 %b) {
entry:
  %r = select i1 %b, double 1.000000e+00, double 0.000000e+00
  ret double %r
}

define float @unsigned_true() {
entry:
  ret float 1.000000e+00
}

define float @unsigned_false() {
entry:
  ret float 0.000000e+00
}

define float @unsigned_poison() {
entry:
  ret float poison
}

define float @unsigned_undef() {
entry:
  ret float 0.000000e+00
}

define <2 x float> @frozen_input(i1 %b) {
entry:
  %f = freeze i1 %b
  %s = select i1 %f, float -1.000000e+00, float 0.000000e+00
  %u = select i1 %f, float 1.000000e+00, float 0.000000e+00
  %a = insertelement <2 x float> poison, float %s, i32 0
  %r = insertelement <2 x float> %a, float %u, i32 1
  ret <2 x float> %r
}

define float @adjacent_debug_multiuse(i1 %b) !dbg !5 {
entry:
  %s = select i1 %b, float -1.000000e+00, float 0.000000e+00, !dbg !8
  %u = select i1 %b, float 1.000000e+00, float 0.000000e+00, !dbg !9
  %sum = fadd float %s, %u
  %r = fadd float %sum, %s
  ret float %r
}

define <2 x float> @vector_signed(<2 x i1> %b) {
entry:
  %r = call <2 x float> @air.convert.f.v2f32.s.v2i1(<2 x i1> %b)
  ret <2 x float> %r
}

declare <2 x float> @air.convert.f.v2f32.s.v2i1(<2 x i1>)

define <2 x float> @vector_unsigned(<2 x i1> %b) {
entry:
  %r = call <2 x float> @air.convert.f.v2f32.u.v2i1(<2 x i1> %b)
  ret <2 x float> %r
}

declare <2 x float> @air.convert.f.v2f32.u.v2i1(<2 x i1>)

define <1 x half> @vector_one_signed(<1 x i1> %b) {
entry:
  %r = call <1 x half> @air.convert.f.v1f16.s.v1i1(<1 x i1> %b)
  ret <1 x half> %r
}

declare <1 x half> @air.convert.f.v1f16.s.v1i1(<1 x i1>)

define float @signed_i8(i8 %b) {
entry:
  %r = call float @air.convert.f.f32.s.i8(i8 %b)
  ret float %r
}

declare float @air.convert.f.f32.s.i8(i8)

define double @unsigned_i64(i64 %b) {
entry:
  %r = call double @air.convert.f.f64.u.i64(i64 %b)
  ret double %r
}

declare double @air.convert.f.f64.u.i64(i64)

define bfloat @unsupported_bfloat(i1 %b) {
entry:
  %r = sitofp i1 %b to bfloat
  ret bfloat %r
}

define fp128 @unsupported_fp128(i1 %b) {
entry:
  %r = uitofp i1 %b to fp128
  ret fp128 %r
}

define i1 @reverse_signed(float %b) {
entry:
  %r = call i1 @air.convert.s.i1.f.f32(float %b)
  ret i1 %r
}

declare i1 @air.convert.s.i1.f.f32(float)

define i1 @reverse_unsigned(float %b) {
entry:
  %r = call i1 @air.convert.u.i1.f.f32(float %b)
  ret i1 %r
}

declare i1 @air.convert.u.i1.f.f32(float)

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "authored symbolic fixture", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "scalar-i1-conversions.c", directory: ".")
!2 = !{}
!3 = !{i32 2, !"Dwarf Version", i32 4}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = distinct !DISubprogram(name: "adjacent_debug_multiuse", scope: !1, file: !1, line: 1, type: !6, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !2)
!6 = !DISubroutineType(types: !2)
!8 = !DILocation(line: 2, column: 3, scope: !5)
!9 = !DILocation(line: 3, column: 3, scope: !5)
