RWByteAddressBuffer output : register(u0);
struct RECORD { uint value; };

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_a([MaxRecords(1)] NodeOutput<RECORD> node_output) {
  ThreadNodeOutputRecords<RECORD> records =
      node_output.GetThreadNodeOutputRecords(1);
  records.Get().value = 0x11111111;
  records.OutputComplete();
  output.Store(0, 0x11111111);
  output.Store(4, 0xaaaa0001);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_b([MaxRecords(1)] NodeOutput<RECORD> node_output) {
  ThreadNodeOutputRecords<RECORD> records =
      node_output.GetThreadNodeOutputRecords(1);
  records.Get().value = 0x22222222;
  records.OutputComplete();
  output.Store(0, 0x22222222);
  output.Store(4, 0xbbbb0002);
}

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_c() {
  Barrier(1, 3);
  Barrier(output, 3);
  output.Store(0, 0x33333333);
  output.Store(4, 0xcccc0003);
}
