// Compile-only Winemetal wire-layout regression; build with -m32 and -m64.
#include "winemetal_thunks.h"
#include <cstddef>
static_assert(sizeof(unixcall_mtldevice_new_compute_icb) == 24);
static_assert(offsetof(unixcall_mtldevice_new_compute_icb, ret) == 16);
static_assert(sizeof(wmtcmd_compute_execute_indirect_commands) == 40);
static_assert(offsetof(wmtcmd_compute_execute_indirect_commands, indirect_commands) == 16);
static_assert(offsetof(wmtcmd_compute_execute_indirect_commands, execution_range_offset) == 32);
