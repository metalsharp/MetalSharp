%dx.types.twoi32 = type { i32, i32 }

define void @main() {
entry:
  %0 = call %dx.types.twoi32 @dx.op.cycleCounterLegacy(i32 109)
  %1 = extractvalue %dx.types.twoi32 %0, 0
  %2 = call %dx.types.twoi32 @dx.op.cycleCounterLegacy(i32 109)
  %3 = extractvalue %dx.types.twoi32 %2, 0
  %4 = sub i32 %3, %1
  call void @dx.op.storeOutput.i32(i32 5, i32 0, i32 0, i8 0, i32 %4)
  call void @dx.op.storeOutput.i32(i32 5, i32 0, i32 0, i8 1, i32 0)
  call void @dx.op.storeOutput.i32(i32 5, i32 0, i32 0, i8 2, i32 0)
  call void @dx.op.storeOutput.i32(i32 5, i32 0, i32 0, i8 3, i32 0)
  ret void
}

declare %dx.types.twoi32 @dx.op.cycleCounterLegacy(i32)
declare void @dx.op.storeOutput.i32(i32, i32, i32, i8, i32)

!dx.version = !{!0}
!dx.valver = !{!0}
!dx.shaderModel = !{!1}
!dx.entryPoints = !{!2}
!llvm.ident = !{!8}
!0 = !{i32 1, i32 0}
!1 = !{!"ps", i32 6, i32 0}
!2 = !{void ()* @main, !"main", !3, null, !7}
!3 = !{null, !4, null}
!4 = !{!5}
!5 = !{i32 0, !"SV_Target", i8 5, i8 16, !6, i8 0, i32 1, i8 4, i32 0, i8 0, null}
!6 = !{i32 0}
!7 = !{i32 0, i64 258}
!8 = !{!"metalsharp CycleCounterLegacy bounded multi-read rejection fixture"}
