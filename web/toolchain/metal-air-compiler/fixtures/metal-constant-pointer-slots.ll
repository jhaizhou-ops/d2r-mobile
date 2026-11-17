; Independently authored public symbolic LLVM regression fixtures.
; No private shader source/IR is included. These files describe compiler IR;
; they do not read host memory or execute generated GPU shader instructions.
; RUN: split-file %s %t
; RUN: opt -enable-new-pm=0 -PointerBCFixup -verify -S %t/slots.ll | FileCheck %s --check-prefix=POS --implicit-check-not='= bitcast {{.*}} addrspace' --implicit-check-not='= load {{.*}} addrspace({{[0-9]+}})*,'
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/unknown_slot.ll 2>&1 | FileCheck %s --check-prefix=REJECT-UNKNOWN-SLOT
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/raw_byte_slot.ll 2>&1 | FileCheck %s --check-prefix=REJECT-RAW-BYTE-SLOT
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/nonzero_pointer_field.ll 2>&1 | FileCheck %s --check-prefix=REJECT-NONZERO-POINTER-FIELD
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/wrong_inner_as.ll 2>&1 | FileCheck %s --check-prefix=REJECT-WRONG-INNER-AS
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/unproved_addrspacecast.ll 2>&1 | FileCheck %s --check-prefix=REJECT-UNPROVED-ADDRSPACECAST
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/wrong_layout.ll 2>&1 | FileCheck %s --check-prefix=REJECT-WRONG-LAYOUT
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/empty_slot_array.ll 2>&1 | FileCheck %s --check-prefix=REJECT-EMPTY-SLOT-ARRAY
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/volatile_slot.ll 2>&1 | FileCheck %s --check-prefix=REJECT-VOLATILE-SLOT
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/atomic_slot.ll 2>&1 | FileCheck %s --check-prefix=REJECT-ATOMIC-SLOT
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/shifted_reverse.ll 2>&1 | FileCheck %s --check-prefix=REJECT-SHIFTED-REVERSE
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/nonexact_reverse.ll 2>&1 | FileCheck %s --check-prefix=REJECT-NONEXACT-REVERSE
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/native_escape.ll 2>&1 | FileCheck %s --check-prefix=REJECT-NATIVE-ESCAPE
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/native_write.ll 2>&1 | FileCheck %s --check-prefix=REJECT-NATIVE-WRITE
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/padded_wrapper.ll 2>&1 | FileCheck %s --check-prefix=REJECT-PADDED-WRAPPER
; REJECT-PADDED-WRAPPER: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: root wrapper layout mismatch
; Original negatives retain a direct select user at the unchanged legacy guard.
; New inverse negatives retain a select or GEP user that the same guard rejects.
; They assert actual compiler rejection, not merely refusal by the new helper.
; Native opt/FileCheck is not run by current CI. No execution is claimed.
; REJECT-UNKNOWN-SLOT: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved pointer slot
; REJECT-RAW-BYTE-SLOT: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved pointer slot
; REJECT-NONZERO-POINTER-FIELD: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved pointer slot
; REJECT-WRONG-INNER-AS: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved pointer slot
; REJECT-UNPROVED-ADDRSPACECAST: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved pointer slot
; REJECT-WRONG-LAYOUT: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: root vector element type mismatch
; REJECT-EMPTY-SLOT-ARRAY: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved pointer slot
; REJECT-VOLATILE-SLOT: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved pointer slot
; REJECT-ATOMIC-SLOT: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved pointer slot
; REJECT-SHIFTED-REVERSE: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved reverse pointer slot
; REJECT-NONEXACT-REVERSE: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unproved reverse pointer slot
; REJECT-NATIVE-ESCAPE: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unsupported native-view user
; REJECT-NATIVE-WRITE: invalid pointer bitcast: failed to run bitcast fixup (unhandled instructions); unsupported users: select; constant-vector graph: unsupported native-view user

; Additional inverse-provenance rejection modules; source-only, not executed.
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_mixed_nominal.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-MIXED-NOMINAL
; REJECT-INVERSE-MIXED-NOMINAL: constant-vector graph: inverse root nominal type mismatch (bitcast)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_vector_root.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-VECTOR-ROOT
; REJECT-INVERSE-VECTOR-ROOT: constant-vector graph: inverse root nominal type mismatch (bitcast)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_zero_gep.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-ZERO-GEP
; REJECT-INVERSE-ZERO-GEP: constant-vector graph: unsupported inverse pointer input (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_shifted_gep.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-SHIFTED-GEP
; REJECT-INVERSE-SHIFTED-GEP: constant-vector graph: unsupported inverse pointer input (getelementptr)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_rootless_subcycle.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-ROOTLESS-SUBCYCLE
; REJECT-INVERSE-ROOTLESS-SUBCYCLE: constant-vector graph: unanchored inverse pointer cycle (phi)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_transitive_native.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-TRANSITIVE-NATIVE
; REJECT-INVERSE-TRANSITIVE-NATIVE: constant-vector graph: inverse root depends on rewritten native graph (bitcast)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_null_native.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-NULL-NATIVE
; REJECT-INVERSE-NULL-NATIVE: constant-vector graph: unproved inverse native root (bitcast)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_undef_native.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-UNDEF-NATIVE
; REJECT-INVERSE-UNDEF-NATIVE: constant-vector graph: unproved inverse native root (bitcast)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_poison_native.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-POISON-NATIVE
; REJECT-INVERSE-POISON-NATIVE: constant-vector graph: unproved inverse native root (bitcast)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_unknown_arm.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-UNKNOWN-ARM
; REJECT-INVERSE-UNKNOWN-ARM: constant-vector graph: unmapped argument or constant pointer root
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_scalar_view.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-SCALAR-VIEW
; REJECT-INVERSE-SCALAR-VIEW: constant-vector graph: scalar-to-aggregate inverse pointer cast (select)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_merged_escape.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-MERGED-ESCAPE
; REJECT-INVERSE-MERGED-ESCAPE: constant-vector graph: unsupported native-view user (call)
; RUN: not opt -enable-new-pm=0 -PointerBCFixup -disable-output %t/inverse_merged_write.ll 2>&1 | FileCheck %s --check-prefix=REJECT-INVERSE-MERGED-WRITE
; REJECT-INVERSE-MERGED-WRITE: constant-vector graph: unsupported native-view user (store)

;--- slots.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
%Arguments = type { %Buffer addrspace(2)*, i64 }
%PackedArguments = type <{ %Buffer addrspace(2)*, i8 }>
%PointerRow = type { [3 x %Buffer addrspace(2)*] }
%NestedSlots = type { [2 x %PointerRow], i64 }
%Other = type { [8 x <4 x float>] }

; The outer slot AS1 is distinct from the AS2 pointer stored in it. Explicit
; alignment 1 and pointer-valued metadata must survive the single slot load.
; POS-LABEL: define floor_fragment <4 x i32> @direct_slot(
; POS: %raw.storage = getelementptr %Buffer, %Buffer addrspace(2)* %base, i32 0, i32 0, i32 0
; POS-NEXT: %loaded.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(1)* %slot, align 1, !invariant.load ![[EMPTY:[0-9]+]], !nonnull ![[EMPTY]], !dereferenceable ![[BYTES:[0-9]+]], !align ![[BYTES]]
; POS-NEXT: %loaded.storage = getelementptr %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i32 0
; POS-NEXT: %chosen.typed = select i1 %condition, <4 x float> addrspace(2)* %raw.storage, <4 x float> addrspace(2)* %loaded.storage
; POS-NEXT: %row.typed = getelementptr inbounds <4 x float>, <4 x float> addrspace(2)* %chosen.typed, i64 %index
; POS-NEXT: %value.typed = load <4 x float>, <4 x float> addrspace(2)* %row.typed, align 4
; POS-NEXT: %value.bits = bitcast <4 x float> %value.typed to <4 x i32>
; POS-NEXT: ret <4 x i32> %value.bits
define floor_fragment <4 x i32> @direct_slot(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(1)* %slot, i1 %condition, i64 %index) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(1)* %slot to <4 x i32> addrspace(2)* addrspace(1)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(1)* %cast.slot, align 1, !invariant.load !0, !nonnull !0, !dereferenceable !1, !align !2
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %row = getelementptr inbounds <4 x i32>, <4 x i32> addrspace(2)* %chosen, i64 %index
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %row, align 4
  ret <4 x i32> %value
}

; The packed argument record has another field and only byte alignment. This is a pointer-sized field-zero
; read, not a claim that the whole argument record is a vector buffer wrapper.
; POS-LABEL: define floor_fragment <4 x i32> @folded_record(
; POS: %loaded.slot = getelementptr %PackedArguments, %PackedArguments addrspace(2)* %args, i32 0, i32 0
; POS-NEXT: %loaded.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %loaded.slot, align 1
; POS-NEXT: %loaded.storage = getelementptr %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i32 0
; POS-NEXT: %chosen.typed = select i1 %condition, <4 x float> addrspace(2)* %loaded.storage, <4 x float> addrspace(2)* %raw.storage
; POS-NEXT: %value.typed = load <4 x float>, <4 x float> addrspace(2)* %chosen.typed, align 16
define floor_fragment <4 x i32> @folded_record(%Buffer addrspace(2)* %base, %PackedArguments addrspace(2)* %args, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %PackedArguments addrspace(2)* %args to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 1
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %loaded, <4 x i32> addrspace(2)* %raw
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

; POS-LABEL: define floor_fragment <4 x i32> @nested_slot(
; POS: %loaded.slot = getelementptr %NestedSlots, %NestedSlots addrspace(2)* %args, i32 0, i32 0, i32 0, i32 0, i32 0
; POS-NEXT: %loaded.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %loaded.slot, align 8
; POS-NEXT: %loaded.storage = getelementptr %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i32 0
; POS: %chosen.typed = select i1 %condition, <4 x float> addrspace(2)* %raw.storage, <4 x float> addrspace(2)* %loaded.storage
define floor_fragment <4 x i32> @nested_slot(%Buffer addrspace(2)* %base, %NestedSlots addrspace(2)* %args, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %NestedSlots addrspace(2)* %args to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

; A slot may already store the exact canonical vector pointer.
; POS-LABEL: define floor_fragment <4 x i32> @vector_slot(
; POS: %loaded.pointer = load <4 x float> addrspace(2)*, <4 x float> addrspace(2)* addrspace(2)* %slot, align 8
; POS-NEXT: %chosen.typed = select i1 %condition, <4 x float> addrspace(2)* %raw.storage, <4 x float> addrspace(2)* %loaded.pointer
define floor_fragment <4 x i32> @vector_slot(%Buffer addrspace(2)* %base, <4 x float> addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast <4 x float> addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

; Scalar pointer recovery must not invent a vector grouping or change strides.
; POS-LABEL: define floor_fragment i32 @scalar_slot(
; POS: %lane.typed = getelementptr <4 x float>, <4 x float> addrspace(2)* %raw.storage, i64 %row, i32 %lane.index
; POS-NEXT: %loaded.pointer = load float addrspace(2)*, float addrspace(2)* addrspace(2)* %slot, align 8
; POS-NEXT: %chosen.typed = select i1 %condition, float addrspace(2)* %lane.typed, float addrspace(2)* %loaded.pointer
; POS-NEXT: %value.typed = load float, float addrspace(2)* %chosen.typed, align 4
define floor_fragment i32 @scalar_slot(%Buffer addrspace(2)* %base, float addrspace(2)* addrspace(2)* %slot, i1 %condition, i64 %row, i32 %lane.index) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %lane = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %raw, i64 %row, i32 %lane.index
  %cast.slot = bitcast float addrspace(2)* addrspace(2)* %slot to i32 addrspace(2)* addrspace(2)*
  %loaded = load i32 addrspace(2)*, i32 addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, i32 addrspace(2)* %lane, i32 addrspace(2)* %loaded
  %value = load i32, i32 addrspace(2)* %chosen, align 4
  ret i32 %value
}

; One recovered pointer feeds both integer-view reads and an exact native
; recast. Native aggregate GEPs, block edges and index widths stay untouched.
; POS-LABEL: define floor_fragment <4 x float> @exact_reverse_mixed(
; POS: entry:
; POS-NEXT: %loaded.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %slot, align 8
; POS-NEXT: %loaded.storage = getelementptr %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i32 0
; POS-NEXT: %words.typed = load <4 x float>, <4 x float> addrspace(2)* %loaded.storage, align 16
; POS-NEXT: %words.bits = bitcast <4 x float> %words.typed to <4 x i32>
; POS-NEXT: %as.float = bitcast <4 x i32> %words.bits to <4 x float>
; POS-NEXT: br i1 %condition, label %left, label %right
; POS: merge:
; POS-NEXT: %native = phi %Buffer addrspace(2)* [ %loaded.pointer, %left ], [ %other, %right ]
; POS-NEXT: %native.row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %native, i32 0, i32 0, i64 %row
; POS-NEXT: %value = load <4 x float>, <4 x float> addrspace(2)* %native.row, align 4
define floor_fragment <4 x float> @exact_reverse_mixed(%Buffer addrspace(2)* addrspace(2)* %slot, %Buffer addrspace(2)* %other, i1 %condition, i64 %row) {
entry:
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %words = load <4 x i32>, <4 x i32> addrspace(2)* %loaded, align 16
  %as.float = bitcast <4 x i32> %words to <4 x float>
  %back = bitcast <4 x i32> addrspace(2)* %loaded to %Buffer addrspace(2)*
  br i1 %condition, label %left, label %right
left:
  br label %merge
right:
  br label %merge
merge:
  %native = phi %Buffer addrspace(2)* [ %back, %left ], [ %other, %right ]
  %native.row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %native, i32 0, i32 0, i64 %row
  %value = load <4 x float>, <4 x float> addrspace(2)* %native.row, align 4
  %sum = fadd <4 x float> %value, %as.float
  ret <4 x float> %sum
}

; Native-only inverse recovery must also retain one load and loop PHI edges.
; POS-LABEL: define floor_fragment float @exact_reverse_native_only(
; POS: %loaded.slot = getelementptr %Arguments, %Arguments addrspace(2)* %args, i32 0, i32 0
; POS-NEXT: %loaded.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %loaded.slot, align 1
; POS: loop:
; POS-NEXT: %native = phi %Buffer addrspace(2)* [ %loaded.pointer, %entry ], [ %next, %loop ]
; POS-NEXT: %next = select i1 %switch, %Buffer addrspace(2)* %other, %Buffer addrspace(2)* %native
; POS: %native.lane = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %native, i32 0, i32 0, i64 %row, i32 2
; POS-NEXT: %value = load float, float addrspace(2)* %native.lane, align 4
define floor_fragment float @exact_reverse_native_only(%Arguments addrspace(2)* %args, %Buffer addrspace(2)* %other, i1 %switch, i1 %again, i64 %row) {
entry:
  %cast.slot = bitcast %Arguments addrspace(2)* %args to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 1
  %back = bitcast <4 x i32> addrspace(2)* %loaded to %Buffer addrspace(2)*
  br label %loop
loop:
  %native = phi %Buffer addrspace(2)* [ %back, %entry ], [ %next, %loop ]
  %next = select i1 %switch, %Buffer addrspace(2)* %other, %Buffer addrspace(2)* %native
  br i1 %again, label %loop, label %exit
exit:
  %native.lane = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %native, i32 0, i32 0, i64 %row, i32 2
  %value = load float, float addrspace(2)* %native.lane, align 4
  ret float %value
}

; A live shared slot cast must remain in the outer worklist. Only the first
; load belongs to the rewritten graph; the second is unchanged legacy input.
; POS-LABEL: define floor_fragment <4 x i32> @shared_slot_cast(
; POS: %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
; POS-NEXT: %loaded.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %slot, align 8
; POS-NEXT: %loaded.storage = getelementptr %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i32 0
; POS-NEXT: %unrelated = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
; POS: %other.value = load <4 x i32>, <4 x i32> addrspace(2)* %unrelated, align 16
define floor_fragment <4 x i32> @shared_slot_cast(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %unrelated = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  %other.value = load <4 x i32>, <4 x i32> addrspace(2)* %unrelated, align 16
  %sum = add <4 x i32> %value, %other.value
  ret <4 x i32> %sum
}

; Two consumed loads share the same slot cast. Both keep their original sites,
; and the now-dead slot cast is removed exactly once after both loads are gone.
; POS-LABEL: define floor_fragment <4 x i32> @shared_consumed_slot(
; POS: %first.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %slot, align 8
; POS-NEXT: %first.storage = getelementptr %Buffer, %Buffer addrspace(2)* %first.pointer, i32 0, i32 0, i32 0
; POS-NEXT: %second.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %slot, align 1
; POS-NEXT: %second.storage = getelementptr %Buffer, %Buffer addrspace(2)* %second.pointer, i32 0, i32 0, i32 0
; POS-NEXT: %loaded.choice.typed = select i1 %condition, <4 x float> addrspace(2)* %first.storage, <4 x float> addrspace(2)* %second.storage
; POS-NEXT: %chosen.typed = select i1 %condition, <4 x float> addrspace(2)* %raw.storage, <4 x float> addrspace(2)* %loaded.choice.typed
define floor_fragment <4 x i32> @shared_consumed_slot(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %first = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %second = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 1
  %loaded.choice = select i1 %condition, <4 x i32> addrspace(2)* %first, <4 x i32> addrspace(2)* %second
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded.choice
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

; The inverse recast is replaced before rebuilding a canonical root which
; depends on a native aggregate GEP. The native GEP itself is not reindexed.
; POS-LABEL: define floor_fragment <4 x i32> @native_recast_crosslink(
; POS: %loaded.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %slot, align 8
; POS-NEXT: %loaded.storage = getelementptr %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i32 0
; POS-NEXT: %native.row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %loaded.pointer, i32 0, i32 0, i64 %row
; POS-NEXT: %chosen.typed = select i1 %condition, <4 x float> addrspace(2)* %loaded.storage, <4 x float> addrspace(2)* %native.row
; POS-NEXT: %value.typed = load <4 x float>, <4 x float> addrspace(2)* %chosen.typed, align 4
define floor_fragment <4 x i32> @native_recast_crosslink(%Buffer addrspace(2)* addrspace(2)* %slot, i1 %condition, i64 %row) {
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %back = bitcast <4 x i32> addrspace(2)* %loaded to %Buffer addrspace(2)*
  %native.row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i64 %row
  %view = bitcast <4 x float> addrspace(2)* %native.row to <4 x i32> addrspace(2)*
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %loaded, <4 x i32> addrspace(2)* %view
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 4
  ret <4 x i32> %value
}


; New inverse provenance cases. These symbolic checks are not a claim that
; the remaining private kernel has any particular optimized incoming graph.
; POS-LABEL: define floor_fragment <4 x float> @inverse_root_roundtrip(
; POS: %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %base, i32 0, i32 0, i64 %index
; POS-NEXT: %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
; POS-NEXT: ret <4 x float> %value
define floor_fragment <4 x float> @inverse_root_roundtrip(%Buffer addrspace(2)* %base, i64 %index) {
  %view = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %back = bitcast <4 x i32> addrspace(2)* %view to %Buffer addrspace(2)*
  %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i64 %index
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

; The slot is read exactly once in its original predecessor. The native and
; canonical PHIs preserve predecessor order; native aggregate indexing is intact.
; POS-LABEL: define floor_fragment <4 x i32> @inverse_diamond(
; POS: left:
; POS: %a.storage = getelementptr %Buffer, %Buffer addrspace(2)* %base, i32 0, i32 0, i32 0
; POS: right:
; POS-NEXT: %b.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(1)* %slot, align 1, !invariant.load ![[EMPTY]]
; POS-NEXT: %b.storage = getelementptr %Buffer, %Buffer addrspace(2)* %b.pointer, i32 0, i32 0, i32 0
; POS: merge:
; POS-NEXT: %p.native = phi %Buffer addrspace(2)* [ %base, %left ], [ %b.pointer, %right ]
; POS-NEXT: %p.typed = phi <4 x float> addrspace(2)* [ %a.storage, %left ], [ %b.storage, %right ]
; POS-NEXT: %bits.typed = load <4 x float>, <4 x float> addrspace(2)* %p.typed, align 4
; POS-NEXT: %bits.bits = bitcast <4 x float> %bits.typed to <4 x i32>
; POS-NEXT: %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %p.native, i32 0, i32 0, i64 %index
; POS-NEXT: %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 8
define floor_fragment <4 x i32> @inverse_diamond(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(1)* %slot, i1 %condition, i64 %index) {
entry:
  br i1 %condition, label %left, label %right
left:
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  br label %merge
right:
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(1)* %slot to <4 x i32> addrspace(2)* addrspace(1)*
  %b = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(1)* %cast.slot, align 1, !invariant.load !0
  br label %merge
merge:
  %p = phi <4 x i32> addrspace(2)* [ %a, %left ], [ %b, %right ]
  %bits = load <4 x i32>, <4 x i32> addrspace(2)* %p, align 4
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i64 %index
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 8
  %value.bits = bitcast <4 x float> %value to <4 x i32>
  %result = xor <4 x i32> %bits, %value.bits
  ret <4 x i32> %result
}

; No original wrapper-to-vector instruction exists in this case. The reverse
; cast must seed proof through the select and both explicitly typed slot loads.
; POS-LABEL: define floor_fragment <4 x float> @inverse_slot_select(
; POS: %a.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(1)* %first, align 1
; POS: %b.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %second, align 8
; POS: %p.native = select i1 %condition, %Buffer addrspace(2)* %a.pointer, %Buffer addrspace(2)* %b.pointer, !prof ![[WEIGHTS:[0-9]+]]
; POS-NEXT: %p.typed = select i1 %condition, <4 x float> addrspace(2)* %a.storage, <4 x float> addrspace(2)* %b.storage, !prof ![[WEIGHTS]]
; POS-NEXT: %row = getelementptr %Buffer, %Buffer addrspace(2)* %p.native, i64 %object, i32 0, i64 %index
; POS-NEXT: %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
define floor_fragment <4 x float> @inverse_slot_select(%Buffer addrspace(2)* addrspace(1)* %first, %Buffer addrspace(2)* addrspace(2)* %second, i1 %condition, i64 %object, i64 %index) {
  %first.cast = bitcast %Buffer addrspace(2)* addrspace(1)* %first to <4 x i32> addrspace(2)* addrspace(1)*
  %a = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(1)* %first.cast, align 1
  %second.cast = bitcast %Buffer addrspace(2)* addrspace(2)* %second to <4 x i32> addrspace(2)* addrspace(2)*
  %b = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %second.cast, align 8
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b, !prof !3
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i64 %object, i32 0, i64 %index
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

; POS-LABEL: define floor_fragment <4 x float> @inverse_loop(
; POS: loop:
; POS-NEXT: %p.native = phi %Buffer addrspace(2)* [ %first, %entry ], [ %next.native, %loop ]
; POS-NEXT: %p.typed = phi <4 x float> addrspace(2)* [ %a.storage, %entry ], [ %next.typed, %loop ]
; POS-NEXT: %next.native = select i1 %choose, %Buffer addrspace(2)* %p.native, %Buffer addrspace(2)* %second
; POS-NEXT: %next.typed = select i1 %choose, <4 x float> addrspace(2)* %p.typed, <4 x float> addrspace(2)* %b.storage
; POS-NEXT: %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %p.native, i32 0, i32 0, i64 %index
; POS-NEXT: %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
define floor_fragment <4 x float> @inverse_loop(%Buffer addrspace(2)* %first, %Buffer addrspace(2)* %second, i1 %choose, i1 %again, i64 %index) {
entry:
  %a = bitcast %Buffer addrspace(2)* %first to <4 x i32> addrspace(2)*
  %b = bitcast %Buffer addrspace(2)* %second to <4 x i32> addrspace(2)*
  br label %loop
loop:
  %p = phi <4 x i32> addrspace(2)* [ %a, %entry ], [ %next, %loop ]
  %next = select i1 %choose, <4 x i32> addrspace(2)* %p, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i64 %index
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  br i1 %again, label %loop, label %exit
exit:
  ret <4 x float> %value
}

; Both PHIs have two identical self-edge entries because the switch has two
; edges back to the loop. Native PHIs must remain in the PHI prefix.
; POS-LABEL: define floor_fragment <4 x float> @inverse_duplicate_edges(
; POS: loop:
; POS-NEXT: %p.native = phi %Buffer addrspace(2)* [ %first, %entry ], [ %q.native, %loop ], [ %q.native, %loop ]
; POS-NEXT: %p.typed = phi <4 x float> addrspace(2)* [ %a.storage, %entry ], [ %q.typed, %loop ], [ %q.typed, %loop ]
; POS-NEXT: %q.native = phi %Buffer addrspace(2)* [ %second, %entry ], [ %p.native, %loop ], [ %p.native, %loop ]
; POS-NEXT: %q.typed = phi <4 x float> addrspace(2)* [ %b.storage, %entry ], [ %p.typed, %loop ], [ %p.typed, %loop ]
; POS-NEXT: %row = getelementptr %Buffer, %Buffer addrspace(2)* %p.native, i32 0, i32 0, i64 %index
define floor_fragment <4 x float> @inverse_duplicate_edges(%Buffer addrspace(2)* %first, %Buffer addrspace(2)* %second, i32 %choice, i64 %index) {
entry:
  %a = bitcast %Buffer addrspace(2)* %first to <4 x i32> addrspace(2)*
  %b = bitcast %Buffer addrspace(2)* %second to <4 x i32> addrspace(2)*
  br label %loop
loop:
  %p = phi <4 x i32> addrspace(2)* [ %a, %entry ], [ %q, %loop ], [ %q, %loop ]
  %q = phi <4 x i32> addrspace(2)* [ %b, %entry ], [ %p, %loop ], [ %p, %loop ]
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i64 %index
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  switch i32 %choice, label %exit [ i32 0, label %loop
                                  i32 1, label %loop ]
exit:
  ret <4 x float> %value
}

; The local inverse subgraph has exact Buffer roots, although an Other slot is
; connected to the same canonical component. Its old direct-slot inverse is
; still accepted, rather than imposing a global nominal-type restriction.
; POS-LABEL: define floor_fragment <4 x i32> @inverse_local_nominal_types(
; POS: %a.pointer = load %Buffer addrspace(2)*, %Buffer addrspace(2)* addrspace(2)* %first, align 8
; POS: %b.pointer = load %Other addrspace(2)*, %Other addrspace(2)* addrspace(2)* %second, align 8
; POS: %p.native = select i1 %condition, %Buffer addrspace(2)* %base, %Buffer addrspace(2)* %a.pointer
; POS: %native.row = getelementptr %Buffer, %Buffer addrspace(2)* %p.native, i32 0, i32 0, i64 %index
; POS: %other.row = getelementptr %Other, %Other addrspace(2)* %b.pointer, i32 0, i32 0, i64 %index
define floor_fragment <4 x i32> @inverse_local_nominal_types(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(2)* %first, %Other addrspace(2)* addrspace(2)* %second, i1 %condition, i64 %index) {
  %seed = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %first.cast = bitcast %Buffer addrspace(2)* addrspace(2)* %first to <4 x i32> addrspace(2)* addrspace(2)*
  %a = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %first.cast, align 8
  %second.cast = bitcast %Other addrspace(2)* addrspace(2)* %second to <4 x i32> addrspace(2)* addrspace(2)*
  %b = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %second.cast, align 8
  %p = select i1 %condition, <4 x i32> addrspace(2)* %seed, <4 x i32> addrspace(2)* %a
  %q = select i1 %condition, <4 x i32> addrspace(2)* %p, <4 x i32> addrspace(2)* %b
  %bits = load <4 x i32>, <4 x i32> addrspace(2)* %q, align 4
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %native.row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i64 %index
  %native.value = load <4 x float>, <4 x float> addrspace(2)* %native.row, align 4
  %other.back = bitcast <4 x i32> addrspace(2)* %b to %Other addrspace(2)*
  %other.row = getelementptr %Other, %Other addrspace(2)* %other.back, i32 0, i32 0, i64 %index
  %other.value = load <4 x float>, <4 x float> addrspace(2)* %other.row, align 4
  %sum = fadd <4 x float> %native.value, %other.value
  %sum.bits = bitcast <4 x float> %sum to <4 x i32>
  %result = xor <4 x i32> %bits, %sum.bits
  ret <4 x i32> %result
}

; The previous direct-slot inverse may restore a vector pointer, not a wrapper.
; The new aggregate-only merge proof must not intercept that accepted case.
; POS-LABEL: define floor_fragment <4 x i32> @inverse_direct_vector_slot(
; POS: %a.pointer = load <4 x float> addrspace(2)*, <4 x float> addrspace(2)* addrspace(2)* %slot, align 8
; POS: %native.row = getelementptr <4 x float>, <4 x float> addrspace(2)* %a.pointer, i64 %index
; POS-NEXT: %native.value = load <4 x float>, <4 x float> addrspace(2)* %native.row, align 4
define floor_fragment <4 x i32> @inverse_direct_vector_slot(%Buffer addrspace(2)* %base, <4 x float> addrspace(2)* addrspace(2)* %slot, i1 %condition, i64 %index) {
  %seed = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %slot.cast = bitcast <4 x float> addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %a = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %slot.cast, align 8
  %p = select i1 %condition, <4 x i32> addrspace(2)* %seed, <4 x i32> addrspace(2)* %a
  %bits = load <4 x i32>, <4 x i32> addrspace(2)* %p, align 4
  %back = bitcast <4 x i32> addrspace(2)* %a to <4 x float> addrspace(2)*
  %native.row = getelementptr <4 x float>, <4 x float> addrspace(2)* %back, i64 %index
  %native.value = load <4 x float>, <4 x float> addrspace(2)* %native.row, align 4
  %native.bits = bitcast <4 x float> %native.value to <4 x i32>
  %result = xor <4 x i32> %bits, %native.bits
  ret <4 x i32> %result
}

; POS: ![[EMPTY]] = !{}
; POS: ![[BYTES]] = !{i64 16}
; POS: ![[WEIGHTS]] = !{!"branch_weights", i32 3, i32 7}
!0 = !{}
!1 = !{i64 16}
!2 = !{i64 16}
!3 = !{!"branch_weights", i32 3, i32 7}

;--- unknown_slot.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x i32> @reject_unknown_slot(%Buffer addrspace(2)* %base, <4 x i32> addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- raw_byte_slot.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x i32> @reject_raw_byte_slot(%Buffer addrspace(2)* %base, i8 addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast i8 addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 1
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- nonzero_pointer_field.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
%Nonzero = type { i64, %Buffer addrspace(2)* }

define floor_fragment <4 x i32> @reject_nonzero_pointer_field(%Buffer addrspace(2)* %base, %Nonzero addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Nonzero addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- wrong_inner_as.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x i32> @reject_wrong_inner_as(%Buffer addrspace(2)* %base, %Buffer addrspace(1)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Buffer addrspace(1)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- unproved_addrspacecast.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x i32> @reject_unproved_addrspacecast(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(1)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = addrspacecast %Buffer addrspace(2)* addrspace(1)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- wrong_layout.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
%Wrong = type { [8 x <2 x double>] }

define floor_fragment <4 x i32> @reject_wrong_layout(%Buffer addrspace(2)* %base, %Wrong addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Wrong addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- empty_slot_array.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x i32> @reject_empty_slot_array(%Buffer addrspace(2)* %base, [0 x %Buffer addrspace(2)*] addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast [0 x %Buffer addrspace(2)*] addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- volatile_slot.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x i32> @reject_volatile_slot(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load volatile <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- atomic_slot.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x i32> @reject_atomic_slot(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load atomic <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot monotonic, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- shifted_reverse.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_shifted_reverse(%Buffer addrspace(2)* addrspace(2)* %slot, %Buffer addrspace(2)* %other, i1 %condition) {
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %offset = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %loaded, i64 1
  %back = bitcast <4 x i32> addrspace(2)* %offset to %Buffer addrspace(2)*
  %chosen = select i1 %condition, %Buffer addrspace(2)* %back, %Buffer addrspace(2)* %other
  %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %chosen, i32 0, i32 0, i32 0

  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 16
  ret <4 x float> %value
}

;--- nonexact_reverse.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
%Other = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_nonexact_reverse(%Buffer addrspace(2)* addrspace(2)* %slot, %Other addrspace(2)* %other, i1 %condition) {
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %back = bitcast <4 x i32> addrspace(2)* %loaded to %Other addrspace(2)*
  %chosen = select i1 %condition, %Other addrspace(2)* %back, %Other addrspace(2)* %other
  %row = getelementptr inbounds %Other, %Other addrspace(2)* %chosen, i32 0, i32 0, i32 0

  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 16
  ret <4 x float> %value
}

;--- native_escape.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
declare void @escape(%Buffer addrspace(2)*)

define floor_fragment <4 x float> @reject_native_escape(%Buffer addrspace(2)* addrspace(2)* %slot, %Buffer addrspace(2)* %other, i1 %condition) {
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %back = bitcast <4 x i32> addrspace(2)* %loaded to %Buffer addrspace(2)*
  %chosen = select i1 %condition, %Buffer addrspace(2)* %back, %Buffer addrspace(2)* %other
  %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %chosen, i32 0, i32 0, i32 0
  call void @escape(%Buffer addrspace(2)* %chosen)
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 16
  ret <4 x float> %value
}

;--- native_write.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_native_write(%Buffer addrspace(2)* addrspace(2)* %slot, %Buffer addrspace(2)* %other, i1 %condition) {
  %cast.slot = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %back = bitcast <4 x i32> addrspace(2)* %loaded to %Buffer addrspace(2)*
  %chosen = select i1 %condition, %Buffer addrspace(2)* %back, %Buffer addrspace(2)* %other
  %row = getelementptr inbounds %Buffer, %Buffer addrspace(2)* %chosen, i32 0, i32 0, i32 0
  store <4 x float> zeroinitializer, <4 x float> addrspace(2)* %row, align 16
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 16
  ret <4 x float> %value
}

;--- padded_wrapper.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
%Padded = type { i32, [8 x <4 x float>] }
define floor_fragment <4 x i32> @reject_padded_wrapper(%Buffer addrspace(2)* %base, %Padded addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %raw = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %cast.slot = bitcast %Padded addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %cast.slot, align 8
  %chosen = select i1 %condition, <4 x i32> addrspace(2)* %raw, <4 x i32> addrspace(2)* %loaded
  %value = load <4 x i32>, <4 x i32> addrspace(2)* %chosen, align 16
  ret <4 x i32> %value
}

;--- inverse_mixed_nominal.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
%Other = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_mixed_nominal(%Buffer addrspace(2)* %first, %Other addrspace(2)* %second, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %first to <4 x i32> addrspace(2)*
  %b = bitcast %Other addrspace(2)* %second to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_vector_root.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_vector_root(%Buffer addrspace(2)* %first, <4 x float> addrspace(2)* %second, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %first to <4 x i32> addrspace(2)*
  %b = bitcast <4 x float> addrspace(2)* %second to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_zero_gep.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_zero_gep(%Buffer addrspace(2)* %base, i1 %condition, i64 %offset) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %b = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 0
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_shifted_gep.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_shifted_gep(%Buffer addrspace(2)* %base, i1 %condition, i64 %offset) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %b = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i64 %offset
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_rootless_subcycle.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

; The unreachable self-loop is valid symbolic SSA. Its rootless PHI must not
; become "proven" merely because the outer select also reaches a valid root.
define floor_fragment <4 x float> @reject_inverse_rootless_subcycle(%Buffer addrspace(2)* %base, i1 %condition) {
entry:
  ret <4 x float> zeroinitializer
dead:
  %rootless = phi <4 x i32> addrspace(2)* [ %rootless, %dead ]
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %rootless
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  br label %dead
}

;--- inverse_transitive_native.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

; The native pointer-use closure stops at the data load. Full SSA ancestry
; must still reject the later inttoptr root as a new inverse recovery boundary.
define floor_fragment <4 x i32> @reject_inverse_transitive_native(%Buffer addrspace(2)* %base, %Buffer addrspace(2)* addrspace(2)* %slot, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %slot.cast = bitcast %Buffer addrspace(2)* addrspace(2)* %slot to <4 x i32> addrspace(2)* addrspace(2)*
  %loaded = load <4 x i32> addrspace(2)*, <4 x i32> addrspace(2)* addrspace(2)* %slot.cast, align 8
  %old.back = bitcast <4 x i32> addrspace(2)* %loaded to %Buffer addrspace(2)*
  %native.lane = getelementptr %Buffer, %Buffer addrspace(2)* %old.back, i32 0, i32 0, i32 0, i32 0
  %number = load float, float addrspace(2)* %native.lane, align 4
  %integer = bitcast float %number to i32
  %wide = zext i32 %integer to i64
  %dependent = inttoptr i64 %wide to %Buffer addrspace(2)*
  %b = bitcast %Buffer addrspace(2)* %dependent to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %q = select i1 %condition, <4 x i32> addrspace(2)* %p, <4 x i32> addrspace(2)* %loaded
  %bits = load <4 x i32>, <4 x i32> addrspace(2)* %q, align 4
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  %value.bits = bitcast <4 x float> %value to <4 x i32>
  %result = xor <4 x i32> %bits, %value.bits
  ret <4 x i32> %result
}

;--- inverse_null_native.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_null_native(%Buffer addrspace(2)* %base, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %b = bitcast %Buffer addrspace(2)* null to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_undef_native.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_undef_native(%Buffer addrspace(2)* %base, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %b = bitcast %Buffer addrspace(2)* undef to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_poison_native.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_poison_native(%Buffer addrspace(2)* %base, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %b = bitcast %Buffer addrspace(2)* poison to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_unknown_arm.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_unknown_arm(%Buffer addrspace(2)* %base, <4 x i32> addrspace(2)* %unknown, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %unknown
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_scalar_view.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_scalar_view(%Buffer addrspace(2)* %base, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %base to <4 x i32> addrspace(2)*
  %lane = getelementptr <4 x i32>, <4 x i32> addrspace(2)* %a, i32 0, i32 0
  %p = select i1 %condition, i32 addrspace(2)* %lane, i32 addrspace(2)* %lane
  %back = bitcast i32 addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_merged_escape.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }
declare void @escape(%Buffer addrspace(2)*)

define floor_fragment <4 x float> @reject_inverse_merged_escape(%Buffer addrspace(2)* %first, %Buffer addrspace(2)* %second, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %first to <4 x i32> addrspace(2)*
  %b = bitcast %Buffer addrspace(2)* %second to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  call void @escape(%Buffer addrspace(2)* %back)
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}

;--- inverse_merged_write.ll
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "air64-apple-ios18.0.0"
%Buffer = type { [8 x <4 x float>] }

define floor_fragment <4 x float> @reject_inverse_merged_write(%Buffer addrspace(2)* %first, %Buffer addrspace(2)* %second, i1 %condition) {
  %a = bitcast %Buffer addrspace(2)* %first to <4 x i32> addrspace(2)*
  %b = bitcast %Buffer addrspace(2)* %second to <4 x i32> addrspace(2)*
  %p = select i1 %condition, <4 x i32> addrspace(2)* %a, <4 x i32> addrspace(2)* %b
  %back = bitcast <4 x i32> addrspace(2)* %p to %Buffer addrspace(2)*
  %row = getelementptr %Buffer, %Buffer addrspace(2)* %back, i32 0, i32 0, i32 0
  store <4 x float> zeroinitializer, <4 x float> addrspace(2)* %row, align 4
  %value = load <4 x float>, <4 x float> addrspace(2)* %row, align 4
  ret <4 x float> %value
}
