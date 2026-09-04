// Compile with -enable-16bit-types. Bounded input-consumption and launch-mode
// fixture; graph output publication and downstream scheduling remain separate.
RWByteAddressBuffer output : register(u0);
struct ScalarRecord { uint value; };
struct VectorRecord { uint4 value; };
struct HalfRecord { uint16_t value; };
struct DynamicGridRecord { uint3 grid : SV_DispatchGrid; uint value; };
struct OffsetTail { uint16_t narrow[4]; uint16_t spacer; uint64_t wide[2]; };
struct OffsetRecord { uint4 prefix; OffsetTail tail; };
struct BroadcastRecord { uint slot; uint value; };

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

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_broadcast_multi(DispatchNodeInputRecord<BroadcastRecord> input) {
  output.Store(input.Get().slot * 4, input.Get().value);
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void node_thread_multi(ThreadNodeInputRecord<BroadcastRecord> input) {
  output.Store(input.Get().slot * 4, input.Get().value);
}

struct CoalescingRecord { uint slot; uint value; };

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(4, 1, 1)]
[NodeIsProgramEntry]
void node_coalescing_multi([MaxRecords(4)] GroupNodeInputRecords<CoalescingRecord> input,
                           uint tid : SV_GroupIndex) {
  if (tid < input.Count())
    output.Store(input[tid].slot * 4, input[tid].value);
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
