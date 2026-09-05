RWByteAddressBuffer result : register(u0);
struct Record { uint count; };
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void threadProducer(ThreadNodeInputRecord<Record> input,
                    [MaxRecords(8)] EmptyNodeOutput sink) {
    sink.ThreadIncrementOutputCount(input.Get().count);
    result.InterlockedAdd(12, 1);
}
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1,1,1)]
[NumThreads(2,1,1)]
[NodeIsProgramEntry]
void groupProducer(DispatchNodeInputRecord<Record> input,
                   [MaxRecords(8)] EmptyNodeOutput sink) {
    sink.GroupIncrementOutputCount(input.Get().count);
    result.InterlockedAdd(12, 1);
}
[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(1,1,1)]
[NodeID("sink")]
void consume(
#ifdef DATA_CONSUMER
    [MaxRecords(4)] GroupNodeInputRecords<Record> input
#else
    [MaxRecords(4)] EmptyNodeInput input
#endif
    ) {
    result.InterlockedAdd(0, input.Count());
    result.InterlockedAdd(4, 1);
    result.InterlockedOr(8, 1u << input.Count());
}
