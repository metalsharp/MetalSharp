RWByteAddressBuffer output : register(u0);

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_main(EmptyNodeInput input) {
  output.Store(0, input.Count());
  output.Store(4, GetRemainingRecursionLevels());
}
