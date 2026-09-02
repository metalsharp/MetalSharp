#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::string read_text(const char *path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}

static void print_error(const char *stage, NSError *error) {
    std::fprintf(stderr, "%s: %s\n", stage,
                 error ? error.localizedDescription.UTF8String : "unknown error");
}

static std::vector<uint32_t> parse_expected(const char *text) {
    std::vector<uint32_t> values;
    std::string remaining = text ? text : "";
    while (!remaining.empty()) {
        const size_t comma = remaining.find(',');
        const std::string token = remaining.substr(0, comma);
        values.push_back(static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 0)));
        if (comma == std::string::npos)
            break;
        remaining.erase(0, comma + 1);
    }
    return values;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: probe-node-workgraph <metal-source> <expected-u32[,u32...]>\n");
        return 2;
    }
    const std::string source = read_text(argv[1]);
    const std::vector<uint32_t> expected = parse_expected(argv[2]);
    if (expected.empty()) {
        std::fprintf(stderr, "expected readback list is empty\n");
        return 2;
    }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::fprintf(stderr, "Metal device unavailable\n");
            return 3;
        }
        NSError *error = nil;
        MTLCompileOptions *compile_options = [[MTLCompileOptions alloc] init];
        id<MTLLibrary> library = [device
            newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                         options:compile_options
                           error:&error];
        if (!library) {
            print_error("Metal source compilation failed", error);
            return 4;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"node_main"];
        if (!function) {
            std::fprintf(stderr, "node_main function missing\n");
            return 5;
        }
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            print_error("node pipeline creation failed", error);
            return 6;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            std::fprintf(stderr, "Metal command queue unavailable\n");
            return 7;
        }
        id<MTLBuffer> output =
            [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        if (!output) {
            std::fprintf(stderr, "Metal output buffer allocation failed\n");
            return 8;
        }
        std::memset(output.contents, 0, output.length);

        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        // The bounded node provider uses the normal compute buffer ABI. Bind
        // one GPU backing allocation to every unused slot so raw and typed
        // record/resource lanes remain deterministic without a CPU scheduler.
        for (NSUInteger index = 0; index < 31; ++index)
            [encoder setBuffer:output offset:0 atIndex:index];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                 threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];

        dispatch_semaphore_t completed = dispatch_semaphore_create(0);
        [command addCompletedHandler:^(id<MTLCommandBuffer>) {
            dispatch_semaphore_signal(completed);
        }];
        [command commit];
        const dispatch_time_t deadline =
            dispatch_time(DISPATCH_TIME_NOW, 15LL * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(completed, deadline) != 0) {
            std::fprintf(stderr, "node dispatch timed out\n");
            return 9;
        }
        const uint32_t *values = reinterpret_cast<const uint32_t *>(output.contents);
        bool values_match = command.status == MTLCommandBufferStatusCompleted;
        for (size_t index = 0; index < expected.size(); ++index)
            values_match = values_match && values[index] == expected[index];
        std::printf("{\n  \"schema\": \"metalsharp.d3d12-metal.node-workgraph-probe.v1\",\n"
                    "  \"pass\": %s,\n  \"command_status\": \"%s\",\n  \"values\": [",
                    values_match ? "true" : "false",
                    command.status == MTLCommandBufferStatusCompleted ? "completed" : "failed");
        for (size_t index = 0; index < expected.size(); ++index)
            std::printf("%s%u", index ? "," : "", values[index]);
        std::printf("],\n  \"expected\": [");
        for (size_t index = 0; index < expected.size(); ++index)
            std::printf("%s%u", index ? "," : "", expected[index]);
        std::printf("]\n}\n");
        return values_match ? 0 : 10;
    }
}
