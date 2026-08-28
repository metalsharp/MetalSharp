#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

static id<MTLLibrary> compile_source(id<MTLDevice> device, NSString* source, NSError** error) {
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_1;
    return [device newLibraryWithSource:source options:options error:error];
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::puts("{\"schema\":\"metalsharp.metal-atomic64.v1\",\"pass\":false,\"error\":\"no Metal device\"}");
            return 1;
        }

        NSString* device_add_source = @R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void test(device atomic_ulong *values [[buffer(0)]],
                 uint tid [[thread_position_in_grid]]) {
  atomic_fetch_add_explicit(values + tid, ulong(1), memory_order_relaxed);
}
)MSL";
        NSString* threadgroup_add_source = @R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void test(device ulong *output [[buffer(0)]],
                 uint tid [[thread_index_in_threadgroup]]) {
  threadgroup atomic_ulong value;
  atomic_fetch_add_explicit(&value, ulong(tid), memory_order_relaxed);
  if (tid == 0) output[0] = 1;
}
)MSL";
        NSString* device_max_source = @R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void test(device atomic_ulong *values [[buffer(0)]],
                 uint tid [[thread_position_in_grid]]) {
  atomic_max_explicit(values + (tid & 3), ulong(5 + tid),
                      memory_order_relaxed);
}
)MSL";

        NSError* device_add_error = nil;
        NSError* threadgroup_add_error = nil;
        NSError* device_max_error = nil;
        id<MTLLibrary> device_add_library = compile_source(device, device_add_source, &device_add_error);
        id<MTLLibrary> threadgroup_add_library = compile_source(device, threadgroup_add_source, &threadgroup_add_error);
        id<MTLLibrary> device_max_library = compile_source(device, device_max_source, &device_max_error);

        bool pso_created = false;
        bool dispatch_completed = false;
        uint64_t observed[4] = {};
        if (device_max_library) {
            NSError* pso_error = nil;
            id<MTLComputePipelineState> pso =
                [device newComputePipelineStateWithFunction:[device_max_library newFunctionWithName:@"test"]
                                                      error:&pso_error];
            pso_created = pso != nil;
            if (pso) {
                id<MTLBuffer> buffer = [device newBufferWithBytes:observed
                                                           length:sizeof(observed)
                                                          options:MTLResourceStorageModeShared];
                id<MTLCommandQueue> queue = [device newCommandQueue];
                id<MTLCommandBuffer> command = [queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                [encoder setComputePipelineState:pso];
                [encoder setBuffer:buffer offset:0 atIndex:0];
                [encoder dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
                [encoder endEncoding];
                [command commit];
                [command waitUntilCompleted];
                dispatch_completed = command.status == MTLCommandBufferStatusCompleted;
                if (dispatch_completed)
                    std::memcpy(observed, buffer.contents, sizeof(observed));
            }
        }

        const uint64_t expected[4] = {17, 18, 19, 20};
        bool max_values_exact = true;
        for (uint32_t i = 0; i < 4; i++)
            max_values_exact = max_values_exact && observed[i] == expected[i];
        const bool full_atomic64_unavailable = !device_add_library && !threadgroup_add_library;
        const bool pass =
            full_atomic64_unavailable && device_max_library && pso_created && dispatch_completed && max_values_exact;

        std::printf("{\n");
        std::printf("  \"schema\": \"metalsharp.metal-atomic64.v1\",\n");
        std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
        std::printf("  \"device\": \"%s\",\n", device.name.UTF8String);
        std::printf("  \"metal_language_version\": \"3.1\",\n");
        std::printf("  \"device_atomic_add_compiled\": %s,\n", device_add_library ? "true" : "false");
        std::printf("  \"threadgroup_atomic_add_compiled\": %s,\n", threadgroup_add_library ? "true" : "false");
        std::printf("  \"device_atomic_max_compiled\": %s,\n", device_max_library ? "true" : "false");
        std::printf("  \"device_atomic_max_pso_created\": %s,\n", pso_created ? "true" : "false");
        std::printf("  \"device_atomic_max_dispatch_completed\": %s,\n", dispatch_completed ? "true" : "false");
        std::printf("  \"device_atomic_max_values\": [%llu,%llu,%llu,%llu],\n",
                    static_cast<unsigned long long>(observed[0]), static_cast<unsigned long long>(observed[1]),
                    static_cast<unsigned long long>(observed[2]), static_cast<unsigned long long>(observed[3]));
        std::printf("  \"full_d3d12_atomic64_available_natively\": false,\n");
        std::printf("  \"decision\": \"%s\"\n",
                    pass ? "software emulation required" : "unexpected Metal atomic64 surface; investigate");
        std::printf("}\n");
        return pass ? 0 : 1;
    }
}
