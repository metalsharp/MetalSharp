#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#import <Foundation/Foundation.h>
#include <map>
#import <Metal/Metal.h>
#include <string>
#include <vector>

static NSString* json_escape(NSString* value) {
    NSMutableString* out = [NSMutableString stringWithString:value ?: @""];
    [out replaceOccurrencesOfString:@"\\" withString:@"\\\\" options:0 range:NSMakeRange(0, out.length)];
    [out replaceOccurrencesOfString:@"\"" withString:@"\\\"" options:0 range:NSMakeRange(0, out.length)];
    [out replaceOccurrencesOfString:@"\n" withString:@"\\n" options:0 range:NSMakeRange(0, out.length)];
    [out replaceOccurrencesOfString:@"\r" withString:@"\\r" options:0 range:NSMakeRange(0, out.length)];
    return out;
}

static bool read_file(NSString* path, NSData** out_data) {
    NSData* data = [NSData dataWithContentsOfFile:path];
    if (!data || data.length == 0)
        return false;
    *out_data = data;
    return true;
}

static std::string sha256_hex(NSData* data) {
    // FNV-1a-64 is sufficient here as a compact non-cryptographic record; the
    // runner validates SHA256 for artifacts it persists. Keep the field name
    // explicit to avoid overclaiming.
    const uint8_t* bytes = static_cast<const uint8_t*>(data.bytes);
    uint64_t hash = 1469598103934665603ull;
    for (NSUInteger i = 0; i < data.length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buffer);
}

static NSString* string_value(NSDictionary* dict, NSString* key) {
    id value = dict[key];
    return [value isKindOfClass:[NSString class]] ? value : @"";
}

static NSDictionary* dictionary_value(NSDictionary* dict, NSString* key) {
    id value = dict[key];
    return [value isKindOfClass:[NSDictionary class]] ? value : nil;
}

static id<MTLFunction> load_function(id<MTLDevice> device, NSString* metallib_path, NSString* function_name,
                                     NSMutableArray<NSDictionary*>* load_records) {
    NSError* error = nil;
    NSData* data = nil;
    BOOL data_ok = read_file(metallib_path, &data);
    NSURL* url = [NSURL fileURLWithPath:metallib_path];
    id<MTLLibrary> library = data_ok ? [device newLibraryWithURL:url error:&error] : nil;
    id<MTLFunction> function = nil;
    if (library)
        function = [library newFunctionWithName:function_name];
    [load_records addObject:@{
        @"requested_metallib" : metallib_path ?: @"",
        @"metallib" : metallib_path ?: @"",
        @"metallib_size" : data_ok ? @(data.length) : @(0),
        @"metallib_fnv1a64" : data_ok ? [NSString stringWithUTF8String:sha256_hex(data).c_str()] : @"",
        @"function" : function_name ?: @"",
        @"library_ok" : @(library != nil),
        @"function_ok" : @(function != nil),
        @"error" : error ? error.localizedDescription : @""
    }];
    return function;
}

static NSDictionary* load_manifest(NSString* path) {
    NSData* data = [NSData dataWithContentsOfFile:path];
    if (!data)
        return nil;
    NSError* error = nil;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    return [json isKindOfClass:[NSDictionary class]] ? json : nil;
}

int main(int argc, const char** argv) {
    @autoreleasepool {
        NSString* manifest_list = nil;
        NSString* archive_path = nil;
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--manifest-list") == 0)
                manifest_list = [NSString stringWithUTF8String:argv[++i]];
            else if (std::strcmp(argv[i], "--archive") == 0)
                archive_path = [NSString stringWithUTF8String:argv[++i]];
        }
        if (!manifest_list || !archive_path) {
            std::printf("{\"ok\":false,\"error\":\"missing_args\"}\n");
            return 2;
        }

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::printf("{\"ok\":false,\"error\":\"no_metal_device\"}\n");
            return 3;
        }

        NSString* list_text = [NSString stringWithContentsOfFile:manifest_list encoding:NSUTF8StringEncoding error:nil];
        NSArray<NSString*>* lines = [list_text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet];
        NSMutableArray<NSDictionary*>* manifest_records = [NSMutableArray array];
        NSMutableArray<NSDictionary*>* load_records = [NSMutableArray array];
        NSMutableArray<MTLRenderPipelineDescriptor*>* render_descs = [NSMutableArray array];
        NSMutableArray<MTLComputePipelineDescriptor*>* compute_descs = [NSMutableArray array];
        NSUInteger render_count = 0;
        NSUInteger compute_count = 0;
        NSUInteger manifest_count = 0;

        for (NSString* raw in lines) {
            NSString* path = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if (path.length == 0)
                continue;
            manifest_count++;
            NSDictionary* manifest = load_manifest(path);
            if (!manifest) {
                [manifest_records addObject:@{@"manifest" : path, @"ok" : @NO, @"error" : @"manifest_parse_failed"}];
                continue;
            }
            NSArray* pipelines = manifest[@"pipelines"];
            if (![pipelines isKindOfClass:[NSArray class]] || pipelines.count == 0) {
                [manifest_records addObject:@{@"manifest" : path, @"ok" : @NO, @"error" : @"missing_pipelines"}];
                continue;
            }
            for (NSDictionary* pipeline in pipelines) {
                if (![pipeline isKindOfClass:[NSDictionary class]])
                    continue;
                NSString* type = string_value(pipeline, @"type");
                NSString* pipeline_name = string_value(pipeline, @"name");
                if ([type isEqualToString:@"render"]) {
                    NSDictionary* vertex = dictionary_value(pipeline, @"vertex");
                    NSDictionary* fragment = dictionary_value(pipeline, @"fragment");
                    NSString* vlib = string_value(vertex, @"metallib");
                    NSString* flib = string_value(fragment, @"metallib");
                    NSString* vfn = string_value(vertex, @"function");
                    NSString* ffn = string_value(fragment, @"function");
                    id<MTLFunction> vf = load_function(device, vlib, vfn, load_records);
                    id<MTLFunction> ff = load_function(device, flib, ffn, load_records);
                    if (vf && ff) {
                        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
                        desc.label =
                            [NSString stringWithFormat:@"fresh-render-%@-%@", path.lastPathComponent, pipeline_name];
                        desc.vertexFunction = vf;
                        desc.fragmentFunction = ff;
                        desc.rasterizationEnabled = YES;
                        desc.rasterSampleCount = 1;
                        desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
                        desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                        NSString* depth_format = string_value(pipeline, @"depth_format");
                        if ([depth_format isEqualToString:@"depth32float"])
                            desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                        [render_descs addObject:desc];
                        render_count++;
                    }
                    [manifest_records addObject:@{
                        @"manifest" : path,
                        @"pipeline" : pipeline_name,
                        @"ok" : @(vf != nil && ff != nil),
                        @"type" : @"render"
                    }];
                } else if ([type isEqualToString:@"compute"]) {
                    NSDictionary* compute = dictionary_value(pipeline, @"shader");
                    NSString* clib = string_value(compute, @"metallib");
                    NSString* cfn = string_value(compute, @"function");
                    id<MTLFunction> cf = load_function(device, clib, cfn, load_records);
                    if (cf) {
                        MTLComputePipelineDescriptor* desc = [[MTLComputePipelineDescriptor alloc] init];
                        desc.label =
                            [NSString stringWithFormat:@"fresh-compute-%@-%@", path.lastPathComponent, pipeline_name];
                        desc.computeFunction = cf;
                        [compute_descs addObject:desc];
                        compute_count++;
                    }
                    [manifest_records addObject:@{
                        @"manifest" : path,
                        @"pipeline" : pipeline_name,
                        @"ok" : @(cf != nil),
                        @"type" : @"compute"
                    }];
                } else {
                    [manifest_records addObject:@{
                        @"manifest" : path,
                        @"pipeline" : pipeline_name,
                        @"ok" : @NO,
                        @"type" : type ?: @"",
                        @"error" : @"unsupported_type"
                    }];
                }
            }
        }

        NSError* error = nil;
        MTLBinaryArchiveDescriptor* archive_desc = [[MTLBinaryArchiveDescriptor alloc] init];
        id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:archive_desc error:&error];
        BOOL archive_create_ok = archive != nil;
        NSUInteger add_render_ok = 0;
        NSUInteger add_compute_ok = 0;
        NSMutableArray<NSString*>* archive_errors = [NSMutableArray array];
        if (archive) {
            for (MTLRenderPipelineDescriptor* desc in render_descs) {
                NSError* add_error = nil;
                if ([archive addRenderPipelineFunctionsWithDescriptor:desc error:&add_error])
                    add_render_ok++;
                else
                    [archive_errors addObject:add_error ? add_error.localizedDescription : @"add_render_failed"];
            }
            for (MTLComputePipelineDescriptor* desc in compute_descs) {
                NSError* add_error = nil;
                if ([archive addComputePipelineFunctionsWithDescriptor:desc error:&add_error])
                    add_compute_ok++;
                else
                    [archive_errors addObject:add_error ? add_error.localizedDescription : @"add_compute_failed"];
            }
        } else if (error) {
            [archive_errors addObject:error.localizedDescription];
        }

        NSURL* archive_url = [NSURL fileURLWithPath:archive_path];
        [[NSFileManager defaultManager] removeItemAtURL:archive_url error:nil];
        NSError* serialize_error = nil;
        BOOL serialize_ok = archive && [archive serializeToURL:archive_url error:&serialize_error];
        if (!serialize_ok && serialize_error)
            [archive_errors addObject:serialize_error.localizedDescription];

        NSDictionary* archive_attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:archive_path error:nil];
        unsigned long long archive_size = archive_attrs ? [archive_attrs fileSize] : 0ull;

        MTLBinaryArchiveDescriptor* lookup_desc = [[MTLBinaryArchiveDescriptor alloc] init];
        lookup_desc.url = archive_url;
        NSError* reload_error = nil;
        id<MTLBinaryArchive> reloaded_archive = [device newBinaryArchiveWithDescriptor:lookup_desc error:&reload_error];
        BOOL reload_ok = reloaded_archive != nil;
        if (!reload_ok && reload_error)
            [archive_errors addObject:reload_error.localizedDescription];

        dispatch_group_t group = dispatch_group_create();
        __block NSUInteger async_render_ok = 0;
        __block NSUInteger async_compute_ok = 0;
        __block NSUInteger async_render_callbacks = 0;
        __block NSUInteger async_compute_callbacks = 0;
        NSMutableArray<NSString*>* async_errors = [NSMutableArray array];
        NSArray* archive_array = reloaded_archive ? @[ reloaded_archive ] : @[];
        for (MTLRenderPipelineDescriptor* desc in render_descs) {
            desc.binaryArchives = archive_array;
            dispatch_group_enter(group);
            [device
                newRenderPipelineStateWithDescriptor:desc
                                             options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                   completionHandler:^(id<MTLRenderPipelineState> state,
                                                       MTLRenderPipelineReflection* reflection, NSError* async_error) {
                                     @synchronized(async_errors) {
                                         async_render_callbacks++;
                                         if (state)
                                             async_render_ok++;
                                         else
                                             [async_errors addObject:async_error ? async_error.localizedDescription
                                                                                 : @"render_async_failed"];
                                     }
                                     (void)reflection;
                                     dispatch_group_leave(group);
                                   }];
        }
        for (MTLComputePipelineDescriptor* desc in compute_descs) {
            desc.binaryArchives = archive_array;
            dispatch_group_enter(group);
            [device newComputePipelineStateWithDescriptor:desc
                                                  options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                        completionHandler:^(id<MTLComputePipelineState> state,
                                                            MTLComputePipelineReflection* reflection,
                                                            NSError* async_error) {
                                          @synchronized(async_errors) {
                                              async_compute_callbacks++;
                                              if (state)
                                                  async_compute_ok++;
                                              else
                                                  [async_errors addObject:async_error ? async_error.localizedDescription
                                                                                      : @"compute_async_failed"];
                                          }
                                          (void)reflection;
                                          dispatch_group_leave(group);
                                        }];
        }
        long wait_result = dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC));
        BOOL async_wait_ok = wait_result == 0;
        NSUInteger async_render_ok_snapshot = 0;
        NSUInteger async_compute_ok_snapshot = 0;
        NSUInteger async_render_callbacks_snapshot = 0;
        NSUInteger async_compute_callbacks_snapshot = 0;
        NSArray<NSString*>* async_errors_snapshot = nil;
        @synchronized(async_errors) {
            async_render_ok_snapshot = async_render_ok;
            async_compute_ok_snapshot = async_compute_ok;
            async_render_callbacks_snapshot = async_render_callbacks;
            async_compute_callbacks_snapshot = async_compute_callbacks;
            async_errors_snapshot = [async_errors copy];
        }

        BOOL load_records_ok = YES;
        for (NSDictionary* rec in load_records) {
            if (![rec[@"library_ok"] boolValue] || ![rec[@"function_ok"] boolValue])
                load_records_ok = NO;
        }
        BOOL manifest_records_ok = YES;
        for (NSDictionary* rec in manifest_records) {
            if (![rec[@"ok"] boolValue])
                manifest_records_ok = NO;
        }
        BOOL ok = archive_create_ok && serialize_ok && reload_ok && archive_size > 0 && load_records_ok &&
                  manifest_records_ok && manifest_records.count >= manifest_count && render_count >= 1 &&
                  compute_count >= 1 && add_render_ok == render_count && add_compute_ok == compute_count &&
                  async_wait_ok && async_render_callbacks_snapshot == render_count &&
                  async_compute_callbacks_snapshot == compute_count && async_render_ok_snapshot == render_count &&
                  async_compute_ok_snapshot == compute_count && archive_errors.count == 0 &&
                  async_errors_snapshot.count == 0;

        NSMutableDictionary* result = [NSMutableDictionary dictionary];
        result[@"schema"] = @"metalsharp.vkd3d.fresh.metal-archive-proof.v1";
        result[@"ok"] = @(ok);
        result[@"device_name"] = device.name ?: @"";
        result[@"manifest_count"] = @(manifest_count);
        result[@"render_descriptor_count"] = @(render_count);
        result[@"compute_descriptor_count"] = @(compute_count);
        result[@"metallib_load_records"] = load_records;
        result[@"manifest_records"] = manifest_records;
        result[@"manifest_records_ok"] = @(manifest_records_ok);
        result[@"binary_archive_create_ok"] = @(archive_create_ok);
        result[@"add_render_pipeline_functions_ok"] = @(add_render_ok);
        result[@"add_compute_pipeline_functions_ok"] = @(add_compute_ok);
        result[@"serialize_ok"] = @(serialize_ok);
        result[@"archive_path"] = archive_path;
        result[@"archive_size"] = @(archive_size);
        result[@"reload_ok"] = @(reload_ok);
        result[@"async_wait_ok"] = @(async_wait_ok);
        result[@"async_render_callbacks"] = @(async_render_callbacks_snapshot);
        result[@"async_render_ok"] = @(async_render_ok_snapshot);
        result[@"async_compute_callbacks"] = @(async_compute_callbacks_snapshot);
        result[@"async_compute_ok"] = @(async_compute_ok_snapshot);
        result[@"archive_errors"] = archive_errors;
        result[@"async_errors"] = async_errors_snapshot;

        NSData* json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
        if (json) {
            fwrite(json.bytes, 1, json.length, stdout);
            fputc('\n', stdout);
        } else {
            std::printf("{\"ok\":false,\"error\":\"json_serialize_failed\"}\n");
            return 4;
        }
        return ok ? 0 : 5;
    }
}
