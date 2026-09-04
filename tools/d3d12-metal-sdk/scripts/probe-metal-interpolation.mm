#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>

static NSString *const kSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct InterpolationInput {
  float4 position [[position]];
  float4 perspective [[user(locn0), center_perspective]];
  float4 no_perspective [[user(locn1), center_no_perspective]];
  float4 centroid_perspective [[user(locn2), centroid_perspective]];
  float4 sample_perspective [[user(locn3), sample_perspective]];
  float4 flat [[user(locn4), flat]];
  interpolant<float4, interpolation::perspective> evaluated [[user(locn5)]];
};

fragment float4 interpolation_probe(InterpolationInput input [[stage_in]],
                                    uint sample_id [[sample_id]]) {
  float4 center = input.evaluated.interpolate_at_center();
  float4 centroid = input.evaluated.interpolate_at_centroid();
  float4 sample = input.evaluated.interpolate_at_sample(sample_id);
  float4 offset = input.evaluated.interpolate_at_offset(float2(0.0f));
  return center + centroid + sample + offset +
         input.perspective + input.no_perspective +
         input.centroid_perspective + input.sample_perspective + input.flat;
}
)MSL";

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::puts("{\"schema\":\"metalsharp.metal-interpolation.v1\",\"exact\":false,\"error\":\"no Metal device\"}");
      return 1;
    }
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion4_0;
    NSError *error = nil;
    id<MTLLibrary> library =
        [device newLibraryWithSource:kSource options:options error:&error];
    const bool compiled = library != nil;
    const bool raster_order_groups = device.rasterOrderGroupsSupported;
    const bool pull_model_interpolation = device.supportsPullModelInterpolation;
    const bool shader_barycentrics = device.supportsShaderBarycentricCoordinates;
    const bool programmable_sample_positions =
        device.programmableSamplePositionsSupported;
    std::printf("{\n  \"schema\": \"metalsharp.metal-interpolation.v1\",\n");
    std::printf("  \"device\": \"%s\",\n", device.name.UTF8String);
    std::printf("  \"metal_language\": \"4.0\",\n");
    std::printf("  \"library_compiled\": %s,\n", compiled ? "true" : "false");
    std::printf("  \"qualified_stage_in\": %s,\n", compiled ? "true" : "false");
    std::printf("  \"explicit_center_centroid_sample_offset\": %s,\n",
                compiled ? "true" : "false");
    std::printf("  \"raster_order_groups_supported\": %s,\n",
                raster_order_groups ? "true" : "false");
    std::printf("  \"pull_model_interpolation_supported\": %s,\n",
                pull_model_interpolation ? "true" : "false");
    std::printf("  \"shader_barycentrics_supported\": %s,\n",
                shader_barycentrics ? "true" : "false");
    std::printf("  \"programmable_sample_positions_supported\": %s,\n",
                programmable_sample_positions ? "true" : "false");
    std::printf("  \"sample_counts\": [");
    const NSUInteger counts[] = {1, 2, 4, 8, 16, 32};
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
      if (i)
        std::printf(", ");
      std::printf("{\"count\": %lu, \"supported\": %s}",
                  static_cast<unsigned long>(counts[i]),
                  [device supportsTextureSampleCount:counts[i]] ? "true"
                                                                  : "false");
    }
    std::printf("],\n");
    if (error)
      std::printf("  \"error\": \"%s\",\n", error.localizedDescription.UTF8String);
    const bool exact = compiled && raster_order_groups &&
                       pull_model_interpolation && shader_barycentrics &&
                       programmable_sample_positions;
    std::printf("  \"exact\": %s\n}\n", exact ? "true" : "false");
    return exact ? 0 : 1;
  }
}
