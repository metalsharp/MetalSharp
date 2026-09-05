// Metadata and routing fixture: dynamic array indices must not collapse to zero.
RWByteAddressBuffer result : register(u0);
struct Record { uint index; uint value; };
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void array_entry(ThreadNodeInputRecord<Record> input,
                 [MaxRecords(1)] [NodeArraySize(4)] [NodeID("array_target")]
                 NodeOutputArray<Record> targets) {
    ThreadNodeOutputRecords<Record> records = targets[input.Get().index].GetThreadNodeOutputRecords(1);
    records.Get().index = input.Get().index;
    records.Get().value = input.Get().value;
    records.OutputComplete();
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void sparse_entry(ThreadNodeInputRecord<Record> input,
                  [MaxRecords(1)] [UnboundedSparseNodes] [NodeID("sparse_target")]
                  NodeOutputArray<Record> targets) {
    if (targets[input.Get().index].IsValid()) {
        ThreadNodeOutputRecords<Record> records = targets[input.Get().index].GetThreadNodeOutputRecords(1);
        records.Get().index = input.Get().index;
        records.Get().value = input.Get().value;
        records.OutputComplete();
    }
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("array_target", 0)]
void target_zero(ThreadNodeInputRecord<Record> input) {
    result.Store(0, input.Get().value);
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("array_target", 1)]
void target_one(ThreadNodeInputRecord<Record> input) {
    result.Store(4, input.Get().value);
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("array_target", 2)]
void target_two(ThreadNodeInputRecord<Record> input) {
    result.Store(8, input.Get().value);
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("array_target", 3)]
void target_three(ThreadNodeInputRecord<Record> input) {
    result.Store(12, input.Get().value);
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("sparse_target", 65536)]
void target_sparse(ThreadNodeInputRecord<Record> input) {
    result.Store(16, input.Get().value);
}
