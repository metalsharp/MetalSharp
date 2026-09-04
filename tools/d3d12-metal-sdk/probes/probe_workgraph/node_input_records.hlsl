// Compile with -enable-16bit-types. Not a scheduling proof.
// Metadata and actual input-consumption fixture.
RWByteAddressBuffer output : register(u0);
struct ScalarRecord { uint value; };
struct VectorRecord { uint4 value; };
struct HalfRecord { uint16_t value; };
struct DynamicGridRecord { uint3 grid : SV_DispatchGrid; uint value; };
struct OffsetTail { uint16_t narrow[4]; uint16_t spacer; uint64_t wide[2]; };
struct OffsetRecord { uint4 prefix; OffsetTail tail; };

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

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(3, 1, 1)]
[NumThreads(4, 1, 1)]
[NodeIsProgramEntry]
void node_offsets(DispatchNodeInputRecord<OffsetRecord> input,
                  uint3 tid : SV_DispatchThreadID) {
  uint value;
  if (tid.x < 4) value = input.Get().prefix[tid.x];
  else if (tid.x < 8) value = uint(input.Get().tail.narrow[tid.x - 4]);
  else value = uint(input.Get().tail.wide[(tid.x - 8) >> 1] >> ((tid.x & 1) * 32));
  output.Store(tid.x * 4, value);
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
