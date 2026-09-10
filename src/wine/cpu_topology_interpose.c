#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define INTERPOSE(replacement, replacee)                                                                               \
    __attribute__((used)) static struct {                                                                              \
        const void* replacement;                                                                                       \
        const void* replacee;                                                                                          \
    } interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {&replacement, &replacee}

static unsigned int cpu_count(void) {
    char* end;
    const char* value = getenv("METALSHARP_CPU_COUNT");
    unsigned long parsed = value ? strtoul(value, &end, 10) : 0;
    if (value && !*end && parsed > 0 && parsed <= 64)
        return (unsigned int)parsed;
    unsigned int count = 0;
    size_t size = sizeof(count);
    return !sysctlbyname("hw.perflevel0.logicalcpu", &count, &size, NULL, 0) && count <= 64 ? count : 0;
}

static long metalsharp_sysconf(int name) {
    unsigned int count = cpu_count();
    if (count && (name == _SC_NPROCESSORS_CONF || name == _SC_NPROCESSORS_ONLN))
        return count;
    return sysconf(name);
}

static int metalsharp_sysctlbyname(const char* name, void* old, size_t* old_size, void* new, size_t new_size) {
    unsigned int count = cpu_count();
    if (count && !new && old_size &&
        (!strcmp(name, "hw.logicalcpu") || !strcmp(name, "hw.logicalcpu_max") || !strcmp(name, "hw.ncpu") ||
         !strcmp(name, "hw.physicalcpu") || !strcmp(name, "hw.physicalcpu_max"))) {
        if (old && *old_size < sizeof(count)) {
            *old_size = sizeof(count);
            errno = ENOMEM;
            return -1;
        }
        if (old)
            *(unsigned int*)old = count;
        *old_size = sizeof(count);
        return 0;
    }
    return sysctlbyname(name, old, old_size, new, new_size);
}

INTERPOSE(metalsharp_sysconf, sysconf);
INTERPOSE(metalsharp_sysctlbyname, sysctlbyname);
