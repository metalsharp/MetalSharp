#include <metal_stdlib>
#include <metal_command_buffer>
using namespace metal;
struct Record { uint x, y, z, index, value; };
struct Commands { command_buffer icb [[id(0)]]; compute_pipeline_state pipeline [[id(1)]]; };
kernel void consume(device atomic_uint *output [[buffer(0)]], device const Record *record [[buffer(1)]],
                    uint3 tid [[thread_position_in_grid]]) {
  atomic_fetch_add_explicit(output + record->index, record->value + tid.x + 10u*tid.y + 100u*tid.z, memory_order_relaxed);
}
kernel void build(constant Commands &commands [[buffer(0)]], device const Record *records [[buffer(1)]],
                  device atomic_uint *output [[buffer(2)]], device uint *range [[buffer(3)]], uint tid [[thread_position_in_grid]]) {
  if (tid != 0) return;
  uint count = min(records[5].x, 4u);
  for (uint i = 0; i < 5; ++i) {
    compute_command command(commands.icb, i + 1u);
    command.set_compute_pipeline_state(commands.pipeline);
    command.set_kernel_buffer(output, 0);
    command.set_kernel_buffer(records + i, 1);
    command.concurrent_dispatch_threadgroups(uint3(records[i].x,records[i].y,records[i].z), uint3(2,1,1));
  }
  // Command 5 is a poison write. Only the GPU-authored range excludes it.
  range[0] = 1u;
  range[1] = count;
}
