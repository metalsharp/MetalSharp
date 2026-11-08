#ifndef METALSHARP_BACKEND_D3DMETAL_BASIC_H
#define METALSHARP_BACKEND_D3DMETAL_BASIC_H
#include <stddef.h>
/* runtime/d3dmetal-gptk3 below macOS 27 where it is installed, else runtime/d3dmetal-gptk4-beta3 where it is
 * installed, else runtime/d3dmetal-gptk4-beta2 (relative to home) */
const char* ms_d3dmetal_runtime(const char* home);
char* ms_d3dmetal_json(const char*, const char*, const unsigned char*, size_t, int*);
#endif
