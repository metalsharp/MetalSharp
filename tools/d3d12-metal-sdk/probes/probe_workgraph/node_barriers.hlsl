RWByteAddressBuffer output : register(u0);

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_main() {
  Barrier(1, 3);
  Barrier(output, 3);
  output.Store(0, 0x24681357);
}
