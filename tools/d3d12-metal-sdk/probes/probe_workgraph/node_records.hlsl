RWByteAddressBuffer output : register(u0);
struct RECORD { uint value; };

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_main([MaxRecords(1)] NodeOutput<RECORD> node_output) {
  ThreadNodeOutputRecords<RECORD> records =
      node_output.GetThreadNodeOutputRecords(1);
  records.Get().value = 0xabcdef01;
  records.OutputComplete();
  output.Store(0, 0xabcdef01);
  output.Store(4, 0x12345678);
}
