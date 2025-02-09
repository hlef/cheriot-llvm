; !DO NOT AUTOGEN!
; RUN: llc @HYBRID_HARDFLOAT_ARGS@ %s -o - | \
@IF-MIPS@; RUN:   FileCheck %s --check-prefix=ASM -DPTR_DIRECTIVE=.8byte
@IF-RISCV64@; RUN:   FileCheck %s --check-prefix=ASM -DPTR_DIRECTIVE=.quad
@IF-RISCV32@; RUN:   FileCheck %s --check-prefix=ASM -DPTR_DIRECTIVE=.word
; RUN: llc @HYBRID_HARDFLOAT_ARGS@ %s -filetype=obj -o - | llvm-objdump -r -t - | \
@IF-MIPS@; RUN:   FileCheck %s --check-prefix=RELOCS '-DINTEGER_RELOC=R_MIPS_64/R_MIPS_NONE/R_MIPS_NONE' '-DCAPABILITY_RELOC=R_MIPS_CHERI_CAPABILITY/R_MIPS_NONE/R_MIPS_NONE'
@IF-RISCV64@; RUN:   FileCheck %s --check-prefix=RELOCS -DINTEGER_RELOC=R_RISCV_64 '-DCAPABILITY_RELOC=R_RISCV_CHERI_CAPABILITY'
@IF-RISCV32@; RUN:   FileCheck %s --check-prefix=RELOCS -DINTEGER_RELOC=R_RISCV_32 '-DCAPABILITY_RELOC=R_RISCV_CHERI_CAPABILITY'
target datalayout = "@HYBRID_DATALAYOUT@"

declare void @extern_fn() #0
@extern_data = external global i8, align 1

; TODO: should the inttoptr ones be tagged -> emit a constructor?

@global_ptr_const = global i8* inttoptr (iCAPRANGE 1234 to i8*), align @CAP_RANGE_BYTES@
; ASM-LABEL: .globl global_ptr_const
; ASM-NEXT:  .p2align @CAP_RANGE_BYTES_P2@
; ASM-NEXT: global_ptr_const:
; ASM-NEXT:  [[PTR_DIRECTIVE]] 1234
; ASM-NEXT:  .size global_ptr_const, @CAP_RANGE_BYTES@
@global_cap_inttoptr = global i8 addrspace(200)* inttoptr (iCAPRANGE 1234 to i8 addrspace(200)*), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_inttoptr
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_inttoptr:
; ASM-NEXT:  .chericap 1234
; ASM-NEXT:  .size global_cap_inttoptr, @CAP_BYTES@
@global_cap_addrspacecast = global i8 addrspace(200)* addrspacecast (i8* inttoptr (iCAPRANGE 1234 to i8*) to i8 addrspace(200)*), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_addrspacecast
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_addrspacecast:
; ASM-NEXT:  .chericap 1234
; ASM-NEXT:  .size global_cap_addrspacecast, @CAP_BYTES@
@global_cap_nullgep = global i8 addrspace(200)* getelementptr (i8, i8 addrspace(200)* null, iCAPRANGE 1234), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_nullgep
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_nullgep:
; ASM-NEXT:  .chericap 1234
; ASM-NEXT:  .size global_cap_nullgep, @CAP_BYTES@

@global_ptr_data = global i8* @extern_data, align @CAP_RANGE_BYTES@
; ASM-LABEL: .globl global_ptr_data
; ASM-NEXT:  .p2align @CAP_RANGE_BYTES_P2@
; ASM-NEXT: global_ptr_data:
; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data
; ASM-NEXT:  .size global_ptr_data, @CAP_RANGE_BYTES@
@global_ptr_data_past_end = global i8* getelementptr inbounds (i8, i8* @extern_data, iCAPRANGE 1), align @CAP_RANGE_BYTES@
; ASM-LABEL: .globl global_ptr_data_past_end
; ASM-NEXT:  .p2align @CAP_RANGE_BYTES_P2@
; ASM-NEXT: global_ptr_data_past_end:
; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data+1
; ASM-NEXT:  .size global_ptr_data_past_end, @CAP_RANGE_BYTES@
@global_ptr_data_two_past_end = global i8* getelementptr (i8, i8* @extern_data, iCAPRANGE 2), align @CAP_RANGE_BYTES@
; ASM-LABEL: .globl global_ptr_data_two_past_end
; ASM-NEXT:  .p2align @CAP_RANGE_BYTES_P2@
; ASM-NEXT: global_ptr_data_two_past_end:
; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data+2
; ASM-NEXT:  .size global_ptr_data_two_past_end, @CAP_RANGE_BYTES@

@global_cap_data_addrspacecast = global i8 addrspace(200)* addrspacecast (i8* @extern_data to i8 addrspace(200)*), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_data_addrspacecast
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_data_addrspacecast:
; ASM-NEXT:  .chericap extern_data
; ASM-NEXT:  .size global_cap_data_addrspacecast, @CAP_BYTES@
@global_cap_data_addrspacecast_past_end = global i8 addrspace(200)* addrspacecast (i8* getelementptr inbounds (i8, i8* @extern_data, iCAPRANGE 1) to i8 addrspace(200)*), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_data_addrspacecast_past_end
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_data_addrspacecast_past_end:
; ASM-NEXT:  .chericap extern_data+1
; ASM-NEXT:  .size global_cap_data_addrspacecast_past_end, @CAP_BYTES@
@global_cap_data_addrspacecast_two_past_end = global i8 addrspace(200)* addrspacecast (i8* getelementptr (i8, i8* @extern_data, iCAPRANGE 2) to i8 addrspace(200)*), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_data_addrspacecast_two_past_end
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_data_addrspacecast_two_past_end:
; ASM-NEXT:  .chericap extern_data+2
; ASM-NEXT:  .size global_cap_data_addrspacecast_two_past_end, @CAP_BYTES@

@global_cap_data_nullgep = global i8 addrspace(200)* getelementptr (i8, i8 addrspace(200)* null, iCAPRANGE ptrtoint (i8* @extern_data to iCAPRANGE)), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_data_nullgep
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_data_nullgep:
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
@IFNOT-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data
; ASM-NEXT:  [[PTR_DIRECTIVE]] 0
@IF-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data
; ASM-NEXT:  .size global_cap_data_nullgep, @CAP_BYTES@
@global_cap_data_nullgep_past_end = global i8 addrspace(200)* getelementptr (i8, i8 addrspace(200)* null, iCAPRANGE ptrtoint (i8* getelementptr inbounds (i8, i8* @extern_data, iCAPRANGE 1) to iCAPRANGE)), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_data_nullgep_past_end
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_data_nullgep_past_end:
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
@IFNOT-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data+1
; ASM-NEXT:  [[PTR_DIRECTIVE]] 0
@IF-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data+1
; ASM-NEXT:  .size global_cap_data_nullgep_past_end, @CAP_BYTES@
@global_cap_data_nullgep_two_past_end = global i8 addrspace(200)* getelementptr (i8, i8 addrspace(200)* null, iCAPRANGE ptrtoint (i8* getelementptr (i8, i8* @extern_data, iCAPRANGE 2) to iCAPRANGE)), align @CAP_BYTES@
; ASM-LABEL: .globl global_cap_data_nullgep_two_past_end
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_cap_data_nullgep_two_past_end:
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
@IFNOT-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data+2
; ASM-NEXT:  [[PTR_DIRECTIVE]] 0
@IF-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_data+2
; ASM-NEXT:  .size global_cap_data_nullgep_two_past_end, @CAP_BYTES@

@global_fnptr = global void ()* @extern_fn, align @CAP_RANGE_BYTES@
; ASM-LABEL: .globl global_fnptr
; ASM-NEXT:  .p2align @CAP_RANGE_BYTES_P2@
; ASM-NEXT: global_fnptr:
; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_fn
; ASM-NEXT:  .size global_fnptr, @CAP_RANGE_BYTES@
@global_fncap_addrspacecast = global void () addrspace(200)* addrspacecast (void ()* @extern_fn to void () addrspace(200)*), align @CAP_BYTES@
; ASM-LABEL: .globl global_fncap_addrspacecast
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_fncap_addrspacecast:
; ASM-NEXT:  .chericap extern_fn
; ASM-NEXT:  .size global_fncap_addrspacecast, @CAP_BYTES@
@global_fncap_intcap_addrspacecast = global i8 addrspace(200)* addrspacecast (i8* bitcast (void ()* @extern_fn to i8*) to i8 addrspace(200)*), align @CAP_BYTES@
; ASM-LABEL: .globl global_fncap_intcap_addrspacecast
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_fncap_intcap_addrspacecast:
; ASM-NEXT:  .chericap extern_fn
; ASM-NEXT:  .size global_fncap_intcap_addrspacecast, @CAP_BYTES@
@global_fncap_intcap_nullgep = global i8 addrspace(200)* getelementptr (i8, i8 addrspace(200)* null, iCAPRANGE ptrtoint (void ()* @extern_fn to iCAPRANGE)), align @CAP_BYTES@
; ASM-LABEL: .globl global_fncap_intcap_nullgep
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_fncap_intcap_nullgep:
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
@IFNOT-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_fn
; ASM-NEXT:  [[PTR_DIRECTIVE]] 0
@IF-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_fn
; ASM-NEXT:  .size global_fncap_intcap_nullgep, @CAP_BYTES@
@global_fncap_addrspacecast_plus_two = global i8 addrspace(200)* addrspacecast (i8* getelementptr (i8, i8* bitcast (void ()* @extern_fn to i8*), iCAPRANGE 2) to i8 addrspace(200)*), align @CAP_BYTES@
; ASM-LABEL: .globl global_fncap_addrspacecast_plus_two
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_fncap_addrspacecast_plus_two:
; ASM-NEXT:  .chericap extern_fn+2
; ASM-NEXT:  .size global_fncap_addrspacecast_plus_two, @CAP_BYTES@
@global_fncap_nullgep_plus_two = global i8 addrspace(200)* getelementptr (i8, i8 addrspace(200)* null, iCAPRANGE ptrtoint (i8* getelementptr (i8, i8* bitcast (void ()* @extern_fn to i8*), iCAPRANGE 2) to iCAPRANGE)), align @CAP_BYTES@
; ASM-LABEL: .globl global_fncap_nullgep_plus_two
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
; ASM-NEXT: global_fncap_nullgep_plus_two:
; ASM-NEXT:  .p2align	@CAP_BYTES_P2@
@IFNOT-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_fn+2
; ASM-NEXT:  [[PTR_DIRECTIVE]] 0
@IF-MIPS@; ASM-NEXT:  [[PTR_DIRECTIVE]] extern_fn+2
; ASM-NEXT:  .size global_fncap_nullgep_plus_two, @CAP_BYTES@


; RELOCS-LABEL: RELOCATION RECORDS FOR [.{{s?}}data]:
; RELOCS-NEXT:   OFFSET   TYPE       VALUE
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_data
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_data+0x1
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_data+0x2
; RELOCS-NEXT:  [[CAPABILITY_RELOC]] extern_data
; RELOCS-NEXT:  [[CAPABILITY_RELOC]] extern_data+0x1
; RELOCS-NEXT:  [[CAPABILITY_RELOC]] extern_data+0x2
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_data
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_data+0x1
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_data+0x2
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_fn
; RELOCS-NEXT:  [[CAPABILITY_RELOC]] extern_fn
; RELOCS-NEXT:  [[CAPABILITY_RELOC]] extern_fn
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_fn
; RELOCS-NEXT:  [[CAPABILITY_RELOC]] extern_fn+0x2
; RELOCS-NEXT:  [[INTEGER_RELOC]]    extern_fn+0x2

; Don't use .sdata for RISC-V, to allow re-using the same RELOCS lines.
!llvm.module.flags = !{!0}
!0 = !{i32 1, !"SmallDataLimit", i32 0}
