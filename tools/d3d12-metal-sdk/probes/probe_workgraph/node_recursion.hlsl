RWByteAddressBuffer result : register(u0);
#ifndef RECURSION_DEPTH
#define RECURSION_DEPTH 3
#endif
struct Record {
    uint value;
#ifdef RECURSION_ICB
    uint3 grid : SV_DispatchGrid;
#endif
};
[Shader("node")]
#ifdef RECURSION_ICB
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(1,1,1)]
[NumThreads(1,1,1)]
#elif defined(RECURSION_COALESCING)
[NodeLaunch("coalescing")]
[NumThreads(1,1,1)]
#else
[NodeLaunch("thread")]
#endif
[NodeIsProgramEntry]
[NodeMaxRecursionDepth(RECURSION_DEPTH)]
void recursive(
#ifdef RECURSION_ICB
               DispatchNodeInputRecord<Record> input,
#elif defined(RECURSION_COALESCING)
               [MaxRecords(2)] GroupNodeInputRecords<Record> input,
#else
               ThreadNodeInputRecord<Record> input,
#endif
#ifdef RECURSION_COALESCING
               [MaxRecords(2)] NodeOutput<Record> recursive
#else
               [MaxRecords(1)] NodeOutput<Record> recursive
#endif
#ifdef SELF_FANOUT
               , [MaxRecords(1)] [NodeID("recursive")] NodeOutput<Record> other
#endif
               ) {
#ifdef RECURSION_COALESCING
    result.InterlockedAdd(32, 1u);
    result.InterlockedOr(36, 1u << input.Count());
    for (uint index = 0; index < input.Count(); ++index) {
        Record current = input.Get(index);
#else
    {
        Record current = input.Get();
#endif
    uint remaining = GetRemainingRecursionLevels();
#ifdef DEPTH_BOUNDARY
    result.InterlockedAdd(0, current.value);
    result.InterlockedAdd(4, remaining);
    result.InterlockedAdd(8, 1u);
    result.InterlockedOr(12, 1u << (remaining & 31u));
#else
    // Keep the rejection fixture's writes in bounds if preflight regresses.
    result.InterlockedAdd(min(remaining, 3u) * 4, current.value);
    if (remaining > 3u) result.Store(28, 0xdeadbeef);
#endif
    bool valid = recursive.IsValid();
    result.InterlockedOr(24, valid ? (1u << (remaining & 31u)) : 0u);
#ifdef SELF_FANOUT
    if (other.IsValid() != valid) result.Store(28, 0xdeadbeef);
#endif
    if (valid
#ifdef EARLY_STOP
        && current.value < 2u
#endif
        ) {
        ThreadNodeOutputRecords<Record> next = recursive.GetThreadNodeOutputRecords(1);
        next.Get().value = current.value + 1;
#ifdef RECURSION_ICB
        next.Get().grid = current.grid;
#endif
        next.OutputComplete();
#ifdef SELF_FANOUT
        ThreadNodeOutputRecords<Record> more = other.GetThreadNodeOutputRecords(1);
        more.Get().value = current.value + 1;
#ifdef RECURSION_ICB
        more.Get().grid = current.grid;
#endif
        more.OutputComplete();
#endif
    }
    }
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
[NodeMaxRecursionDepth(3)]
void nonrecursive(ThreadNodeInputRecord<Record> input) {
    result.Store(16, GetRemainingRecursionLevels());
    result.Store(20, input.Get().value);
}
