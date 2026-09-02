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
  Barrier(records, 0);
  output.Store(0, 0x13572468);
}
