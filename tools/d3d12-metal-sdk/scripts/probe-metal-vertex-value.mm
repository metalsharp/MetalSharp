#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

static NSString *const kShaderSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float4 color;
};

vertex VertexOut vertex_value_probe_vs(uint vid [[vertex_id]]) {
  VertexOut out;
  out.position = vid == 0
      ? float4(-1.0f, -1.0f, 0.0f, 1.0f)
      : (vid == 1 ? float4(3.0f, -1.0f, 0.0f, 1.0f)
                 : float4(-1.0f, 3.0f, 0.0f, 1.0f));
  out.color = vid == 0
      ? float4(1.0f, 0.0f, 0.0f, 1.0f)
      : (vid == 1 ? float4(0.0f, 1.0f, 0.0f, 1.0f)
                 : float4(0.0f, 0.0f, 1.0f, 1.0f));
  return out;
}

struct VertexValueProbeIn {
  float4 position [[position]];
  vertex_value<float4> color;
};

fragment float4 vertex_value_probe_ps(VertexValueProbeIn in [[stage_in]]) {
  return in.color.get(vertex_index::first);
}

fragment float4 vertex_value_probe_constant(VertexValueProbeIn in [[stage_in]]) {
  return float4(1.0f, 0.0f, 0.0f, 1.0f);
}
)MSL";

struct RenderResult {
    bool pso_created = false;
    bool completed = false;
    uint8_t first[4] = {};
    uint8_t center[4] = {};
    const char *error = nullptr;
};

static id<MTLRenderPipelineState>
make_pipeline(id<MTLDevice> device, id<MTLLibrary> library,
              NSString *fragment_name, NSError **error) {
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_value_probe_vs"];
    descriptor.fragmentFunction = [library newFunctionWithName:fragment_name];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

static RenderResult render(id<MTLDevice> device, id<MTLRenderPipelineState> pipeline) {
    RenderResult result;
    if (!pipeline) {
        result.error = "pipeline missing";
        return result;
    }
    result.pso_created = true;

    MTLTextureDescriptor *texture_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                            width:4
                                                           height:4
                                                        mipmapped:NO];
    texture_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:texture_desc];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!texture || !queue) {
        result.error = "render resources missing";
        return result;
    }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    result.completed = command.status == MTLCommandBufferStatusCompleted;
    if (!result.completed) {
        result.error = command.error.localizedDescription.UTF8String;
        return result;
    }

    uint8_t pixels[4 * 4 * 4] = {};
    [texture getBytes:pixels
          bytesPerRow:4 * 4
        fromRegion:MTLRegionMake2D(0, 0, 4, 4)
       mipmapLevel:0];
    std::memcpy(result.first, pixels, sizeof(result.first));
    std::memcpy(result.center, pixels + (2 * 4 + 2) * 4, sizeof(result.center));
    return result;
}

static void print_rgba(const char *name, const uint8_t rgba[4]) {
    std::printf("  \"%s\": [%u, %u, %u, %u],\n", name, rgba[0], rgba[1],
                rgba[2], rgba[3]);
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::puts("{\"schema\":\"metalsharp.metal-vertex-value.v1\",\"pass\":false,\"error\":\"no Metal device\"}");
            return 1;
        }

        bool supports_apple10 = false;
        if (@available(macOS 26.0, *))
            supports_apple10 = [device supportsFamily:MTLGPUFamilyApple10];
        NSError *compile_error = nil;
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion4_0;
        id<MTLLibrary> library =
            [device newLibraryWithSource:kShaderSource options:options error:&compile_error];
        if (!library) {
            std::printf("{\n  \"schema\": \"metalsharp.metal-vertex-value.v1\",\n");
            std::printf("  \"pass\": false,\n  \"device\": \"%s\",\n",
                        device.name.UTF8String);
            std::printf("  \"supports_apple10\": %s,\n",
                        supports_apple10 ? "true" : "false");
            std::printf("  \"library_compiled\": false,\n  \"error\": \"%s\"\n}\n",
                        compile_error.localizedDescription.UTF8String);
            return 1;
        }

        NSError *constant_error = nil;
        NSError *vertex_error = nil;
        id<MTLRenderPipelineState> constant_pipeline =
            make_pipeline(device, library, @"vertex_value_probe_constant",
                          &constant_error);
        id<MTLRenderPipelineState> vertex_pipeline =
            make_pipeline(device, library, @"vertex_value_probe_ps",
                          &vertex_error);

        RenderResult constant = render(device, constant_pipeline);
        RenderResult vertex = render(device, vertex_pipeline);
        static const uint8_t kRed[4] = {255, 0, 0, 255};
        static const uint8_t kZero[4] = {0, 0, 0, 0};
        const bool constant_exact =
            constant.completed && std::memcmp(constant.first, kRed, 4) == 0 &&
            std::memcmp(constant.center, kRed, 4) == 0;
        const bool vertex_zero = vertex.completed &&
            std::memcmp(vertex.first, kZero, 4) == 0 &&
            std::memcmp(vertex.center, kZero, 4) == 0;
        // On Apple 9 this is a negative capability proof: the Metal 4 source
        // and PSO can be accepted while the pre-raster per-vertex feature is
        // unavailable.  It must never be used to promote DXIL opcode 137.
        const bool negative_boundary = !supports_apple10 && constant_exact && vertex_zero;

        std::printf("{\n");
        std::printf("  \"schema\": \"metalsharp.metal-vertex-value.v1\",\n");
        std::printf("  \"pass\": %s,\n", negative_boundary ? "true" : "false");
        std::printf("  \"device\": \"%s\",\n", device.name.UTF8String);
        std::printf("  \"metal_language_version\": \"4.0\",\n");
        std::printf("  \"supports_apple10\": %s,\n", supports_apple10 ? "true" : "false");
        std::printf("  \"library_compiled\": true,\n");
        std::printf("  \"constant_pso_created\": %s,\n", constant.pso_created ? "true" : "false");
        std::printf("  \"vertex_value_pso_created\": %s,\n", vertex.pso_created ? "true" : "false");
        std::printf("  \"constant_command_completed\": %s,\n", constant.completed ? "true" : "false");
        std::printf("  \"vertex_value_command_completed\": %s,\n", vertex.completed ? "true" : "false");
        print_rgba("constant_first_rgba", constant.first);
        print_rgba("constant_center_rgba", constant.center);
        print_rgba("vertex_value_first_rgba", vertex.first);
        print_rgba("vertex_value_center_rgba", vertex.center);
        std::printf("  \"constant_exact\": %s,\n", constant_exact ? "true" : "false");
        std::printf("  \"vertex_value_zero_readback\": %s,\n", vertex_zero ? "true" : "false");
        std::printf("  \"negative_boundary_verified\": %s\n", negative_boundary ? "true" : "false");
        std::printf("}\n");
        return negative_boundary ? 0 : 1;
    }
}
