#include "winemetal.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            printf("FAIL %d: %s\n", __LINE__, #x);                                                                     \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
template <typename T> static T load_api(HMODULE module, const char* name) {
    FARPROC address = GetProcAddress(module, name);
    T result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
#define LOAD_API(name)                                                                                                 \
    const auto name = load_api<decltype(&::name)>(module, #name);                                                      \
    CHECK(name)

struct Record {
    uint32_t x, y, z, index, value;
};
int main(int argc, char** argv) {
    CHECK(argc >= 2 && argc <= 3);
    CHECK(argc != 3 || !strcmp(argv[2], "empty"));
    const bool empty = argc > 2;
    HMODULE module = LoadLibraryA("winemetal.dll");
    CHECK(module);
    LOAD_API(WMTCopyAllDevices);
    LOAD_API(NSArray_count);
    LOAD_API(NSArray_object);
    LOAD_API(NSAutoreleasePool_alloc_init);
    LOAD_API(NSObject_release);
    LOAD_API(MTLDevice_newComputeIndirectCommandBuffer);
    LOAD_API(MTLIndirectCommandBuffer_gpuResourceID);
    LOAD_API(MTLComputePipelineState_gpuResourceID);
    LOAD_API(MTLDevice_newIndirectComputePipelineState);
    LOAD_API(MTLDevice_newLibraryWithSource);
    LOAD_API(MTLLibrary_newFunction);
    LOAD_API(MTLDevice_newComputePipelineState);
    LOAD_API(MTLDevice_newBuffer);
    LOAD_API(MTLDevice_newCommandQueue);
    LOAD_API(MTLCommandQueue_commandBuffer);
    LOAD_API(MTLCommandBuffer_computeCommandEncoder);
    LOAD_API(MTLComputeCommandEncoder_encodeCommands);
    LOAD_API(MTLCommandEncoder_endEncoding);
    LOAD_API(MTLCommandBuffer_retainObjectsUntilCompleted);
    LOAD_API(MTLCommandBuffer_commit);
    LOAD_API(MTLCommandBuffer_status);
    LOAD_API(MTLCommandBuffer_waitUntilCompleted);
    auto pool = NSAutoreleasePool_alloc_init();
    CHECK(pool);
    auto devices = WMTCopyAllDevices();
    CHECK(devices && NSArray_count(devices));
    auto d = NSArray_object(devices, 0);
    CHECK(d);
    CHECK(!MTLDevice_newComputeIndirectCommandBuffer(0, 8, 2));
    CHECK(!MTLDevice_newComputeIndirectCommandBuffer(d, 0, 2));
    CHECK(!MTLDevice_newComputeIndirectCommandBuffer(d, 8, 32));
    CHECK(!MTLIndirectCommandBuffer_gpuResourceID(0));
    CHECK(!MTLComputePipelineState_gpuResourceID(0));
    std::ifstream f(argv[1]);
    std::string source((std::istreambuf_iterator<char>(f)), {});
    CHECK(!source.empty());
    obj_handle_t error = ~uint64_t(0);
    CHECK(!MTLDevice_newIndirectComputePipelineState(d, nullptr, &error) && error == 0);
    auto lib = MTLDevice_newLibraryWithSource(d, source.data(), source.size(), &error);
    CHECK(lib && !error);
    auto consume = MTLLibrary_newFunction(lib, "consume"), build = MTLLibrary_newFunction(lib, "build");
    CHECK(consume && build);
    WMTComputePipelineInfo pi = {};
    pi.compute_function = consume;
    auto consumer = MTLDevice_newIndirectComputePipelineState(d, &pi, &error);
    CHECK(consumer && !error);
    pi.compute_function = build;
    auto builder = MTLDevice_newComputePipelineState(d, &pi, &error);
    CHECK(builder && !error);
    auto icb = MTLDevice_newComputeIndirectCommandBuffer(d, 8, 2);
    CHECK(icb);
    const uint64_t ids[2] = {MTLIndirectCommandBuffer_gpuResourceID(icb),
                             MTLComputePipelineState_gpuResourceID(consumer)};
    CHECK(ids[0] && ids[1]);
    WMTBufferInfo ai = {}, ri = {}, oi = {}, xi = {};
    ai.length = 16;
    ri.length = 6 * sizeof(Record);
    oi.length = 32;
    xi.length = 8;
    auto args = MTLDevice_newBuffer(d, &ai), records = MTLDevice_newBuffer(d, &ri),
         output = MTLDevice_newBuffer(d, &oi), range = MTLDevice_newBuffer(d, &xi);
    CHECK(args && records && output && range && ai.memory.ptr && ri.memory.ptr && oi.memory.ptr && xi.memory.ptr);
    memcpy(ai.memory.ptr, ids, 16);
    memset(ri.memory.ptr, 0, ri.length);
    memset(oi.memory.ptr, 0, 32);
    memset(xi.memory.ptr, 0, 8);
    auto q = MTLDevice_newCommandQueue(d, 16);
    CHECK(q);
    auto cb = MTLCommandQueue_commandBuffer(q);
    CHECK(cb);
    auto enc = MTLCommandBuffer_computeCommandEncoder(cb, false);
    CHECK(enc);
    alignas(8) uint8_t chain[4096] = {};
    size_t used = 0;
    wmtcmd_base* tail = nullptr;
    auto append = [&](const auto& body) {
        auto* p = reinterpret_cast<wmtcmd_base*>(chain + used);
        memcpy(p, &body, sizeof(body));
        p->next.set(nullptr);
        if (tail)
            tail->next.set(p);
        tail = p;
        used += sizeof(body);
    };
    wmtcmd_compute_setpso pso = {};
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = builder;
    pso.threadgroup_size = {1, 1, 1};
    append(pso);
    obj_handle_t bufs[] = {args, records, output, range};
    for (unsigned i = 0; i < 4; i++) {
        wmtcmd_compute_setbuffer b = {};
        b.type = WMTComputeCommandSetBuffer;
        b.buffer = bufs[i];
        b.index = i;
        append(b);
    }
    wmtcmd_compute_useresource use = {};
    use.type = WMTComputeCommandUseResource;
    use.resource = icb;
    use.usage = WMTResourceUsageWrite;
    append(use);
    wmtcmd_compute_dispatch dispatch = {};
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {1, 1, 1};
    append(dispatch);
    CHECK(MTLComputeCommandEncoder_encodeCommands(enc, reinterpret_cast<wmtcmd_base*>(chain)));
    MTLCommandEncoder_endEncoding(enc);
    enc = MTLCommandBuffer_computeCommandEncoder(cb, false);
    CHECK(enc);
    // Indirect buffers are root-bound inside GPU-generated commands, so declare
    // their usage explicitly on the execution encoder.
    used = 0;
    tail = nullptr;
    wmtcmd_compute_execute_indirect_commands bad = {};
    bad.type = WMTComputeCommandExecuteIndirectCommands;
    bad.indirect_commands = icb;
    bad.execution_range_buffer = range;
    bad.execution_range_offset = 1;
    CHECK(!MTLComputeCommandEncoder_encodeCommands(enc, reinterpret_cast<wmtcmd_base*>(&bad)));
    bad.execution_range_offset = 4;
    CHECK(!MTLComputeCommandEncoder_encodeCommands(enc, reinterpret_cast<wmtcmd_base*>(&bad)));
    bad.execution_range_offset = 0;
    bad.indirect_commands = 0;
    CHECK(!MTLComputeCommandEncoder_encodeCommands(enc, reinterpret_cast<wmtcmd_base*>(&bad)));
    bad.indirect_commands = icb;
    bad.execution_range_buffer = 0;
    CHECK(!MTLComputeCommandEncoder_encodeCommands(enc, reinterpret_cast<wmtcmd_base*>(&bad)));
    bad.execution_range_buffer = range;
    CHECK(!MTLComputeCommandEncoder_encodeCommands(0, reinterpret_cast<wmtcmd_base*>(&bad)));
    for (auto handle : bufs) {
        use.resource = handle;
        use.usage = WMTResourceUsageRead;
        if (handle == output)
            use.usage = WMTResourceUsageWrite;
        append(use);
    }
    use.resource = icb;
    use.usage = WMTResourceUsageRead;
    append(use);
    wmtcmd_compute_execute_indirect_commands execute = {};
    execute.type = WMTComputeCommandExecuteIndirectCommands;
    execute.indirect_commands = icb;
    execute.execution_range_buffer = range;
    append(execute);
    CHECK(MTLComputeCommandEncoder_encodeCommands(enc, reinterpret_cast<wmtcmd_base*>(chain)));
    MTLCommandEncoder_endEncoding(enc);
    Record payload[6] = {{2, 1, 1, 0, 1},    {1, 2, 1, 1, 10},     {1, 1, 2, 2, 100},
                         {0, 1, 1, 3, 1000}, {1, 1, 1, 4, 999999}, {empty ? 0u : 4u, 0, 0, 0, 0}};
    memcpy(ri.memory.ptr, payload, sizeof(payload));
    obj_handle_t retained[] = {consumer, builder, icb, args, records, output, range};
    MTLCommandBuffer_retainObjectsUntilCompleted(cb, retained, 7);
    MTLCommandBuffer_commit(cb);
    const ULONGLONG deadline = GetTickCount64() + 10000;
    auto status = MTLCommandBuffer_status(cb);
    while (status != WMTCommandBufferStatusCompleted && status != WMTCommandBufferStatusError &&
           GetTickCount64() < deadline) {
        Sleep(1);
        status = MTLCommandBuffer_status(cb);
    }
    CHECK(status == WMTCommandBufferStatusCompleted);
    // Drain completion handlers before releasing resources and the PE module.
    MTLCommandBuffer_waitUntilCompleted(cb);
    uint32_t expected[8] = {10, 62, 602, 0, 0, 0, 0, 0};
    if (empty)
        memset(expected, 0, 32);
    auto actual = static_cast<uint32_t*>(oi.memory.ptr);
    auto actualRange = static_cast<uint32_t*>(xi.memory.ptr);
    bool exact = !memcmp(actual, expected, 32) && actualRange[0] == 1 && actualRange[1] == (empty ? 0u : 4u);
    printf("{\"schema\":\"metalsharp.workgraph-icb-bridge.v1\",\"pass\":%s,\"bridge_icb\":true,\"gpu_completion_ok\":true,\"invalid_arguments_rejected\":true,\"d3d12_integrated\":false,\"range\":["
           "%u,%u],\"values\":[",
           exact ? "true" : "false", actualRange[0], actualRange[1]);
    for (unsigned i = 0; i < 8; i++)
        printf("%s%u", i ? "," : "", actual[i]);
    puts("]}");
    for (auto handle : retained)
        NSObject_release(handle);
    NSObject_release(consume);
    NSObject_release(build);
    NSObject_release(lib);
    NSObject_release(q);
    NSObject_release(devices);
    NSObject_release(pool);
    FreeLibrary(module);
    return exact ? 0 : 1;
}
