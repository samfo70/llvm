; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=i686-unknown-unknown | FileCheck %s --check-prefix=i686
; RUN: llc < %s -mtriple=x86_64-unknown-unknown | FileCheck %s --check-prefix=x86_64

define void @test_wbinvd() {
; i686-LABEL: test_wbinvd:
; i686:       # %bb.0:
; i686-NEXT:    wbinvd
; i686-NEXT:    retl
;
; x86_64-LABEL: test_wbinvd:
; x86_64:       # %bb.0:
; x86_64-NEXT:    wbinvd
; x86_64-NEXT:    retq
  call void @llvm.x86.wbinvd()
  ret void
}

declare void @llvm.x86.wbinvd()
