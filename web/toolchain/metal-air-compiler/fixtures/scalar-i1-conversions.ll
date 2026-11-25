; Hand-authored symbolic fixtures, not captured compiler output.
; No RUN line: compiler / LLVM verifier execution is intentionally out of scope.
; Ordinary functions isolate the cast helper, not a complete AIR entry pipeline.
; The expected file shows immediate post-helper shapes modulo SSA names.
; Later full-pipeline debug cleanup and optimization may change their spelling.

define half @signed_half(i1 %b) {
entry:
  %r = sitofp i1 %b to half
  ret half %r
}

define float @signed_float(i1 %b) {
entry:
  %r = sitofp i1 %b to float
  ret float %r
}

define double @signed_double(i1 %b) {
entry:
  %r = sitofp i1 %b to double
  ret double %r
}

define float @signed_true() {
entry:
  %r = sitofp i1 true to float
  ret float %r
}

define float @signed_false() {
entry:
  %r = sitofp i1 false to float
  ret float %r
}

define float @signed_poison() {
entry:
  %r = sitofp i1 poison to float
  ret float %r
}

define float @signed_undef() {
entry:
  %r = sitofp i1 undef to float
  ret float %r
}

define half @unsigned_half(i1 %b) {
entry:
  %r = uitofp i1 %b to half
  ret half %r
}

define float @unsigned_float(i1 %b) {
entry:
  %r = uitofp i1 %b to float
  ret float %r
}

define double @unsigned_double(i1 %b) {
entry:
  %r = uitofp i1 %b to double
  ret double %r
}

define float @unsigned_true() {
entry:
  %r = uitofp i1 true to float
  ret float %r
}

define float @unsigned_false() {
entry:
  %r = uitofp i1 false to float
  ret float %r
}

define float @unsigned_poison() {
entry:
  %r = uitofp i1 poison to float
  ret float %r
}

define float @unsigned_undef() {
entry:
  %r = uitofp i1 undef to float
  ret float %r
}

define <2 x float> @frozen_input(i1 %b) {
entry:
  %f = freeze i1 %b
  %s = sitofp i1 %f to float
  %u = uitofp i1 %f to float
  %a = insertelement <2 x float> poison, float %s, i32 0
  %r = insertelement <2 x float> %a, float %u, i32 1
  ret <2 x float> %r
}

define float @adjacent_debug_multiuse(i1 %b) !dbg !5 {
entry:
  %s = sitofp i1 %b to float, !dbg !8
  %u = uitofp i1 %b to float, !dbg !9
  %sum = fadd float %s, %u
  %r = fadd float %sum, %s
  ret float %r
}

define <2 x float> @vector_signed(<2 x i1> %b) {
entry:
  %r = sitofp <2 x i1> %b to <2 x float>
  ret <2 x float> %r
}

define <2 x float> @vector_unsigned(<2 x i1> %b) {
entry:
  %r = uitofp <2 x i1> %b to <2 x float>
  ret <2 x float> %r
}

define <1 x half> @vector_one_signed(<1 x i1> %b) {
entry:
  %r = sitofp <1 x i1> %b to <1 x half>
  ret <1 x half> %r
}

define float @signed_i8(i8 %b) {
entry:
  %r = sitofp i8 %b to float
  ret float %r
}

define double @unsigned_i64(i64 %b) {
entry:
  %r = uitofp i64 %b to double
  ret double %r
}

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
  %r = fptosi float %b to i1
  ret i1 %r
}

define i1 @reverse_unsigned(float %b) {
entry:
  %r = fptoui float %b to i1
  ret i1 %r
}

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
