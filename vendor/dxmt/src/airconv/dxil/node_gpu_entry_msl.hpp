#pragma once
namespace dxmt::dxil {
// Include after node_gpu_input_msl.hpp's source, with M12_GPU_ENTRY_COUNT set
// from the immutable program snapshot. This first generator handles one
// thread-launch command for the entire input stream; other launch forms reject.
inline constexpr const char kNodeGPUEntryMSL[] = R"MSL(
struct m12_gpu_entry_handles {
  command_buffer commands [[id(0)]];
  array<compute_pipeline_state, M12_GPU_ENTRY_COUNT> pipelines [[id(1)]];
};
struct m12_gpu_input_pointer_view {
  uint entrypoint_index, record_count;
  device const uchar *records;
  ulong record_stride;
};
struct m12_gpu_entry_build_parameters { uint bound_count, command_capacity; };
struct m12_gpu_entry_context {
  uint version, count;
  ulong stride, size, length;
  uint batch_size, reserved;
  ulong routing_table_address;
  uint routing_table_count, source_node;
  uint remaining_levels, reserved2;
};
static_assert(sizeof(m12_gpu_entry_context) == 64, "GPU entry context ABI");
static inline uint m12_gpu_entry_error(m12_node_gpu_input_header header,
    device const m12_node_gpu_entry_layout *layouts,
    device const m12_node_gpu_address_bound *bounds,
    constant m12_gpu_entry_build_parameters &parameters) {
  if (!m12_node_gpu_input_valid(header, layouts, M12_GPU_ENTRY_COUNT,
                                bounds, parameters.bound_count)) return 1u;
  if (header.record_count != 0u && parameters.command_capacity == 0u) return 2u;
  if (header.record_count != 0u) {
    auto layout = layouts[header.entrypoint_index];
    if (layout.launch_type != 1u && layout.launch_type != 2u && layout.launch_type != 3u) return 3u;
    if (layout.threads_x == 0u || layout.threads_y == 0u || layout.threads_z == 0u ||
        layout.threads_x > 1024u || layout.threads_y > 1024u || layout.threads_z > 1024u) return 3u;
    ulong threads = ulong(layout.threads_x) * ulong(layout.threads_y) * ulong(layout.threads_z);
    if (threads > 1024ul || (layout.launch_type == 3u && threads != 1ul) ||
        (layout.launch_type == 2u && layout.max_records == 0u)) return 3u;
    if (layout.launch_type == 1u) {
      if (layout.grid_from_record != 0u || !layout.grid_x || !layout.grid_y || !layout.grid_z ||
          ulong(header.record_count) > ulong(parameters.command_capacity)) return 3u;
      ulong grid = ulong(layout.grid_x) * ulong(layout.grid_y) * ulong(layout.grid_z);
      if (grid == 0ul || grid > 0xfffffffful) return 3u;
    }
  }
  return 0u;
}
kernel void m12_prepare_gpu_entry(
    device const m12_node_gpu_input_header *header [[buffer(1)]],
    device const m12_node_gpu_entry_layout *layouts [[buffer(2)]],
    device const m12_node_gpu_address_bound *bounds [[buffer(3)]],
    device uint *range [[buffer(5)]], device uint *status [[buffer(6)]],
    constant m12_gpu_entry_build_parameters &parameters [[buffer(7)]],
    device uint *backing [[buffer(10)]], uint tid [[thread_position_in_grid]]) {
  if (tid != 0u) return;
  uint error = m12_gpu_entry_error(*header, layouts, bounds, parameters);
  status[0] = error;
  status[1] = error == 0u && header->record_count != 0u ? 1u : 0u;
  range[0] = 0u; range[1] = 0u;
  if (error != 0u || header->record_count == 0u) return;
  // Existing allocator ABI: 32-byte header plus 256 16-byte entries.
  // Payload storage starts later and is deliberately not touched.
  for (uint word = 0u; word < 1032u; ++word) backing[word] = 0u;
}
kernel void m12_build_gpu_entry_commands(
    constant m12_gpu_entry_handles &handles [[buffer(0)]],
    device const m12_node_gpu_input_header *header [[buffer(1)]],
    device const m12_node_gpu_entry_layout *layouts [[buffer(2)]],
    device const m12_node_gpu_address_bound *bounds [[buffer(3)]],
    device uchar *output [[buffer(4)]], device uint *range [[buffer(5)]],
    device uint *status [[buffer(6)]],
    constant m12_gpu_entry_build_parameters &parameters [[buffer(7)]],
    device const m12_gpu_entry_context *templates [[buffer(8)]],
    device m12_gpu_entry_context *contexts [[buffer(9)]],
    device uchar *backing [[buffer(10)]],
    uint tid [[thread_position_in_grid]]) {
  uint error = m12_gpu_entry_error(*header, layouts, bounds, parameters);
  // Do not index the GPU-selected layout until validation has established that
  // the entrypoint is within the immutable table. Invalid headers are a
  // no-write path, not permission to perform an out-of-bounds device read.
  if (error != 0u || header->record_count == 0u) {
    if (tid == 0u) {
      range[0] = 0u; range[1] = 0u;
      status[0] = error;
      status[1] = 0u;
    }
    return;
  }
  auto layout = layouts[header->entrypoint_index];
  uint command_count = layout.launch_type == 1u ? header->record_count : 1u;
  if (tid == 0u) {
    range[0] = 0u; range[1] = command_count;
    status[0] = 0u;
    status[1] = 1u;
  }
  if (tid >= command_count) return;
  auto pointer_header = reinterpret_cast<device const m12_gpu_input_pointer_view *>(header);
  uint command_index = layout.launch_type == 1u ? tid : 0u;
  contexts[command_index] = templates[header->entrypoint_index];
  // Broadcasting binds one record per command. Every group in that command
  // must read the same record, using the template's raw single-record ABI.
  // Only thread/coalescing streams select successive records by group.
  if (layout.launch_type != 1u)
    contexts[command_index].version = contexts[command_index].routing_table_count != 0u ? 9u : 8u;
  contexts[command_index].count = layout.launch_type == 1u ? 1u : header->record_count;
  uint batch = layout.launch_type == 2u ? layout.max_records : 1u;
  contexts[command_index].batch_size = batch;
  contexts[command_index].stride = header->record_stride;
  contexts[command_index].size = ulong(layout.record_size);
  contexts[command_index].length = layout.record_size != 0u
      ? (layout.launch_type == 1u ? ulong(layout.record_size) :
         (ulong(header->record_count) - 1ul) * header->record_stride + ulong(layout.record_size)) : 0ul;
  device const uchar *records = layout.record_size != 0u
      ? pointer_header->records + (layout.launch_type == 1u ? ulong(tid) * header->record_stride : 0ul)
      : reinterpret_cast<device const uchar *>(header);
  compute_command command(handles.commands, command_index);
  command.set_compute_pipeline_state(handles.pipelines[header->entrypoint_index]);
  command.set_kernel_buffer(output, 0);
  command.set_kernel_buffer(contexts + command_index, 28);
  command.set_kernel_buffer(records, 29);
  command.set_kernel_buffer(backing, 30);
  uint groups = layout.launch_type == 1u
      ? 1u : (header->record_count - 1u) / batch + 1u;
  uint3 grid = layout.launch_type == 1u
      ? uint3(layout.grid_x, layout.grid_y, layout.grid_z)
      : uint3(groups,1u,1u);
  uint3 threads = uint3(layout.threads_x, layout.threads_y, layout.threads_z);
  command.concurrent_dispatch_threadgroups(grid, threads);
}
struct m12_gpu_multi_input_header {
  uint num_node_inputs, reserved;
  ulong node_inputs, node_input_stride;
};
static_assert(sizeof(m12_gpu_multi_input_header) == 24, "multi GPU entry header ABI");
static inline uint m12_gpu_multi_entry_error(
    m12_gpu_multi_input_header header,
    device const m12_node_gpu_entry_layout *layouts,
    device const m12_node_gpu_address_bound *bounds,
    constant m12_gpu_entry_build_parameters &parameters) {
  if (header.num_node_inputs == 0u) return 0u;
  if (!header.node_inputs || (header.node_inputs & 7ul) != 0ul ||
      header.node_input_stride < 24ul ||
      (header.node_input_stride & 7ul) != 0ul ||
      header.num_node_inputs > parameters.command_capacity) return 2u;
  ulong last = ulong(header.num_node_inputs) - 1ul;
  if (last > (0xfffffffffffffffful - 24ul) / header.node_input_stride) return 3u;
  ulong table_length = last * header.node_input_stride + 24ul;
  if (!m12_node_gpu_address_contains(bounds, parameters.bound_count,
                                     header.node_inputs, table_length)) return 4u;
  for (uint index = 0u; index < header.num_node_inputs; ++index) {
    auto input = reinterpret_cast<device const m12_node_gpu_input_header *>(
        header.node_inputs + ulong(index) * header.node_input_stride);
    if (input == nullptr || m12_gpu_entry_error(*input, layouts, bounds, parameters) != 0u)
      return 5u;
    if (input->record_count != 0u && layouts[input->entrypoint_index].launch_type == 1u)
      return 6u;
  }
  return 0u;
}
kernel void m12_prepare_gpu_multi_entry(
    device const m12_gpu_multi_input_header *header [[buffer(1)]],
    device const m12_node_gpu_entry_layout *layouts [[buffer(2)]],
    device const m12_node_gpu_address_bound *bounds [[buffer(3)]],
    device uint *range [[buffer(5)]], device uint *status [[buffer(6)]],
    constant m12_gpu_entry_build_parameters &parameters [[buffer(7)]],
    device uint *backing [[buffer(10)]], uint tid [[thread_position_in_grid]]) {
  if (tid != 0u) return;
  uint error = m12_gpu_multi_entry_error(*header, layouts, bounds, parameters);
  uint work = 0u;
  if (error == 0u) {
    for (uint index = 0u; index < header->num_node_inputs; ++index) {
      auto input = reinterpret_cast<device const m12_node_gpu_input_header *>(
          header->node_inputs + ulong(index) * header->node_input_stride);
      if (input->record_count != 0u) work = 1u;
    }
  }
  status[0] = error; status[1] = work;
  range[0] = 0u; range[1] = 0u;
  if (error != 0u || work == 0u) return;
  for (uint word = 0u; word < 1032u; ++word) backing[word] = 0u;
}
kernel void m12_build_gpu_multi_entry_commands(
    constant m12_gpu_entry_handles &handles [[buffer(0)]],
    device const m12_gpu_multi_input_header *header [[buffer(1)]],
    device const m12_node_gpu_entry_layout *layouts [[buffer(2)]],
    device const m12_node_gpu_address_bound *bounds [[buffer(3)]],
    device uchar *output [[buffer(4)]], device uint *range [[buffer(5)]],
    device uint *status [[buffer(6)]],
    constant m12_gpu_entry_build_parameters &parameters [[buffer(7)]],
    device const m12_gpu_entry_context *templates [[buffer(8)]],
    device m12_gpu_entry_context *contexts [[buffer(9)]],
    device uchar *backing [[buffer(10)]], uint tid [[thread_position_in_grid]]) {
  if (tid >= parameters.command_capacity || status[0] != 0u) return;
  auto multi = *header;
  if (tid >= multi.num_node_inputs) return;
  auto input = reinterpret_cast<device const m12_node_gpu_input_header *>(
      multi.node_inputs + ulong(tid) * multi.node_input_stride);
  if (input->record_count == 0u) return;
  auto layout = layouts[input->entrypoint_index];
  uint command_index = atomic_fetch_add_explicit(
      reinterpret_cast<device atomic_uint *>(range + 1), 1u,
      memory_order_relaxed);
  uint batch = layout.launch_type == 2u ? layout.max_records : 1u;
  contexts[command_index] = templates[input->entrypoint_index];
  contexts[command_index].version = contexts[command_index].routing_table_count != 0u ? 9u : 8u;
  contexts[command_index].count = input->record_count;
  contexts[command_index].batch_size = batch;
  contexts[command_index].stride = input->record_stride;
  contexts[command_index].size = ulong(layout.record_size);
  contexts[command_index].length = layout.record_size != 0u
      ? (ulong(input->record_count) - 1ul) * input->record_stride + ulong(layout.record_size) : 0ul;
  auto pointer_header = reinterpret_cast<device const m12_gpu_input_pointer_view *>(input);
  compute_command command(handles.commands, command_index);
  command.set_compute_pipeline_state(handles.pipelines[input->entrypoint_index]);
  command.set_kernel_buffer(output, 0);
  command.set_kernel_buffer(contexts + command_index, 28);
  command.set_kernel_buffer(layout.record_size != 0u
      ? pointer_header->records : reinterpret_cast<device const uchar *>(input), 29);
  command.set_kernel_buffer(backing, 30);
  uint groups = (input->record_count - 1u) / batch + 1u;
  command.concurrent_dispatch_threadgroups(uint3(groups,1u,1u),
      uint3(layout.threads_x, layout.threads_y, layout.threads_z));
}
)MSL";
} // namespace dxmt::dxil
