// Metadata and actual input-consumption fixture. Not a scheduling proof.
RWByteAddressBuffer output : register(u0);
struct ScalarRecord { uint value; };
struct VectorRecord { uint4 value; };

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_main(DispatchNodeInputRecord<ScalarRecord> input) {
  output.Store(0, 0xabcdef00u + input.Get().value);
  output.Store(4, 0x12345678u);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(2, 1, 1)]
[NumThreads(4, 1, 1)]
[NodeIsProgramEntry]
void node_vector(DispatchNodeInputRecord<VectorRecord> input,
                 uint3 tid : SV_DispatchThreadID) {
  output.Store(tid.x * 4, input.Get().value[tid.x & 3]);
}
