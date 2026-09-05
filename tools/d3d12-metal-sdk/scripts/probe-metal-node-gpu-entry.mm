#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "d3d12_node_gpu_input.hpp"
#include "d3d12_node_dispatch_abi.hpp"
#include "node_gpu_input_msl.hpp"
#include "node_gpu_entry_msl.hpp"
#include <cstring>
#include <iostream>
#include <string>
#define REQUIRE(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << '\n'; return 1; } } while (0)
int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); REQUIRE(device);
    std::string source = "#include <metal_stdlib>\n#include <metal_command_buffer>\nusing namespace metal;\n#define M12_GPU_ENTRY_COUNT 3\n";
    source += dxmt::dxil::kNodeGPUInputMSL; source += dxmt::dxil::kNodeGPUEntryMSL;
    source += R"MSL(
kernel void produce(device m12_gpu_input_pointer_view *header [[buffer(0)]],
    device uint2 *records [[buffer(1)]], constant uint &mode [[buffer(2)]], uint i [[thread_position_in_grid]]) {
  if (i != 0u) return;
  records[0] = uint2(0,7); records[1] = uint2(1,9); records[2] = uint2(2,11);
  header->entrypoint_index = mode == 0u ? 0u : (mode == 2u ? 3u : (mode == 6u ? 2u : 1u));
  header->record_count = (mode == 3u || mode == 6u) ? 3u : (mode == 4u ? 0u : 2u);
  header->records = (mode == 4u || mode == 6u) ? nullptr : reinterpret_cast<device const uchar *>(records);
  header->record_stride = mode == 5u ? 0ul : (mode == 6u ? 0xfffffffffffffffful : 8ul);
}
kernel void entry0(device atomic_uint *output [[buffer(0)]], device const uint2 *record [[buffer(29)]], device const m12_gpu_entry_context *context [[buffer(28)]], uint group [[threadgroup_position_in_grid]]) {
  record = reinterpret_cast<device const uint2 *>(reinterpret_cast<device const uchar *>(record) + ulong(group) * context->stride);
  if (context->count != 2u || context->source_node != 0u) atomic_fetch_or_explicit(output + 3u, 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(output + record->x, record->y + 100u, memory_order_relaxed);
}
kernel void entry1(device atomic_uint *output [[buffer(0)]], device const uint2 *record [[buffer(29)]], device const m12_gpu_entry_context *context [[buffer(28)]], uint group [[threadgroup_position_in_grid]]) {
  record = reinterpret_cast<device const uint2 *>(reinterpret_cast<device const uchar *>(record) + ulong(group) * context->stride);
  if (context->count != 2u || context->source_node != 1u) atomic_fetch_or_explicit(output + 3u, 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(output + record->x, record->y + 200u, memory_order_relaxed);
}
kernel void empty_entry(device atomic_uint *output [[buffer(0)]], device const m12_gpu_entry_context *context [[buffer(28)]], uint group [[threadgroup_position_in_grid]], uint lane [[thread_index_in_threadgroup]]) {
  if (lane != 0u) return;
  if (context->size != 0ul || context->source_node != 2u) atomic_fetch_or_explicit(output + 3u, 0x80000000u, memory_order_relaxed);
  atomic_fetch_add_explicit(output + 3u, 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(output + 2u, min(context->batch_size, context->count - group * context->batch_size), memory_order_relaxed);
}
)MSL";
    MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion4_0;
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
    if (!library) { std::cerr << error.localizedDescription.UTF8String << '\n'; return 1; }
    auto make_pipeline = [&](NSString *name, bool indirect) {
      MTLComputePipelineDescriptor *desc = [MTLComputePipelineDescriptor new];
      desc.computeFunction = [library newFunctionWithName:name]; desc.supportIndirectCommandBuffers = indirect;
      return [device newComputePipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:&error];
    };
    id<MTLComputePipelineState> producer = make_pipeline(@"produce", false);
    id<MTLComputePipelineState> builder = make_pipeline(@"m12_build_gpu_entry_commands", false);
    id<MTLComputePipelineState> prepare = make_pipeline(@"m12_prepare_gpu_entry", false);
    id<MTLComputePipelineState> first = make_pipeline(@"entry0", true), second = make_pipeline(@"entry1", true);
    id<MTLComputePipelineState> empty = make_pipeline(@"empty_entry", true);
    REQUIRE(producer && builder && prepare && first && second && empty);
    MTLIndirectCommandBufferDescriptor *desc = [MTLIndirectCommandBufferDescriptor new];
    desc.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    desc.inheritBuffers = NO; desc.inheritPipelineState = NO; desc.maxKernelBufferBindCount = 31;
    id<MTLIndirectCommandBuffer> icb = [device newIndirectCommandBufferWithDescriptor:desc maxCommandCount:2 options:MTLResourceStorageModePrivate]; REQUIRE(icb);
    [icb resetWithRange:NSMakeRange(0,2)];
    uint64_t handles[4] = {};
    auto command_id = icb.gpuResourceID, first_id = first.gpuResourceID, second_id = second.gpuResourceID, empty_id = empty.gpuResourceID;
    static_assert(sizeof(command_id) == 8 && sizeof(first_id) == 8);
    std::memcpy(handles, &command_id, 8); std::memcpy(handles+1, &first_id, 8); std::memcpy(handles+2, &second_id, 8); std::memcpy(handles+3, &empty_id, 8);
    id<MTLBuffer> args = [device newBufferWithBytes:handles length:sizeof(handles) options:MTLResourceStorageModeShared];
    id<MTLBuffer> header = [device newBufferWithLength:24 options:MTLResourceStorageModeShared];
    id<MTLBuffer> records = [device newBufferWithLength:24 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> range = [device newBufferWithLength:8 options:MTLResourceStorageModeShared];
    id<MTLBuffer> status = [device newBufferWithLength:8 options:MTLResourceStorageModeShared];
    REQUIRE(args && header && records && output && range && status);
    D3D12NodeGPUEntryLayout layouts[3] = {{8,4,1,3},{8,4,1,3},{0,0,1,2,{2,1,1},2}};
    D3D12NodeRecursionContext templates[3];
    for (unsigned i = 0; i < 3; ++i) { templates[i].routing.version = 2; templates[i].routing.source_node = i; }
    id<MTLBuffer> template_buffer = [device newBufferWithBytes:templates length:sizeof(templates) options:MTLResourceStorageModeShared];
    id<MTLBuffer> context_buffer = [device newBufferWithLength:2*sizeof(D3D12NodeRecursionContext) options:MTLResourceStorageModeShared];
    constexpr size_t table_bytes = kNodeOutputAllocationBase + kNodeOutputMaxAllocations * kNodeOutputAllocationStride;
    id<MTLBuffer> backing = [device newBufferWithLength:table_bytes + 16 options:MTLResourceStorageModeShared];
    REQUIRE(template_buffer && context_buffer && backing);
    auto bounds = BuildNodeGPUAddressBounds({{records.gpuAddress, records.length}}); REQUIRE(bounds);
    id<MTLBuffer> layout_buffer = [device newBufferWithBytes:layouts length:sizeof(layouts) options:MTLResourceStorageModeShared];
    id<MTLBuffer> bound_buffer = [device newBufferWithBytes:bounds->data() length:bounds->size()*sizeof((*bounds)[0]) options:MTLResourceStorageModeShared];
    REQUIRE(layout_buffer && bound_buffer);
    uint32_t parameters[2] = {static_cast<uint32_t>(bounds->size()),2};
    id<MTLCommandQueue> queue = [device newCommandQueue]; REQUIRE(queue);
    const uint32_t expected[7][4] = {{107,109,0,0},{207,209,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{414,0,0,0},{0,0,3,2}};
    for (uint32_t mode = 0; mode < 7; ++mode) {
      std::memset(header.contents, 0xab, 24); std::memset(records.contents, 0xcd, 24); std::memset(output.contents, 0, 16);
      parameters[1] = mode == 3 ? 0u : 2u;
      std::memset(backing.contents, 0xa5, backing.length);
      id<MTLCommandBuffer> command = [queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder]; REQUIRE(command && encoder);
      [encoder setComputePipelineState:producer];
      [encoder setBuffer:header offset:0 atIndex:0]; [encoder setBuffer:records offset:0 atIndex:1];
      [encoder setBytes:&mode length:4 atIndex:2];
      [encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [encoder setComputePipelineState:builder];
      [encoder setBuffer:args offset:0 atIndex:0]; [encoder setBuffer:header offset:0 atIndex:1];
      [encoder setBuffer:layout_buffer offset:0 atIndex:2]; [encoder setBuffer:bound_buffer offset:0 atIndex:3];
      [encoder setBuffer:output offset:0 atIndex:4]; [encoder setBuffer:range offset:0 atIndex:5];
      [encoder setBuffer:status offset:0 atIndex:6]; [encoder setBytes:parameters length:8 atIndex:7];
      [encoder setBuffer:template_buffer offset:0 atIndex:8]; [encoder setBuffer:context_buffer offset:0 atIndex:9];
      [encoder setBuffer:backing offset:0 atIndex:10];
      [encoder setComputePipelineState:prepare];
      [encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [encoder setComputePipelineState:builder];
      [encoder useResource:icb usage:MTLResourceUsageWrite];
      [encoder dispatchThreads:MTLSizeMake(2,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [encoder useResource:icb usage:MTLResourceUsageRead]; [encoder useResource:records usage:MTLResourceUsageRead];
      [encoder useResource:output usage:MTLResourceUsageWrite];
      [encoder useResource:context_buffer usage:MTLResourceUsageRead]; [encoder useResource:backing usage:MTLResourceUsageRead];
      [encoder executeCommandsInBuffer:icb indirectBuffer:range indirectBufferOffset:0];
      [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
      REQUIRE(command.status == MTLCommandBufferStatusCompleted);
      REQUIRE(!std::memcmp(output.contents, expected[mode], 16));
      const uint32_t expected_status = mode == 2 ? 1u : (mode == 3 ? 2u : 0u);
      REQUIRE(*static_cast<uint32_t *>(status.contents) == expected_status);
      const bool reset = mode < 2 || mode == 5 || mode == 6;
      REQUIRE(static_cast<uint32_t *>(status.contents)[1] == uint32_t(reset));
      const auto *backing_bytes = static_cast<const uint8_t *>(backing.contents);
      for (size_t byte = 0; byte < backing.length; ++byte)
        REQUIRE(backing_bytes[byte] == (reset && byte < table_bytes ? 0u : 0xa5u));
      const uint32_t *actual_range = static_cast<const uint32_t *>(range.contents);
      REQUIRE(actual_range[0] == 0u && actual_range[1] == ((mode < 2 || mode == 5 || mode == 6) ? 1u : 0u));
    }
    std::cout << "{\"pass\":true,\"gpu_generated_headers\":true,\"gpu_selected_pipelines\":true,\"intermediate_readback\":false,\"cases\":7,\"d3d12_integrated\":false}\n";
  }
}
