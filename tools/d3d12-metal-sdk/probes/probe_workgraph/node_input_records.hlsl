// Compile with -enable-16bit-types. Not a scheduling proof.
// Metadata and actual input-consumption fixture.
RWByteAddressBuffer output : register(u0);
struct ScalarRecord { uint value; };
struct VectorRecord { uint4 value; };
struct HalfRecord { uint16_t value; };
struct DynamicGridRecord { uint3 grid : SV_DispatchGrid; uint value; };

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

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_half(DispatchNodeInputRecord<HalfRecord> input) {
  output.Store(0, uint(input.Get().value));
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_empty() {
  output.Store(0, 0);
}

// Requires record-driven launch decoding; max grid is not a fixed grid.
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(2, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_dynamic(DispatchNodeInputRecord<DynamicGridRecord> input) {
  output.Store(0, input.Get().value);
}
