// Metadata and routing fixture: dynamic array indices must not collapse to zero.
RWByteAddressBuffer result : register(u0);
struct Record { uint index; uint value; };
[Shader("node")]
#ifdef GPU_ENTRY_COALESCING
[NodeLaunch("coalescing")]
[NumThreads(1,1,1)]
#elif defined(GPU_ENTRY_BROADCASTING)
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(2,2,2)]
[NumThreads(1,1,1)]
#else
[NodeLaunch("thread")]
#endif
[NodeIsProgramEntry]
void array_entry(
#ifdef GPU_ENTRY_COALESCING
                 [MaxRecords(2)] GroupNodeInputRecords<Record> input,
                 [MaxRecords(2)]
#elif defined(GPU_ENTRY_BROADCASTING)
                 DispatchNodeInputRecord<Record> input,
                 [MaxRecords(1)]
#else
                 ThreadNodeInputRecord<Record> input,
                 [MaxRecords(1)]
#endif
                 [NodeArraySize(4)] [NodeID("array_target")] NodeOutputArray<Record> targets) {
#ifdef GPU_ENTRY_COALESCING
    result.InterlockedAdd(20, 1u);
    result.InterlockedOr(24, 1u << input.Count());
    for (uint i = 0; i < input.Count(); ++i) {
        Record current = input.Get(i);
#else
    {
        Record current = input.Get();
#endif
    ThreadNodeOutputRecords<Record> records = targets[current.index].GetThreadNodeOutputRecords(1);
    records.Get().index = current.index;
    records.Get().value = current.value;
    records.OutputComplete();
    }
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
#if defined(GPU_ENTRY_REPLICATION) || defined(GPU_ENTRY_BROADCASTING)
    result.InterlockedAdd(0, input.Get().value);
#else
    result.Store(0, input.Get().value);
#endif
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("array_target", 1)]
void target_one(ThreadNodeInputRecord<Record> input) {
#ifdef GPU_ENTRY_BROADCASTING
    result.InterlockedAdd(4, input.Get().value);
#else
    result.Store(4, input.Get().value);
#endif
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("array_target", 2)]
void target_two(ThreadNodeInputRecord<Record> input) {
#ifdef GPU_ENTRY_BROADCASTING
    result.InterlockedAdd(8, input.Get().value);
#else
    result.Store(8, input.Get().value);
#endif
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("array_target", 3)]
void target_three(ThreadNodeInputRecord<Record> input) {
#ifdef GPU_ENTRY_BROADCASTING
    result.InterlockedAdd(12, input.Get().value);
#else
    result.Store(12, input.Get().value);
#endif
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeID("sparse_target", 65536)]
void target_sparse(ThreadNodeInputRecord<Record> input) {
    result.Store(16, input.Get().value);
}
