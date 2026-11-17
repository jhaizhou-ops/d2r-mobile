; Independently authored public symbolic LLVM GPU-shader IR regression sources.
; No private source/IR, host-memory access, or generated shader execution.
; Native opt/FileCheck is NOT run by current CI; these RUN lines are unexecuted.
; The production failure names a GEP input but does not establish its type or
; indices. These cases test the bounded correction, not the private kernel IR.
; RUN: split-file %s %t
; RUN: opt -enable-new-pm=0 -PointerBCFixup -verify -S %t/positive.ll | FileCheck %s --check-prefix=POS --implicit-check-not='= bitcast {{.*}} addrspace' --implicit-check-not='= load '

; Every negative retains a direct GEP/select at the unchanged legacy guard.
; These are expected errors, not merely exclusions from the new helper.
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/scalar_wrong_width.ll 2>&1 | FileCheck %s --check-prefix=REJECT-SCALAR-WRONG-WIDTH
; REJECT-SCALAR-WRONG-WIDTH: constant-vector graph: noncanonical scalar inverse pointer cast (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/vector_lane_regroup.ll 2>&1 | FileCheck %s --check-prefix=REJECT-VECTOR-LANE-REGROUP
; REJECT-VECTOR-LANE-REGROUP: constant-vector graph: noncanonical vector inverse pointer cast (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/vector_to_scalar.ll 2>&1 | FileCheck %s --check-prefix=REJECT-VECTOR-TO-SCALAR
; REJECT-VECTOR-TO-SCALAR: constant-vector graph: vector-to-scalar inverse pointer cast (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/scalar_to_vector.ll 2>&1 | FileCheck %s --check-prefix=REJECT-SCALAR-TO-VECTOR
; REJECT-SCALAR-TO-VECTOR: constant-vector graph: scalar-to-vector inverse pointer cast (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/scalar_to_record.ll 2>&1 | FileCheck %s --check-prefix=REJECT-SCALAR-TO-RECORD
; REJECT-SCALAR-TO-RECORD: constant-vector graph: scalar-to-aggregate inverse pointer cast (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/scalar_to_array.ll 2>&1 | FileCheck %s --check-prefix=REJECT-SCALAR-TO-ARRAY
; REJECT-SCALAR-TO-ARRAY: constant-vector graph: scalar-to-aggregate inverse pointer cast (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/vector_to_record.ll 2>&1 | FileCheck %s --check-prefix=REJECT-VECTOR-TO-RECORD
; REJECT-VECTOR-TO-RECORD: constant-vector graph: unsupported inverse pointer input (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/canonical_write.ll 2>&1 | FileCheck %s --check-prefix=REJECT-CANONICAL-WRITE
; REJECT-CANONICAL-WRITE: constant-vector graph: unsupported native-view user (store)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/canonical_escape.ll 2>&1 | FileCheck %s --check-prefix=REJECT-CANONICAL-ESCAPE
; REJECT-CANONICAL-ESCAPE: constant-vector graph: unsupported native-view user (call)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/canonical_volatile.ll 2>&1 | FileCheck %s --check-prefix=REJECT-CANONICAL-VOLATILE
; REJECT-CANONICAL-VOLATILE: constant-vector graph: unsupported native-view load (load)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/canonical_atomic.ll 2>&1 | FileCheck %s --check-prefix=REJECT-CANONICAL-ATOMIC
; REJECT-CANONICAL-ATOMIC: constant-vector graph: unsupported native-view load (load)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/canonical_address_space.ll 2>&1 | FileCheck %s --check-prefix=REJECT-CANONICAL-ADDRESS-SPACE
; REJECT-CANONICAL-ADDRESS-SPACE: constant-vector graph: unsupported native-view user (addrspacecast)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/canonical_select_input.ll 2>&1 | FileCheck %s --check-prefix=REJECT-CANONICAL-SELECT-INPUT
; REJECT-CANONICAL-SELECT-INPUT: constant-vector graph: canonical inverse input is not a GEP (select)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/canonical_unknown_arm.ll 2>&1 | FileCheck %s --check-prefix=REJECT-CANONICAL-UNKNOWN-ARM
; REJECT-CANONICAL-UNKNOWN-ARM: constant-vector graph: unmapped argument or constant pointer root
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/canonical_wrong_root_layout.ll 2>&1 | FileCheck %s --check-prefix=REJECT-CANONICAL-WRONG-ROOT-LAYOUT
; REJECT-CANONICAL-WRONG-ROOT-LAYOUT: constant-vector graph: root wrapper layout mismatch (bitcast)

; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/scalar_to_pointer.ll 2>&1 | FileCheck %s --check-prefix=REJECT-SCALAR-TO-POINTER
; REJECT-SCALAR-TO-POINTER: constant-vector graph: scalar-to-pointer inverse pointer cast (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/vector_to_pointer.ll 2>&1 | FileCheck %s --check-prefix=REJECT-VECTOR-TO-POINTER
; REJECT-VECTOR-TO-POINTER: constant-vector graph: vector-to-pointer inverse pointer cast (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/vector_to_array.ll 2>&1 | FileCheck %s --check-prefix=REJECT-VECTOR-TO-ARRAY
; REJECT-VECTOR-TO-ARRAY: constant-vector graph: vector-to-array inverse pointer cast (getelementptr)

;--- positive.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
%IntBuffer = type { [8 x <4 x i32>] }

; Mixed-width dynamic row/lane indices, exact scalar pointer and native load.
; POS-LABEL: define floor_fragment float @canonical_scalar_gep(
; POS: %a.storage = getelementptr %Buffer, %Buffer addrspace(2)* %base, i32 0, i32 0, i32 0
; POS-NEXT: %lane.typed = getelementptr inbounds <4 x float>, <4 x float> addrspace(2)* %a.storage, i64 %row, i32 %component
; POS-NEXT: %value = load float, float addrspace(2)* %lane.typed, align 1, !invariant.load ![[EMPTY:[0-9]+]], !nontemporal ![[NT:[0-9]+]]
; POS-NEXT: ret float %value
define floor_fragment float @canonical_scalar_gep(%Buffer addrspace(2)* %base, i64 %row, i32 %component) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %lane = getelementptr inbounds <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %row, i32 %component
  %back = bitcast i32 addrspace(2)* %lane to float addrspace(2)*
  %value = load float, float addrspace(2)* %back, align 1, !invariant.load !0, !nontemporal !1
  ret float %value
}

; A non-inbounds negative vector index remains negative and non-inbounds.
; POS-LABEL: define floor_fragment <4 x float> @canonical_vector_gep(
; POS: %row.typed = getelementptr <4 x float>, <4 x float> addrspace(2)* %a.storage, i32 -2
; POS-NEXT: %next = getelementptr <4 x float>, <4 x float> addrspace(2)* %row.typed, i64 %advance
; POS-NEXT: %value = load <4 x float>, <4 x float> addrspace(2)* %next, align 4
; POS-NEXT: ret <4 x float> %value
define floor_fragment <4 x float> @canonical_vector_gep(%Buffer addrspace(2)* %base, i64 %advance) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %row = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i32 -2
  %back = bitcast <4 x i32> addrspace(2)* %row to <4 x float> addrspace(2)*
  %next = getelementptr <4 x float>, <4 x float> addrspace(2)* %back, i64 %advance
  %value = load <4 x float>, <4 x float> addrspace(2)* %next, align 4
  ret <4 x float> %value
}

; Both GEP strides are preserved. The native load is unchanged; the separate
; integer-view load is retyped at its own site and must lose integer !range.
; POS-LABEL: define floor_fragment float @canonical_chained_scalar(
; POS: %lane.typed = getelementptr <4 x float>, <4 x float> addrspace(2)* %a.storage, i16 -1, i32 2
; POS-NEXT: %offset.typed = getelementptr float, float addrspace(2)* %lane.typed, i64 %advance
; POS-NEXT: %native = load float, float addrspace(2)* %offset.typed, align 4
; POS-NEXT: %bits.typed = load float, float addrspace(2)* %lane.typed, align 1{{$}}
; POS-NEXT: %bits.bits = bitcast float %bits.typed to i32
; POS-NEXT: %number = sitofp i32 %bits.bits to float
; POS-NEXT: %sum = fadd float %native, %number
; POS-NEXT: ret float %sum
define floor_fragment float @canonical_chained_scalar(%Buffer addrspace(2)* %base, i64 %advance) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %lane = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i16 -1, i32 2
  %offset = getelementptr i32, i32 addrspace(2)* %lane, i64 %advance
  %back = bitcast i32 addrspace(2)* %offset to float addrspace(2)*
  %native = load float, float addrspace(2)* %back, align 4
  %bits = load i32, i32 addrspace(2)* %lane, align 1, !range !2
  %number = sitofp i32 %bits to float
  %sum = fadd float %native, %number
  ret float %sum
}

; Existing native PHIs/selects keep their original other arms and edge blocks.
; POS-LABEL: define floor_fragment float @canonical_native_merges(
; POS: %lane.typed = getelementptr inbounds <4 x float>, <4 x float> addrspace(2)* %a.storage, i64 %row, i32 3
; POS: %native = phi float addrspace(2)* [ %lane.typed, %left ], [ %other, %right ]
; POS-NEXT: %chosen = select i1 %condition, float addrspace(2)* %native, float addrspace(2)* %other
; POS-NEXT: %value = load float, float addrspace(2)* %chosen, align 4
; POS-NEXT: ret float %value
define floor_fragment float @canonical_native_merges(%Buffer addrspace(2)* %base, float addrspace(2)* %other, i1 %condition, i64 %row) {
entry:
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  br i1 %condition, label %left, label %right
left:
  %lane = getelementptr inbounds <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %row, i32 3
  %back = bitcast i32 addrspace(2)* %lane to float addrspace(2)*
  br label %merge
right:
  br label %merge
merge:
  %native = phi float addrspace(2)* [ %back, %left ], [ %other, %right ]
  %chosen = select i1 %condition, float addrspace(2)* %native, float addrspace(2)* %other
  %value = load float, float addrspace(2)* %chosen, align 4
  ret float %value
}

; The direct root alias %again exercises both PHI operand RAUW and replacement
; cache updates before rewriting %bits. A stale old recast cannot survive.
; POS-LABEL: define floor_fragment <4 x i32> @canonical_loop_alias(
; POS: loop:
; POS-NEXT: %p.typed = phi <4 x float> addrspace(2)* [ %a.storage, %entry ], [ %advance.typed, %loop ]
; POS-NEXT: %step = phi i32 [ 0, %entry ], [ %next, %loop ]
; POS-NEXT: %advance.typed = getelementptr <4 x float>, <4 x float> addrspace(2)* %p.typed, i64 %stride
; POS-NEXT: %bits.typed = load <4 x float>, <4 x float> addrspace(2)* %advance.typed, align 4
; POS-NEXT: %bits.bits = bitcast <4 x float> %bits.typed to <4 x i32>
; POS: ret <4 x i32> %bits.bits
define floor_fragment <4 x i32> @canonical_loop_alias(%Buffer addrspace(2)* %base, i64 %stride, i32 %count) {
entry:
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  br label %loop
loop:
  %p = phi <4 x i32> addrspace(2)* [ %a, %entry ], [ %again, %loop ]
  %step = phi i32 [ 0, %entry ], [ %next, %loop ]
  %advance = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %p, i64 %stride
  %back = bitcast <4 x i32> addrspace(2)* %advance to <4 x float> addrspace(2)*
  %again = bitcast <4 x float> addrspace(2)* %back to <4 x i32> addrspace(2)*
  %bits = load <4 x i32>, <4 x i32> addrspace(2)* %again, align 4
  %next = add i32 %step, 1
  %more = icmp ult i32 %next, %count
  br i1 %more, label %loop, label %exit
exit:
  ret <4 x i32> %bits
}

; A retained native GEP feeding another integer root is not reconstructed with
; aggregate or integer indices. Only its exact pointer operand changes.
; POS-LABEL: define floor_fragment i32 @canonical_native_gep_crosslink(
; POS: %lane.typed = getelementptr <4 x float>, <4 x float> addrspace(2)* %a.storage, i32 1, i32 2
; POS-NEXT: %native.offset = getelementptr float, float addrspace(2)* %lane.typed, i64 %offset
; POS-NEXT: %bits.typed = load float, float addrspace(2)* %native.offset, align 1
; POS-NEXT: %bits.bits = bitcast float %bits.typed to i32
; POS-NEXT: ret i32 %bits.bits
define floor_fragment i32 @canonical_native_gep_crosslink(%Buffer addrspace(2)* %base, i64 %offset) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %lane = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i32 1, i32 2
  %back = bitcast i32 addrspace(2)* %lane to float addrspace(2)*
  %native.offset = getelementptr float, float addrspace(2)* %back, i64 %offset
  %again = bitcast float addrspace(2)* %native.offset to i32 addrspace(2)*
  %bits = load i32, i32 addrspace(2)* %again, align 1
  ret i32 %bits
}

; Two inverse users of one GEP share its replacement, not additional loads.
; POS-LABEL: define floor_fragment float @canonical_shared_inverse(
; POS: %lane.typed = getelementptr <4 x float>, <4 x float> addrspace(2)* %a.storage, i32 0, i32 1
; POS-NEXT: %first = load float, float addrspace(2)* %lane.typed, align 4
; POS-NEXT: %second = load float, float addrspace(2)* %lane.typed, align 1
; POS-NEXT: %sum = fadd float %first, %second
; POS-NEXT: ret float %sum
define floor_fragment float @canonical_shared_inverse(%Buffer addrspace(2)* %base) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %lane = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i32 0, i32 1
  %back.first = bitcast i32 addrspace(2)* %lane to float addrspace(2)*
  %first = load float, float addrspace(2)* %back.first, align 4
  %back.second = bitcast i32 addrspace(2)* %lane to float addrspace(2)*
  %second = load float, float addrspace(2)* %back.second, align 1
  %sum = fadd float %first, %second
  ret float %sum
}

; One proved pointer-slot load also retains the old exact-wrapper inverse path.
; POS-LABEL: define floor_fragment float @canonical_with_slot_inverse(
; POS: %loaded.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(1)* %slot, align 1, !nonnull ![[EMPTY]]
; POS-NEXT: %loaded.storage = getelementptr %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i32 0
; POS-NEXT: %chosen.typed = select i1 %condition, <4 x float> addrspace(2)* %a.storage, <4 x float> addrspace(2)* %loaded.storage
; POS-NEXT: %lane.typed = getelementptr <4 x float>, <4 x float> addrspace(2)* %chosen.typed, i32 2, i32 1
; POS-NEXT: %native.row = getelementptr %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i32 3, i32 2
; POS-NEXT: %first = load float, float addrspace(2)* %lane.typed, align 4
; POS-NEXT: %second = load float, float addrspace(2)* %native.row, align 4
; POS-NEXT: %sum = fadd float %first, %second
; POS-NEXT: ret float %sum
define floor_fragment float @canonical_with_slot_inverse(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(1)* %slot, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(1)* %slot to <4 x i32> addrspace(2)* addrspace(1)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(1)* %cast.slot, align 1, !nonnull !0
  %direct = bitcast <4 x i32> addrspace(2)* %loaded to %Buffer addrspace(2)*
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %loaded
  %lane = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %chosen, i32 2, i32 1
  %back = bitcast i32 addrspace(2)* %lane to float addrspace(2)*
  %native.row = getelementptr %Buffer, %Buffer addrspace(2)* %direct, i32 0, i32 0, i32 3, i32 2
  %first = load float, float addrspace(2)* %back, align 4
  %second = load float, float addrspace(2)* %native.row, align 4
  %sum = fadd float %first, %second
  ret float %sum
}

; Canonical means the proven source type, not an unconditional float view.
; POS-LABEL: define floor_fragment i32 @canonical_integer_storage(
; POS: %lane.typed = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a.storage, i32 %row, i32 1
; POS-NEXT: %value = load i32, i32 addrspace(2)* %lane.typed, align 4, !range ![[RANGE:[0-9]+]]
; POS-NEXT: ret i32 %value
define floor_fragment i32 @canonical_integer_storage(%IntBuffer addrspace(2)* %base, i32 %row) {
  %a = bitcast %IntBuffer addrspace(2)* %base to <4 x float> addrspace(2)*
  %lane = getelementptr <4 x float>, <4 x float> addrspace(2)* %a, i32 %row, i32 1
  %back = bitcast float addrspace(2)* %lane to i32 addrspace(2)*
  %value = load i32, i32 addrspace(2)* %back, align 4, !range !2
  ret i32 %value
}

; POS: ![[EMPTY]] = !{}
; POS: ![[NT]] = !{i32 1}
; POS: ![[RANGE]] = !{i32 0, i32 8}
!0 = !{}
!1 = !{i32 1}
!2 = !{i32 0, i32 8}

;--- scalar_wrong_width.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_scalar_wrong_width(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to double addrspace(2)*
  %value = load double, double addrspace(2)* %back, align 1
  ret void
}

;--- vector_lane_regroup.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_vector_lane_regroup(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index
  %back = bitcast <4 x i32> addrspace(2)* %p to <2 x double> addrspace(2)*
  %value = load <2 x double>, <2 x double> addrspace(2)* %back, align 1
  ret void
}

;--- vector_to_scalar.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_vector_to_scalar(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index
  %back = bitcast <4 x i32> addrspace(2)* %p to float addrspace(2)*
  %value = load float, float addrspace(2)* %back, align 1
  ret void
}

;--- scalar_to_vector.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_scalar_to_vector(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to <4 x float> addrspace(2)*
  %value = load <4 x float>, <4 x float> addrspace(2)* %back, align 1
  ret void
}

;--- scalar_to_record.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_scalar_to_record(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to %Buffer addrspace(2)*
  %value = load %Buffer, %Buffer addrspace(2)* %back, align 1
  ret void
}

;--- scalar_to_array.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_scalar_to_array(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to [4 x float] addrspace(2)*
  %value = load [4 x float], [4 x float] addrspace(2)* %back, align 1
  ret void
}

;--- vector_to_record.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_vector_to_record(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %value = load %Buffer, %Buffer addrspace(2)* %back, align 1
  ret void
}

;--- canonical_write.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_canonical_write(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to float addrspace(2)*
  store float 1.0, float addrspace(2)* %back, align 4
  ret void
}

;--- canonical_escape.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
declare void @escape(float addrspace(2)*)

define floor_fragment void @reject_canonical_escape(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to float addrspace(2)*
  call void @escape(float addrspace(2)* %back)
  ret void
}

;--- canonical_volatile.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_canonical_volatile(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to float addrspace(2)*
  %value = load volatile float, float addrspace(2)* %back, align 4
  ret void
}

;--- canonical_atomic.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_canonical_atomic(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to float addrspace(2)*
  %value = load atomic float, float addrspace(2)* %back monotonic, align 4
  ret void
}

;--- canonical_address_space.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_canonical_address_space(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to float addrspace(2)*
  %other = addrspacecast float addrspace(2)* %back to float addrspace(1)*
  %value = load float, float addrspace(1)* %other, align 4
  ret void
}

;--- canonical_select_input.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_canonical_select_input(%Buffer addrspace(2)* %base, i64 %index, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %a
  %back = bitcast <4 x i32> addrspace(2)* %p to <4 x float> addrspace(2)*
  %value = load <4 x float>, <4 x float> addrspace(2)* %back, align 1
  ret void
}

;--- canonical_unknown_arm.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_canonical_unknown_arm(%Buffer addrspace(2)* %base, i64 %index, <4 x i32> addrspace(2)* %unknown, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %unknown
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %chosen, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to float addrspace(2)*
  %value = load float, float addrspace(2)* %back, align 1
  ret void
}

;--- canonical_wrong_root_layout.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
%Padded = type { <4 x float>, i32 }

define floor_fragment void @reject_canonical_wrong_root_layout(%Buffer addrspace(2)* %base, i64 %index, %Padded addrspace(2)* %padded, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %b = bitcast %Padded addrspace(2)* %padded to <4 x i32> addrspace(2)*
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %chosen, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to float addrspace(2)*
  %value = load float, float addrspace(2)* %back, align 1
  ret void
}

;--- scalar_to_pointer.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_scalar_to_pointer(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index, i32 1
  %back = bitcast i32 addrspace(2)* %p to float addrspace(2)* addrspace(2)*
  %value = load float addrspace(2)*, float addrspace(2)* addrspace(2)* %back, align 1
  ret void
}

;--- vector_to_pointer.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_vector_to_pointer(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index
  %back = bitcast <4 x i32> addrspace(2)* %p to float addrspace(2)* addrspace(2)*
  %value = load float addrspace(2)*, float addrspace(2)* addrspace(2)* %back, align 1
  ret void
}

;--- vector_to_array.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment void @reject_vector_to_array(%Buffer addrspace(2)* %base, i64 %index) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %index
  %back = bitcast <4 x i32> addrspace(2)* %p to [4 x float] addrspace(2)*
  %value = load [4 x float], [4 x float] addrspace(2)* %back, align 1
  ret void
}
