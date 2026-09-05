// Exact three-stage witness: grids {2,1,2,1} must produce
// UAV[0..7] = {18,5,18,5,4,2,4,2}. No intermediate CPU readback.
RWStructuredBuffer<uint> UAV : register(u0);
#ifdef GRID_U16
struct Entry { uint16_t index; uint16_t3 grid : SV_DispatchGrid; };
#elif defined(GRID_VECTOR)
struct Entry { uint index; uint3 grid : SV_DispatchGrid; };
#elif defined(GRID_OFFSET)
struct Entry { uint index; uint grid : SV_DispatchGrid; uint2 padding; };
#else
struct Entry { uint grid : SV_DispatchGrid; uint index; };
#endif
#ifdef OVERSIZED_OUTPUT
struct Work { uint index; uint increment; uint padding[64]; };
#else
struct Work { uint index; uint increment; };
#endif
struct Done { uint index; };

[Shader("node")]
[NodeLaunch("broadcasting")]
#if defined(GRID_VECTOR) || defined(GRID_U16)
[NodeMaxDispatchGrid(16,3,2)]
#else
[NodeMaxDispatchGrid(16,1,1)]
#endif
[NumThreads(2,1,1)]
void firstNode(DispatchNodeInputRecord<Entry> input,
#ifdef OVERSIZED_OUTPUT
               [NodeID("unconnected_output")]
#endif
               [MaxRecords(2)] NodeOutput<Work> secondNode,
#ifdef FANOUT
               [MaxRecords(1)] NodeOutput<Done> thirdNode,
#endif
               uint lane : SV_GroupIndex, uint tid : SV_DispatchThreadID)
{
#if defined(BAD_TARGET) || defined(CYCLE) || defined(OVERSIZED_OUTPUT)
    if (lane == 0) UAV[15] = 0xdeadbeef;
#endif
    GroupNodeOutputRecords<Work> records = secondNode.GetGroupNodeOutputRecords(2);
    records[lane].index = input.Get().index;
    records[lane].increment = tid * 2 + lane + 1;
    records.OutputComplete();
#ifdef FANOUT
    GroupNodeOutputRecords<Done> direct = thirdNode.GetGroupNodeOutputRecords(1);
    if (lane == 0) direct.Get().index = input.Get().index;
    direct.OutputComplete();
#endif
}

[Shader("node")]
#ifdef CYCLE
[NodeMaxRecursionDepth(2)]
#endif
#ifdef BAD_TARGET
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1,1,1)]
[NumThreads(1,1,1)]
void secondNode(DispatchNodeInputRecord<Work> input,
#else
[NodeLaunch("thread")]
void secondNode(ThreadNodeInputRecord<Work> input,
#endif
#ifdef CYCLE
                [MaxRecords(1)] [NodeID("secondNode")] NodeOutput<Work> again)
#else
                [MaxRecords(1)] NodeOutput<Done> thirdNode)
#endif
{
    InterlockedAdd(UAV[input.Get().index], input.Get().increment);
#ifdef CYCLE
    ThreadNodeOutputRecords<Work> record = again.GetThreadNodeOutputRecords(1);
    record.Get().increment = input.Get().increment;
#else
    ThreadNodeOutputRecords<Done> record = thirdNode.GetThreadNodeOutputRecords(1);
#endif
    record.Get().index = input.Get().index;
    record.OutputComplete();
}

groupshared uint counts[4];
[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(32,1,1)]
void thirdNode([MaxRecords(32)] GroupNodeInputRecords<Done> input,
               uint lane : SV_GroupIndex)
{
    // Constant GEPs deliberately exercise all four groupshared addresses.
    if (lane == 0) {
        counts[0] = 0; counts[1] = 0; counts[2] = 0; counts[3] = 0;
    }
    Barrier(GROUP_SHARED_MEMORY, GROUP_SCOPE | GROUP_SYNC);
    if (lane < input.Count())
        InterlockedAdd(counts[input[lane].index], 1);
    Barrier(GROUP_SHARED_MEMORY, GROUP_SCOPE | GROUP_SYNC);
    if (lane == 0) {
        InterlockedAdd(UAV[4], counts[0]);
        InterlockedAdd(UAV[5], counts[1]);
        InterlockedAdd(UAV[6], counts[2]);
        InterlockedAdd(UAV[7], counts[3]);
    }
}
