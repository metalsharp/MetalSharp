#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "d3d12_node_routing.hpp"
#include "node_routing_msl.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
int main() {
  @autoreleasepool {
    try {
      id<MTLDevice> device = MTLCreateSystemDefaultDevice();
      require(device != nil, "Metal device missing");
      const std::vector<dxmt::NodeRoutingTarget> nodes = {{"entry",0},{"sparse_entry",0},
        {"array",0},{"array",1},{"array",2},{"array",3},{"sparse",65536},{"self",0,3}};
      auto routes = dxmt::buildNodeOutputRoutes(nodes, {
        {0,0,"array",0,4,true,false}, {1,0,"sparse",0,UINT32_MAX,true,true}, {7,0,"self",0,1,false,false}});
      require(routes && routes->size() == 10, "route construction failed");
      id<MTLBuffer> table = [device newBufferWithBytes:routes->data()
        length:routes->size()*sizeof(D3D12NodeOutputRoute) options:MTLResourceStorageModeShared];
      require(table != nil, "table allocation failed");
      D3D12NodeRecursionContext contexts[6];
      for (auto &context : contexts) {
        context.routing.routing_table_address = table.gpuAddress;
        context.routing.routing_table_count = static_cast<uint32_t>(routes->size());
      }
      contexts[1].routing.version = 5; contexts[1].routing.source_node = 1;
      contexts[2].routing.version = 3; contexts[2].routing.source_node = 1;
      contexts[3].routing.routing_table_address = 0;
      contexts[4].routing.version = 6; contexts[4].routing.source_node = 7; contexts[4].remaining_levels = 3;
      contexts[5].routing.version = 7; contexts[5].routing.source_node = 7;
      uint32_t cases[10][2] = {{0,0xdeadbeef},{0,0xdeadbeef},{1,0xdeadbeef},{1,0xdeadbeef},
                             {1,0xdeadbeef},{1,0xdeadbeef},{2,0xdeadbeef},{3,0xdeadbeef},{4,0xdeadbeef},{5,0xdeadbeef}};
      id<MTLBuffer> context_buffer = [device newBufferWithBytes:contexts length:sizeof(contexts) options:MTLResourceStorageModeShared];
      id<MTLBuffer> indices = [device newBufferWithBytes:cases length:sizeof(cases) options:MTLResourceStorageModeShared];
      id<MTLBuffer> result = [device newBufferWithLength:10*4*sizeof(uint32_t) options:MTLResourceStorageModeShared];
      require(context_buffer && indices && result, "buffer allocation failed");
      std::string source = "#include <metal_stdlib>\nusing namespace metal;\n";
      source += dxmt::dxil::kNodeRoutingMSL;
      source += R"MSL(
kernel void build_indices(device uint2 *cases [[buffer(1)]], uint i [[thread_position_in_grid]]) {
  const uint values[10] = {3u, 4u, 65536u, 1u, 0u, 0xffffffffu, 65536u, 0u, 0u, 0u};
  cases[i].y = values[i];
}
kernel void lookup(device const char *contexts [[buffer(0)]], device const uint2 *cases [[buffer(1)]],
                   device uint4 *result [[buffer(2)]], uint i [[thread_position_in_grid]]) {
  auto c = m12_node_route_context(contexts + cases[i].x * 64u);
  uint base = m12_node_route_find(c, 0u, 0u);
  uint token = m12_node_route_index(c, base, cases[i].y);
  uint target = token != 0u ? c->routes[token - 1u].target_node : 0xffffffffu;
  uint foreign = c != nullptr && c->source_node == 0u ? 6u : 1u;
  uint invalid_checks = m12_node_route_index(c, foreign, 0u) |
                        uint(m12_node_route_valid(c, 0u)) | uint(m12_node_route_valid(c, 11u));
  result[i] = uint4(token, target, uint(m12_node_route_valid(c, token)), (invalid_checks << 31u) | m12_node_remaining_levels(contexts + cases[i].x * 64u));
}
)MSL";
      MTLCompileOptions *options = [MTLCompileOptions new];
      options.languageVersion = MTLLanguageVersion4_0;
      NSError *error = nil;
      id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
        options:options error:&error];
      if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
      id<MTLComputePipelineState> producer = [device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"build_indices"] error:&error];
      id<MTLComputePipelineState> consumer = [device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"lookup"] error:&error];
      require(producer && consumer, "pipeline creation failed");
      id<MTLCommandQueue> queue = [device newCommandQueue];
      id<MTLCommandBuffer> command = [queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      require(queue && command && encoder, "command creation failed");
      [encoder useResource:table usage:MTLResourceUsageRead];
      [encoder setBuffer:context_buffer offset:0 atIndex:0];
      [encoder setBuffer:indices offset:0 atIndex:1];
      [encoder setBuffer:result offset:0 atIndex:2];
      [encoder setComputePipelineState:producer];
      [encoder dispatchThreads:MTLSizeMake(10,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [encoder setComputePipelineState:consumer];
      [encoder dispatchThreads:MTLSizeMake(10,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
      [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
      require(command.status == MTLCommandBufferStatusCompleted, "GPU execution failed");
      const uint32_t expected[10][4] = {{4,5,1,0},{5,UINT32_MAX,0,0},{7,6,1,0},{8,UINT32_MAX,0,0},
        {6,UINT32_MAX,0,0},{8,UINT32_MAX,0,0},{0,UINT32_MAX,0,0},{0,UINT32_MAX,0,0},{9,7,1,3},{9,7,0,0}};
      const auto *actual = static_cast<const uint32_t *>(result.contents);
      for (unsigned i = 0; i < 40; ++i) require(actual[i] == expected[i/4][i%4], "routing result mismatch");
      std::cout << "{\"pass\":true,\"gpu_generated_indices\":true,\"routing_cases\":10,"
                   "\"d3d12_integrated\":false,\"intermediate_readback\":false}\n";
      return 0;
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      std::cout << "{\"pass\":false,\"d3d12_integrated\":false}\n";
      return 1;
    }
  }
}
