#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <string>

static NSString *const kShaderSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

kernel void cycle_counter_probe(device uint2 *out [[buffer(0)]]) {
  uint2 value = clock();
  out[0] = value;
}
)MSL";

static std::string json_escape(const char *value) {
    std::string out;
    if (!value)
        return out;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(value);
         *cursor; ++cursor) {
        switch (*cursor) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (*cursor < 0x20) {
                char escaped[7] = {};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                out += escaped;
            } else {
                out.push_back(static_cast<char>(*cursor));
            }
            break;
        }
    }
    return out;
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::puts("{\"schema\":\"metalsharp.metal-cycle-counter.v1\",\"pass\":false,\"error\":\"no Metal device\"}");
            return 1;
        }

        NSError *error = nil;
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion4_0;
        id<MTLLibrary> library =
            [device newLibraryWithSource:kShaderSource options:options error:&error];
        const char *description = error.localizedDescription.UTF8String;
        const std::string error_text = json_escape(description);
        const bool missing_native_clock =
            !library && (error_text.find("clock") != std::string::npos ||
                         error_text.find("undeclared") != std::string::npos);

        std::printf("{\n");
        std::printf("  \"schema\": \"metalsharp.metal-cycle-counter.v1\",\n");
        std::printf("  \"pass\": %s,\n", missing_native_clock ? "true" : "false");
        std::printf("  \"device\": \"%s\",\n", device.name.UTF8String);
        std::printf("  \"metal_language_version\": \"4.0\",\n");
        std::printf("  \"library_compiled\": %s,\n", library ? "true" : "false");
        std::printf("  \"native_clock_rejected\": %s,\n",
                    missing_native_clock ? "true" : "false");
        std::printf("  \"error\": \"%s\"\n", error_text.c_str());
        std::printf("}\n");
        return missing_native_clock ? 0 : 1;
    }
}
