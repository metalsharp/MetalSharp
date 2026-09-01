target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%struct.RWByteAddressBuffer = type { i32 }

define void @main() {
entry:
  %out = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)
  %scratch = alloca [16 x float], align 4
  %base = getelementptr [16 x float], [16 x float]* %scratch, i32 0, i32 0
  call void @dx.op.tempRegStore.i32(i32 1, i32 0, i32 4660)
  %temp_loaded = call i32 @dx.op.tempRegLoad.i32(i32 0, i32 0)
  %temp_value = add i32 %temp_loaded, 1
  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %out, i32 0, i32 undef, i32 %temp_value, i32 undef, i32 undef, i32 undef, i8 1)
  call void @dx.op.minPrecXRegStore.f32(i32 3, float* %base, i32 0, i8 0, float 5.0)
  %min_loaded = call float @dx.op.minPrecXRegLoad.f32(i32 2, float* %base, i32 0, i8 0)
  %min_value = fadd float %min_loaded, 1.0
  %min_bits = call i32 @dx.op.bitcastF32toI32(i32 127, float %min_value)
  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %out, i32 4, i32 undef, i32 %min_bits, i32 undef, i32 undef, i32 undef, i8 1)
  ret void
}

declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #0
declare void @dx.op.tempRegStore.i32(i32, i32, i32) #1
declare i32 @dx.op.tempRegLoad.i32(i32, i32) #0
declare void @dx.op.minPrecXRegStore.f32(i32, float*, i32, i8, float) #1
declare float @dx.op.minPrecXRegLoad.f32(i32, float*, i32, i8) #0
declare i32 @dx.op.bitcastF32toI32(i32, float) #0
declare void @dx.op.bufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8) #1

attributes #0 = { nounwind readonly }
attributes #1 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.entryPoints = !{!7}

!0 = !{!"dxcoob 1.9.2602.17"}
!1 = !{i32 1, i32 0}
!2 = !{i32 1, i32 9}
!3 = !{!"cs", i32 6, i32 0}
!4 = !{null, !5, null, null}
!5 = !{!6}
!6 = !{i32 0, %struct.RWByteAddressBuffer* undef, !"", i32 0, i32 0, i32 1, i32 11, i1 false, i1 false, i1 false, null}
!7 = !{void ()* @main, !"main", null, !4, !8}
!8 = !{i32 0, i64 16, i32 1, !9}
!9 = !{i32 1, i32 1, i32 1}
