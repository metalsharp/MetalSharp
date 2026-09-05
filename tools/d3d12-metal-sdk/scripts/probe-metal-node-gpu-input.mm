#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "d3d12_node_gpu_input.hpp"
#include "node_gpu_input_msl.hpp"
#include <iostream>
#include <string>
int main() {
  @autoreleasepool {
    auto bounds = BuildNodeGPUAddressBounds({{100,1000},{200,10},{100,20},{1100,100},{4096,24}});
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || !bounds) return 1;
    const uint64_t queries[][2] = {{205,500},{100,1000},{1090,20},{99,1},{1200,1},{100,UINT64_MAX}};
    id<MTLBuffer> table = [device newBufferWithBytes:bounds->data() length:bounds->size()*sizeof((*bounds)[0]) options:MTLResourceStorageModeShared];
    id<MTLBuffer> input = [device newBufferWithBytes:queries length:sizeof(queries) options:MTLResourceStorageModeShared];
    const D3D12NodeGPUEntryLayout layouts[] = {{8,4,1,0},{0,0,1,0},{8,4,0,0}};
    const D3D12NodeGPUInputHeader headers[] = {
      {0,2,4096,16},{0,UINT32_MAX,4096,0},{1,99,UINT64_MAX,UINT64_MAX},
      {0,0,0,UINT64_MAX},{0,3,4096,16},{0,2,4096,4},{0,1,4097,8},
      {0,UINT32_MAX,4096,UINT64_MAX-3},{2,1,4096,8},{3,0,0,0}};
    id<MTLBuffer> layout_buffer = [device newBufferWithBytes:layouts length:sizeof(layouts) options:MTLResourceStorageModeShared];
    id<MTLBuffer> header_buffer = [device newBufferWithBytes:headers length:sizeof(headers) options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:16*sizeof(uint32_t) options:MTLResourceStorageModeShared];
    if (!table || !input || !output || !layout_buffer || !header_buffer) return 1;
    std::string source = "#include <metal_stdlib>\nusing namespace metal;\n";
    source += dxmt::dxil::kNodeGPUInputMSL;
    source += R"MSL(
kernel void validate_ranges(device const m12_node_gpu_address_bound *bounds [[buffer(0)]],
    device const ulong2 *queries [[buffer(1)]], device uint *output [[buffer(2)]],
    constant uint &count [[buffer(3)]], uint i [[thread_position_in_grid]]) {
  output[i] = uint(m12_node_gpu_address_contains(bounds, count, queries[i].x, queries[i].y));
}
kernel void validate_headers(device const m12_node_gpu_address_bound *bounds [[buffer(0)]],
    device uint *output [[buffer(2)]], constant uint &count [[buffer(3)]],
    device const m12_node_gpu_entry_layout *entries [[buffer(4)]],
    device const m12_node_gpu_input_header *headers [[buffer(5)]], uint i [[thread_position_in_grid]]) {
  output[6u + i] = uint(m12_node_gpu_input_valid(headers[i], entries, 3u, bounds, count));
}
)MSL";
    MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion4_0;
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
    if (!library) { std::cerr << error.localizedDescription.UTF8String << '\n'; return 1; }
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"validate_ranges"] error:&error];
    id<MTLComputePipelineState> header_pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"validate_headers"] error:&error];
    if (!pipeline || !header_pipeline) return 1;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!queue || !command || !encoder) return 1;
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:table offset:0 atIndex:0]; [encoder setBuffer:input offset:0 atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];
    const uint32_t count = static_cast<uint32_t>(bounds->size());
    [encoder setBytes:&count length:sizeof(count) atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(6,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [encoder setComputePipelineState:header_pipeline];
    [encoder setBuffer:layout_buffer offset:0 atIndex:4];
    [encoder setBuffer:header_buffer offset:0 atIndex:5];
    [encoder dispatchThreads:MTLSizeMake(10,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) return 1;
    auto actual = static_cast<const uint32_t *>(output.contents);
    const uint32_t expected[6] = {1,1,0,0,0,0};
    bool pass = true;
    for (unsigned i = 0; i < 6; ++i)
      pass &= actual[i] == expected[i] && actual[i] == uint32_t(NodeGPUAddressContains(*bounds, queries[i][0], queries[i][1]));
    const uint32_t expected_headers[10] = {1,1,1,1,0,0,0,0,0,0};
    for (unsigned i = 0; i < 10; ++i) pass &= actual[6+i] == expected_headers[i];
    std::cout << "{\"pass\":" << (pass ? "true" : "false")
              << ",\"gpu_range_validation\":true,\"d3d12_integrated\":false,\"gpu_generated_headers\":false}\n";
    return pass ? 0 : 1;
  }
}
