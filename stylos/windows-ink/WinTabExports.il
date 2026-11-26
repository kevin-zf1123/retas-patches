.assembly extern mscorlib {}
.assembly extern StylosInk {}
.assembly WinTab32 {}
.module wintab32.dll
.corflags 0x00000002
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTInfoA(int32,int32,native int) cil managed
{
  .export [1] as WTInfoA
  .maxstack 3
  ldarg.0
  ldarg.1
  ldarg.2
  call int32 [StylosInk]StylosInk.WinTab::WTInfoA(int32,int32,native int)
  ret
}
.method public hidebysig static native int modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTOpenA(native int,native int,int32) cil managed
{
  .export [2] as WTOpenA
  .maxstack 3
  ldarg.0
  ldarg.1
  ldarg.2
  call native int [StylosInk]StylosInk.WinTab::WTOpenA(native int,native int,int32)
  ret
}
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTClose(native int) cil managed
{
  .export [3] as WTClose
  .maxstack 1
  ldarg.0
  call int32 [StylosInk]StylosInk.WinTab::WTClose(native int)
  ret
}
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTPacket(native int,int32,native int) cil managed
{
  .export [4] as WTPacket
  .maxstack 3
  ldarg.0
  ldarg.1
  ldarg.2
  call int32 [StylosInk]StylosInk.WinTab::WTPacket(native int,int32,native int)
  ret
}
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTEnable(native int,int32) cil managed
{
  .export [5] as WTEnable
  .maxstack 2
  ldarg.0
  ldarg.1
  call int32 [StylosInk]StylosInk.WinTab::WTEnable(native int,int32)
  ret
}
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTOverlap(native int,int32) cil managed
{
  .export [6] as WTOverlap
  .maxstack 2
  ldarg.0
  ldarg.1
  call int32 [StylosInk]StylosInk.WinTab::WTOverlap(native int,int32)
  ret
}
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTQueueSizeSet(native int,int32) cil managed
{
  .export [7] as WTQueueSizeSet
  .maxstack 2
  ldarg.0
  ldarg.1
  call int32 [StylosInk]StylosInk.WinTab::WTQueueSizeSet(native int,int32)
  ret
}
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTPacketsGet(native int,int32,native int) cil managed
{
  .export [8] as WTPacketsGet
  .maxstack 3
  ldarg.0
  ldarg.1
  ldarg.2
  call int32 [StylosInk]StylosInk.WinTab::WTPacketsGet(native int,int32,native int)
  ret
}
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTGetA(native int,native int) cil managed
{
  .export [9] as WTGetA
  .maxstack 2
  ldarg.0
  ldarg.1
  call int32 [StylosInk]StylosInk.WinTab::WTGetA(native int,native int)
  ret
}
.method public hidebysig static int32 modopt([mscorlib]System.Runtime.CompilerServices.CallConvStdcall) WTSetA(native int,native int) cil managed
{
  .export [10] as WTSetA
  .maxstack 2
  ldarg.0
  ldarg.1
  call int32 [StylosInk]StylosInk.WinTab::WTSetA(native int,native int)
  ret
}
