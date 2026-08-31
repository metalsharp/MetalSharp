#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

static const char *kTestKernel = R"MSL(

kernel void metalsharp_f64_emulation_test(device ulong* out [[buffer(0)]]) {
  const ulong sqrt_inputs[8] = {
    0x3ff0000000000000ul, 0x4000000000000000ul,
    0x4008000000000000ul, 0x4010000000000000ul,
    0x01a56e1fc2f8f359ul, 0x7feffffffffffffful,
    0x7ff0000000000000ul, 0xbff0000000000000ul
  };
  for (uint i = 0; i < 8; ++i)
    out[i] = m12_f64_sqrt(sqrt_inputs[i]);

  const ulong round_inputs[8] = {
    0x400c000000000000ul, 0x400a000000000000ul,
    0xc00c000000000000ul, 0xc00a000000000000ul,
    0x3fe0000000000000ul, 0xbfe0000000000000ul,
    0x3ff0000000000000ul, 0xbff0000000000000ul
  };
  for (uint i = 0; i < 8; ++i) {
    out[8 + i * 4] = m12_f64_trunc(round_inputs[i]);
    out[9 + i * 4] = m12_f64_floor(round_inputs[i]);
    out[10 + i * 4] = m12_f64_ceil(round_inputs[i]);
    out[11 + i * 4] = m12_f64_round_ne(round_inputs[i]);
  }

  const ulong frac_inputs[7] = {
    0x400a000000000000ul, 0xc00a000000000000ul,
    0x3fe8000000000000ul, 0xbfe8000000000000ul,
    0x7ff0000000000000ul, 0xfff0000000000000ul,
    0x7ff8000000000000ul
  };
  for (uint i = 0; i < 7; ++i)
    out[40 + i] = m12_f64_frac(frac_inputs[i]);

  const ulong rsqrt_inputs[4] = {
    0x4010000000000000ul, 0x4000000000000000ul,
    0x3fd0000000000000ul, 0x7ff0000000000000ul
  };
  for (uint i = 0; i < 4; ++i)
    out[47 + i] = m12_f64_div(
        0x3ff0000000000000ul, m12_f64_sqrt(rsqrt_inputs[i]));
}
)MSL";

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <generated-msl>\n", argv[0]);
        return 2;
    }
    @autoreleasepool {
        NSError *error = nil;
        NSString *path = [NSString stringWithUTF8String:argv[1]];
        NSString *source = [NSString stringWithContentsOfFile:path
                                                       encoding:NSUTF8StringEncoding
                                                          error:&error];
        if (!source) {
            std::fprintf(stderr, "read failed: %s\n",
                         error.localizedDescription.UTF8String);
            return 2;
        }
        source = [source stringByAppendingString:
                            [NSString stringWithUTF8String:kTestKernel]];
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::puts("{\"schema\":\"metalsharp.d3d12.f64-emulation.v1\",\"pass\":false,\"error\":\"no Metal device\"}");
            return 1;
        }
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source
                                                       options:options
                                                         error:&error];
        if (!library) {
            std::fprintf(stderr, "Metal compile failed: %s\n",
                         error.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLFunction> function =
            [library newFunctionWithName:@"metalsharp_f64_emulation_test"];
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            std::fprintf(stderr, "pipeline creation failed: %s\n",
                         error.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer =
            [device newBufferWithLength:51 * sizeof(uint64_t)
                                options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:buffer offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
           threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];

        static const uint64_t expected[51] = {
            0x3ff0000000000000ull, 0x3ff6a09e667f3bcdull,
            0x3ffbb67ae8584caaull, 0x4000000000000000ull,
            0x20ca2fe76a3f9475ull, 0x5fefffffffffffffull,
            0x7ff0000000000000ull, 0x7ff8000000000000ull,
            0x4008000000000000ull, 0x4008000000000000ull,
            0x4010000000000000ull, 0x4010000000000000ull,
            0x4008000000000000ull, 0x4008000000000000ull,
            0x4010000000000000ull, 0x4008000000000000ull,
            0xc008000000000000ull, 0xc010000000000000ull,
            0xc008000000000000ull, 0xc010000000000000ull,
            0xc008000000000000ull, 0xc010000000000000ull,
            0xc008000000000000ull, 0xc008000000000000ull,
            0x0000000000000000ull, 0x0000000000000000ull,
            0x3ff0000000000000ull, 0x0000000000000000ull,
            0x8000000000000000ull, 0xbff0000000000000ull,
            0x8000000000000000ull, 0x8000000000000000ull,
            0x3ff0000000000000ull, 0x3ff0000000000000ull,
            0x3ff0000000000000ull, 0x3ff0000000000000ull,
            0xbff0000000000000ull, 0xbff0000000000000ull,
            0xbff0000000000000ull, 0xbff0000000000000ull,
            0x3fd0000000000000ull, 0x3fe8000000000000ull,
            0x3fe8000000000000ull, 0x3fd0000000000000ull,
            0x7ff8000000000000ull, 0x7ff8000000000000ull,
            0x7ff8000000000000ull,
            0x3fe0000000000000ull, 0x3fe6a09e667f3bccull,
            0x4000000000000000ull, 0x0000000000000000ull,
        };
        const uint64_t *actual =
            static_cast<const uint64_t *>(buffer.contents);
        bool pass = command.status == MTLCommandBufferStatusCompleted &&
                    std::memcmp(actual, expected, sizeof(expected)) == 0;
        if (!pass)
            for (size_t i = 0; i < 51; ++i)
                if (actual[i] != expected[i])
                    std::fprintf(stderr, "mismatch[%zu]=%016llx expected=%016llx\\n",
                                 i, static_cast<unsigned long long>(actual[i]),
                                 static_cast<unsigned long long>(expected[i]));
        std::printf("{\n  \"schema\": \"metalsharp.d3d12.f64-emulation.v1\",\n");
        std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
        std::printf("  \"command_completed\": %s,\n",
                    command.status == MTLCommandBufferStatusCompleted ? "true" : "false");
        std::printf("  \"case_count\": 51,\n  \"exact_bits\": %s\n}\n",
                    pass ? "true" : "false");
        return pass ? 0 : 1;
    }
}
