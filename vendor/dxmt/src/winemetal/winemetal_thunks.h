
#ifndef __WINEMETAL_THUNKS_H
#define __WINEMETAL_THUNKS_H

#include "winemetal.h"

#pragma pack(push, 8)

struct unixcall_generic_obj_ret {
  obj_handle_t ret;
};

struct unixcall_generic_obj_noret {
  obj_handle_t handle;
};

struct unixcall_generic_obj_obj_ret {
  obj_handle_t handle;
  obj_handle_t ret;
};

struct unixcall_generic_obj_uint64_obj_ret {
  obj_handle_t handle;
  uint64_t arg;
  obj_handle_t ret;
};

struct unixcall_generic_obj_uint64_ret {
  obj_handle_t handle;
  uint64_t ret;
};

struct unixcall_generic_obj_uint64_noret {
  obj_handle_t handle;
  uint64_t arg;
};

struct unixcall_generic_obj_ptr_noret {
  obj_handle_t handle;
  struct WMTMemoryPointer arg;
};

struct unixcall_generic_obj_constptr_noret {
  obj_handle_t handle;
  struct WMTConstMemoryPointer arg;
};

struct unixcall_generic_obj_obj_uint64_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  uint64_t arg1;
};

struct unixcall_mtlcommandbuffer_retain_objects_until_completed {
  obj_handle_t cmdbuf;
  struct WMTConstMemoryPointer objects;
  uint64_t count;
};

struct unixcall_generic_obj_uint64_uint64_ret {
  obj_handle_t handle;
  uint64_t arg;
  uint64_t ret;
};

struct unixcall_nsstring_getcstring {
  obj_handle_t str;
  uint64_t buffer_ptr;
  uint64_t max_length;
  enum WMTStringEncoding encoding;
  uint32_t ret;
};

struct unixcall_mtldevice_newbuffer {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newsparsebuffer {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newmtl4commandqueue {
  obj_handle_t device;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newheap {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtlheap_newtexture {
  obj_handle_t heap;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtlheap_newbuffer_offset {
  obj_handle_t heap;
  struct WMTMemoryPointer info;
  uint64_t offset;
  obj_handle_t ret;
};

struct unixcall_mtlheap_newtexture_offset {
  obj_handle_t heap;
  struct WMTMemoryPointer info;
  uint64_t offset;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newsamplerstate {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newdepthstencilstate {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newtexture {
  obj_handle_t device;
  struct WMTMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtlbuffer_newtexture {
  obj_handle_t buffer;
  struct WMTMemoryPointer info;
  uint64_t offset;
  uint64_t bytes_per_row;
  obj_handle_t ret;
};

struct unixcall_mtltexture_newtextureview {
  obj_handle_t texture;
  enum WMTPixelFormat format;
  enum WMTTextureType texture_type;
  uint16_t level_start;
  uint16_t level_count;
  uint16_t slice_start;
  uint16_t slice_count;
  struct WMTTextureSwizzleChannels swizzle;
  obj_handle_t ret;
  uint64_t gpu_resource_id;
};

struct unixcall_mtldevice_newlibrary {
  obj_handle_t device;
  obj_handle_t data;
  obj_handle_t ret_error;
  obj_handle_t ret_library;
};

struct unixcall_mtldevice_newcomputepso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_mtldevice_newrenderpso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_mtldevice_newmeshrenderpso {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_error;
  obj_handle_t ret_pso;
};

struct unixcall_generic_obj_cmd_noret {
  obj_handle_t encoder;
  struct WMTConstMemoryPointer cmd_head;
};

struct unixcall_mtlresource_state_update_texture_mappings {
  obj_handle_t encoder;
  obj_handle_t texture;
  struct WMTConstMemoryPointer mappings;
  uint64_t mapping_count;
  uint64_t ret_success;
};

struct unixcall_mtl4commandqueue_update_buffer_mappings {
  obj_handle_t queue;
  obj_handle_t buffer;
  obj_handle_t heap;
  struct WMTConstMemoryPointer operations;
  uint64_t operation_count;
  uint64_t ret_success;
};

struct unixcall_mtltexture_replaceregion {
  obj_handle_t texture;
  struct WMTOrigin origin;
  struct WMTSize size;
  uint64_t level;
  uint64_t slice;
  struct WMTMemoryPointer data;
  uint64_t bytes_per_row;
  uint64_t bytes_per_image;
};

struct unixcall_generic_obj_obj_noret {
  obj_handle_t handle;
  obj_handle_t arg;
};

struct unixcall_generic_obj_obj_obj_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  obj_handle_t arg1;
};

struct unixcall_generic_obj_obj_obj_uint64_ret {
  obj_handle_t handle;
  obj_handle_t arg0;
  obj_handle_t arg1;
  uint64_t ret;
};

struct unixcall_generic_obj_obj_double_noret {
  obj_handle_t handle;
  obj_handle_t arg0;
  double arg1;
};

struct unixcall_mtlcapturemanager_startcapture {
  obj_handle_t capture_manager;
  struct WMTMemoryPointer info;
  bool ret;
};

struct unixcall_mtldevice_newfxtemporalscaler {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newfxspatialscaler {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret;
};

struct unixcall_mtlcommandbuffer_temporal_scale {
  obj_handle_t cmdbuf;
  obj_handle_t scaler;
  obj_handle_t color;
  obj_handle_t output;
  obj_handle_t depth;
  obj_handle_t motion;
  obj_handle_t exposure;
  obj_handle_t fence;
  struct WMTConstMemoryPointer props;
};

struct unixcall_mtlcommandbuffer_spatial_scale {
  obj_handle_t cmdbuf;
  obj_handle_t scaler;
  obj_handle_t color;
  obj_handle_t output;
  obj_handle_t fence;
};

struct unixcall_nsstring_string {
  struct WMTConstMemoryPointer buffer_ptr;
  enum WMTStringEncoding encoding;
  obj_handle_t ret;
};

struct unixcall_create_metal_view_from_hwnd {
  uint64_t hwnd;
  obj_handle_t device;
  obj_handle_t ret_view;
  obj_handle_t ret_layer;
};

struct unixcall_enumerate {
  obj_handle_t enumerable;
  uint64_t start;
  uint64_t buffer_size;
  struct WMTMemoryPointer buffer;
  uint64_t ret_read;
};

struct unixcall_mtllibrary_newfunction_with_constants {
  obj_handle_t library;
  struct WMTConstMemoryPointer name;
  struct WMTConstMemoryPointer constants;
  uint64_t num_constants;
  obj_handle_t ret;
  obj_handle_t ret_error;
};

struct unixcall_mtllibrary_newfunction_with_descriptor {
  obj_handle_t library;
  struct WMTConstMemoryPointer name;
  struct WMTConstMemoryPointer specialized_name;
  struct WMTConstMemoryPointer constants;
  uint64_t num_constants;
  uint32_t options;
  obj_handle_t ret;
  obj_handle_t ret_error;
};

struct unixcall_query_display_setting {
  uint64_t display_id;
  enum WMTColorSpace colorspace;
  struct WMTMemoryPointer hdr_metadata;
  bool ret;
};

struct unixcall_update_display_setting {
  uint64_t display_id;
  enum WMTColorSpace colorspace;
  struct WMTConstMemoryPointer hdr_metadata;
};

struct unixcall_query_display_setting_for_layer {
  obj_handle_t layer;
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTMemoryPointer hdr_metadata;
  struct WMTEDRValue edr_value;
};

struct unixcall_mtlbuffer_updatecontents {
  obj_handle_t buffer;
  uint64_t offset;
  struct WMTConstMemoryPointer data;
  uint64_t length;
};

struct unixcall_mtlsharedevent_setevent {
  obj_handle_t shared_event;
  obj_handle_t event_handle;
  obj_handle_t shared_event_listener;
  uint64_t value;
};

struct unixcall_mtlsharedevent_createmachport {
  obj_handle_t event;
  uint32_t ret_mach_port;
};

struct unixcall_mtldevice_newsharedeventwithmachport {
  obj_handle_t device;
  uint32_t mach_port;
  obj_handle_t ret_event;
};

struct unixcall_get_os_version {
  uint64_t ret_major;
  uint64_t ret_minor;
  uint64_t ret_patch;
};

struct unixcall_mtldevice_newbinaryarchive {
  obj_handle_t device;
  struct WMTConstMemoryPointer url;
  obj_handle_t ret_archive;
  obj_handle_t ret_error;
};

struct unixcall_mtlbinaryarchive_serialize {
  obj_handle_t archive;
  struct WMTConstMemoryPointer url;
  obj_handle_t ret_error;
};

struct unixcall_cache_alloc_init {
  struct WMTConstMemoryPointer path;
  uint64_t version;
  obj_handle_t ret_cache;
};

struct unixcall_cache_get {
  obj_handle_t cache;
  struct WMTConstMemoryPointer key;
  uint64_t key_length;
  obj_handle_t ret_data;
};

struct unixcall_cache_set {
  obj_handle_t cache;
  struct WMTConstMemoryPointer key;
  uint64_t key_length;
  obj_handle_t value_data;
};

struct unixcall_setmetalcachepath {
  struct WMTConstMemoryPointer path;
  uint64_t ret_success;
};

struct unixcall_bootstrap {
  char name[128];
  uint32_t mach_port;
  uint32_t reserved;
};

struct unixcall_mtlsharedevent_waituntilsignaledvalue {
  obj_handle_t event;
  uint64_t value;
  uint64_t timeout_ms;
  bool ret_timeout;
};

struct unixcall_mtlcountersamplebuffer_newtimestampbuffer {
  obj_handle_t device;
  uint32_t sample_count;
  uint32_t shared;
  obj_handle_t ret;
};

struct unixcall_mtlcountersamplebuffer_resolvecounterrange {
  obj_handle_t sample_buffer;
  uint32_t start;
  uint32_t len;
  struct WMTMemoryPointer data_out;
  uint64_t data_length;
};

struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers {
  obj_handle_t cmdbuf;
  struct WMTMemoryPointer attachments;
  uint64_t num_attachments;
  obj_handle_t ret;
};

struct unixcall_mtldevice_newlibrary_source {
  obj_handle_t device;
  struct WMTConstMemoryPointer source;
  uint64_t source_length;
  obj_handle_t ret_error;
  obj_handle_t ret_library;
};

struct unixcall_mtldevice_acceleration_structure_sizes {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  struct WMTMemoryPointer sizes;
  uint64_t ret_success;
};

struct unixcall_mtldevice_new_acceleration_structure {
  obj_handle_t device;
  uint64_t size;
  obj_handle_t ret_acceleration_structure;
};

struct unixcall_mtlcommandbuffer_build_triangle_acceleration_structure {
  obj_handle_t cmdbuf;
  obj_handle_t acceleration_structure;
  struct WMTConstMemoryPointer info;
  obj_handle_t scratch_buffer;
  uint64_t scratch_buffer_offset;
  uint64_t ret_success;
};

struct unixcall_mtldevice_acceleration_structure_sizes_for_instances {
  obj_handle_t device;
  uint64_t instance_count;
  uint64_t allow_refit;
  struct WMTMemoryPointer sizes;
  uint64_t ret_success;
};

struct unixcall_mtldevice_new_raytracing_compute_pipeline {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  obj_handle_t ret_visible_function_table;
  obj_handle_t ret_intersection_function_table;
  obj_handle_t ret_error;
  obj_handle_t ret_pipeline;
};

struct unixcall_mtlcommandbuffer_build_instance_acceleration_structure {
  obj_handle_t cmdbuf;
  obj_handle_t acceleration_structure;
  obj_handle_t instance_descriptor_buffer;
  uint64_t instance_descriptor_buffer_offset;
  uint64_t instance_count;
  struct WMTConstMemoryPointer instanced_acceleration_structures;
  uint64_t instanced_acceleration_structure_count;
  uint64_t allow_refit;
  obj_handle_t scratch_buffer;
  uint64_t scratch_buffer_offset;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_refit_instance_acceleration_structure {
  obj_handle_t cmdbuf;
  obj_handle_t source_acceleration_structure;
  obj_handle_t destination_acceleration_structure;
  obj_handle_t instance_descriptor_buffer;
  uint64_t instance_descriptor_buffer_offset;
  uint64_t instance_count;
  struct WMTConstMemoryPointer instanced_acceleration_structures;
  uint64_t instanced_acceleration_structure_count;
  obj_handle_t scratch_buffer;
  uint64_t scratch_buffer_offset;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_write_timestamp_results {
  obj_handle_t cmdbuf;
  obj_handle_t destination_buffer;
  uint64_t destination_offset;
  uint32_t result_count;
  uint32_t reserved;
  uint64_t ret_success;
};

struct unixcall_mtldevice_acceleration_structure_sizes_for_aabbs {
  obj_handle_t device;
  struct WMTConstMemoryPointer info;
  struct WMTMemoryPointer sizes;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_build_aabb_acceleration_structure {
  obj_handle_t cmdbuf;
  obj_handle_t acceleration_structure;
  struct WMTConstMemoryPointer info;
  obj_handle_t scratch_buffer;
  uint64_t scratch_buffer_offset;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_refit_aabb_acceleration_structure {
  obj_handle_t cmdbuf;
  obj_handle_t source_acceleration_structure;
  obj_handle_t destination_acceleration_structure;
  struct WMTConstMemoryPointer info;
  obj_handle_t scratch_buffer;
  uint64_t scratch_buffer_offset;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_copy_acceleration_structure {
  obj_handle_t cmdbuf;
  obj_handle_t source_acceleration_structure;
  obj_handle_t destination_acceleration_structure;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_copy_and_compact_acceleration_structure {
  obj_handle_t cmdbuf;
  obj_handle_t source_acceleration_structure;
  obj_handle_t destination_acceleration_structure;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_write_compacted_acceleration_structure_size {
  obj_handle_t cmdbuf;
  obj_handle_t source_acceleration_structure;
  obj_handle_t destination_buffer;
  uint64_t destination_buffer_offset;
  uint64_t ret_success;
};

struct unixcall_mtldevice_acceleration_structure_sizes_for_triangle_geometries {
  obj_handle_t device;
  struct WMTConstMemoryPointer infos;
  uint64_t info_count;
  struct WMTMemoryPointer sizes;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_build_triangle_acceleration_structures {
  obj_handle_t cmdbuf;
  obj_handle_t acceleration_structure;
  struct WMTConstMemoryPointer infos;
  uint64_t info_count;
  obj_handle_t scratch_buffer;
  uint64_t scratch_buffer_offset;
  uint64_t ret_success;
};

struct unixcall_mtlcommandbuffer_refit_triangle_acceleration_structure {
  obj_handle_t cmdbuf;
  obj_handle_t source_acceleration_structure;
  obj_handle_t destination_acceleration_structure;
  struct WMTConstMemoryPointer info;
  obj_handle_t scratch_buffer;
  uint64_t scratch_buffer_offset;
  uint64_t ret_success;
};

#pragma pack(pop)

#endif
