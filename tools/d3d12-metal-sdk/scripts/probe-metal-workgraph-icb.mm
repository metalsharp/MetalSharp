#include <cstdio>
#include <cstring>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #x,                                                   \
                    error ? error.localizedDescription.UTF8String : "no error");                                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
struct Record {
    uint32_t x, y, z, index, value;
};
int main(int argc, char** argv) {
    @autoreleasepool {
        @try {
            NSError* error = nil;
            if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "empty"))) {
                fprintf(stderr, "usage: probe-metal-workgraph-icb shader.metal [empty]\n");
                return 2;
            }
            id<MTLDevice> d = MTLCreateSystemDefaultDevice();
            CHECK(d);
            fprintf(stderr, "device=%s\n", d.name.UTF8String);
            NSString* source = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]
                                                         encoding:NSUTF8StringEncoding
                                                            error:&error];
            CHECK(source);
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.languageVersion = MTLLanguageVersion4_0;
            id<MTLLibrary> library = [d newLibraryWithSource:source options:options error:&error];
            CHECK(library);
            id<MTL4Compiler> compiler = [d newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
            CHECK(compiler);
            auto pipeline = [&](NSString* name, bool indirect) -> id<MTLComputePipelineState> {
                MTL4LibraryFunctionDescriptor* function = [MTL4LibraryFunctionDescriptor new];
                function.library = library;
                function.name = name;
                MTL4ComputePipelineDescriptor* desc = [MTL4ComputePipelineDescriptor new];
                desc.computeFunctionDescriptor = function;
                desc.supportIndirectCommandBuffers = indirect ? MTL4IndirectCommandBufferSupportStateEnabled
                                                              : MTL4IndirectCommandBufferSupportStateDisabled;
                return [compiler newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error];
            };
            id<MTLComputePipelineState> consumer = pipeline(@"consume", true);
            CHECK(consumer);
            id<MTLComputePipelineState> builder = pipeline(@"build", false);
            CHECK(builder);
            CHECK(consumer.supportIndirectCommandBuffers);
            MTLIndirectCommandBufferDescriptor* descriptor = [MTLIndirectCommandBufferDescriptor new];
            descriptor.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
            descriptor.inheritBuffers = NO;
            descriptor.inheritPipelineState = NO;
            descriptor.maxKernelBufferBindCount = 2;
            id<MTLIndirectCommandBuffer> icb = [d newIndirectCommandBufferWithDescriptor:descriptor
                                                                         maxCommandCount:8
                                                                                 options:MTLResourceStorageModePrivate];
            CHECK(icb);
            [icb resetWithRange:NSMakeRange(0, 8)];
            id<MTLBuffer> records = [d newBufferWithLength:6 * sizeof(Record) options:MTLResourceStorageModeShared];
            CHECK(records);
            memset(records.contents, 0, records.length);
            id<MTLBuffer> output = [d newBufferWithLength:32 options:MTLResourceStorageModeShared];
            CHECK(output);
            memset(output.contents, 0, 32);
            id<MTLBuffer> range = [d newBufferWithLength:8 options:MTLResourceStorageModeShared];
            CHECK(range);
            memset(range.contents, 0, 8);
            id<MTLFunction> builderFunction = [library newFunctionWithName:@"build"];
            CHECK(builderFunction);
            id<MTLArgumentEncoder> arguments = [builderFunction newArgumentEncoderWithBufferIndex:0];
            CHECK(arguments);
            id<MTLBuffer> argumentBuffer = [d newBufferWithLength:arguments.encodedLength
                                                          options:MTLResourceStorageModeShared];
            CHECK(argumentBuffer);
            [arguments setArgumentBuffer:argumentBuffer offset:0];
            [arguments setIndirectCommandBuffer:icb atIndex:0];
            [arguments setComputePipelineState:consumer atIndex:1];
            MTL4ArgumentTableDescriptor* tableDesc = [MTL4ArgumentTableDescriptor new];
            tableDesc.maxBufferBindCount = 4;
            id<MTL4ArgumentTable> table = [d newArgumentTableWithDescriptor:tableDesc error:&error];
            CHECK(table);
            [table setAddress:argumentBuffer.gpuAddress atIndex:0];
            [table setAddress:records.gpuAddress atIndex:1];
            [table setAddress:output.gpuAddress atIndex:2];
            [table setAddress:range.gpuAddress atIndex:3];
            id<MTLResidencySet> residency = [d newResidencySetWithDescriptor:[MTLResidencySetDescriptor new]
                                                                       error:&error];
            CHECK(residency);
            [residency addAllocation:icb];
            [residency addAllocation:consumer];
            [residency addAllocation:builder];
            [residency addAllocation:argumentBuffer];
            [residency addAllocation:records];
            [residency addAllocation:output];
            [residency addAllocation:range];
            [residency commit];
            id<MTL4CommandQueue> queue = [d newMTL4CommandQueue];
            CHECK(queue);
            [queue addResidencySet:residency];
            id<MTL4CommandAllocator> allocator = [d newCommandAllocator];
            CHECK(allocator);
            id<MTL4CommandBuffer> buffer = [d newCommandBuffer];
            CHECK(buffer);
            [buffer beginCommandBufferWithAllocator:allocator];
            id<MTL4ComputeCommandEncoder> encoder = [buffer computeCommandEncoder];
            CHECK(encoder);
            [encoder setComputePipelineState:builder];
            [encoder setArgumentTable:table];
            [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [encoder endEncoding];
            encoder = [buffer computeCommandEncoder];
            CHECK(encoder);
            [encoder barrierAfterQueueStages:MTLStageAll
                                beforeStages:MTLStageAll
                           visibilityOptions:MTL4VisibilityOptionDevice];
            [encoder executeCommandsInBuffer:icb indirectBuffer:range.gpuAddress];
            [encoder endEncoding];
            [buffer endCommandBuffer];
            const bool empty = argc > 2 && !strcmp(argv[2], "empty");
            Record payload[6] = {{2, 1, 1, 0, 1},    {1, 2, 1, 1, 10},     {1, 1, 2, 2, 100},
                                 {0, 1, 1, 3, 1000}, {1, 1, 1, 4, 999999}, {empty ? 0u : 4u, 0, 0, 0, 0}};
            memcpy(records.contents, payload, sizeof(payload)); // only after command encoding
            id<MTLSharedEvent> done = [d newSharedEvent];
            CHECK(done);
            dispatch_semaphore_t feedbackReady = dispatch_semaphore_create(0);
            __block NSError* feedbackError = nil;
            MTL4CommitOptions* commitOptions = [MTL4CommitOptions new];
            [commitOptions addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
              feedbackError = feedback.error;
              dispatch_semaphore_signal(feedbackReady);
            }];
            id<MTL4CommandBuffer> buffers[] = {buffer};
            [queue commit:buffers count:1 options:commitOptions];
            [queue signalEvent:done value:1];
            CHECK([done waitUntilSignaledValue:1 timeoutMS:10000]);
            CHECK(dispatch_semaphore_wait(feedbackReady, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
            error = feedbackError;
            CHECK(!error);
            uint32_t expected[8] = {10, 62, 602, 0, 0, 0, 0, 0};
            if (empty)
                memset(expected, 0, sizeof(expected));
            auto actual = (uint32_t*)output.contents;
            auto actualRange = (uint32_t*)range.contents;
            const bool exact = !memcmp(output.contents, expected, sizeof(expected)) && actualRange[0] == 1u &&
                               actualRange[1] == (empty ? 0u : 4u);
            printf(
                "{\"schema\":\"metalsharp.workgraph-native-icb.v1\",\"pass\":%s,\"metal4\":true,\"gpu_encoded_"
                "commands\":true,\"gpu_completion_ok\":true,\"d3d12_integrated\":false,\"range\":[%u,%u],\"values\":[",
                exact ? "true" : "false", actualRange[0], actualRange[1]);
            for (unsigned i = 0; i < 8; ++i)
                printf("%s%u", i ? "," : "", actual[i]);
            puts("]}");
            return exact ? 0 : 1;
        } @catch (NSException* e) {
            fprintf(stderr, "exception: %s\n", e.reason.UTF8String);
            return 1;
        }
    }
}
