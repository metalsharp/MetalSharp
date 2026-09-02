RWByteAddressBuffer output : register(u0);

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_main(
    [NodeArraySize(2)] [MaxRecords(4)] EmptyNodeOutputArray outputs) {
  bool valid = outputs[1].IsValid();
  outputs[1].GroupIncrementOutputCount(1);
  outputs[1].ThreadIncrementOutputCount(1);
  output.Store(0, valid ? 1u : 0u);
}
