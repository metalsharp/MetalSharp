/*
 * MetalSharp Linux user-space substrate (Darwin host side).
 *
 * The protected EAC launcher has a Linux-specific Wine path.  It discovers
 * libc by parsing an ELF image named in /proc/<pid>/maps and then calls the
 * resolved dlopen/dlsym entry points.  macOS dyld cannot load that ELF image,
 * so this library provides the missing ABI boundary inside the existing
 * MetalSharp Wine 11.5 host process.  It is intentionally a transparent
 * compatibility layer: it does not change Wine identity, patch an EAC
 * payload, or suppress a vendor check.
 *
 * The complete boundary maps the real MetalSharp-generated ELF symbol image,
 * exposes it through the native /proc view, relocates the protected ELF, and
 * bridges its Linux libc/pthread/TSD calls into the existing Wine process.
 * Keeping the boundary here makes the real protected-module path testable
 * without changing the vendor image.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_info.h>
#include <netdb.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <wchar.h>

#define MS_PAGE_SIZE             0x1000u
#define MS_STUB_SIZE             16u
#define MS_MAX_PATH              4096u
#define MS_PREFERRED_ELF_BASE    ((void*)(uintptr_t)0x700100000000ULL)
#define MS_WINE115_KERNEL32_BASE ((mach_vm_address_t)0x6fffffa00000ULL)

typedef struct {
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} MsElfHeader;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} MsElfProgramHeader;

typedef struct {
    uint32_t name;
    unsigned char info;
    unsigned char other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} MsElfSymbol;

typedef struct {
    int64_t tag;
    uint64_t value;
} MsElfDynamic;

typedef struct {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
} MsElfRela;

typedef struct {
    void* base;
    void* mapping_start;
    size_t size;
    uint64_t minimum_vaddr;
    const MsElfProgramHeader* program_headers;
    uint16_t program_count;
    MsElfDynamic* dynamic;
    size_t dynamic_count;
    const char* strings;
    size_t string_size;
    MsElfSymbol* symbols;
    size_t symbol_count;
    int module_fd;
    size_t tls_size;
    size_t tls_align;
    uint64_t init;
    uint64_t init_array;
    size_t init_array_count;
    size_t init_array_called;
    size_t rela_count;
    size_t plt_count;
    bool rela_relocated;
    bool plt_relocated;
    bool protections_applied;
    bool loaded;
    bool initialized;
} MsLinuxLoadedModule;

enum {
    MS_PT_DYNAMIC = 2,
};

typedef int32_t (*MsNtdllGetUnixFileNameFn)(const uint16_t* dos, char** unix_name, unsigned int disposition);

static void* g_elf_mapping;
static size_t g_elf_mapping_size;
static char g_elf_path[MS_MAX_PATH];
static char g_log_path[MS_MAX_PATH];
static char g_maps_path[MS_MAX_PATH];
static __thread int g_in_open_hook;
static MsNtdllGetUnixFileNameFn g_original_ntdll_get_unix_file_name;
static void* g_ntdll_unix_name_target;
static uintptr_t g_ntdll_image_base;
static volatile int g_ntdll_patch_done;
typedef char* (*MsKernel32GetUnixFileNameFn)(const uint16_t* dos) __attribute__((ms_abi));
static MsKernel32GetUnixFileNameFn g_original_kernel32_get_unix_file_name;
static volatile int g_kernel32_patch_done;
static volatile int g_kernel32_scan_reported;
typedef void* (*MsGetProcAddressFn)(void* module, const char* name) __attribute__((ms_abi));
static MsGetProcAddressFn g_original_get_proc_address;
static volatile int g_get_proc_address_patch_done;
static int (*g_real_sigaction)(int, const struct sigaction*, struct sigaction*);
static struct sigaction g_wine_sigsys_action;
static volatile sig_atomic_t g_wine_sigsys_action_valid;
static struct sigaction g_wine_sigsegv_action;
static volatile sig_atomic_t g_wine_sigsegv_action_valid;
static volatile sig_atomic_t g_memfd_sequence;
static MsLinuxLoadedModule g_linux_module;
static char g_linux_dlerror[256];
static __thread void* g_linux_tls_block;
static volatile uint64_t g_eac_module_load_attempts;
static volatile uint64_t g_eac_module_load_successes;
static volatile uint64_t g_eac_export_a_successes;
static volatile uint32_t g_eac_export_mask;
typedef uintptr_t (*MsLinuxUnaryExportFn)(void* argument);
static MsLinuxUnaryExportFn g_linux_export_a;
static MsLinuxUnaryExportFn g_linux_export_b;
static MsLinuxUnaryExportFn g_linux_export_c;
static MsLinuxUnaryExportFn g_linux_export_e;
static void (*g_linux_export_d)(void);
typedef void (*MsThreadSetTsdBaseFn)(void* base);
typedef void* (*MsNtCurrentTebFn)(void);
static MsThreadSetTsdBaseFn g_thread_set_tsd_base;
static MsNtCurrentTebFn g_nt_current_teb;
/* Keep the diagnostic target in ordinary data: Darwin's compiler TLS access
 * itself can consult the host pthread TSD base while Wine has GS pointed at
 * its TEB.  The actual bridge state below is per Wine stack arena. */
static void* g_last_guest_teb;
static MsThreadSetTsdBaseFn g_real_thread_set_tsd_base;
static volatile uint64_t g_tsd_callback_count;
static volatile uint64_t g_tsd_pseudo_count;
static volatile uint64_t g_tsd_mapped_count;
static volatile uintptr_t g_tsd_last_requested;
static volatile uintptr_t g_tsd_last_effective;
typedef struct {
    uintptr_t requested;
    uintptr_t effective;
    uint32_t thread;
} MsTsdCallbackRecord;
static MsTsdCallbackRecord g_tsd_callback_records[64];
typedef struct {
    uintptr_t thread;
    uintptr_t guest_teb;
    uintptr_t host_tsd;
    uint32_t host_depth;
    bool used;
} MsWineThreadBridgeState;
static MsWineThreadBridgeState g_wine_thread_bridge_states[64];
static volatile int g_wine_thread_bridge_lock;
typedef int (*MsHostPthreadSetspecificFn)(pthread_key_t key, const void* value);
typedef const void* (*MsHostPthreadGetspecificFn)(pthread_key_t key);
typedef pthread_t (*MsHostPthreadSelfFn)(void);
static MsHostPthreadSetspecificFn g_host_pthread_setspecific;
static MsHostPthreadGetspecificFn g_host_pthread_getspecific;
static MsHostPthreadSelfFn g_host_pthread_self;
static void* g_libsystem_pthread;
static volatile int g_host_tsd_interpose_ready;
static uintptr_t g_native_host_tsd_base;
static pthread_t g_native_host_pthread;
static void* g_wine_pe_tls_array;
static void* g_wine_pe_tls_block;
static pthread_t g_wine_tls_monitor_thread;
static volatile int g_wine_tls_monitor_started;
static volatile uint64_t g_wine_tls_monitor_iterations;
static volatile uint64_t g_wine_tls_monitor_tebs;
static volatile uint64_t g_wine_tls_monitor_repairs;
static volatile int g_ntdll_tsd_stub_patched;
static volatile int g_ntdll_pthread_getspecific_patched;
static void* g_libsystem_kernel;

static void ms_log(const char* format, ...);
static bool wine_read_word(void* address, uintptr_t* value);
static void* wine_guest_teb_for_current_thread(void);
static void ensure_wine_pe_tls(void);
static void metalsharp_sigsegv_handler(int signal, siginfo_t* siginfo, void* context);
static const void* metalsharp_host_pthread_getspecific(pthread_key_t key);
static void patch_wine115_pthread_getspecific_stub(void);

static uintptr_t read_guest_gs_tls_pointer(void) {
#if defined(__x86_64__)
    uintptr_t value = 0;
    __asm__ volatile("movq %%gs:0x58, %0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

/* Called only from the private TSD callback, where Mach APIs are not safe.
 * The exact Wine 11.5 ntdll list is an ordinary in-process doubly-linked
 * list; match its per-TEB native TSD field directly to recover the public TEB
 * for a newly-created Wine thread. */
static void* wine_public_teb_for_tsd_base(uintptr_t tsd_base) {
    if (g_ntdll_image_base == 0 || tsd_base == 0) {
        return NULL;
    }
    uintptr_t list_head = g_ntdll_image_base + 0x98530u;
    uintptr_t next_entry = *(volatile uintptr_t*)(uintptr_t)list_head;
    for (size_t pass = 0; pass < 256 && next_entry != list_head; pass++) {
        if (next_entry < 0x3b0u || ((next_entry - 0x3b0u) & 0xffffu) != 0) {
            return NULL;
        }
        uintptr_t teb = next_entry - 0x3b0u;
        if (*(volatile uintptr_t*)(teb + 0x320u) == tsd_base) {
            return (void*)(uintptr_t)teb;
        }
        next_entry = *(volatile uintptr_t*)(uintptr_t)next_entry;
    }
    return NULL;
}

static void* wine_public_teb_for_stack_pointer(uintptr_t stack) {
    if (g_ntdll_image_base == 0 || stack == 0) {
        return NULL;
    }
    uintptr_t list_head = g_ntdll_image_base + 0x98530u;
    uintptr_t next_entry = *(volatile uintptr_t*)(uintptr_t)list_head;
    for (size_t pass = 0; pass < 256 && next_entry != list_head; pass++) {
        if (next_entry < 0x3b0u || ((next_entry - 0x3b0u) & 0xffffu) != 0) {
            return NULL;
        }
        uintptr_t teb = next_entry - 0x3b0u;
        uintptr_t stack_base = 0;
        uintptr_t stack_limit = 0;
        if (wine_read_word((void*)(teb + 0x8u), &stack_base) && wine_read_word((void*)(teb + 0x10u), &stack_limit) &&
            stack_limit <= stack && stack < stack_base) {
            return (void*)(uintptr_t)teb;
        }
        next_entry = *(volatile uintptr_t*)(uintptr_t)next_entry;
    }
    return NULL;
}

static uintptr_t wine_native_tsd_for_thread(uint32_t thread) {
    uint64_t count = g_tsd_callback_count;
    if (count > sizeof(g_tsd_callback_records) / sizeof(g_tsd_callback_records[0])) {
        count = sizeof(g_tsd_callback_records) / sizeof(g_tsd_callback_records[0]);
    }
    while (count != 0) {
        count--;
        MsTsdCallbackRecord* record = &g_tsd_callback_records[count];
        if (record->thread == thread && record->requested != 0 && (record->requested & 0xffffu) != 0) {
            return record->requested;
        }
    }
    return 0;
}

static uintptr_t wine_current_thread_token(void) {
    uintptr_t stack = 0;
#if defined(__x86_64__)
    __asm__ volatile("movq %%rsp, %0" : "=r"(stack));
#endif
    return stack & ~(uintptr_t)0xffffu;
}

static bool wine_thread_bridge_lookup(uintptr_t thread, uintptr_t* guest_teb, uintptr_t* host_tsd,
                                      uint32_t* host_depth) {
    bool found = false;
    while (__sync_lock_test_and_set(&g_wine_thread_bridge_lock, 1) != 0) {
        __builtin_ia32_pause();
    }
    for (size_t index = 0; index < sizeof(g_wine_thread_bridge_states) / sizeof(g_wine_thread_bridge_states[0]);
         index++) {
        MsWineThreadBridgeState* state = &g_wine_thread_bridge_states[index];
        if (!state->used || state->thread != thread) {
            continue;
        }
        if (guest_teb != NULL) {
            *guest_teb = state->guest_teb;
        }
        if (host_tsd != NULL) {
            *host_tsd = state->host_tsd;
        }
        if (host_depth != NULL) {
            *host_depth = state->host_depth;
        }
        found = true;
        break;
    }
    __sync_lock_release(&g_wine_thread_bridge_lock);
    return found;
}

static MsWineThreadBridgeState* wine_thread_bridge_state(uintptr_t thread) {
    MsWineThreadBridgeState* result = NULL;
    while (__sync_lock_test_and_set(&g_wine_thread_bridge_lock, 1) != 0) {
        __builtin_ia32_pause();
    }
    MsWineThreadBridgeState* free_state = NULL;
    for (size_t index = 0; index < sizeof(g_wine_thread_bridge_states) / sizeof(g_wine_thread_bridge_states[0]);
         index++) {
        MsWineThreadBridgeState* state = &g_wine_thread_bridge_states[index];
        if (state->used && state->thread == thread) {
            result = state;
            break;
        }
        if (!state->used && free_state == NULL) {
            free_state = state;
        }
    }
    if (result == NULL && free_state != NULL) {
        free_state->used = true;
        free_state->thread = thread;
        free_state->guest_teb = 0;
        free_state->host_tsd = 0;
        free_state->host_depth = 0;
        result = free_state;
    }
    __sync_lock_release(&g_wine_thread_bridge_lock);
    return result;
}

/* Capture the real Darwin pthread TSD base before Wine's ntdll switches GS
 * to the Windows TEB.  On Darwin x86-64 libsystem_pthread keeps the native
 * TSD object at pthread_self()+0xe0; Mach's thread_handle is not equivalent
 * on the Rosetta Wine thread. */
__attribute__((constructor(101))) static void capture_native_host_tsd_base(void) {
    pthread_t self = pthread_self();
    if (self != (pthread_t)0) {
        g_native_host_pthread = self;
        g_native_host_tsd_base = (uintptr_t)self + 0xe0u;
    }
}

#define MS_LINUX_MEMFD_CREATE 319
#define MS_MFD_CLOEXEC        0x0001u
#define MS_LINUX_READ         0
#define MS_LINUX_WRITE        1
#define MS_LINUX_OPEN         2
#define MS_LINUX_CLOSE        3
#define MS_LINUX_LSEEK        8
#define MS_LINUX_MMAP         9
#define MS_LINUX_MPROTECT     10
#define MS_LINUX_MUNMAP       11
#define MS_LINUX_GETPID       39
#define MS_LINUX_GETPPID      110
#define MS_LINUX_FSTAT        5
#define MS_LINUX_FTRUNCATE    77
#define MS_LINUX_GETRANDOM    318

enum {
    MS_DT_NULL = 0,
    MS_DT_NEEDED = 1,
    MS_DT_PLTRELSZ = 2,
    MS_DT_HASH = 4,
    MS_DT_STRTAB = 5,
    MS_DT_SYMTAB = 6,
    MS_DT_RELA = 7,
    MS_DT_RELASZ = 8,
    MS_DT_RELAENT = 9,
    MS_DT_STRSZ = 10,
    MS_DT_SYMENT = 11,
    MS_DT_INIT = 12,
    MS_DT_FINI = 13,
    MS_DT_SONAME = 14,
    MS_DT_RPATH = 15,
    MS_DT_SYMBOLIC = 16,
    MS_DT_PLTREL = 20,
    MS_DT_JMPREL = 23,
    MS_DT_INIT_ARRAY = 25,
    MS_DT_FINI_ARRAY = 26,
    MS_DT_INIT_ARRAYSZ = 27,
    MS_DT_FINI_ARRAYSZ = 28,
    MS_DT_GNU_HASH = 0x6ffffef5,
    MS_DT_VERNEED = 0x6ffffffe,
    MS_DT_VERNEEDNUM = 0x6fffffff,
};

enum {
    MS_R_X86_64_64 = 1,
    MS_R_X86_64_GLOB_DAT = 6,
    MS_R_X86_64_JUMP_SLOT = 7,
    MS_R_X86_64_RELATIVE = 8,
    MS_R_X86_64_DTPMOD64 = 16,
};

static int metalsharp_linux_open_flags(int flags) {
    int translated = 0;
    switch (flags & 3) {
    case 1:
        translated |= O_WRONLY;
        break;
    case 2:
        translated |= O_RDWR;
        break;
    default:
        translated |= O_RDONLY;
        break;
    }
    if ((flags & 000100) != 0) {
        translated |= O_CREAT;
    }
    if ((flags & 000200) != 0) {
        translated |= O_EXCL;
    }
    if ((flags & 001000) != 0) {
        translated |= O_TRUNC;
    }
    if ((flags & 002000) != 0) {
        translated |= O_APPEND;
    }
    if ((flags & 004000) != 0) {
        translated |= O_NONBLOCK;
    }
#ifdef O_CLOEXEC
    if ((flags & 02000000) != 0) {
        translated |= O_CLOEXEC;
    }
#endif
    return translated;
}

static int ms_raw_open(const char* path, int flags, mode_t mode);
static void ms_log(const char* format, ...);
static bool is_proc_maps_path(const char* path);
extern int __sigaction(int signal, const struct sigaction* action, struct sigaction* old_action);
extern int __platform_sigaction(int signal, const struct sigaction* action, struct sigaction* old_action);
extern int32_t ntdll_get_unix_file_name(const uint16_t* dos, char** unix_name, unsigned int disposition)
    __attribute__((weak_import));

static int resolve_real_sigaction(void) {
    if (g_real_sigaction == NULL) {
        /* libsystem_platform's public-layout entry performs the normal
         * Darwin signal-trampoline conversion before calling the kernel.
         * Calling libsystem_c's exported sigaction through dyld is recursive
         * once this library is interposed. */
        g_real_sigaction = __platform_sigaction;
    }
    return g_real_sigaction != NULL;
}

static int metalsharp_memfd_create(unsigned int flags) {
    /* memfd_create() returns an anonymous, seekable file descriptor.  An
     * unlinked temporary file has the same lifetime and mmap/ftruncate
     * semantics on Darwin, while keeping the implementation inside the
     * existing host kernel rather than asking macOS for a Linux syscall. */
    unsigned int sequence = (unsigned int)__atomic_fetch_add(&g_memfd_sequence, 1, __ATOMIC_RELAXED);
    char path[128];
    int length = snprintf(path, sizeof(path), "/tmp/metalsharp-eac-memfd-%d-%u", (int)getpid(), sequence);
    if (length <= 0 || (size_t)length >= sizeof(path)) {
        errno = EINVAL;
        return -1;
    }
    int fd = ms_raw_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }
    (void)unlink(path);
    if ((flags & MS_MFD_CLOEXEC) != 0) {
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}

static uint64_t metalsharp_linux_syscall_result(ssize_t result) {
    if (result >= 0) {
        return (uint64_t)result;
    }
    return (uint64_t)(intptr_t)-errno;
}

static void metalsharp_sigsys_handler(int signal, siginfo_t* siginfo, void* context) {
    ucontext_t* ucontext = (ucontext_t*)context;
#if defined(__x86_64__)
    if (ucontext != NULL && ucontext->uc_mcontext != NULL) {
        ms_log("received SIGSYS rax=%llu rip=0x%llx", (unsigned long long)ucontext->uc_mcontext->__ss.__rax,
               (unsigned long long)ucontext->uc_mcontext->__ss.__rip);
    }
    if (ucontext != NULL && ucontext->uc_mcontext != NULL &&
        (uint64_t)ucontext->uc_mcontext->__ss.__rax == MS_LINUX_MEMFD_CREATE) {
        unsigned int flags = (unsigned int)ucontext->uc_mcontext->__ss.__rsi;
        int fd = metalsharp_memfd_create(flags);
        if (fd >= 0) {
            ucontext->uc_mcontext->__ss.__rax = (uint64_t)fd;
        } else {
            ucontext->uc_mcontext->__ss.__rax = (uint64_t)(intptr_t)-errno;
        }
        /* Rosetta delivers SIGSYS with RIP already advanced past `syscall`.
         * Re-executing or manually advancing it would loop on the same
         * instruction. */
        ms_log("translated Linux memfd_create flags=0x%x -> fd=%d", flags, fd);
        return;
    }
    if (ucontext != NULL && ucontext->uc_mcontext != NULL &&
        (uint64_t)ucontext->uc_mcontext->__ss.__rax == MS_LINUX_WRITE) {
        int fd = (int)ucontext->uc_mcontext->__ss.__rdi;
        const void* buffer = (const void*)(uintptr_t)ucontext->uc_mcontext->__ss.__rsi;
        size_t length = (size_t)ucontext->uc_mcontext->__ss.__rdx;
        ssize_t result = write(fd, buffer, length);
        ucontext->uc_mcontext->__ss.__rax = metalsharp_linux_syscall_result(result);
        ms_log("translated Linux write fd=%d buffer=0x%llx length=0x%zx -> %lld", fd,
               (unsigned long long)(uintptr_t)buffer, length, (long long)result);
        return;
    }
    if (ucontext != NULL && ucontext->uc_mcontext != NULL &&
        (uint64_t)ucontext->uc_mcontext->__ss.__rax == MS_LINUX_READ) {
        int fd = (int)ucontext->uc_mcontext->__ss.__rdi;
        void* buffer = (void*)(uintptr_t)ucontext->uc_mcontext->__ss.__rsi;
        size_t length = (size_t)ucontext->uc_mcontext->__ss.__rdx;
        ssize_t result = read(fd, buffer, length);
        ucontext->uc_mcontext->__ss.__rax = metalsharp_linux_syscall_result(result);
        ms_log("translated Linux read fd=%d buffer=0x%llx length=0x%zx -> %lld", fd,
               (unsigned long long)(uintptr_t)buffer, length, (long long)result);
        return;
    }
    if (ucontext != NULL && ucontext->uc_mcontext != NULL &&
        (uint64_t)ucontext->uc_mcontext->__ss.__rax == MS_LINUX_CLOSE) {
        int fd = (int)ucontext->uc_mcontext->__ss.__rdi;
        int result = close(fd);
        ucontext->uc_mcontext->__ss.__rax = metalsharp_linux_syscall_result(result);
        ms_log("translated Linux close fd=%d -> %d", fd, result);
        return;
    }
    if (ucontext != NULL && ucontext->uc_mcontext != NULL &&
        (uint64_t)ucontext->uc_mcontext->__ss.__rax == MS_LINUX_GETPID) {
        pid_t result = getpid();
        ucontext->uc_mcontext->__ss.__rax = (uint64_t)result;
        ms_log("translated Linux getpid -> %d", (int)result);
        return;
    }
    if (ucontext != NULL && ucontext->uc_mcontext != NULL &&
        (uint64_t)ucontext->uc_mcontext->__ss.__rax == MS_LINUX_OPEN) {
        const char* requested = (const char*)(uintptr_t)ucontext->uc_mcontext->__ss.__rdi;
        int linux_flags = (int)ucontext->uc_mcontext->__ss.__rsi;
        mode_t mode = (mode_t)ucontext->uc_mcontext->__ss.__rdx;
        int flags = metalsharp_linux_open_flags(linux_flags);
        const char* path = requested;
        if (is_proc_maps_path(requested) && g_maps_path[0] != '\0') {
            path = g_maps_path;
        }
        int result = (flags & O_CREAT) != 0 ? ms_raw_open(path, flags, mode) : ms_raw_open(path, flags, 0);
        ucontext->uc_mcontext->__ss.__rax = metalsharp_linux_syscall_result(result);
        ms_log("translated Linux open path=%s flags=0x%x -> %d", path != NULL ? path : "<null>", linux_flags, result);
        return;
    }
#else
    (void)ucontext;
#endif

    /* Preserve Wine 11.5's normal SIGSYS-to-SEH path for every syscall that
     * is not part of the implemented Linux substrate. */
    if (__atomic_load_n(&g_wine_sigsys_action_valid, __ATOMIC_ACQUIRE)) {
        if ((g_wine_sigsys_action.sa_flags & SA_SIGINFO) != 0 && g_wine_sigsys_action.sa_sigaction != NULL) {
            g_wine_sigsys_action.sa_sigaction(signal, siginfo, context);
        } else if (g_wine_sigsys_action.sa_handler != NULL && g_wine_sigsys_action.sa_handler != SIG_DFL &&
                   g_wine_sigsys_action.sa_handler != SIG_IGN) {
            g_wine_sigsys_action.sa_handler(signal);
        }
    }
}

static int metalsharp_sigaction(int signal, const struct sigaction* action, struct sigaction* old_action) {
    if (!resolve_real_sigaction()) {
        errno = ENOSYS;
        return -1;
    }
    if (action == NULL) {
        return g_real_sigaction(signal, action, old_action);
    }

    if (signal == SIGSEGV) {
        g_wine_sigsegv_action = *action;
        __atomic_store_n(&g_wine_sigsegv_action_valid, 1, __ATOMIC_RELEASE);
        struct sigaction wrapped = *action;
        wrapped.sa_flags |= SA_SIGINFO;
        wrapped.sa_sigaction = metalsharp_sigsegv_handler;
        return g_real_sigaction(signal, &wrapped, old_action);
    }

    if (signal != SIGSYS) {
        return g_real_sigaction(signal, action, old_action);
    }

    ms_log("intercepting Wine SIGSYS registration flags=0x%x", action->sa_flags);
    g_wine_sigsys_action = *action;
    __atomic_store_n(&g_wine_sigsys_action_valid, 1, __ATOMIC_RELEASE);
    struct sigaction wrapped = *action;
    wrapped.sa_flags |= SA_SIGINFO;
    wrapped.sa_sigaction = metalsharp_sigsys_handler;
    return g_real_sigaction(signal, &wrapped, old_action);
}

/*
 * DYLD_INTERPOSE applies before constructors run.  Calling dlsym(RTLD_NEXT,
 * "open") from that early path can re-enter dyld while it is loading Wine.
 * Darwin's syscall ABI gives the interposer a small, non-recursive escape
 * hatch for ordinary host files and for the temporary procfs view.
 */
static int ms_raw_open(const char* path, int flags, mode_t mode) {
#ifdef SYS_open
    return (int)syscall(SYS_open, path, flags, mode);
#else
    (void)path;
    (void)flags;
    (void)mode;
    errno = ENOSYS;
    return -1;
#endif
}

static int ms_raw_openat(int dirfd, const char* path, int flags, mode_t mode) {
#ifdef SYS_openat
    return (int)syscall(SYS_openat, dirfd, path, flags, mode);
#else
    (void)dirfd;
    return ms_raw_open(path, flags, mode);
#endif
}

static void ms_log(const char* format, ...) {
    if (g_log_path[0] == '\0') {
        const char* configured = getenv("METALSHARP_EAC_SUBSTRATE_LOG");
        if (configured != NULL && configured[0] != '\0') {
            snprintf(g_log_path, sizeof(g_log_path), "%s", configured);
        } else {
            snprintf(g_log_path, sizeof(g_log_path), "/tmp/metalsharp-eac-substrate-%d.log", (int)getpid());
        }
    }

    char line[2048];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length <= 0) {
        return;
    }
    if ((size_t)length > sizeof(line) - 1) {
        length = (int)(sizeof(line) - 1);
    }

    int fd = ms_raw_open(g_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)write(fd, line, (size_t)length);
        (void)write(fd, "\n", 1);
        (void)close(fd);
    }
}

/* ntdll.so imports this private Darwin entry through a lazy symbol stub.  A
 * Wine thread can switch to a newly-created TEB after the EAC Linux module
 * has been loaded, so the PE TLS pointer must follow that switch rather than
 * being initialized only on the loader's current thread.  Keep the real
 * syscall wrapper behind RTLD_NEXT and only touch 64-KiB-aligned guest bases;
 * the native Darwin pthread base used by the host-TSD guard is not aligned
 * that way. */
static void metalsharp_thread_set_tsd_base(void* base) {
    if (g_real_thread_set_tsd_base == NULL) {
        g_real_thread_set_tsd_base = g_thread_set_tsd_base;
    }
    void* effective_base = base;
    __sync_fetch_and_add(&g_tsd_callback_count, 1);
    g_tsd_last_requested = (uintptr_t)base;
    if (g_wine_pe_tls_array != NULL && base != NULL && (((uintptr_t)base & 0xffffu) != 0)) {
        void* public_teb = wine_public_teb_for_tsd_base((uintptr_t)base);
        if (public_teb != NULL) {
            effective_base = public_teb;
            __sync_fetch_and_add(&g_tsd_mapped_count, 1);
            volatile uintptr_t* tls_pointer = (volatile uintptr_t*)((uint8_t*)public_teb + 0x58u);
            if (*tls_pointer == 0) {
                *tls_pointer = (uintptr_t)g_wine_pe_tls_array;
            } else if (*(volatile uintptr_t*)(uintptr_t)*tls_pointer == 0 && g_wine_pe_tls_block != NULL) {
                *(volatile uintptr_t*)(uintptr_t)*tls_pointer = (uintptr_t)g_wine_pe_tls_block;
            }
        }
    }
    if (base != NULL && (((uintptr_t)base & 0xffffu) == 0)) {
        /* This callback is reached from ntdll's private TSD stub, where Mach
         * VM calls are unsafe because Rosetta is in the middle of a guest
         * signal/TSD transition.  The two private fields are in the valid
         * ntdll-created stack pseudo-TEB, so direct loads are sufficient to
         * distinguish it from the public TEB. */
        uintptr_t syscall_table = *(volatile uintptr_t*)((uint8_t*)base + 0x370u);
        uintptr_t syscall_frame = *(volatile uintptr_t*)((uint8_t*)base + 0x378u);
        if (syscall_table == 0 && syscall_frame == 0) {
            /* Wine's signal-only stack TEB is not in the public TEB list.
             * Resolve it through the current stack arena, never through a
             * process-global "last guest" value: a worker callback can race
             * the loader thread and otherwise receive another thread's GS
             * base. */
            uintptr_t mapped_guest = 0;
            if (wine_thread_bridge_lookup(wine_current_thread_token(), &mapped_guest, NULL, NULL) &&
                mapped_guest != 0) {
                __sync_fetch_and_add(&g_tsd_pseudo_count, 1);
                effective_base = (void*)(uintptr_t)mapped_guest;
                __sync_fetch_and_add(&g_tsd_mapped_count, 1);
            }
        }
    }
    g_tsd_last_effective = (uintptr_t)effective_base;
    size_t record_index = (size_t)__sync_fetch_and_add(&g_tsd_callback_count, 0);
    if (record_index != 0) {
        record_index--;
        if (record_index < sizeof(g_tsd_callback_records) / sizeof(g_tsd_callback_records[0])) {
            mach_port_t callback_thread = mach_thread_self();
            g_tsd_callback_records[record_index].requested = (uintptr_t)base;
            g_tsd_callback_records[record_index].effective = (uintptr_t)effective_base;
            g_tsd_callback_records[record_index].thread = (uint32_t)callback_thread;
            mach_port_deallocate(mach_task_self(), callback_thread);
        }
    }
    if (g_real_thread_set_tsd_base != NULL) {
        g_real_thread_set_tsd_base(effective_base);
    }
}

static void patch_wine115_tsd_stub(void) {
    if (g_ntdll_tsd_stub_patched || g_ntdll_image_base == 0) {
        return;
    }

    /* ntdll.so from the installed MetalSharp Wine 11.5 runtime calls its
     * private Darwin TSD syscall through the resolved lazy pointer at this
     * stable RVA.  Rosetta may translate the six-byte code stub itself, so
     * patch the data pointer rather than assuming its instruction bytes are
     * still x86-encoded in the translated mapping. */
    const mach_vm_address_t target = (mach_vm_address_t)g_ntdll_image_base + 0x951e0u;
    uintptr_t current = 0;
    mach_vm_size_t read_size = 0;
    kern_return_t read_result =
        mach_vm_read_overwrite(mach_task_self(), target, sizeof(current), (mach_vm_address_t)&current, &read_size);
    if (read_result != KERN_SUCCESS || read_size != sizeof(current)) {
        mach_vm_address_t region = target;
        mach_vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t region_info = {0};
        mach_msg_type_number_t region_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name = MACH_PORT_NULL;
        kern_return_t region_result = mach_vm_region(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
                                                     (vm_region_info_t)&region_info, &region_count, &object_name);
        if (object_name != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object_name);
        }
        ms_log("Wine 11.5 TSD pointer patch skipped target=0x%llx read_result=%d read_size=%llu region_result=%d "
               "region=0x%llx region_size=0x%llx prot=0x%x",
               (unsigned long long)target, read_result, (unsigned long long)read_size, region_result,
               (unsigned long long)region, (unsigned long long)region_size, region_info.protection);
        /* Rosetta's translated Mach-O data-const pages reject
         * mach_vm_read_overwrite even though the x86 guest can read them.
         * The region query above proves this exact pointer is mapped and
         * readable, so use the native load as the fallback. */
        if (region_result == KERN_SUCCESS && (region_info.protection & VM_PROT_READ) != 0 && region <= target &&
            target + sizeof(current) <= region + region_size) {
            current = *(volatile uintptr_t*)(uintptr_t)target;
            read_size = sizeof(current);
            read_result = KERN_SUCCESS;
            ms_log("Wine 11.5 TSD pointer direct read current=0x%llx", (unsigned long long)current);
        } else {
            return;
        }
    }
    uintptr_t replacement = (uintptr_t)&metalsharp_thread_set_tsd_base;
    mach_vm_address_t page = target & ~(mach_vm_address_t)(MS_PAGE_SIZE - 1u);
    (void)mach_vm_protect(mach_task_self(), page, MS_PAGE_SIZE, false, VM_PROT_READ | VM_PROT_WRITE);
    kern_return_t result = mach_vm_write(mach_task_self(), target, (vm_offset_t)&replacement, sizeof(replacement));
    (void)mach_vm_protect(mach_task_self(), page, MS_PAGE_SIZE, false, VM_PROT_READ);
    if (result == KERN_SUCCESS) {
        g_ntdll_tsd_stub_patched = 1;
    }
    ms_log("Wine 11.5 TSD pointer patch target=0x%llx current=0x%llx replacement=0x%llx result=%d",
           (unsigned long long)target, (unsigned long long)current, (unsigned long long)replacement, result);
}

/*
 * Wine's x86-64 Darwin signal layer uses the private Darwin
 * __thread_set_tsd_base entry to put the Windows TEB in the x86 GS base.
 * That is correct for guest code, but Darwin's libsystem pthread routines
 * use the same GS base for their native pthread object.  Calling a host
 * routine such as pthread_setspecific while GS points at the TEB therefore
 * writes into the wrong address space and faults.
 *
 * Darwin's thread_identifier_info.thread_handle is the native TSD base used
 * by libsystem_pthread.  Temporarily selecting it around calls made from the
 * Linux ELF module preserves both ABIs without changing Wine's runtime or
 * the EAC image.  The guard is nestable because the loader calls dlsym and
 * other bridge functions while a module entry point is active.
 */
static bool resolve_host_tsd_bridge(void) {
    if (g_thread_set_tsd_base == NULL) {
        /* Resolve against libsystem_kernel itself so DYLD interpose does not
         * hand the callback its own replacement and recurse during Wine's
         * early thread setup. */
        g_libsystem_kernel = dlopen("/usr/lib/system/libsystem_kernel.dylib", RTLD_NOW | RTLD_LOCAL);
        if (g_libsystem_kernel != NULL) {
            /* dlsym(handle, name) still observes the process-wide interpose
             * table for this private symbol.  Anchor the exact x86_64
             * libsystem_kernel export through its non-interposed syscall
             * symbol and its installed MetalSharp/Rosetta-compatible RVA. */
            void* syscall_symbol = dlsym(g_libsystem_kernel, "syscall");
            Dl_info kernel_image = {0};
            if (syscall_symbol != NULL && dladdr(syscall_symbol, &kernel_image) != 0 &&
                kernel_image.dli_fbase != NULL) {
                g_thread_set_tsd_base = (MsThreadSetTsdBaseFn)((uint8_t*)kernel_image.dli_fbase + 0x2c150u);
            }
        }
        if (g_thread_set_tsd_base == NULL) {
            g_thread_set_tsd_base = (MsThreadSetTsdBaseFn)dlsym(RTLD_NEXT, "_thread_set_tsd_base");
        }
    }
    /* The lazy ntdll stub can call the interposed replacement while Wine is
     * still switching a thread into its guest TSD state.  Never resolve
     * dyld from that callback: cache the original Darwin syscall while the
     * constructor is still on the native host TSD base. */
    if (g_real_thread_set_tsd_base == NULL) {
        g_real_thread_set_tsd_base = g_thread_set_tsd_base;
    }
    if (g_nt_current_teb == NULL) {
        g_nt_current_teb = (MsNtCurrentTebFn)dlsym(RTLD_DEFAULT, "NtCurrentTeb");
    }
    return g_thread_set_tsd_base != NULL && g_nt_current_teb != NULL;
}

static bool patch_ntdll_unix_name(void);

static void resolve_host_pthread_symbols(void) {
    if (g_host_pthread_setspecific != NULL && g_host_pthread_getspecific != NULL && g_host_pthread_self != NULL) {
        return;
    }
    g_libsystem_pthread = dlopen("/usr/lib/system/libsystem_pthread.dylib", RTLD_NOW | RTLD_LOCAL);
    if (g_libsystem_pthread != NULL) {
        void* pthread_create_symbol = dlsym(g_libsystem_pthread, "pthread_create");
        Dl_info pthread_image = {0};
        if (pthread_create_symbol != NULL && dladdr(pthread_create_symbol, &pthread_image) != 0 &&
            pthread_image.dli_fbase != NULL) {
            uintptr_t image_base = (uintptr_t)pthread_image.dli_fbase;
            /* These are the exported x86_64 entry RVAs in the installed
             * macOS libsystem_pthread used by the MetalSharp Rosetta
             * process.  Resolve through pthread_create, which is not
             * interposed, to avoid recursive dlsym lookups. */
            g_host_pthread_getspecific = (MsHostPthreadGetspecificFn)(image_base + 0x18d9u);
            g_host_pthread_setspecific = (MsHostPthreadSetspecificFn)(image_base + 0x18e3u);
            g_host_pthread_self = (MsHostPthreadSelfFn)(image_base + 0x2147u);
        }
    }
    if (g_host_pthread_setspecific == NULL) {
        g_host_pthread_setspecific = (MsHostPthreadSetspecificFn)dlsym(RTLD_NEXT, "pthread_setspecific");
    }
    if (g_host_pthread_getspecific == NULL) {
        g_host_pthread_getspecific = (MsHostPthreadGetspecificFn)dlsym(RTLD_NEXT, "pthread_getspecific");
    }
    if (g_host_pthread_self == NULL) {
        g_host_pthread_self = (MsHostPthreadSelfFn)dlsym(RTLD_NEXT, "pthread_self");
    }
}

static void resolve_nt_current_teb_from_wine(void) {
    if (g_nt_current_teb != NULL) {
        return;
    }

    if (g_ntdll_unix_name_target == NULL) {
        (void)patch_ntdll_unix_name();
    }
    if (g_ntdll_unix_name_target != NULL) {
        Dl_info image = {0};
        if (dladdr(g_ntdll_unix_name_target, &image) != 0 && image.dli_fbase != NULL) {
            g_ntdll_image_base = (uintptr_t)image.dli_fbase;
            g_nt_current_teb = (MsNtCurrentTebFn)((uint8_t*)image.dli_fbase + 0x68e20u);
            ms_log("resolved Wine 11.5 NtCurrentTeb from ntdll base=0x%llx target=0x%llx",
                   (unsigned long long)(uintptr_t)image.dli_fbase, (unsigned long long)(uintptr_t)g_nt_current_teb);
            patch_wine115_tsd_stub();
            return;
        }
    }

    /* NtCurrentTeb is a local Unix-library symbol rather than a dyld export.
     * Locate its stable Wine 11.5 prologue directly in the executable
     * ntdll.so mapping instead of calling dlsym while the guest TSD base is
     * active. */
    static const uint8_t pattern[] = {
        0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x3d, 0, 0, 0, 0, 0xe8, 0, 0, 0, 0, 0x5d, 0xc3,
    };
    mach_vm_address_t cursor = 0;
    while (cursor < UINT64_MAX) {
        mach_vm_address_t region_start = cursor;
        mach_vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name = MACH_PORT_NULL;
        kern_return_t result = mach_vm_region(mach_task_self(), &region_start, &region_size, VM_REGION_BASIC_INFO_64,
                                              (vm_region_info_t)&info, &info_count, &object_name);
        if (object_name != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object_name);
        }
        if (result != KERN_SUCCESS || region_size == 0) {
            break;
        }
        cursor = region_start + region_size;
        if ((info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE)) {
            continue;
        }
        for (mach_vm_size_t offset = 0; offset < region_size;) {
            mach_vm_size_t request = region_size - offset;
            if (request > 0x10000) {
                request = 0x10000;
            }
            uint8_t bytes[0x10000];
            mach_vm_size_t read_size = 0;
            if (mach_vm_read_overwrite(mach_task_self(), region_start + offset, request, (mach_vm_address_t)bytes,
                                       &read_size) == KERN_SUCCESS) {
                for (mach_vm_size_t index = 0; index + sizeof(pattern) <= read_size; index++) {
                    bool match = true;
                    for (size_t byte = 0; byte < sizeof(pattern); byte++) {
                        if (byte != 7 && byte != 8 && byte != 9 && byte != 10 && byte != 12 && byte != 13 &&
                            byte != 14 && byte != 15 && bytes[index + byte] != pattern[byte]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        g_nt_current_teb = (MsNtCurrentTebFn)(uintptr_t)(region_start + offset + index);
                        ms_log("found exact Wine 11.5 NtCurrentTeb at 0x%llx",
                               (unsigned long long)(uintptr_t)g_nt_current_teb);
                        return;
                    }
                }
            }
            if (request == 0) {
                break;
            }
            offset += request;
        }
    }
    if (g_nt_current_teb == NULL) {
        ms_log("Wine 11.5 NtCurrentTeb prologue was not found in executable mappings");
    }
}

static void* wine_guest_teb_from_stack(void) {
#if defined(__x86_64__)
    uintptr_t stack = 0;
    /* The exact Wine 11.5 ntdll `_get_current_teb` implementation derives
     * the TEB from the 64-KiB-aligned Wine thread stack.  Use that same
     * guest-owned rule here; reading an x86 segment register from a Darwin
     * host function is not valid under Rosetta. */
    __asm__ volatile("movq %%rsp, %0" : "=r"(stack));
    return (void*)(stack & ~(uintptr_t)0xffffu);
#else
    return NULL;
#endif
}

static bool wine_read_word(void* address, uintptr_t* value) {
    mach_vm_size_t read_size = 0;
    return value != NULL &&
           mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)address, sizeof(*value),
                                  (mach_vm_address_t)value, &read_size) == KERN_SUCCESS &&
           read_size == sizeof(*value);
}

static void* wine_guest_teb_from_identifier(const thread_identifier_info_data_t* identifier) {
    if (identifier != NULL && identifier->thread_handle != 0 && (identifier->thread_handle & 0xffffu) == 0) {
        void* candidate = (void*)(uintptr_t)identifier->thread_handle;
        uintptr_t self = 0;
        uintptr_t syscall_table = 0;
        uintptr_t syscall_frame = 0;
        if (wine_read_word((uint8_t*)candidate + 0x30u, &self) &&
            wine_read_word((uint8_t*)candidate + 0x370u, &syscall_table) &&
            wine_read_word((uint8_t*)candidate + 0x378u, &syscall_frame) && self == (uintptr_t)candidate &&
            (syscall_table != 0 || syscall_frame != 0)) {
            return candidate;
        }
    }
    return wine_guest_teb_from_stack();
}

static void* wine_guest_teb_for_current_thread(void) {
    uintptr_t current_token = wine_current_thread_token();
    uintptr_t remembered_guest = 0;
    if (wine_thread_bridge_lookup(current_token, &remembered_guest, NULL, NULL) && remembered_guest != 0) {
        return (void*)(uintptr_t)remembered_guest;
    }
    thread_identifier_info_data_t identifier = {0};
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    mach_port_t thread = mach_thread_self();
    kern_return_t result = thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&identifier, &count);
    mach_port_deallocate(mach_task_self(), thread);
    if (result != KERN_SUCCESS) {
        return wine_guest_teb_from_stack();
    }
    return wine_guest_teb_from_identifier(&identifier);
}

static bool wine_write_word(void* address, uintptr_t value) {
    return mach_vm_write(mach_task_self(), (mach_vm_address_t)(uintptr_t)address, (vm_offset_t)&value, sizeof(value)) ==
           KERN_SUCCESS;
}

static size_t repair_wine_teb(void* teb, uintptr_t replacement) {
    if (teb == NULL) {
        return 0;
    }
    uintptr_t self = 0;
    uintptr_t tls_pointer = 0;
    uintptr_t tsd_teb = 0;
    uintptr_t syscall_table = 0;
    uintptr_t syscall_frame = 0;
    bool self_read = wine_read_word((uint8_t*)teb + 0x30u, &self);
    bool tls_read = wine_read_word((uint8_t*)teb + 0x58u, &tls_pointer);
    bool tsd_read = wine_read_word((uint8_t*)teb + 0x320u, &tsd_teb);
    bool table_read = wine_read_word((uint8_t*)teb + 0x370u, &syscall_table);
    bool frame_read = wine_read_word((uint8_t*)teb + 0x378u, &syscall_frame);
    bool public_teb = table_read && frame_read && (syscall_table != 0 || syscall_frame != 0);
    size_t repaired = 0;
    if (public_teb && self_read && self == 0 && wine_write_word((uint8_t*)teb + 0x30u, (uintptr_t)teb)) {
        repaired++;
    }
    if (public_teb && tsd_read && tsd_teb == 0 && wine_write_word((uint8_t*)teb + 0x320u, (uintptr_t)teb)) {
        repaired++;
    }
    if (tls_read) {
        if (tls_pointer == 0) {
            if (wine_write_word((uint8_t*)teb + 0x58u, replacement)) {
                repaired++;
            }
        } else {
            uintptr_t tls_block = 0;
            if (wine_read_word((void*)(uintptr_t)tls_pointer, &tls_block) && tls_block == 0 &&
                wine_write_word((void*)(uintptr_t)tls_pointer, (uintptr_t)g_wine_pe_tls_block)) {
                repaired++;
            }
        }
    }
    return repaired;
}

static void maintain_wine_teb_list(void) {
    if (g_ntdll_image_base == 0 || g_wine_pe_tls_array == NULL || g_wine_pe_tls_block == NULL) {
        return;
    }
    __sync_fetch_and_add(&g_wine_tls_monitor_iterations, 1);
    mach_vm_address_t list_head = (mach_vm_address_t)g_ntdll_image_base + 0x98530u;
    uintptr_t next_entry = 0;
    mach_vm_size_t next_size = 0;
    if (mach_vm_read_overwrite(mach_task_self(), list_head, sizeof(next_entry), (mach_vm_address_t)&next_entry,
                               &next_size) != KERN_SUCCESS ||
        next_size != sizeof(next_entry)) {
        return;
    }
    for (size_t pass = 0; pass < 256 && next_entry != (uintptr_t)list_head; pass++) {
        if (next_entry < 0x3b0u || ((next_entry - 0x3b0u) & 0xffffu) != 0) {
            return;
        }
        void* teb = (void*)(next_entry - 0x3b0u);
        __sync_fetch_and_add(&g_wine_tls_monitor_tebs, 1);
        size_t repaired = repair_wine_teb(teb, (uintptr_t)g_wine_pe_tls_array);
        if (repaired != 0) {
            __sync_fetch_and_add(&g_wine_tls_monitor_repairs, repaired);
            uintptr_t repaired_self = 0;
            uintptr_t repaired_tls = 0;
            uintptr_t repaired_tls_block = 0;
            uintptr_t repaired_tsd = 0;
            (void)wine_read_word((uint8_t*)teb + 0x30u, &repaired_self);
            (void)wine_read_word((uint8_t*)teb + 0x58u, &repaired_tls);
            (void)wine_read_word((uint8_t*)teb + 0x320u, &repaired_tsd);
            (void)wine_read_word((void*)(uintptr_t)repaired_tls, &repaired_tls_block);
            ms_log("Wine PE TLS maintainer repaired teb=0x%llx fields=%zu self=0x%llx tls=0x%llx tls_block=0x%llx "
                   "tsd=0x%llx",
                   (unsigned long long)(uintptr_t)teb, repaired, (unsigned long long)repaired_self,
                   (unsigned long long)repaired_tls, (unsigned long long)repaired_tls_block,
                   (unsigned long long)repaired_tsd);
        }
        uintptr_t following = 0;
        mach_vm_size_t following_size = 0;
        if (mach_vm_read_overwrite(mach_task_self(), next_entry, sizeof(following), (mach_vm_address_t)&following,
                                   &following_size) != KERN_SUCCESS ||
            following_size != sizeof(following)) {
            return;
        }
        next_entry = following;
    }
}

/* `_get_current_teb` in this Wine 11.5 build derives a second, private TEB
 * address from the current Windows stack.  It is not linked in `_teb_list`,
 * yet Rosetta can leave GS on that address while returning from a worker
 * transition.  The launcher fault observed on the exact external Elden
 * Ring executable used the 0x01900000 stack TEB.  Cover the small Wine
 * stack-arena range directly, but only when all private syscall fields are
 * zero; this avoids touching arbitrary application memory. */
static void maintain_wine_stack_teb_tls(void) {
    if (g_wine_pe_tls_array == NULL || g_wine_pe_tls_block == NULL) {
        return;
    }
    static uint64_t scan_iteration;
    if ((__sync_fetch_and_add(&scan_iteration, 1) % 10u) != 0) {
        return;
    }
    for (uintptr_t base = 0x01800000u; base < 0x01c00000u; base += 0x10000u) {
        uintptr_t self = 0;
        uintptr_t tls_pointer = 0;
        uintptr_t syscall_table = 0;
        uintptr_t syscall_frame = 0;
        if (!wine_read_word((void*)(base + 0x30u), &self) || !wine_read_word((void*)(base + 0x58u), &tls_pointer) ||
            !wine_read_word((void*)(base + 0x370u), &syscall_table) ||
            !wine_read_word((void*)(base + 0x378u), &syscall_frame)) {
            continue;
        }
        if ((self != 0 && self != base) || syscall_table != 0 || syscall_frame != 0) {
            continue;
        }
        size_t repaired = 0;
        if (self == 0 && wine_write_word((void*)(base + 0x30u), base)) {
            repaired++;
        }
        if (tls_pointer == 0 && wine_write_word((void*)(base + 0x58u), (uintptr_t)g_wine_pe_tls_array)) {
            repaired++;
            tls_pointer = (uintptr_t)g_wine_pe_tls_array;
        }
        if (tls_pointer != 0) {
            uintptr_t tls_block = 0;
            if (wine_read_word((void*)tls_pointer, &tls_block) && tls_block == 0 &&
                wine_write_word((void*)tls_pointer, (uintptr_t)g_wine_pe_tls_block)) {
                repaired++;
            }
        }
        if (repaired != 0) {
            ms_log("Wine PE TLS stack-TEB maintainer repaired teb=0x%llx fields=%zu", (unsigned long long)base,
                   repaired);
        }
    }
}

static void* wine_tls_monitor(void* unused) {
    (void)unused;
    for (;;) {
        maintain_wine_teb_list();
        maintain_wine_stack_teb_tls();
        uint64_t iteration = g_wine_tls_monitor_iterations;
        if (iteration != 0 && (iteration % 1000u) == 0) {
            ms_log("Wine PE TLS maintainer heartbeat iterations=%llu tebs=%llu repairs=%llu",
                   (unsigned long long)iteration, (unsigned long long)g_wine_tls_monitor_tebs,
                   (unsigned long long)g_wine_tls_monitor_repairs);
            ms_log("Wine TSD monitor diagnostics callbacks=%llu pseudo=%llu mapped=%llu last_requested=0x%llx "
                   "last_effective=0x%llx",
                   (unsigned long long)g_tsd_callback_count, (unsigned long long)g_tsd_pseudo_count,
                   (unsigned long long)g_tsd_mapped_count, (unsigned long long)g_tsd_last_requested,
                   (unsigned long long)g_tsd_last_effective);
            uint64_t callback_count = g_tsd_callback_count;
            uint64_t first_record = callback_count > 8 ? callback_count - 8 : 0;
            for (uint64_t record = first_record; record < callback_count && record < 64; record++) {
                ms_log("Wine TSD callback record=%llu thread=0x%x requested=0x%llx effective=0x%llx",
                       (unsigned long long)record, g_tsd_callback_records[record].thread,
                       (unsigned long long)g_tsd_callback_records[record].requested,
                       (unsigned long long)g_tsd_callback_records[record].effective);
            }
        }
        usleep(1000);
    }
    return NULL;
}

static void start_wine_tls_monitor(void) {
    if (__sync_bool_compare_and_swap(&g_wine_tls_monitor_started, 0, 1)) {
        if (pthread_create(&g_wine_tls_monitor_thread, NULL, wine_tls_monitor, NULL) == 0) {
            (void)pthread_detach(g_wine_tls_monitor_thread);
            ms_log("Wine PE TLS maintainer started");
        } else {
            g_wine_tls_monitor_started = 0;
            ms_log("Wine PE TLS maintainer could not start errno=%d", errno);
        }
    }
}

static void ensure_wine_pe_tls(void) {
    void* teb = wine_guest_teb_for_current_thread();
    if (teb == NULL) {
        ms_log("Wine PE TLS setup skipped: guest TEB could not be derived from stack");
        return;
    }
    if (g_wine_pe_tls_array == NULL || g_wine_pe_tls_block == NULL) {
        g_wine_pe_tls_array = mmap(NULL, MS_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        g_wine_pe_tls_block = mmap(NULL, MS_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_wine_pe_tls_array == MAP_FAILED || g_wine_pe_tls_block == MAP_FAILED) {
            if (g_wine_pe_tls_array != NULL && g_wine_pe_tls_array != MAP_FAILED) {
                munmap(g_wine_pe_tls_array, MS_PAGE_SIZE);
            }
            if (g_wine_pe_tls_block != NULL && g_wine_pe_tls_block != MAP_FAILED) {
                munmap(g_wine_pe_tls_block, MS_PAGE_SIZE);
            }
            g_wine_pe_tls_array = NULL;
            g_wine_pe_tls_block = NULL;
            ms_log("Wine PE TLS setup allocation failed");
            return;
        }
        /* The launcher is the first TLS image in this Wine process.  Keep
         * slot zero populated; the block is zero-filled just like the
         * executable's eight-byte TLS template. */
        ((uintptr_t*)g_wine_pe_tls_array)[0] = (uintptr_t)g_wine_pe_tls_block;
    }

    uintptr_t replacement = (uintptr_t)g_wine_pe_tls_array;
    size_t patched = 0;

    /* Always cover the caller first, even if the list is being changed by
     * Wine's thread teardown path. */
    void* teb_cursor = teb;
    patched += repair_wine_teb(teb_cursor, replacement);

    /* ThreadLocalStoragePointer is a field of every Wine TEB, not process
     * global state.  EAC's Linux loader can run on a helper thread while the
     * Windows launcher later resumes on the process thread, so initialize all
     * TEBs already linked by the exact Wine 11.5 ntdll.  The list entry is
     * TEB+0x3b0 in this runtime (thread_data+0xc0); the offsets are derived
     * from ntdll.so's local _init_teb implementation and are intentionally
     * kept alongside the pinned MetalSharp runtime contract. */
    if (g_ntdll_image_base != 0) {
        mach_vm_address_t list_head = (mach_vm_address_t)g_ntdll_image_base + 0x98530u;
        uintptr_t next_entry = 0;
        mach_vm_size_t next_size = 0;
        bool list_available = mach_vm_read_overwrite(mach_task_self(), list_head, sizeof(next_entry),
                                                     (mach_vm_address_t)&next_entry, &next_size) == KERN_SUCCESS &&
                              next_size == sizeof(next_entry) && next_entry != (uintptr_t)list_head;
        for (size_t pass = 0; list_available && pass < 128 && next_entry != (uintptr_t)list_head; pass++) {
            /* A Wine TEB is allocated on a 64-KiB boundary.  This check
             * prevents a corrupt/transient list from turning the diagnostic
             * bridge into a write through an arbitrary pointer. */
            if (next_entry < 0x3b0u || ((next_entry - 0x3b0u) & 0xffffu) != 0) {
                break;
            }
            teb_cursor = (void*)(next_entry - 0x3b0u);
            if ((uintptr_t)teb_cursor != (uintptr_t)teb) {
                patched += repair_wine_teb(teb_cursor, replacement);
                uintptr_t list_self = 0;
                uintptr_t list_tls = 0;
                uintptr_t list_tsd = 0;
                uintptr_t list_table = 0;
                uintptr_t list_frame = 0;
                (void)wine_read_word((uint8_t*)teb_cursor + 0x30u, &list_self);
                (void)wine_read_word((uint8_t*)teb_cursor + 0x58u, &list_tls);
                (void)wine_read_word((uint8_t*)teb_cursor + 0x320u, &list_tsd);
                (void)wine_read_word((uint8_t*)teb_cursor + 0x370u, &list_table);
                (void)wine_read_word((uint8_t*)teb_cursor + 0x378u, &list_frame);
                ms_log("Wine PE TLS list_teb=0x%llx self=0x%llx tls=0x%llx tsd_teb=0x%llx syscall_table=0x%llx "
                       "syscall_frame=0x%llx",
                       (unsigned long long)(uintptr_t)teb_cursor, (unsigned long long)list_self,
                       (unsigned long long)list_tls, (unsigned long long)list_tsd, (unsigned long long)list_table,
                       (unsigned long long)list_frame);
            }
            uintptr_t following = 0;
            mach_vm_size_t following_size = 0;
            if (mach_vm_read_overwrite(mach_task_self(), next_entry, sizeof(following), (mach_vm_address_t)&following,
                                       &following_size) != KERN_SUCCESS ||
                following_size != sizeof(following)) {
                break;
            }
            next_entry = following;
        }
    }
    uintptr_t current_self = 0;
    uintptr_t current_tls = 0;
    uintptr_t current_tsd = 0;
    uintptr_t current_syscall_table = 0;
    uintptr_t current_syscall_frame = 0;
    uintptr_t current_syscall_flags = 0;
    uintptr_t current_tls_block = 0;
    (void)wine_read_word((uint8_t*)teb + 0x30u, &current_self);
    (void)wine_read_word((uint8_t*)teb + 0x58u, &current_tls);
    (void)wine_read_word((uint8_t*)teb + 0x320u, &current_tsd);
    (void)wine_read_word((uint8_t*)teb + 0x370u, &current_syscall_table);
    (void)wine_read_word((uint8_t*)teb + 0x378u, &current_syscall_frame);
    (void)wine_read_word((uint8_t*)teb + 0x380u, &current_syscall_flags);
    (void)wine_read_word((void*)(uintptr_t)current_tls, &current_tls_block);
    ms_log("Wine PE TLS setup current_teb=0x%llx self=0x%llx tls=0x%llx tls_block=0x%llx tsd_teb=0x%llx "
           "syscall_table=0x%llx syscall_frame=0x%llx syscall_flags=0x%llx array=0x%llx block=0x%llx patched=%zu "
           "ntdll=0x%llx",
           (unsigned long long)(uintptr_t)teb, (unsigned long long)current_self, (unsigned long long)current_tls,
           (unsigned long long)current_tls_block, (unsigned long long)current_tsd,
           (unsigned long long)current_syscall_table, (unsigned long long)current_syscall_frame,
           (unsigned long long)current_syscall_flags, (unsigned long long)(uintptr_t)g_wine_pe_tls_array,
           (unsigned long long)(uintptr_t)g_wine_pe_tls_block, patched, (unsigned long long)g_ntdll_image_base);

    /* The Linux module runs under the temporary native Darwin TSD base.  A
     * host pthread callback can leave that base selected after its final
     * nested return, especially when Wine resumes the PE caller from a
     * helper thread.  Re-establish the exact public Wine TEB at this ABI
     * boundary before protected Windows code executes again.  This is the
     * same private Wine 11.5 transition used by ntdll; it is not a launcher
     * instruction patch or a fault bypass. */
    uint32_t bridge_depth = 0;
    (void)wine_thread_bridge_lookup(wine_current_thread_token(), NULL, NULL, &bridge_depth);
    if (g_real_thread_set_tsd_base != NULL && bridge_depth == 0) {
        g_last_guest_teb = teb;
        g_real_thread_set_tsd_base(teb);
        ms_log("restored Wine guest TSD base=0x%llx after PE TLS setup", (unsigned long long)(uintptr_t)teb);
    }
}

static void metalsharp_sigsegv_handler(int signal, siginfo_t* siginfo, void* context) {
    static volatile sig_atomic_t report_count;
    ucontext_t* ucontext = (ucontext_t*)context;
    uintptr_t rip = 0;
    uintptr_t rsp = 0;
    uintptr_t context_gs = 0;
    uintptr_t context_fs = 0;
#if defined(__x86_64__)
    if (ucontext != NULL && ucontext->uc_mcontext != NULL) {
        rip = (uintptr_t)ucontext->uc_mcontext->__ss.__rip;
        rsp = (uintptr_t)ucontext->uc_mcontext->__ss.__rsp;
        context_gs = (uintptr_t)ucontext->uc_mcontext->__ss.__gs;
        context_fs = (uintptr_t)ucontext->uc_mcontext->__ss.__fs;
    }
    if ((rip == 0x14001c622ULL || rip == 0x14001cb98ULL) && g_wine_pe_tls_block != NULL) {
        /* Both observed launcher TLS accessors fault on the second
         * instruction (`mov rax,[rax]`) when Rosetta has temporarily left GS
         * on a native/empty TSD base.  First repair the actual Wine TSD
         * transition and retry the unmodified launcher instruction.  The
         * diagnostic fallback below remains only so a failed repair cannot
         * strand the protected-launch probe in a fault loop. */
        uintptr_t fault_rax = 0;
        uintptr_t fault_gs_tls = 0;
#if defined(__x86_64__)
        fault_rax = (uintptr_t)ucontext->uc_mcontext->__ss.__rax;
        __asm__ volatile("movq %%gs:0x58, %0" : "=r"(fault_gs_tls));
#endif
        static volatile sig_atomic_t tls_repair_attempts;
        void* fault_guest_teb = NULL;
        /* A protected launcher callback can execute on Wine's low
         * 64-KiB-aligned signal/guest stack arena rather than on the public
         * TEB's normal stack range.  In that case the arena TEB is the GS
         * state that `_get_current_teb` selected for this exact frame. */
        uintptr_t stack_teb = rsp & ~(uintptr_t)0xffffu;
        uintptr_t stack_self = 0;
        uintptr_t stack_tls = 0;
        if (stack_teb != 0 && wine_read_word((uint8_t*)stack_teb + 0x30u, &stack_self) &&
            wine_read_word((uint8_t*)stack_teb + 0x58u, &stack_tls) && stack_self == stack_teb && stack_tls != 0) {
            fault_guest_teb = (void*)(uintptr_t)stack_teb;
        }
        if (fault_guest_teb == NULL) {
            fault_guest_teb = wine_public_teb_for_stack_pointer(rsp);
        }
        if (fault_guest_teb == NULL) {
            fault_guest_teb = g_last_guest_teb;
        }
        sig_atomic_t attempt = __sync_fetch_and_add(&tls_repair_attempts, 1);
        if (attempt < 2 && g_real_thread_set_tsd_base != NULL && fault_guest_teb != NULL) {
            g_real_thread_set_tsd_base(fault_guest_teb);
            uintptr_t repaired_gs_tls = read_guest_gs_tls_pointer();
            ms_log("repaired Wine guest TSD in PE TLS fault rip=0x%llx rsp=0x%llx rax=0x%llx gs_tls=0x%llx "
                   "repaired_gs_tls=0x%llx guest_teb=0x%llx; retrying",
                   (unsigned long long)rip, (unsigned long long)rsp, (unsigned long long)fault_rax,
                   (unsigned long long)fault_gs_tls, (unsigned long long)repaired_gs_tls,
                   (unsigned long long)(uintptr_t)fault_guest_teb);
            if (repaired_gs_tls != 0) {
                return;
            }
        }
        ucontext->uc_mcontext->__ss.__rax = (uint64_t)(uintptr_t)g_wine_pe_tls_block;
        ucontext->uc_mcontext->__ss.__rip = (uint64_t)(rip + 3u);
        ms_log("emulated PE TLS block load fallback rip=0x%llx rsp=0x%llx rax=0x%llx gs_tls=0x%llx fault=0x%llx "
               "block=0x%llx",
               (unsigned long long)rip, (unsigned long long)rsp, (unsigned long long)fault_rax,
               (unsigned long long)fault_gs_tls,
               (unsigned long long)(uintptr_t)(siginfo != NULL ? siginfo->si_addr : NULL),
               (unsigned long long)(uintptr_t)g_wine_pe_tls_block);
        return;
    }
    int report = __sync_fetch_and_add(&report_count, 1);
    if (report < 16) {
        uintptr_t gs_self = 0;
        uintptr_t gs_tls = 0;
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(gs_self));
        __asm__ volatile("movq %%gs:0x58, %0" : "=r"(gs_tls));
        ms_log("PE SIGSEGV diagnostic rip=0x%llx rsp=0x%llx context_gs=0x%llx context_fs=0x%llx gs_self=0x%llx "
               "gs_tls=0x%llx fault=0x%llx requested_tsd=0x%llx",
               (unsigned long long)rip, (unsigned long long)rsp, (unsigned long long)context_gs,
               (unsigned long long)context_fs, (unsigned long long)gs_self, (unsigned long long)gs_tls,
               (unsigned long long)(uintptr_t)(siginfo != NULL ? siginfo->si_addr : NULL),
               (unsigned long long)g_tsd_last_requested);
    }
#else
    (void)ucontext;
#endif
    if (__atomic_load_n(&g_wine_sigsegv_action_valid, __ATOMIC_ACQUIRE)) {
        if ((g_wine_sigsegv_action.sa_flags & SA_SIGINFO) != 0 && g_wine_sigsegv_action.sa_sigaction != NULL) {
            g_wine_sigsegv_action.sa_sigaction(signal, siginfo, context);
        } else if (g_wine_sigsegv_action.sa_handler != NULL && g_wine_sigsegv_action.sa_handler != SIG_DFL &&
                   g_wine_sigsegv_action.sa_handler != SIG_IGN) {
            g_wine_sigsegv_action.sa_handler(signal);
        }
    }
}

static bool metalsharp_eac_host_tsd_enter(void) {
    uintptr_t current_thread = wine_current_thread_token();
    MsWineThreadBridgeState* bridge_state = wine_thread_bridge_state(current_thread);
    if (bridge_state == NULL) {
        return false;
    }
    if (bridge_state->host_depth != 0) {
        bridge_state->host_depth++;
        return true;
    }
    /* Resolve these pointers while the process still has its native Darwin
     * TSD base.  Re-entering dyld from a Wine guest thread can itself call
     * pthread_setspecific, which is precisely the boundary this guard fixes. */
    if (g_thread_set_tsd_base == NULL || g_nt_current_teb == NULL) {
        return false;
    }

    thread_identifier_info_data_t identifier = {0};
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    mach_port_t thread = mach_thread_self();
    uint32_t current_thread_token = (uint32_t)thread;
    kern_return_t result = thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&identifier, &count);
    mach_port_deallocate(mach_task_self(), thread);
    if (result != KERN_SUCCESS || identifier.thread_handle == 0) {
        return false;
    }

    uintptr_t remembered_guest = bridge_state->guest_teb;
    uintptr_t remembered_host = bridge_state->host_tsd;

    bool identifier_is_public_teb = false;
    if (identifier.thread_handle != 0 && (identifier.thread_handle & 0xffffu) == 0) {
        uintptr_t identifier_self = 0;
        uintptr_t identifier_syscall_table = 0;
        uintptr_t identifier_syscall_frame = 0;
        void* identifier_teb = (void*)(uintptr_t)identifier.thread_handle;
        identifier_is_public_teb = wine_read_word((uint8_t*)identifier_teb + 0x30u, &identifier_self) &&
                                   wine_read_word((uint8_t*)identifier_teb + 0x370u, &identifier_syscall_table) &&
                                   wine_read_word((uint8_t*)identifier_teb + 0x378u, &identifier_syscall_frame) &&
                                   identifier_self == (uintptr_t)identifier_teb &&
                                   (identifier_syscall_table != 0 || identifier_syscall_frame != 0);
    }
    void* guest_base =
        remembered_guest != 0 ? (void*)(uintptr_t)remembered_guest : wine_guest_teb_from_identifier(&identifier);
    if (guest_base == NULL) {
        return false;
    }
    /* The public Wine TEB is the 64-KiB-aligned thread handle.  The stack
     * aligned address used by ntdll's signal-only `_get_current_teb` is a
     * separate internal signal frame and must never be installed as GS for
     * PE code. */
    uintptr_t wine_tsd_base = (uintptr_t)guest_base;
    uintptr_t current_stack = 0;
#if defined(__x86_64__)
    __asm__ volatile("movq %%rsp, %0" : "=r"(current_stack));
#endif
    void* stack_public_teb = wine_public_teb_for_stack_pointer(current_stack);
    /* Main-thread Wine uses the public aligned TEB as thread_handle, while
     * worker threads in this exact runtime expose their native Darwin TSD
     * object there.  The latter must stay per-thread; using the process
     * thread's captured pthread+0xe0 base corrupts host pthread_getspecific
     * during worker startup. */
    uintptr_t host_tsd_base = remembered_host != 0 ? remembered_host
                              : identifier_is_public_teb
                                  ? (g_native_host_tsd_base != 0 ? g_native_host_tsd_base : identifier.thread_handle)
                                  : identifier.thread_handle;
    if (remembered_host == 0 && !identifier_is_public_teb) {
        uintptr_t remembered_tsd = wine_native_tsd_for_thread(current_thread_token);
        if (remembered_tsd != 0) {
            host_tsd_base = remembered_tsd;
        }
        uintptr_t guest_thread_tsd = 0;
        if (remembered_tsd == 0 && stack_public_teb != NULL &&
            wine_read_word((uint8_t*)stack_public_teb + 0x320u, &guest_thread_tsd) && guest_thread_tsd != 0) {
            host_tsd_base = guest_thread_tsd;
        } else if (remembered_tsd == 0 && wine_read_word((uint8_t*)guest_base + 0x320u, &guest_thread_tsd) &&
                   guest_thread_tsd != 0) {
            host_tsd_base = guest_thread_tsd;
        }
    }
    if (host_tsd_base == 0) {
        return false;
    }
    bridge_state->guest_teb = wine_tsd_base;
    bridge_state->host_tsd = host_tsd_base;
    bridge_state->host_depth = 1;
    g_last_guest_teb = guest_base;
    g_thread_set_tsd_base((void*)host_tsd_base);
    uintptr_t guest_tls_pointer = 0;
    uintptr_t host_zero = 0;
    uintptr_t host_tls_pointer = 0;
    mach_vm_size_t guest_read = 0;
    mach_vm_size_t host_zero_read = 0;
    mach_vm_size_t host_tls_read = 0;
    (void)mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)guest_base + 0x58u,
                                 sizeof(guest_tls_pointer), (mach_vm_address_t)&guest_tls_pointer, &guest_read);
    (void)mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)host_tsd_base, sizeof(host_zero),
                                 (mach_vm_address_t)&host_zero, &host_zero_read);
    (void)mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)host_tsd_base + 0x58u, sizeof(host_tls_pointer),
                                 (mach_vm_address_t)&host_tls_pointer, &host_tls_read);
    ms_log("entered Darwin host TSD base=0x%llx native_pthread=0x%llx thread_handle=0x%llx guest TEB=0x%llx "
           "restore=0x%llx field_read=%llu",
           (unsigned long long)host_tsd_base, (unsigned long long)(uintptr_t)g_native_host_pthread,
           (unsigned long long)identifier.thread_handle, (unsigned long long)(uintptr_t)guest_base,
           (unsigned long long)wine_tsd_base, (unsigned long long)sizeof(wine_tsd_base));
    ms_log("TSD diagnostics guest_tls=0x%llx/%llu host_self=0x%llx host0=0x%llx/%llu host58=0x%llx/%llu",
           (unsigned long long)guest_tls_pointer, (unsigned long long)guest_read,
           (unsigned long long)(uintptr_t)g_native_host_pthread, (unsigned long long)host_zero,
           (unsigned long long)host_zero_read, (unsigned long long)host_tls_pointer, (unsigned long long)host_tls_read);
    return true;
}

static void metalsharp_eac_host_tsd_leave(void) {
    uintptr_t current_thread = wine_current_thread_token();
    MsWineThreadBridgeState* bridge_state = wine_thread_bridge_state(current_thread);
    if (bridge_state == NULL || bridge_state->host_depth == 0) {
        return;
    }
    bridge_state->host_depth--;
    if (bridge_state->host_depth == 0) {
        void* guest_base = (void*)(uintptr_t)bridge_state->guest_teb;
        if (g_thread_set_tsd_base != NULL && guest_base != NULL) {
            g_thread_set_tsd_base(guest_base);
        }
    }
}

static __attribute__((unused)) int metalsharp_host_pthread_setspecific(pthread_key_t key, const void* value) {
    resolve_host_pthread_symbols();
    if (g_host_pthread_setspecific == NULL) {
        return ENOSYS;
    }
    if (!__atomic_load_n(&g_host_tsd_interpose_ready, __ATOMIC_ACQUIRE)) {
        return g_host_pthread_setspecific(key, value);
    }
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    int result = g_host_pthread_setspecific(key, value);
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    ensure_wine_pe_tls();
    return result;
}

static const void* metalsharp_host_pthread_getspecific(pthread_key_t key) {
    resolve_host_pthread_symbols();
    if (g_host_pthread_getspecific == NULL) {
        return NULL;
    }
    if (!__atomic_load_n(&g_host_tsd_interpose_ready, __ATOMIC_ACQUIRE)) {
        return g_host_pthread_getspecific(key);
    }
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    const void* result = g_host_pthread_getspecific(key);
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    return result;
}

static void patch_wine115_pthread_getspecific_stub(void) {
    if (g_ntdll_pthread_getspecific_patched || g_ntdll_image_base == 0) {
        return;
    }
    resolve_host_pthread_symbols();
    if (g_host_pthread_getspecific == NULL) {
        ms_log("Wine 11.5 pthread_getspecific bridge unavailable");
        return;
    }
    /* In the exact installed ntdll.so the _pthread_getspecific stub at
     * 0x846ec jumps through __DATA_CONST+0x951c0.  Directly reading this
     * Rosetta data-const slot works where mach_vm_read_overwrite rejects the
     * translated page. */
    volatile uintptr_t* slot = (volatile uintptr_t*)(g_ntdll_image_base + 0x951c0u);
    uintptr_t current = *slot;
    uint8_t* stub = (uint8_t*)(g_ntdll_image_base + 0x846ecu);
    if (*stub == 0xe9) {
        g_ntdll_pthread_getspecific_patched = 1;
        return;
    }
    int64_t displacement = (int64_t)(uintptr_t)&metalsharp_host_pthread_getspecific - (int64_t)((uintptr_t)stub + 5u);
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        ms_log("Wine 11.5 pthread_getspecific bridge is out of rel32 range stub=0x%llx replacement=0x%llx",
               (unsigned long long)(uintptr_t)stub,
               (unsigned long long)(uintptr_t)&metalsharp_host_pthread_getspecific);
        return;
    }
    uintptr_t stub_page = (uintptr_t)stub & ~(uintptr_t)(MS_PAGE_SIZE - 1u);
    if (mprotect((void*)stub_page, MS_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        ms_log("Wine 11.5 pthread_getspecific stub is not writable target=0x%llx errno=%d",
               (unsigned long long)(uintptr_t)stub, errno);
        return;
    }
    uint8_t branch[6] = {0xe9, 0, 0, 0, 0, 0x90};
    int32_t relative = (int32_t)displacement;
    memcpy(branch + 1, &relative, sizeof(relative));
    memcpy(stub, branch, sizeof(branch));
    __builtin___clear_cache((char*)stub, (char*)stub + sizeof(branch));
    (void)mprotect((void*)stub_page, MS_PAGE_SIZE, PROT_READ | PROT_EXEC);
    g_ntdll_pthread_getspecific_patched = 1;
    ms_log("patched Wine 11.5 pthread_getspecific stub target=0x%llx got=0x%llx original=0x%llx replacement=0x%llx",
           (unsigned long long)(uintptr_t)stub, (unsigned long long)(uintptr_t)slot, (unsigned long long)current,
           (unsigned long long)(uintptr_t)&metalsharp_host_pthread_getspecific);
    return;
#if 0
    uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(MS_PAGE_SIZE - 1u);
    if (mprotect((void *)page, MS_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        ms_log("Wine 11.5 pthread_getspecific slot is not writable target=0x%llx errno=%d",
               (unsigned long long)(uintptr_t)slot, errno);
        return;
    }
    *slot = (uintptr_t)&metalsharp_host_pthread_getspecific;
    __builtin___clear_cache((char *)slot, (char *)slot + sizeof(*slot));
    (void)mprotect((void *)page, MS_PAGE_SIZE, PROT_READ);
    g_ntdll_pthread_getspecific_patched = 1;
    ms_log("patched Wine 11.5 pthread_getspecific slot target=0x%llx original=0x%llx replacement=0x%llx",
           (unsigned long long)(uintptr_t)slot,
           (unsigned long long)current,
           (unsigned long long)(uintptr_t)&metalsharp_host_pthread_getspecific);
#endif
}

static __attribute__((unused)) pthread_t metalsharp_host_pthread_self(void) {
    resolve_host_pthread_symbols();
    if (g_host_pthread_self == NULL) {
        return (pthread_t)0;
    }
    if (!__atomic_load_n(&g_host_tsd_interpose_ready, __ATOMIC_ACQUIRE)) {
        return g_host_pthread_self();
    }
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    pthread_t result = g_host_pthread_self();
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    ensure_wine_pe_tls();
    return result;
}

static bool is_proc_maps_path(const char* path) {
    if (path == NULL) {
        return false;
    }
    const char* proc = strstr(path, "/proc/");
    if (proc == NULL) {
        return false;
    }
    size_t length = strlen(path);
    return length >= 5 && strcmp(path + length - 5, "/maps") == 0;
}

static bool is_wide_proc_maps_path(const uint16_t* path) {
    if (path == NULL) {
        return false;
    }
    char ascii[MS_MAX_PATH];
    size_t index = 0;
    for (; path[index] != 0 && index + 1 < sizeof(ascii); index++) {
        uint16_t codepoint = path[index];
        ascii[index] = codepoint < 0x80 ? (char)codepoint : '?';
    }
    ascii[index] = '\0';
    return is_proc_maps_path(ascii) || (strstr(ascii, "proc") != NULL && strstr(ascii, "maps") != NULL);
}

extern int32_t ntdll_get_unix_file_name(const uint16_t* dos, char** unix_name, unsigned int disposition)
    __attribute__((weak_import));

static int32_t metalsharp_eac_ntdll_get_unix_file_name(const uint16_t* dos, char** unix_name,
                                                       unsigned int disposition) {
    if (is_wide_proc_maps_path(dos) && unix_name != NULL && g_maps_path[0] != '\0') {
        char* replacement = strdup(g_maps_path);
        if (replacement == NULL) {
            return (int32_t)0xC0000017L; /* STATUS_NO_MEMORY */
        }
        *unix_name = replacement;
        ms_log("redirected wine_get_unix_file_name(/proc/*/maps) -> %s", g_maps_path);
        return 0;
    }
    MsNtdllGetUnixFileNameFn real_fn = g_original_ntdll_get_unix_file_name;
    return real_fn != NULL ? real_fn(dos, unix_name, disposition) : (int32_t)0xC0000034L;
}

static void write_absolute_jump(uint8_t* destination, const void* target) {
    destination[0] = 0x48;
    destination[1] = 0xb8; /* movabs rax, imm64 */
    uintptr_t address = (uintptr_t)target;
    memcpy(destination + 2, &address, sizeof(address));
    destination[10] = 0xff; /* jmp rax */
    destination[11] = 0xe0;
}

/*
 * ntdll.so is loaded by Wine after DYLD_INSERT_LIBRARIES constructors run,
 * and its Unix-call table binds ntdll_get_unix_file_name internally.  A dyld
 * interpose tuple therefore cannot replace that already-bound call.  Once
 * ntdll is present, install a small in-process entry trampoline instead.  It
 * only redirects the /proc maps lookup; all other path conversions execute
 * the original function through the copied prologue.
 */
static bool patch_ntdll_unix_name(void) {
    if (__atomic_load_n(&g_ntdll_patch_done, __ATOMIC_ACQUIRE)) {
        return true;
    }
    void* target = dlsym(RTLD_DEFAULT, "ntdll_get_unix_file_name");
    if (target == NULL) {
        void* ntdll_handle = dlopen("ntdll.so", RTLD_NOW | RTLD_NOLOAD);
        if (ntdll_handle == NULL) {
            ntdll_handle = dlopen("/Users/averyfelts/.metalsharp/runtime/wine/lib/wine/x86_64-unix/ntdll.so",
                                  RTLD_NOW | RTLD_NOLOAD);
        }
        if (ntdll_handle != NULL) {
            target = dlsym(ntdll_handle, "ntdll_get_unix_file_name");
        }
    }
    if (target == NULL || target == (void*)&metalsharp_eac_ntdll_get_unix_file_name) {
        return false;
    }
    g_ntdll_unix_name_target = target;

    const size_t overwritten = 15; /* complete ntdll.so prologue through 0x1cdf */
    const size_t trampoline_size = 32;
    uint8_t* trampoline =
        mmap(NULL, trampoline_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (trampoline == MAP_FAILED) {
        ms_log("cannot allocate ntdll path trampoline errno=%d", errno);
        return false;
    }
    memcpy(trampoline, target, overwritten);
    write_absolute_jump(trampoline + overwritten, (uint8_t*)target + overwritten);

    uintptr_t page = (uintptr_t)target & ~(uintptr_t)(MS_PAGE_SIZE - 1u);
    if (mprotect((void*)page, MS_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        ms_log("cannot make ntdll path page writable errno=%d", errno);
        munmap(trampoline, trampoline_size);
        return false;
    }
    uint8_t patch[15];
    memset(patch, 0x90, sizeof(patch));
    write_absolute_jump(patch, (const void*)&metalsharp_eac_ntdll_get_unix_file_name);
    memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache((char*)target, (char*)target + sizeof(patch));
    (void)mprotect((void*)page, MS_PAGE_SIZE, PROT_READ | PROT_EXEC);
    g_original_ntdll_get_unix_file_name = (MsNtdllGetUnixFileNameFn)trampoline;
    __atomic_store_n(&g_ntdll_patch_done, 1, __ATOMIC_RELEASE);
    ms_log("patched exact Wine 11.5 ntdll_get_unix_file_name at 0x%llx", (unsigned long long)(uintptr_t)target);
    return true;
}

typedef struct {
    uint32_t characteristics;
    uint32_t timestamp;
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t name;
    uint32_t ordinal_base;
    uint32_t number_of_functions;
    uint32_t number_of_names;
    uint32_t address_of_functions;
    uint32_t address_of_names;
    uint32_t address_of_name_ordinals;
} MsPeExportDirectory;

static bool process_read(mach_vm_address_t address, void* buffer, mach_vm_size_t size) {
    mach_vm_size_t read_size = 0;
    kern_return_t result =
        mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)buffer, &read_size);
    return result == KERN_SUCCESS && read_size == size;
}

static bool process_read_c_string(mach_vm_address_t address, char* buffer, size_t capacity) {
    if (capacity == 0) {
        return false;
    }
    for (size_t index = 0; index + 1 < capacity; index++) {
        if (!process_read(address + index, &buffer[index], 1)) {
            buffer[index] = '\0';
            return false;
        }
        if (buffer[index] == '\0') {
            return true;
        }
    }
    buffer[capacity - 1] = '\0';
    return false;
}

static bool find_x86_kernel32_prologue(void** target_out) {
    /* This is the stable, non-relocated beginning of Wine 11.5's builtin
     * kernel32!wine_get_unix_file_name.  Wine maps builtin PE sections
     * without retaining the DOS header, so an MZ/export scan is insufficient
     * on the exact runtime. */
    static const uint8_t prologue[] = {
        0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x55, 0x57, 0x56, 0x53,
        0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x45, 0x31, 0xc9,
    };
    mach_vm_address_t cursor = 0;
    while (cursor < UINT64_MAX) {
        mach_vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name = MACH_PORT_NULL;
        mach_vm_address_t region_start = cursor;
        kern_return_t result = mach_vm_region(mach_task_self(), &region_start, &region_size, VM_REGION_BASIC_INFO_64,
                                              (vm_region_info_t)&info, &info_count, &object_name);
        if (object_name != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object_name);
        }
        if (result != KERN_SUCCESS || region_size == 0) {
            return false;
        }
        cursor = region_start + region_size;
        if ((info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE)) {
            continue;
        }
        for (mach_vm_size_t offset = 0; offset < region_size;) {
            mach_vm_size_t request = region_size - offset;
            if (request > 0x10000) {
                request = 0x10000;
            }
            uint8_t buffer[0x10000];
            mach_vm_size_t read_size = 0;
            if (mach_vm_read_overwrite(mach_task_self(), region_start + offset, request, (mach_vm_address_t)buffer,
                                       &read_size) == KERN_SUCCESS) {
                for (mach_vm_size_t index = 0; index + sizeof(prologue) <= read_size; index++) {
                    if (memcmp(buffer + index, prologue, sizeof(prologue)) == 0) {
                        *target_out = (void*)(region_start + offset + index);
                        ms_log("found kernel32.dll wine_get_unix_file_name by Wine 11.5 prologue at 0x%llx",
                               (unsigned long long)(uintptr_t)*target_out);
                        return true;
                    }
                }
            }
            if (request == 0) {
                break;
            }
            offset += request;
        }
    }
    return false;
}

static void* find_kernel32_unix_name_export(void) {
    static const uint8_t known_prologue[] = {
        0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x55, 0x57, 0x56, 0x53,
        0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x45, 0x31, 0xc9,
    };
    uint8_t known_bytes[sizeof(known_prologue)];
    if (process_read(MS_WINE115_KERNEL32_BASE + 0x32560u, known_bytes, sizeof(known_bytes)) &&
        memcmp(known_bytes, known_prologue, sizeof(known_prologue)) == 0) {
        void* known_target = (void*)(MS_WINE115_KERNEL32_BASE + 0x32560u);
        ms_log("found exact Wine 11.5 kernel32 base at 0x%llx", (unsigned long long)MS_WINE115_KERNEL32_BASE);
        return known_target;
    }
    mach_vm_address_t cursor = 0;
    unsigned int region_count = 0;
    unsigned int mz_count = 0;
    while (cursor < UINT64_MAX) {
        mach_vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name = MACH_PORT_NULL;
        mach_vm_address_t region_start = cursor;
        kern_return_t result = mach_vm_region(mach_task_self(), &region_start, &region_size, VM_REGION_BASIC_INFO_64,
                                              (vm_region_info_t)&info, &info_count, &object_name);
        if (object_name != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), object_name);
        }
        if (result != KERN_SUCCESS || region_size == 0) {
            break;
        }
        region_count++;
        cursor = region_start + region_size;
        if ((info.protection & VM_PROT_READ) == 0 || region_size < 0x1000) {
            continue;
        }

        uint8_t header[0x1000];
        if (!process_read(region_start, header, sizeof(header)) || header[0] != 'M' || header[1] != 'Z') {
            continue;
        }
        mz_count++;
        uint32_t pe_offset;
        memcpy(&pe_offset, header + 0x3c, sizeof(pe_offset));
        if (pe_offset > sizeof(header) - 0x100 || pe_offset + 4 + 20 + 2 > sizeof(header)) {
            continue;
        }
        uint32_t pe_signature;
        memcpy(&pe_signature, header + pe_offset, sizeof(pe_signature));
        if (pe_signature != 0x00004550) {
            continue;
        }
        uint16_t optional_magic;
        memcpy(&optional_magic, header + pe_offset + 4 + 20, sizeof(optional_magic));
        if (optional_magic != 0x20b) {
            continue;
        }
        mach_vm_address_t optional = region_start + pe_offset + 4 + 20;
        uint32_t export_rva;
        if (!process_read(optional + 112, &export_rva, sizeof(export_rva)) || export_rva == 0) {
            continue;
        }
        MsPeExportDirectory exports;
        if (!process_read(region_start + export_rva, &exports, sizeof(exports))) {
            continue;
        }
        if (exports.number_of_names == 0 || exports.number_of_names > 10000) {
            continue;
        }
        for (uint32_t index = 0; index < exports.number_of_names; index++) {
            uint32_t name_rva;
            if (!process_read(region_start + exports.address_of_names + index * sizeof(uint32_t), &name_rva,
                              sizeof(name_rva))) {
                break;
            }
            char name[96];
            if (!process_read_c_string(region_start + name_rva, name, sizeof(name))) {
                continue;
            }
            if (strcmp(name, "wine_get_unix_file_name") != 0) {
                continue;
            }
            uint16_t ordinal;
            uint32_t function_rva;
            if (!process_read(region_start + exports.address_of_name_ordinals + index * sizeof(uint16_t), &ordinal,
                              sizeof(ordinal)) ||
                !process_read(region_start + exports.address_of_functions + ordinal * sizeof(uint32_t), &function_rva,
                              sizeof(function_rva))) {
                return NULL;
            }
            ms_log("found kernel32.dll wine_get_unix_file_name base=0x%llx rva=0x%x", (unsigned long long)region_start,
                   function_rva);
            return (void*)(region_start + function_rva);
        }
    }
    void* prologue_target = NULL;
    if (find_x86_kernel32_prologue(&prologue_target)) {
        return prologue_target;
    }
    if (region_count > 0 && !__atomic_exchange_n(&g_kernel32_scan_reported, 1, __ATOMIC_ACQ_REL)) {
        ms_log("kernel32 export scan found no target regions=%u mz=%u pid=%d", region_count, mz_count, (int)getpid());
    }
    return NULL;
}

static char* __attribute__((ms_abi)) metalsharp_eac_kernel32_get_unix_file_name(const uint16_t* dos) {
    if (is_wide_proc_maps_path(dos) && g_maps_path[0] != '\0') {
        char* replacement = strdup(g_maps_path);
        if (replacement == NULL) {
            return NULL;
        }
        ms_log("redirected kernel32 wine_get_unix_file_name(/proc/*/maps) -> %s", g_maps_path);
        return replacement;
    }
    return g_original_kernel32_get_unix_file_name != NULL ? g_original_kernel32_get_unix_file_name(dos) : NULL;
}

static void* metalsharp_eac_get_proc_address(void* module, const char* name) __attribute__((ms_abi));

static void* metalsharp_eac_get_proc_address(void* module, const char* name) {
    if (name != NULL && strcmp(name, "wine_get_unix_file_name") == 0) {
        ms_log("provided Wine private export wine_get_unix_file_name module=0x%llx",
               (unsigned long long)(uintptr_t)module);
        return (void*)&metalsharp_eac_kernel32_get_unix_file_name;
    }
    return g_original_get_proc_address != NULL ? g_original_get_proc_address(module, name) : NULL;
}

static bool patch_get_proc_address_from_kernel32(void* wine_name_target) {
    if (__atomic_load_n(&g_get_proc_address_patch_done, __ATOMIC_ACQUIRE)) {
        return true;
    }
    /* These RVAs are from the exact MetalSharp Wine 11.5 kernel32.dll that
     * supplied the prologue above.  GetProcAddress is a builtin PE export,
     * so the module base is recoverable without a PE header in memory. */
    uintptr_t kernel32_base = (uintptr_t)wine_name_target - 0x32560u;
    void* target = (void*)(kernel32_base + 0x114c0u);
    const size_t overwritten = 18;
    const size_t trampoline_size = 48;
    uint8_t* trampoline =
        mmap(NULL, trampoline_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (trampoline == MAP_FAILED) {
        ms_log("cannot allocate GetProcAddress trampoline errno=%d", errno);
        return false;
    }
    uint8_t expected[] = {0x48, 0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x60, 0x48, 0x83, 0xe4, 0xf0};
    if (memcmp(target, expected, sizeof(expected)) != 0) {
        ms_log("Wine 11.5 GetProcAddress prologue mismatch at 0x%llx", (unsigned long long)(uintptr_t)target);
        munmap(trampoline, trampoline_size);
        return false;
    }
    memcpy(trampoline, target, overwritten);
    write_absolute_jump(trampoline + overwritten, (uint8_t*)target + overwritten);
    uintptr_t page = (uintptr_t)target & ~(uintptr_t)(MS_PAGE_SIZE - 1u);
    if (mprotect((void*)page, MS_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        ms_log("cannot make GetProcAddress page writable errno=%d", errno);
        munmap(trampoline, trampoline_size);
        return false;
    }
    uint8_t patch[18];
    memset(patch, 0x90, sizeof(patch));
    write_absolute_jump(patch, (const void*)&metalsharp_eac_get_proc_address);
    memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache((char*)target, (char*)target + sizeof(patch));
    (void)mprotect((void*)page, MS_PAGE_SIZE, PROT_READ | PROT_EXEC);
    g_original_get_proc_address = (MsGetProcAddressFn)trampoline;
    __atomic_store_n(&g_get_proc_address_patch_done, 1, __ATOMIC_RELEASE);
    ms_log("patched exact Wine 11.5 kernel32 GetProcAddress at 0x%llx", (unsigned long long)(uintptr_t)target);
    return true;
}

static bool patch_kernel32_unix_name(void) {
    if (__atomic_load_n(&g_kernel32_patch_done, __ATOMIC_ACQUIRE)) {
        return true;
    }
    void* target = find_kernel32_unix_name_export();
    if (target == NULL || target == (void*)&metalsharp_eac_kernel32_get_unix_file_name) {
        return false;
    }
    if (!patch_get_proc_address_from_kernel32(target)) {
        return false;
    }

    /* kernel32's Wine 11.5 builtin prologue is 17 bytes; include the next
     * complete register-save instruction before installing the jump. */
    const size_t overwritten = 20;
    const size_t trampoline_size = 48;
    uint8_t* trampoline =
        mmap(NULL, trampoline_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (trampoline == MAP_FAILED) {
        ms_log("cannot allocate kernel32 path trampoline errno=%d", errno);
        return false;
    }
    memcpy(trampoline, target, overwritten);
    write_absolute_jump(trampoline + overwritten, (uint8_t*)target + overwritten);

    uintptr_t page = (uintptr_t)target & ~(uintptr_t)(MS_PAGE_SIZE - 1u);
    if (mprotect((void*)page, MS_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        ms_log("cannot make kernel32 path page writable errno=%d", errno);
        munmap(trampoline, trampoline_size);
        return false;
    }
    uint8_t patch[20];
    memset(patch, 0x90, sizeof(patch));
    write_absolute_jump(patch, (const void*)&metalsharp_eac_kernel32_get_unix_file_name);
    memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache((char*)target, (char*)target + sizeof(patch));
    (void)mprotect((void*)page, MS_PAGE_SIZE, PROT_READ | PROT_EXEC);
    g_original_kernel32_get_unix_file_name = (MsKernel32GetUnixFileNameFn)trampoline;
    __atomic_store_n(&g_kernel32_patch_done, 1, __ATOMIC_RELEASE);
    ms_log("patched exact Wine 11.5 kernel32 wine_get_unix_file_name at 0x%llx", (unsigned long long)(uintptr_t)target);
    return true;
}

static __attribute__((unused)) void* ntdll_patch_thread(void* unused) {
    (void)unused;
    for (unsigned int attempt = 0; attempt < 1000; attempt++) {
        if (patch_kernel32_unix_name()) {
            return NULL;
        }
        if (patch_ntdll_unix_name()) {
            return NULL;
        }
        if (attempt == 0) {
            ms_log("Wine 11.5 ntdll_get_unix_file_name not visible yet pid=%d", (int)getpid());
        }
        usleep(10000);
    }
    ms_log("Wine 11.5 ntdll path patch target did not appear");
    return NULL;
}

static int create_virtual_maps_fd(int flags, mode_t mode) {
    if (g_elf_mapping == NULL || g_elf_mapping_size == 0 || g_elf_path[0] == '\0') {
        return -1;
    }

    static unsigned int sequence;
    char template_path[128];
    int fd = -1;
    for (unsigned int attempt = 0; attempt < 32 && fd < 0; attempt++) {
        unsigned int value = __atomic_fetch_add(&sequence, 1, __ATOMIC_RELAXED);
        snprintf(template_path, sizeof(template_path), "/tmp/metalsharp-eac-maps-%d-%u", (int)getpid(), value);
        fd = ms_raw_open(template_path, O_RDWR | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) {
        return -1;
    }
    (void)unlink(template_path);

    uintptr_t start = (uintptr_t)g_elf_mapping;
    uintptr_t end = start + g_elf_mapping_size;
    char maps_line[MS_MAX_PATH + 160];
    int length = snprintf(maps_line, sizeof(maps_line), "%016llx-%016llx r-xp 00000000 00:00 0 %s\n",
                          (unsigned long long)start, (unsigned long long)end, g_elf_path);
    if (length <= 0 || (size_t)length >= sizeof(maps_line)) {
        (void)close(fd);
        return -1;
    }
    if (write(fd, maps_line, (size_t)length) != length || lseek(fd, 0, SEEK_SET) < 0) {
        (void)close(fd);
        return -1;
    }
    (void)fchmod(fd, mode == 0 ? 0600 : mode);
    (void)fcntl(fd, F_SETFD, flags & O_CLOEXEC ? FD_CLOEXEC : 0);
    ms_log("virtualized %s -> %s base=0x%llx size=0x%zx", "proc maps", g_elf_path, (unsigned long long)start,
           g_elf_mapping_size);
    return fd;
}

int metalsharp_eac_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if (!g_in_open_hook && is_proc_maps_path(path)) {
        g_in_open_hook = 1;
        int virtual_fd = create_virtual_maps_fd(flags, mode);
        g_in_open_hook = 0;
        if (virtual_fd >= 0) {
            ms_log("intercepted open(%s) => fd %d", path, virtual_fd);
            return virtual_fd;
        }
    }
    if ((flags & O_CREAT) != 0) {
        return ms_raw_open(path, flags, mode);
    }
    return ms_raw_open(path, flags, 0);
}

int metalsharp_eac_openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if (!g_in_open_hook && is_proc_maps_path(path)) {
        g_in_open_hook = 1;
        int virtual_fd = create_virtual_maps_fd(flags, mode);
        g_in_open_hook = 0;
        if (virtual_fd >= 0) {
            ms_log("intercepted openat(%s) => fd %d", path, virtual_fd);
            return virtual_fd;
        }
    }
    if ((flags & O_CREAT) != 0) {
        return ms_raw_openat(dirfd, path, flags, mode);
    }
    return ms_raw_openat(dirfd, path, flags, 0);
}

static void* resolve_substrate_target(const char* name);
static void* load_linux_module_fd(int fd);
static void* lookup_linux_module_symbol(MsLinuxLoadedModule* module, const char* name);
static int metalsharp_eac_dlclose(void* handle);
static const char* metalsharp_eac_dlerror(void);
static void* metalsharp_eac_malloc(size_t size);
static void* metalsharp_eac_calloc(size_t count, size_t size);
static void metalsharp_eac_free(void* pointer);

static bool dump_linux_module_fd(int fd) {
    const char* configured = getenv("METALSHARP_EAC_MODULE_DUMP");
    const char* path = configured != NULL && configured[0] != '\0' ? configured : "/tmp/metalsharp-eac-module.bin";
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        ms_log("cannot stat Linux module fd=%d errno=%d", fd, errno);
        return false;
    }
    int output = ms_raw_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0) {
        ms_log("cannot create Linux module dump path=%s errno=%d", path, errno);
        return false;
    }
    uint8_t* buffer = metalsharp_eac_malloc(1024 * 1024);
    if (buffer == NULL) {
        close(output);
        ms_log("cannot allocate Linux module dump buffer");
        return false;
    }
    off_t offset = 0;
    bool success = true;
    while (offset < st.st_size) {
        size_t request = (size_t)(st.st_size - offset);
        if (request > 1024 * 1024) {
            request = 1024 * 1024;
        }
        ssize_t count = pread(fd, buffer, request, offset);
        if (count <= 0) {
            ms_log("cannot read Linux module fd=%d offset=%lld errno=%d", fd, (long long)offset, errno);
            success = false;
            break;
        }
        ssize_t written = write(output, buffer, (size_t)count);
        if (written != count) {
            ms_log("cannot write Linux module dump path=%s errno=%d", path, errno);
            success = false;
            break;
        }
        offset += count;
    }
    metalsharp_eac_free(buffer);
    close(output);
    ms_log("captured Linux module fd=%d size=%lld path=%s success=%d", fd, (long long)st.st_size, path,
           success ? 1 : 0);
    return success;
}

static void* metalsharp_eac_dlopen(const char* path, int flags) {
    (void)flags;
    ms_log("__libc_dlopen_mode requested path=%s", path != NULL ? path : "<null>");
    resolve_nt_current_teb_from_wine();
    void* direct_teb = g_nt_current_teb != NULL ? g_nt_current_teb() : NULL;
    ms_log("Wine 11.5 NtCurrentTeb direct before host bridge=0x%llx", (unsigned long long)(uintptr_t)direct_teb);
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    ms_log("__libc_dlopen_mode(%s) host_tsd=%d", path != NULL ? path : "<null>", host_tsd ? 1 : 0);
    ms_log("Wine TSD callback diagnostics count=%llu pseudo=%llu mapped=%llu last_requested=0x%llx "
           "last_effective=0x%llx guest_teb=0x%llx",
           (unsigned long long)g_tsd_callback_count, (unsigned long long)g_tsd_pseudo_count,
           (unsigned long long)g_tsd_mapped_count, (unsigned long long)g_tsd_last_requested,
           (unsigned long long)g_tsd_last_effective, (unsigned long long)(uintptr_t)g_last_guest_teb);
    void* result = NULL;
    if (path != NULL && strncmp(path, "/proc/self/fd/", 14) == 0) {
        char* end = NULL;
        long value = strtol(path + 14, &end, 10);
        if (end != path + 14 && *end == '\0' && value >= 0 && value <= INT32_MAX) {
            result = load_linux_module_fd((int)value);
        } else {
            ms_log("invalid Linux module proc fd path=%s", path);
        }
    }
    if (result == NULL) {
        snprintf(g_linux_dlerror, sizeof(g_linux_dlerror), "unsupported Linux dlopen path: %s",
                 path != NULL ? path : "<null>");
    } else {
        ensure_wine_pe_tls();
        patch_wine115_pthread_getspecific_stub();
        start_wine_tls_monitor();
    }
    metalsharp_eac_host_tsd_leave();
    if (result != NULL) {
        /* The Wine thread that called into the Linux loader may differ from
         * the thread that resumes the PE launcher.  Re-run the TEB walk after
         * restoring the guest TSD base so the returning guest thread is
         * covered at the actual ABI boundary. */
        ensure_wine_pe_tls();
        ms_log("guest GS after Linux dlopen return gs_tls=0x%llx expected_teb=0x%llx expected_array=0x%llx",
               (unsigned long long)read_guest_gs_tls_pointer(), (unsigned long long)(uintptr_t)g_last_guest_teb,
               (unsigned long long)(uintptr_t)g_wine_pe_tls_array);
    }
    return result;
}

/* The protected launcher resolves the EAC module's public a-e entry points
 * from inside the Linux image and then calls them while Wine's GS base is
 * still the Windows TEB.  Their implementations immediately use Darwin
 * libunwind/libc, so the same host-TSD boundary used for constructors and
 * dlsym must also surround these Linux entry points.  The current EAC ABI is
 * intentionally small: a, b, c and e take one context pointer; d is a
 * no-argument teardown marker. */
static uintptr_t metalsharp_eac_export_a(void* argument) {
    ms_log("Linux EAC export a enter argument=0x%llx", (unsigned long long)(uintptr_t)argument);
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    uintptr_t result = g_linux_export_a != NULL ? g_linux_export_a(argument) : 0;
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    ensure_wine_pe_tls();
    if (result == 1) {
        __sync_fetch_and_add(&g_eac_export_a_successes, 1);
    }
    ms_log("Linux EAC export a return=0x%llx", (unsigned long long)result);
    if (result == 1) {
        ms_log("EAC_PROOF export_a_success=1 module_base=0x%llx", (unsigned long long)(uintptr_t)g_linux_module.base);
    }
    return result;
}

static uintptr_t metalsharp_eac_export_b(void* argument) {
    ms_log("Linux EAC export b enter argument=0x%llx", (unsigned long long)(uintptr_t)argument);
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    uintptr_t result = g_linux_export_b != NULL ? g_linux_export_b(argument) : 0;
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    ms_log("Linux EAC export b return=0x%llx", (unsigned long long)result);
    return result;
}

static uintptr_t metalsharp_eac_export_c(void* argument) {
    ms_log("Linux EAC export c enter argument=0x%llx", (unsigned long long)(uintptr_t)argument);
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    uintptr_t result = g_linux_export_c != NULL ? g_linux_export_c(argument) : 0;
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    ms_log("Linux EAC export c return=0x%llx", (unsigned long long)result);
    return result;
}

static void metalsharp_eac_export_d(void) {
    ms_log("Linux EAC export d enter");
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    if (g_linux_export_d != NULL) {
        g_linux_export_d();
    }
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    ensure_wine_pe_tls();
    ms_log("Linux EAC export d return");
}

static uintptr_t metalsharp_eac_export_e(void* argument) {
    ms_log("Linux EAC export e enter argument=0x%llx", (unsigned long long)(uintptr_t)argument);
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    uintptr_t result = g_linux_export_e != NULL ? g_linux_export_e(argument) : 0;
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    ensure_wine_pe_tls();
    ms_log("Linux EAC export e return=0x%llx", (unsigned long long)result);
    return result;
}

static void* wrap_linux_export(const char* name, void* target) {
    if (name == NULL || target == NULL) {
        return target;
    }
    if (strcmp(name, "a") == 0) {
        __atomic_fetch_or(&g_eac_export_mask, 1u << 0, __ATOMIC_RELAXED);
        g_linux_export_a = (MsLinuxUnaryExportFn)target;
        return (void*)&metalsharp_eac_export_a;
    }
    if (strcmp(name, "b") == 0) {
        __atomic_fetch_or(&g_eac_export_mask, 1u << 1, __ATOMIC_RELAXED);
        g_linux_export_b = (MsLinuxUnaryExportFn)target;
        return (void*)&metalsharp_eac_export_b;
    }
    if (strcmp(name, "c") == 0) {
        __atomic_fetch_or(&g_eac_export_mask, 1u << 2, __ATOMIC_RELAXED);
        g_linux_export_c = (MsLinuxUnaryExportFn)target;
        return (void*)&metalsharp_eac_export_c;
    }
    if (strcmp(name, "d") == 0) {
        __atomic_fetch_or(&g_eac_export_mask, 1u << 3, __ATOMIC_RELAXED);
        g_linux_export_d = (void (*)(void))target;
        return (void*)&metalsharp_eac_export_d;
    }
    if (strcmp(name, "e") == 0) {
        __atomic_fetch_or(&g_eac_export_mask, 1u << 4, __ATOMIC_RELAXED);
        g_linux_export_e = (MsLinuxUnaryExportFn)target;
        return (void*)&metalsharp_eac_export_e;
    }
    return target;
}

static void* metalsharp_eac_dlsym(void* handle, const char* name) {
    bool host_tsd = metalsharp_eac_host_tsd_enter();
    ms_log("__libc_dlsym(%s) host_tsd=%d", name != NULL ? name : "<null>", host_tsd ? 1 : 0);
    void* result = NULL;
    if (handle == g_linux_module.base && g_linux_module.loaded) {
        result = lookup_linux_module_symbol(&g_linux_module, name);
        if (result != NULL) {
            result = wrap_linux_export(name, result);
            if (host_tsd) {
                metalsharp_eac_host_tsd_leave();
            }
            ensure_wine_pe_tls();
            ms_log("guest GS after Linux dlsym name=%s gs_tls=0x%llx expected_teb=0x%llx",
                   name != NULL ? name : "<null>", (unsigned long long)read_guest_gs_tls_pointer(),
                   (unsigned long long)(uintptr_t)g_last_guest_teb);
            return result;
        }
    }
    result = resolve_substrate_target(name);
    if (host_tsd) {
        metalsharp_eac_host_tsd_leave();
    }
    ensure_wine_pe_tls();
    ms_log("guest GS after Linux dlsym fallback name=%s gs_tls=0x%llx expected_teb=0x%llx",
           name != NULL ? name : "<null>", (unsigned long long)read_guest_gs_tls_pointer(),
           (unsigned long long)(uintptr_t)g_last_guest_teb);
    return result;
}

static void* metalsharp_eac_unimplemented(void) {
    ms_log("unimplemented Linux ABI symbol called");
    return NULL;
}

static int* metalsharp_eac_linux_errno_location(void);

static int* metalsharp_eac_errno_location(void) {
    return metalsharp_eac_linux_errno_location();
}

typedef struct {
    uintptr_t module;
    uintptr_t offset;
} MsLinuxTlsIndex;

static void* metalsharp_eac_tls_get_addr(const MsLinuxTlsIndex* index) {
    if (index == NULL) {
        return NULL;
    }
    if (g_linux_tls_block == NULL) {
        size_t size = g_linux_module.tls_size != 0 ? g_linux_module.tls_size : 16;
        g_linux_tls_block = metalsharp_eac_calloc(1, size + 16);
    }
    if (g_linux_tls_block == NULL || index->offset >= g_linux_module.tls_size + 16) {
        return NULL;
    }
    return (uint8_t*)g_linux_tls_block + index->offset;
}

typedef struct {
    uint64_t magic;
    size_t mapping_size;
    size_t requested_size;
} MsLinuxAllocationHeader;

#define MS_LINUX_ALLOCATION_MAGIC UINT64_C(0x4d53454143414c4c)

static uint64_t align_up_linux(uint64_t value);

static void* metalsharp_eac_malloc(size_t size) {
    if (size == 0) {
        size = 1;
    }
    if (size > SIZE_MAX - sizeof(MsLinuxAllocationHeader) - MS_PAGE_SIZE) {
        errno = ENOMEM;
        return NULL;
    }
    size_t mapping_size = align_up_linux(size + sizeof(MsLinuxAllocationHeader));
    void* mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        return NULL;
    }
    MsLinuxAllocationHeader* header = mapping;
    header->magic = MS_LINUX_ALLOCATION_MAGIC;
    header->mapping_size = mapping_size;
    header->requested_size = size;
    return header + 1;
}

static void* metalsharp_eac_calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = count * size;
    void* result = metalsharp_eac_malloc(total);
    if (result != NULL) {
        memset(result, 0, total == 0 ? 1 : total);
    }
    return result;
}

static bool is_metalsharp_eac_allocation(void* pointer, MsLinuxAllocationHeader** header_out) {
    if (pointer == NULL) {
        return false;
    }
    MsLinuxAllocationHeader* header = (MsLinuxAllocationHeader*)pointer - 1;
    if (header->magic != MS_LINUX_ALLOCATION_MAGIC || header->mapping_size == 0 ||
        (header->mapping_size & (MS_PAGE_SIZE - 1u)) != 0) {
        return false;
    }
    if (header_out != NULL) {
        *header_out = header;
    }
    return true;
}

static void metalsharp_eac_free(void* pointer) {
    MsLinuxAllocationHeader* header = NULL;
    if (is_metalsharp_eac_allocation(pointer, &header)) {
        size_t mapping_size = header->mapping_size;
        header->magic = 0;
        (void)munmap(header, mapping_size);
    }
}

static void* metalsharp_eac_realloc(void* pointer, size_t size) {
    if (pointer == NULL) {
        return metalsharp_eac_malloc(size);
    }
    MsLinuxAllocationHeader* header = NULL;
    if (!is_metalsharp_eac_allocation(pointer, &header)) {
        return metalsharp_eac_malloc(size);
    }
    void* replacement = metalsharp_eac_malloc(size);
    if (replacement != NULL) {
        size_t copy_size = header->requested_size < size ? header->requested_size : size;
        memcpy(replacement, pointer, copy_size);
    }
    metalsharp_eac_free(pointer);
    return replacement;
}

static void* metalsharp_eac_operator_new(size_t size) {
    return metalsharp_eac_malloc(size);
}

static void metalsharp_eac_operator_delete(void* pointer) {
    metalsharp_eac_free(pointer);
}

static void metalsharp_eac_noop(void) {}

#define MS_LINUX_THREAD_SLOTS   128
#define MS_LINUX_SPECIFIC_SLOTS 256

typedef struct {
    uint32_t thread;
    int errno_value;
    bool used;
} MsLinuxThreadSlot;

typedef struct {
    uint32_t thread;
    uint32_t key;
    const void* value;
    bool used;
} MsLinuxSpecificSlot;

static MsLinuxThreadSlot g_linux_thread_slots[MS_LINUX_THREAD_SLOTS];
static MsLinuxSpecificSlot g_linux_specific_slots[MS_LINUX_SPECIFIC_SLOTS];
static volatile int g_linux_thread_slot_lock;

static uint32_t metalsharp_eac_thread_token(void) {
    return (uint32_t)mach_thread_self();
}

static int* metalsharp_eac_linux_errno_location(void) {
    uint32_t thread = metalsharp_eac_thread_token();
    while (__sync_lock_test_and_set(&g_linux_thread_slot_lock, 1) != 0) {
        __builtin_ia32_pause();
    }
    MsLinuxThreadSlot* free_slot = NULL;
    for (size_t index = 0; index < MS_LINUX_THREAD_SLOTS; index++) {
        MsLinuxThreadSlot* slot = &g_linux_thread_slots[index];
        if (slot->used && slot->thread == thread) {
            __sync_lock_release(&g_linux_thread_slot_lock);
            return &slot->errno_value;
        }
        if (!slot->used && free_slot == NULL) {
            free_slot = slot;
        }
    }
    if (free_slot != NULL) {
        free_slot->thread = thread;
        free_slot->errno_value = 0;
        free_slot->used = true;
        __sync_lock_release(&g_linux_thread_slot_lock);
        return &free_slot->errno_value;
    }
    __sync_lock_release(&g_linux_thread_slot_lock);
    static int fallback_errno;
    return &fallback_errno;
}

static int metalsharp_eac_pthread_setspecific(uint32_t key, const void* value) {
    uint32_t thread = metalsharp_eac_thread_token();
    while (__sync_lock_test_and_set(&g_linux_thread_slot_lock, 1) != 0) {
        __builtin_ia32_pause();
    }
    MsLinuxSpecificSlot* free_slot = NULL;
    for (size_t index = 0; index < MS_LINUX_SPECIFIC_SLOTS; index++) {
        MsLinuxSpecificSlot* slot = &g_linux_specific_slots[index];
        if (slot->used && slot->thread == thread && slot->key == key) {
            if (value == NULL) {
                slot->used = false;
            } else {
                slot->value = value;
            }
            __sync_lock_release(&g_linux_thread_slot_lock);
            return 0;
        }
        if (!slot->used && free_slot == NULL) {
            free_slot = slot;
        }
    }
    if (value != NULL && free_slot != NULL) {
        free_slot->thread = thread;
        free_slot->key = key;
        free_slot->value = value;
        free_slot->used = true;
    }
    __sync_lock_release(&g_linux_thread_slot_lock);
    return value == NULL || free_slot != NULL ? 0 : EAGAIN;
}

static void* metalsharp_eac_pthread_getspecific(uint32_t key) {
    uint32_t thread = metalsharp_eac_thread_token();
    while (__sync_lock_test_and_set(&g_linux_thread_slot_lock, 1) != 0) {
        __builtin_ia32_pause();
    }
    for (size_t index = 0; index < MS_LINUX_SPECIFIC_SLOTS; index++) {
        MsLinuxSpecificSlot* slot = &g_linux_specific_slots[index];
        if (slot->used && slot->thread == thread && slot->key == key) {
            void* value = (void*)slot->value;
            __sync_lock_release(&g_linux_thread_slot_lock);
            return value;
        }
    }
    __sync_lock_release(&g_linux_thread_slot_lock);
    return NULL;
}

static int metalsharp_eac_pthread_key_delete(uint32_t key) {
    while (__sync_lock_test_and_set(&g_linux_thread_slot_lock, 1) != 0) {
        __builtin_ia32_pause();
    }
    for (size_t index = 0; index < MS_LINUX_SPECIFIC_SLOTS; index++) {
        if (g_linux_specific_slots[index].used && g_linux_specific_slots[index].key == key) {
            g_linux_specific_slots[index].used = false;
        }
    }
    __sync_lock_release(&g_linux_thread_slot_lock);
    return 0;
}

static void* metalsharp_eac_pthread_self(void) {
    return (void*)(uintptr_t)metalsharp_eac_thread_token();
}

typedef struct MsLinuxMutexEntry {
    void* guest;
    volatile int state;
    uint32_t owner;
    uint32_t recursion;
    bool recursive;
    struct MsLinuxMutexEntry* next;
} MsLinuxMutexEntry;

static MsLinuxMutexEntry* g_linux_mutex_entries;
static volatile int g_linux_mutex_registry_lock;

static void lock_linux_mutex_registry(void) {
    while (__sync_lock_test_and_set(&g_linux_mutex_registry_lock, 1) != 0) {
        __builtin_ia32_pause();
    }
}

static void unlock_linux_mutex_registry(void) {
    __sync_lock_release(&g_linux_mutex_registry_lock);
}

static MsLinuxMutexEntry* find_linux_mutex_entry(void* guest) {
    for (MsLinuxMutexEntry* entry = g_linux_mutex_entries; entry != NULL; entry = entry->next) {
        if (entry->guest == guest) {
            return entry;
        }
    }
    return NULL;
}

static MsLinuxMutexEntry* get_linux_mutex_entry(void* guest, bool create, const int* guest_attr) {
    if (guest == NULL) {
        return NULL;
    }
    lock_linux_mutex_registry();
    MsLinuxMutexEntry* entry = find_linux_mutex_entry(guest);
    if (entry == NULL && create) {
        entry = metalsharp_eac_calloc(1, sizeof(*entry));
        if (entry != NULL) {
            entry->guest = guest;
            entry->recursive = guest_attr != NULL && *guest_attr != 0;
            entry->next = g_linux_mutex_entries;
            g_linux_mutex_entries = entry;
        }
    }
    unlock_linux_mutex_registry();
    return entry;
}

static int metalsharp_eac_pthread_mutexattr_init(int* attribute) {
    ms_log("Linux pthread_mutexattr_init guest=0x%llx", (unsigned long long)(uintptr_t)attribute);
    if (attribute == NULL) {
        return EINVAL;
    }
    *attribute = 0;
    return 0;
}

static int metalsharp_eac_pthread_mutexattr_destroy(int* attribute) {
    ms_log("Linux pthread_mutexattr_destroy guest=0x%llx", (unsigned long long)(uintptr_t)attribute);
    (void)attribute;
    return 0;
}

static int metalsharp_eac_pthread_mutexattr_settype(int* attribute, int type) {
    ms_log("Linux pthread_mutexattr_settype guest=0x%llx type=%d", (unsigned long long)(uintptr_t)attribute, type);
    if (attribute == NULL) {
        return EINVAL;
    }
    *attribute = type;
    return 0;
}

static int metalsharp_eac_pthread_mutex_init(void* guest, const int* attribute) {
    ms_log("Linux pthread_mutex_init guest=0x%llx attr=0x%llx", (unsigned long long)(uintptr_t)guest,
           (unsigned long long)(uintptr_t)attribute);
    int result = get_linux_mutex_entry(guest, true, attribute) != NULL ? 0 : ENOMEM;
    ms_log("Linux pthread_mutex_init result=%d", result);
    return result;
}

static int metalsharp_eac_pthread_mutex_destroy(void* guest) {
    ms_log("Linux pthread_mutex_destroy guest=0x%llx", (unsigned long long)(uintptr_t)guest);
    if (guest == NULL) {
        return EINVAL;
    }
    lock_linux_mutex_registry();
    MsLinuxMutexEntry* entry = find_linux_mutex_entry(guest);
    if (entry != NULL && entry->state != 0) {
        unlock_linux_mutex_registry();
        return EBUSY;
    }
    if (entry != NULL) {
        entry->owner = 0;
        entry->recursion = 0;
    }
    unlock_linux_mutex_registry();
    if (entry == NULL) {
        return 0;
    }
    return 0;
}

static int metalsharp_eac_pthread_mutex_lock(void* guest) {
    ms_log("Linux pthread_mutex_lock guest=0x%llx", (unsigned long long)(uintptr_t)guest);
    MsLinuxMutexEntry* entry = get_linux_mutex_entry(guest, true, NULL);
    if (entry == NULL) {
        return ENOMEM;
    }
    uint32_t owner = (uint32_t)mach_thread_self();
    if (entry->recursive && entry->owner == owner && entry->state != 0) {
        entry->recursion++;
        return 0;
    }
    while (__sync_lock_test_and_set(&entry->state, 1) != 0) {
        __builtin_ia32_pause();
    }
    entry->owner = owner;
    entry->recursion = 1;
    return 0;
}

static int metalsharp_eac_pthread_mutex_unlock(void* guest) {
    ms_log("Linux pthread_mutex_unlock guest=0x%llx", (unsigned long long)(uintptr_t)guest);
    MsLinuxMutexEntry* entry = get_linux_mutex_entry(guest, false, NULL);
    if (entry == NULL || entry->state == 0) {
        return 0;
    }
    if (entry->recursive && entry->recursion > 1) {
        entry->recursion--;
        return 0;
    }
    entry->owner = 0;
    entry->recursion = 0;
    __sync_lock_release(&entry->state);
    return 0;
}

static int metalsharp_eac_pthread_mutex_trylock(void* guest) {
    ms_log("Linux pthread_mutex_trylock guest=0x%llx", (unsigned long long)(uintptr_t)guest);
    MsLinuxMutexEntry* entry = get_linux_mutex_entry(guest, true, NULL);
    if (entry == NULL) {
        return ENOMEM;
    }
    uint32_t owner = (uint32_t)mach_thread_self();
    if (entry->recursive && entry->owner == owner && entry->state != 0) {
        entry->recursion++;
        return 0;
    }
    if (__sync_lock_test_and_set(&entry->state, 1) != 0) {
        return EBUSY;
    }
    entry->owner = owner;
    entry->recursion = 1;
    return 0;
}

static int metalsharp_eac_pthread_key_create(uint32_t* key, void (*destructor)(void*)) {
    if (key == NULL) {
        return EINVAL;
    }
    (void)destructor;
    static volatile uint32_t sequence = 1;
    *key = __sync_fetch_and_add(&sequence, 1);
    return 0;
}

typedef void (*MsCxaDestructor)(void*);
typedef struct MsCxaExitEntry {
    MsCxaDestructor destructor;
    void* argument;
    void* dso_handle;
    struct MsCxaExitEntry* next;
} MsCxaExitEntry;

static MsCxaExitEntry* g_linux_cxa_exit_entries;

static int metalsharp_eac_cxa_atexit(MsCxaDestructor destructor, void* argument, void* dso_handle) {
    if (destructor == NULL) {
        return 0;
    }
    MsCxaExitEntry* entry = metalsharp_eac_calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return ENOMEM;
    }
    entry->destructor = destructor;
    entry->argument = argument;
    entry->dso_handle = dso_handle;
    entry->next = g_linux_cxa_exit_entries;
    g_linux_cxa_exit_entries = entry;
    ms_log("registered Linux EAC destructor dso=0x%llx", (unsigned long long)(uintptr_t)dso_handle);
    return 0;
}

static void metalsharp_eac_cxa_finalize(void* dso_handle) {
    /* The module remains mapped for the life of the launcher.  Keep the
     * registrations in the Linux substrate instead of passing an ELF dso
     * handle to dyld's __cxa_finalize, whose data structures are unrelated. */
    ms_log("Linux EAC __cxa_finalize dso=0x%llx deferred", (unsigned long long)(uintptr_t)dso_handle);
}

/* Linux's dl_iterate_phdr callback ABI is stable and deliberately smaller
 * than the host dyld image model.  Report the loaded EAC image through the
 * same program-header table used by the loader; callers that only need to
 * discover their own image can then use the real mapped addresses. */
typedef struct {
    uintptr_t dlpi_addr;
    const char* dlpi_name;
    const MsElfProgramHeader* dlpi_phdr;
    uint16_t dlpi_phnum;
    uint16_t reserved0;
    uint32_t reserved1;
    uint64_t dlpi_adds;
    uint64_t dlpi_subs;
    size_t dlpi_tls_modid;
    uintptr_t dlpi_tls_data;
} MsLinuxDlPhdrInfo;

typedef int (*MsLinuxDlPhdrCallback)(MsLinuxDlPhdrInfo* info, size_t size, void* data);

static int metalsharp_eac_dl_iterate_phdr(MsLinuxDlPhdrCallback callback, void* data) {
    if (callback == NULL || !g_linux_module.loaded) {
        return 0;
    }
    MsLinuxDlPhdrInfo info = {
        .dlpi_addr = (uintptr_t)g_linux_module.base,
        .dlpi_name = "[metalsharp-eac-linux-module]",
        .dlpi_phdr = g_linux_module.program_headers,
        .dlpi_phnum = g_linux_module.program_count,
        .dlpi_tls_modid = g_linux_module.tls_size != 0 ? 1 : 0,
        .dlpi_tls_data = (uintptr_t)g_linux_tls_block,
    };
    return callback(&info, sizeof(info), data);
}

static long metalsharp_eac_prctl(int option, ...) {
    /* Linux callers use prctl for process metadata and dumpability.  There
     * is no Darwin equivalent for every option; options that only establish
     * advisory metadata are successful, while unknown operations retain a
     * real errno instead of pretending that a security operation happened. */
    if (option == 15 /* PR_SET_NAME */ || option == 4 /* PR_SET_DUMPABLE */ || option == 1 /* PR_SET_PDEATHSIG */) {
        return 0;
    }
    errno = ENOSYS;
    return -1;
}

static char* metalsharp_eac_secure_getenv(const char* name) {
    return getenv(name);
}

static const char* metalsharp_eac_gettext(const char* message) {
    return message != NULL ? message : "";
}

typedef struct {
    int64_t seconds;
    int64_t nanoseconds;
} MsLinuxTimespec;

typedef struct {
    uint64_t device;
    uint64_t inode;
    uint64_t link_count;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t padding0;
    uint64_t rdevice;
    int64_t size;
    int64_t block_size;
    int64_t blocks;
    MsLinuxTimespec access_time;
    MsLinuxTimespec modify_time;
    MsLinuxTimespec change_time;
    int64_t reserved[3];
} MsLinuxStat;

static int metalsharp_eac_xstat(int version, const char* path, MsLinuxStat* result) {
    (void)version;
    if (path == NULL || result == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct stat host_stat;
    if (stat(path, &host_stat) != 0) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->device = (uint64_t)host_stat.st_dev;
    result->inode = (uint64_t)host_stat.st_ino;
    result->link_count = (uint64_t)host_stat.st_nlink;
    result->mode = (uint32_t)host_stat.st_mode;
    result->uid = (uint32_t)host_stat.st_uid;
    result->gid = (uint32_t)host_stat.st_gid;
    result->rdevice = (uint64_t)host_stat.st_rdev;
    result->size = (int64_t)host_stat.st_size;
    result->block_size = (int64_t)host_stat.st_blksize;
    result->blocks = (int64_t)host_stat.st_blocks;
    result->access_time.seconds = (int64_t)host_stat.st_atimespec.tv_sec;
    result->access_time.nanoseconds = (int64_t)host_stat.st_atimespec.tv_nsec;
    result->modify_time.seconds = (int64_t)host_stat.st_mtimespec.tv_sec;
    result->modify_time.nanoseconds = (int64_t)host_stat.st_mtimespec.tv_nsec;
    result->change_time.seconds = (int64_t)host_stat.st_ctimespec.tv_sec;
    result->change_time.nanoseconds = (int64_t)host_stat.st_ctimespec.tv_nsec;
    return 0;
}

static char* metalsharp_eac_xpg_basename(char* path) {
    return path != NULL ? basename(path) : (errno = EINVAL, NULL);
}

static long metalsharp_eac_syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    long result = -1;
    switch (number) {
    case MS_LINUX_READ: {
        int fd = va_arg(args, int);
        void* buffer = va_arg(args, void*);
        size_t length = va_arg(args, size_t);
        result = read(fd, buffer, length);
        break;
    }
    case MS_LINUX_WRITE: {
        int fd = va_arg(args, int);
        const void* buffer = va_arg(args, const void*);
        size_t length = va_arg(args, size_t);
        result = write(fd, buffer, length);
        break;
    }
    case MS_LINUX_OPEN: {
        const char* path = va_arg(args, const char*);
        int flags = va_arg(args, int);
        mode_t mode = va_arg(args, int);
        result = (flags & O_CREAT) != 0 ? metalsharp_eac_open(path, flags, mode) : metalsharp_eac_open(path, flags);
        break;
    }
    case MS_LINUX_CLOSE:
        result = close(va_arg(args, int));
        break;
    case MS_LINUX_FSTAT: {
        int fd = va_arg(args, int);
        MsLinuxStat* output = va_arg(args, MsLinuxStat*);
        struct stat host_stat;
        if (fstat(fd, &host_stat) == 0 && output != NULL) {
            memset(output, 0, sizeof(*output));
            output->device = (uint64_t)host_stat.st_dev;
            output->inode = (uint64_t)host_stat.st_ino;
            output->link_count = (uint64_t)host_stat.st_nlink;
            output->mode = (uint32_t)host_stat.st_mode;
            output->uid = (uint32_t)host_stat.st_uid;
            output->gid = (uint32_t)host_stat.st_gid;
            output->rdevice = (uint64_t)host_stat.st_rdev;
            output->size = (int64_t)host_stat.st_size;
            output->block_size = (int64_t)host_stat.st_blksize;
            output->blocks = (int64_t)host_stat.st_blocks;
            output->access_time.seconds = (int64_t)host_stat.st_atimespec.tv_sec;
            output->access_time.nanoseconds = (int64_t)host_stat.st_atimespec.tv_nsec;
            output->modify_time.seconds = (int64_t)host_stat.st_mtimespec.tv_sec;
            output->modify_time.nanoseconds = (int64_t)host_stat.st_mtimespec.tv_nsec;
            output->change_time.seconds = (int64_t)host_stat.st_ctimespec.tv_sec;
            output->change_time.nanoseconds = (int64_t)host_stat.st_ctimespec.tv_nsec;
        } else if (output == NULL) {
            errno = EINVAL;
            result = -1;
        } else {
            result = -1;
        }
        break;
    }
    case MS_LINUX_LSEEK:
        result = (long)lseek(va_arg(args, int), va_arg(args, off_t), va_arg(args, int));
        break;
    case MS_LINUX_MMAP: {
        void* address = va_arg(args, void*);
        size_t length = va_arg(args, size_t);
        int prot = va_arg(args, int);
        int flags = va_arg(args, int);
        int fd = va_arg(args, int);
        off_t offset = va_arg(args, off_t);
        if ((flags & 0x20) != 0) {
            flags &= ~0x20;
            flags |= MAP_ANON;
        }
        result = (long)(intptr_t)mmap(address, length, prot, flags, fd, offset);
        break;
    }
    case MS_LINUX_MPROTECT:
        result = mprotect(va_arg(args, void*), va_arg(args, size_t), va_arg(args, int));
        break;
    case MS_LINUX_MUNMAP:
        result = munmap(va_arg(args, void*), va_arg(args, size_t));
        break;
    case MS_LINUX_GETPID:
        result = getpid();
        break;
    case MS_LINUX_GETPPID:
        result = getppid();
        break;
    case MS_LINUX_FTRUNCATE:
        result = ftruncate(va_arg(args, int), va_arg(args, off_t));
        break;
    case MS_LINUX_MEMFD_CREATE:
        result = metalsharp_memfd_create((unsigned int)va_arg(args, unsigned int));
        break;
    default:
        errno = ENOSYS;
        result = -1;
        break;
    }
    va_end(args);
    return result;
}

static void* host_symbol(const char* name) {
    if (name == NULL) {
        return NULL;
    }
    (void)dlerror();
    void* result = dlsym(RTLD_DEFAULT, name);
    (void)dlerror();
    if (g_linux_module.loaded) {
        ms_log("resolved EAC Linux host symbol name=%s address=0x%llx", name, (unsigned long long)(uintptr_t)result);
    }
    return result;
}

static void* resolve_substrate_target(const char* name) {
    if (name == NULL) {
        return (void*)&metalsharp_eac_unimplemented;
    }
    if (strcmp(name, "__libc_dlopen_mode") == 0 || strcmp(name, "dlopen") == 0) {
        return (void*)&metalsharp_eac_dlopen;
    }
    if (strcmp(name, "__libc_dlsym") == 0 || strcmp(name, "dlsym") == 0) {
        return (void*)&metalsharp_eac_dlsym;
    }
    if (strcmp(name, "dlclose") == 0) {
        return (void*)&metalsharp_eac_dlclose;
    }
    if (strcmp(name, "dlerror") == 0) {
        return (void*)&metalsharp_eac_dlerror;
    }
    if (strcmp(name, "__cxa_atexit") == 0) {
        return (void*)&metalsharp_eac_cxa_atexit;
    }
    if (strcmp(name, "__cxa_finalize") == 0) {
        return (void*)&metalsharp_eac_cxa_finalize;
    }
    if (strcmp(name, "__errno_location") == 0) {
        return (void*)&metalsharp_eac_errno_location;
    }
    if (strcmp(name, "__tls_get_addr") == 0) {
        return (void*)&metalsharp_eac_tls_get_addr;
    }
    if (strcmp(name, "__pthread_key_create") == 0) {
        return (void*)&metalsharp_eac_pthread_key_create;
    }
    if (strcmp(name, "pthread_key_create") == 0) {
        return (void*)&metalsharp_eac_pthread_key_create;
    }
    if (strcmp(name, "pthread_key_delete") == 0) {
        return (void*)&metalsharp_eac_pthread_key_delete;
    }
    if (strcmp(name, "pthread_setspecific") == 0) {
        return (void*)&metalsharp_eac_pthread_setspecific;
    }
    if (strcmp(name, "pthread_getspecific") == 0) {
        return (void*)&metalsharp_eac_pthread_getspecific;
    }
    if (strcmp(name, "pthread_self") == 0) {
        return (void*)&metalsharp_eac_pthread_self;
    }
    if (strcmp(name, "stdin") == 0) {
        return (void*)&stdin;
    }
    if (strcmp(name, "stdout") == 0) {
        return (void*)&stdout;
    }
    if (strcmp(name, "stderr") == 0) {
        return (void*)&stderr;
    }
    if (strcmp(name, "dl_iterate_phdr") == 0) {
        return (void*)&metalsharp_eac_dl_iterate_phdr;
    }
    if (strcmp(name, "pthread_mutexattr_init") == 0) {
        return (void*)&metalsharp_eac_pthread_mutexattr_init;
    }
    if (strcmp(name, "pthread_mutexattr_destroy") == 0) {
        return (void*)&metalsharp_eac_pthread_mutexattr_destroy;
    }
    if (strcmp(name, "pthread_mutexattr_settype") == 0) {
        return (void*)&metalsharp_eac_pthread_mutexattr_settype;
    }
    if (strcmp(name, "pthread_mutex_init") == 0) {
        return (void*)&metalsharp_eac_pthread_mutex_init;
    }
    if (strcmp(name, "pthread_mutex_destroy") == 0) {
        return (void*)&metalsharp_eac_pthread_mutex_destroy;
    }
    if (strcmp(name, "pthread_mutex_lock") == 0) {
        return (void*)&metalsharp_eac_pthread_mutex_lock;
    }
    if (strcmp(name, "pthread_mutex_unlock") == 0) {
        return (void*)&metalsharp_eac_pthread_mutex_unlock;
    }
    if (strcmp(name, "pthread_mutex_trylock") == 0) {
        return (void*)&metalsharp_eac_pthread_mutex_trylock;
    }
    if (strcmp(name, "malloc") == 0) {
        return (void*)&metalsharp_eac_malloc;
    }
    if (strcmp(name, "calloc") == 0) {
        return (void*)&metalsharp_eac_calloc;
    }
    if (strcmp(name, "realloc") == 0) {
        return (void*)&metalsharp_eac_realloc;
    }
    if (strcmp(name, "free") == 0) {
        return (void*)&metalsharp_eac_free;
    }
    if (strcmp(name, "_ZGTtnam") == 0) {
        return (void*)&metalsharp_eac_operator_new;
    }
    if (strcmp(name, "_ZGTtdlPv") == 0) {
        return (void*)&metalsharp_eac_operator_delete;
    }
    if (strcmp(name, "__gmon_start__") == 0 || strncmp(name, "_ITM_", 5) == 0) {
        return (void*)&metalsharp_eac_noop;
    }
    if (strcmp(name, "open") == 0 || strcmp(name, "open64") == 0) {
        return (void*)&metalsharp_eac_open;
    }
    if (strcmp(name, "prctl") == 0) {
        return (void*)&metalsharp_eac_prctl;
    }
    if (strcmp(name, "secure_getenv") == 0) {
        return (void*)&metalsharp_eac_secure_getenv;
    }
    if (strcmp(name, "gettext") == 0) {
        return (void*)&metalsharp_eac_gettext;
    }
    if (strcmp(name, "__xstat") == 0) {
        return (void*)&metalsharp_eac_xstat;
    }
    if (strcmp(name, "__xpg_basename") == 0) {
        return (void*)&metalsharp_eac_xpg_basename;
    }
    if (strcmp(name, "syscall") == 0) {
        return (void*)&metalsharp_eac_syscall;
    }
    void* host = host_symbol(name);
    if (host != NULL) {
        return host;
    }
    ms_log("unresolved Linux ABI symbol name=%s", name);
    return (void*)&metalsharp_eac_unimplemented;
}

static uint64_t align_down_linux(uint64_t value) {
    return value & ~((uint64_t)MS_PAGE_SIZE - 1u);
}

static uint64_t align_up_linux(uint64_t value) {
    return (value + MS_PAGE_SIZE - 1u) & ~((uint64_t)MS_PAGE_SIZE - 1u);
}

static void* linux_module_address(const MsLinuxLoadedModule* module, uint64_t vaddr) {
    if (module == NULL || module->mapping_start == NULL || vaddr < module->minimum_vaddr ||
        vaddr - module->minimum_vaddr >= module->size) {
        return NULL;
    }
    return (uint8_t*)module->mapping_start + (vaddr - module->minimum_vaddr);
}

static size_t translate_linux_stack_canary_segment(MsLinuxLoadedModule* module) {
    if (module == NULL || module->mapping_start == NULL) {
        return 0;
    }
    /* Wine's x86-64 guest TEB is carried in GS on the existing MetalSharp
     * runtime.  Linux ELF code emitted by the EAC toolchain uses FS:0x28 for
     * the stack guard.  Translate only the canonical stack-protector load and
     * compare forms; all other segment operations remain guest code. */
    uint8_t* bytes = module->mapping_start;
    size_t translated = 0;
    for (size_t offset = 0; offset + 9 <= module->size; offset++) {
        if (bytes[offset] != 0x64 || bytes[offset + 1] != 0x48 ||
            (bytes[offset + 2] != 0x8b && bytes[offset + 2] != 0x33) || bytes[offset + 4] != 0x25 ||
            bytes[offset + 5] != 0x28 || bytes[offset + 6] != 0x00 || bytes[offset + 7] != 0x00 ||
            bytes[offset + 8] != 0x00) {
            continue;
        }
        bytes[offset] = 0x65; /* GS */
        translated++;
    }
    if (translated != 0) {
        ms_log("translated Linux FS stack-canary accesses to Wine GS count=%zu", translated);
    }
    return translated;
}

static bool protect_linux_module_segments(MsLinuxLoadedModule* module) {
    if (module == NULL || module->mapping_start == NULL || module->size == 0 || (module->size % MS_PAGE_SIZE) != 0) {
        return false;
    }
    size_t page_count = module->size / MS_PAGE_SIZE;
    uint8_t* page_protections = calloc(page_count, sizeof(*page_protections));
    if (page_protections == NULL) {
        ms_log("cannot allocate EAC Linux module page protections");
        return false;
    }
    for (uint16_t index = 0; index < module->program_count; index++) {
        const MsElfProgramHeader* program = &module->program_headers[index];
        if (program->type != 1 /* PT_LOAD */ || program->memsz == 0) {
            continue;
        }
        uint64_t start = align_down_linux(program->vaddr);
        uint64_t end = align_up_linux(program->vaddr + program->memsz);
        if (start < module->minimum_vaddr || end < start || end - module->minimum_vaddr > module->size) {
            free(page_protections);
            ms_log("invalid EAC Linux module protection range index=%u", index);
            return false;
        }
        unsigned char protection = 0;
        if ((program->flags & 4u) != 0) {
            protection |= PROT_READ;
        }
        if ((program->flags & 2u) != 0) {
            protection |= PROT_WRITE;
        }
        if ((program->flags & 1u) != 0) {
            protection |= PROT_EXEC;
        }
        size_t first_page = (size_t)((start - module->minimum_vaddr) / MS_PAGE_SIZE);
        size_t last_page = (size_t)((end - module->minimum_vaddr) / MS_PAGE_SIZE);
        for (size_t page = first_page; page < last_page; page++) {
            page_protections[page] |= protection;
        }
    }
    bool success = true;
    size_t run_start = 0;
    while (run_start < page_count) {
        uint8_t protection = page_protections[run_start];
        size_t run_end = run_start + 1;
        while (run_end < page_count && page_protections[run_end] == protection) {
            run_end++;
        }
        int mach_protection = protection == 0 ? PROT_NONE : protection;
        if (mprotect((uint8_t*)module->mapping_start + run_start * MS_PAGE_SIZE, (run_end - run_start) * MS_PAGE_SIZE,
                     mach_protection) != 0) {
            ms_log("cannot apply EAC Linux module protections page=%zu count=%zu prot=0x%x errno=%d", run_start,
                   run_end - run_start, mach_protection, errno);
            success = false;
            break;
        }
        run_start = run_end;
    }
    free(page_protections);
    if (success) {
        module->protections_applied = true;
        ms_log("applied EAC Linux PT_LOAD protections pages=%zu", page_count);
    }
    return success;
}

static uint64_t linux_dynamic_value(const MsLinuxLoadedModule* module, int64_t tag) {
    for (size_t index = 0; index < module->dynamic_count; index++) {
        if (module->dynamic[index].tag == tag) {
            return module->dynamic[index].value;
        }
    }
    return 0;
}

static const char* linux_module_symbol_name(const MsLinuxLoadedModule* module, size_t index) {
    if (module == NULL || module->symbols == NULL || index >= module->symbol_count) {
        return NULL;
    }
    uint32_t offset = module->symbols[index].name;
    if (module->strings == NULL || module->string_size == 0 || offset >= module->string_size) {
        return NULL;
    }
    const char* name = module->strings + offset;
    if (memchr(name, '\0', module->string_size - offset) == NULL) {
        return NULL;
    }
    return name;
}

static void* linux_module_symbol_value(MsLinuxLoadedModule* module, size_t index) {
    if (module == NULL || module->symbols == NULL || index >= module->symbol_count) {
        return NULL;
    }
    MsElfSymbol* symbol = &module->symbols[index];
    if (symbol->shndx == 0) {
        const char* name = linux_module_symbol_name(module, index);
        return resolve_substrate_target(name);
    }
    return linux_module_address(module, symbol->value);
}

static bool apply_linux_module_relocations(MsLinuxLoadedModule* module, uint64_t address, uint64_t size,
                                           const char* kind) {
    if (address == 0 || size == 0) {
        return true;
    }
    if ((size % sizeof(MsElfRela)) != 0) {
        ms_log("invalid Linux %s relocation size=0x%llx", kind, (unsigned long long)size);
        return false;
    }
    MsElfRela* relocations = linux_module_address(module, address);
    if (relocations == NULL) {
        ms_log("Linux %s relocation table is outside module address=0x%llx", kind, (unsigned long long)address);
        return false;
    }
    size_t count = (size_t)(size / sizeof(MsElfRela));
    for (size_t index = 0; index < count; index++) {
        MsElfRela* relocation = &relocations[index];
        uint32_t type = (uint32_t)(relocation->info & 0xffffffffu);
        uint32_t symbol_index = (uint32_t)(relocation->info >> 32);
        if (index < 8 || (index % 128) == 0) {
            ms_log("EAC %s relocation index=%zu/%zu type=%u symbol=%u offset=0x%llx", kind, index, count, type,
                   symbol_index, (unsigned long long)relocation->offset);
        }
        uintptr_t* target = linux_module_address(module, relocation->offset);
        if (target == NULL) {
            ms_log("Linux %s relocation target outside module offset=0x%llx", kind,
                   (unsigned long long)relocation->offset);
            return false;
        }
        switch (type) {
        case MS_R_X86_64_RELATIVE:
            *target = (uintptr_t)module->base + (intptr_t)relocation->addend;
            break;
        case MS_R_X86_64_DTPMOD64:
            *target = 1;
            break;
        case MS_R_X86_64_64:
        case MS_R_X86_64_GLOB_DAT:
        case MS_R_X86_64_JUMP_SLOT: {
            uintptr_t value = 0;
            if (symbol_index != 0) {
                const char* relocation_name = linux_module_symbol_name(module, symbol_index);
                ms_log("EAC %s symbol relocation index=%zu symbol=%u name_ptr=0x%llx name=%s", kind, index,
                       symbol_index, (unsigned long long)(uintptr_t)relocation_name,
                       relocation_name != NULL ? relocation_name : "<invalid>");
                value = (uintptr_t)linux_module_symbol_value(module, symbol_index);
                if (value == 0) {
                    ms_log("Linux relocation unresolved symbol=%s type=%u",
                           relocation_name != NULL ? relocation_name : "<null>", type);
                    if ((module->symbols[symbol_index].info >> 4) != 2 /* STB_WEAK */) {
                        return false;
                    }
                }
            }
            *target = value + (intptr_t)relocation->addend;
            break;
        }
        default:
            ms_log("unsupported Linux relocation type=%u offset=0x%llx", type, (unsigned long long)relocation->offset);
            return false;
        }
    }
    return true;
}

static void append_linux_module_map(const MsLinuxLoadedModule* module, int fd) {
    if (g_maps_path[0] == '\0' || module == NULL) {
        return;
    }
    int maps_fd = ms_raw_open(g_maps_path, O_WRONLY | O_APPEND, 0);
    if (maps_fd < 0) {
        ms_log("cannot append Linux module map errno=%d", errno);
        return;
    }
    const char* dump = getenv("METALSHARP_EAC_MODULE_DUMP");
    const char* name = dump != NULL && dump[0] != '\0' ? dump : "/proc/self/fd";
    char line[512];
    int length = snprintf(line, sizeof(line), "%016llx-%016llx rwxp 00000000 00:00 0 %s\n",
                          (unsigned long long)(uintptr_t)module->base,
                          (unsigned long long)((uintptr_t)module->base + module->size), name);
    if (length > 0) {
        (void)write(maps_fd, line, (size_t)length);
    }
    close(maps_fd);
    ms_log("published Linux EAC module map fd=%d base=0x%llx size=0x%zx", fd,
           (unsigned long long)(uintptr_t)module->base, module->size);
}

static bool initialize_linux_module(MsLinuxLoadedModule* module) {
    if (module->initialized) {
        return true;
    }
    module->initialized = true;
    if (module->init != 0) {
        void (*initializer)(void) = (void (*)(void))linux_module_address(module, module->init);
        if (initializer != NULL) {
            ms_log("calling Linux EAC DT_INIT base=0x%llx", (unsigned long long)(uintptr_t)initializer);
            initializer();
        }
    }
    if (module->init_array != 0) {
        void (**initializers)(void) = linux_module_address(module, module->init_array);
        for (size_t index = 0; initializers != NULL && index < module->init_array_count; index++) {
            if (initializers[index] != NULL && (uintptr_t)initializers[index] != UINTPTR_MAX) {
                ms_log("calling Linux EAC init_array[%zu] base=0x%llx", index,
                       (unsigned long long)(uintptr_t)initializers[index]);
                initializers[index]();
                module->init_array_called++;
            }
        }
    }
    return true;
}

static void* load_linux_module_fd(int fd) {
    __sync_fetch_and_add(&g_eac_module_load_attempts, 1);
    if (g_linux_module.loaded) {
        return g_linux_module.base;
    }
    ms_log("loading EAC Linux module fd=%d", fd);
    struct stat file_stat;
    if (fstat(fd, &file_stat) != 0 || file_stat.st_size < (off_t)sizeof(MsElfHeader)) {
        ms_log("cannot stat EAC Linux module fd=%d errno=%d", fd, errno);
        return NULL;
    }
    MsElfHeader header;
    if (pread(fd, &header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
        memcmp(header.ident,
               "\x7f"
               "ELF",
               4) != 0 ||
        header.ident[4] != 2 || header.ident[5] != 1 || header.type != 3 || header.machine != 62 ||
        header.phentsize != sizeof(MsElfProgramHeader) || header.phnum == 0) {
        ms_log("invalid EAC Linux ELF header fd=%d", fd);
        return NULL;
    }
    ms_log("validated EAC Linux ELF fd=%d size=%lld phnum=%u", fd, (long long)file_stat.st_size, header.phnum);
    if (getenv("METALSHARP_EAC_MODULE_DUMP") != NULL) {
        (void)dump_linux_module_fd(fd);
    }
    size_t phdr_bytes = (size_t)header.phnum * header.phentsize;
    if (header.phoff > (uint64_t)file_stat.st_size || phdr_bytes > (size_t)file_stat.st_size - header.phoff) {
        ms_log("EAC Linux ELF program headers exceed file fd=%d", fd);
        return NULL;
    }
    MsElfProgramHeader* program_headers = metalsharp_eac_calloc(header.phnum, sizeof(*program_headers));
    if (program_headers == NULL || pread(fd, program_headers, phdr_bytes, (off_t)header.phoff) != (ssize_t)phdr_bytes) {
        metalsharp_eac_free(program_headers);
        ms_log("cannot read EAC Linux ELF program headers fd=%d", fd);
        return NULL;
    }

    uint64_t minimum = UINT64_MAX;
    uint64_t maximum = 0;
    for (uint16_t index = 0; index < header.phnum; index++) {
        MsElfProgramHeader* program = &program_headers[index];
        if (program->type != 1 /* PT_LOAD */) {
            continue;
        }
        if (program->filesz > program->memsz || program->offset > (uint64_t)file_stat.st_size ||
            program->filesz > (uint64_t)file_stat.st_size - program->offset) {
            metalsharp_eac_free(program_headers);
            ms_log("invalid EAC Linux load segment index=%u", index);
            return NULL;
        }
        uint64_t start = align_down_linux(program->vaddr);
        uint64_t end = align_up_linux(program->vaddr + program->memsz);
        if (end < start) {
            metalsharp_eac_free(program_headers);
            return NULL;
        }
        if (start < minimum) {
            minimum = start;
        }
        if (end > maximum) {
            maximum = end;
        }
    }
    if (minimum == UINT64_MAX || maximum <= minimum || maximum - minimum > SIZE_MAX) {
        metalsharp_eac_free(program_headers);
        ms_log("EAC Linux ELF has no usable load segments");
        return NULL;
    }
    size_t image_size = (size_t)(maximum - minimum);
    void* mapping = mmap((void*)(uintptr_t)0x700300000000ULL, image_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        mapping = mmap(NULL, image_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
    if (mapping == MAP_FAILED) {
        metalsharp_eac_free(program_headers);
        ms_log("cannot map EAC Linux module size=0x%zx errno=%d", image_size, errno);
        return NULL;
    }
    for (uint16_t index = 0; index < header.phnum; index++) {
        MsElfProgramHeader* program = &program_headers[index];
        if (program->type != 1) {
            continue;
        }
        uint8_t* destination = (uint8_t*)mapping + (program->vaddr - minimum);
        if (program->filesz != 0 &&
            pread(fd, destination, (size_t)program->filesz, (off_t)program->offset) != (ssize_t)program->filesz) {
            munmap(mapping, image_size);
            metalsharp_eac_free(program_headers);
            ms_log("cannot read EAC Linux load segment index=%u", index);
            return NULL;
        }
        if (program->memsz > program->filesz) {
            memset(destination + program->filesz, 0, (size_t)(program->memsz - program->filesz));
        }
    }

    MsLinuxLoadedModule module = {0};
    module.mapping_start = mapping;
    module.base = (uint8_t*)mapping - minimum;
    module.size = image_size;
    module.minimum_vaddr = minimum;
    module.program_headers = program_headers;
    module.program_count = header.phnum;
    module.module_fd = fd;
    module.loaded = true;
    for (uint16_t index = 0; index < header.phnum; index++) {
        MsElfProgramHeader* program = &program_headers[index];
        if (program->type == MS_PT_DYNAMIC) {
            module.dynamic = linux_module_address(&module, program->vaddr);
            module.dynamic_count = (size_t)(program->filesz / sizeof(MsElfDynamic));
        } else if (program->type == 7 /* PT_TLS */) {
            module.tls_size = (size_t)program->memsz;
            module.tls_align = (size_t)(program->align != 0 ? program->align : 1);
        }
    }
    if (module.dynamic == NULL || module.dynamic_count == 0) {
        munmap(mapping, image_size);
        metalsharp_eac_free(program_headers);
        ms_log("EAC Linux module has no PT_DYNAMIC segment");
        return NULL;
    }
    uint64_t strings_vaddr = linux_dynamic_value(&module, MS_DT_STRTAB);
    uint64_t symbols_vaddr = linux_dynamic_value(&module, MS_DT_SYMTAB);
    uint64_t hash_vaddr = linux_dynamic_value(&module, MS_DT_HASH);
    module.strings = linux_module_address(&module, strings_vaddr);
    module.string_size = (size_t)linux_dynamic_value(&module, MS_DT_STRSZ);
    module.symbols = linux_module_address(&module, symbols_vaddr);
    if (module.strings == NULL || module.string_size == 0 || module.symbols == NULL || hash_vaddr == 0) {
        munmap(mapping, image_size);
        metalsharp_eac_free(program_headers);
        ms_log("EAC Linux module dynamic symbol tables are invalid");
        return NULL;
    }
    uint32_t* hash = linux_module_address(&module, hash_vaddr);
    module.symbol_count = hash != NULL ? hash[1] : 0;
    module.init = linux_dynamic_value(&module, MS_DT_INIT);
    module.init_array = linux_dynamic_value(&module, MS_DT_INIT_ARRAY);
    module.init_array_count = (size_t)(linux_dynamic_value(&module, MS_DT_INIT_ARRAYSZ) / sizeof(uintptr_t));
    module.rela_count = (size_t)(linux_dynamic_value(&module, MS_DT_RELASZ) / sizeof(MsElfRela));
    module.plt_count = (size_t)(linux_dynamic_value(&module, MS_DT_PLTRELSZ) / sizeof(MsElfRela));
    g_linux_module = module;
    ms_log("mapped EAC Linux ELF base=0x%llx size=0x%zx symbols=%zu tls=0x%zx",
           (unsigned long long)(uintptr_t)module.base, module.size, module.symbol_count, module.tls_size);
    (void)translate_linux_stack_canary_segment(&g_linux_module);

    uint64_t rela_address = linux_dynamic_value(&g_linux_module, MS_DT_RELA);
    uint64_t rela_size = linux_dynamic_value(&g_linux_module, MS_DT_RELASZ);
    uint64_t plt_address = linux_dynamic_value(&g_linux_module, MS_DT_JMPREL);
    uint64_t plt_size = linux_dynamic_value(&g_linux_module, MS_DT_PLTRELSZ);
    ms_log("EAC relocation tables rela=0x%llx/0x%llx plt=0x%llx/0x%llx", (unsigned long long)rela_address,
           (unsigned long long)rela_size, (unsigned long long)plt_address, (unsigned long long)plt_size);
    bool rela_relocated = apply_linux_module_relocations(&g_linux_module, rela_address, rela_size, "RELA");
    g_linux_module.rela_relocated = rela_relocated;
    ms_log("EAC RELA relocation pass complete=%d", rela_relocated ? 1 : 0);
    bool plt_relocated =
        rela_relocated && apply_linux_module_relocations(&g_linux_module, plt_address, plt_size, "PLT");
    g_linux_module.plt_relocated = plt_relocated;
    ms_log("EAC PLT relocation pass complete=%d", plt_relocated ? 1 : 0);
    bool relocated = rela_relocated && plt_relocated;
    if (!relocated) {
        (void)dump_linux_module_fd(fd);
        munmap(mapping, image_size);
        metalsharp_eac_free(program_headers);
        memset(&g_linux_module, 0, sizeof(g_linux_module));
        return NULL;
    }
    if (!protect_linux_module_segments(&g_linux_module)) {
        (void)dump_linux_module_fd(fd);
        munmap(mapping, image_size);
        metalsharp_eac_free(program_headers);
        memset(&g_linux_module, 0, sizeof(g_linux_module));
        return NULL;
    }
    append_linux_module_map(&g_linux_module, fd);
    if (!initialize_linux_module(&g_linux_module)) {
        (void)dump_linux_module_fd(fd);
        munmap(mapping, image_size);
        metalsharp_eac_free(program_headers);
        memset(&g_linux_module, 0, sizeof(g_linux_module));
        return NULL;
    }
    __sync_fetch_and_add(&g_eac_module_load_successes, 1);
    ms_log("EAC_PROOF module_loaded=1 base=0x%llx size=0x%zx rela_count=%zu plt_count=%zu init_array_count=%zu "
           "init_array_called=%zu protections=0x%x exports_mask=0x%x",
           (unsigned long long)(uintptr_t)g_linux_module.base, g_linux_module.size, g_linux_module.rela_count,
           g_linux_module.plt_count, g_linux_module.init_array_count, g_linux_module.init_array_called,
           g_linux_module.protections_applied ? 1 : 0, __atomic_load_n(&g_eac_export_mask, __ATOMIC_RELAXED));
    return g_linux_module.base;
}

static void* lookup_linux_module_symbol(MsLinuxLoadedModule* module, const char* name) {
    if (module == NULL || !module->loaded || name == NULL) {
        return NULL;
    }
    for (size_t index = 1; index < module->symbol_count; index++) {
        MsElfSymbol* symbol = &module->symbols[index];
        if (symbol->shndx == 0) {
            continue;
        }
        const char* symbol_name = linux_module_symbol_name(module, index);
        if (symbol_name != NULL && strcmp(symbol_name, name) == 0) {
            return linux_module_address(module, symbol->value);
        }
    }
    return NULL;
}

static int metalsharp_eac_dlclose(void* handle) {
    if (handle == g_linux_module.base) {
        ms_log("__libc_dlclose EAC module handle=0x%llx", (unsigned long long)(uintptr_t)handle);
        return 0;
    }
    return 0;
}

static const char* metalsharp_eac_dlerror(void) {
    return g_linux_dlerror[0] != '\0' ? g_linux_dlerror : NULL;
}

static bool map_linux_libc_image(void) {
    const char* configured = getenv("METALSHARP_EAC_SUBSTRATE_LIBC");
    if (configured == NULL || configured[0] == '\0') {
        ms_log("METALSHARP_EAC_SUBSTRATE_LIBC is not set");
        return false;
    }
    snprintf(g_elf_path, sizeof(g_elf_path), "%s", configured);

    int fd = ms_raw_open(g_elf_path, O_RDWR, 0);
    if (fd < 0) {
        ms_log("open ELF image failed path=%s errno=%d", g_elf_path, errno);
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        ms_log("stat ELF image failed errno=%d", errno);
        close(fd);
        return false;
    }
    size_t length = (size_t)st.st_size;
    size_t map_length = (length + MS_PAGE_SIZE - 1u) & ~(MS_PAGE_SIZE - 1u);
    void* mapping =
        mmap(MS_PREFERRED_ELF_BASE, map_length, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        int readonly_fd = ms_raw_open(g_elf_path, O_RDONLY, 0);
        mapping =
            mmap(MS_PREFERRED_ELF_BASE, map_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, readonly_fd, 0);
        if (readonly_fd >= 0) {
            close(readonly_fd);
        }
        if (mapping == MAP_FAILED) {
            ms_log("mmap ELF image failed errno=%d", errno);
            return false;
        }
        if (mprotect(mapping, map_length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            ms_log("mprotect ELF image failed errno=%d", errno);
            munmap(mapping, map_length);
            return false;
        }
    }
    if (mprotect(mapping, map_length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        ms_log("mprotect ELF image failed errno=%d", errno);
        munmap(mapping, map_length);
        return false;
    }
    g_elf_mapping = mapping;
    g_elf_mapping_size = map_length;
    ms_log("mapped ELF libc image path=%s base=0x%llx size=0x%zx", g_elf_path, (unsigned long long)(uintptr_t)mapping,
           map_length);

    const char* configured_maps = getenv("METALSHARP_EAC_SUBSTRATE_MAPS");
    if (configured_maps != NULL && configured_maps[0] != '\0') {
        snprintf(g_maps_path, sizeof(g_maps_path), "%s", configured_maps);
    } else {
        snprintf(g_maps_path, sizeof(g_maps_path), "/tmp/metalsharp-eac-maps");
    }
    int maps_fd = ms_raw_open(g_maps_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (maps_fd >= 0) {
        int maps_length =
            dprintf(maps_fd, "%016llx-%016llx r-xp 00000000 00:00 0 %s\n", (unsigned long long)(uintptr_t)mapping,
                    (unsigned long long)((uintptr_t)mapping + map_length), g_elf_path);
        if (maps_length <= 0) {
            g_maps_path[0] = '\0';
        }
        close(maps_fd);
    } else {
        g_maps_path[0] = '\0';
    }

    MsElfHeader* header = (MsElfHeader*)mapping;
    if (memcmp(header->ident,
               "\x7f"
               "ELF",
               4) != 0 ||
        header->type != 3 || header->machine != 62) {
        ms_log("invalid ELF libc image header");
        return false;
    }
    const char* strings = NULL;
    MsElfSymbol* symbols = NULL;
    if (header->phentsize != sizeof(MsElfProgramHeader)) {
        ms_log("unexpected ELF program-header size=%u", header->phentsize);
        return false;
    }
    for (uint16_t index = 0; index < header->phnum; index++) {
        MsElfProgramHeader* program =
            (MsElfProgramHeader*)((uint8_t*)mapping + header->phoff + index * header->phentsize);
        if (program->type != MS_PT_DYNAMIC) {
            continue;
        }
        uint64_t* dynamic = (uint64_t*)((uint8_t*)mapping + program->vaddr);
        for (size_t cursor = 0; cursor + 1 < program->filesz / sizeof(uint64_t); cursor += 2) {
            uint64_t tag = dynamic[cursor];
            uint64_t value = dynamic[cursor + 1];
            if (tag == MS_DT_NULL) {
                break;
            }
            if (tag == MS_DT_STRTAB) {
                strings = (const char*)mapping + value;
                /* The protected loader's ELF resolver consumes d_ptr values
                 * as process addresses after it has found the PT_DYNAMIC
                 * segment.  The on-disk image remains a conventional ET_DYN
                 * image with relative virtual addresses; publish the mapped
                 * addresses only in this private, writable instance. */
                dynamic[cursor + 1] = (uint64_t)(uintptr_t)mapping + value;
            } else if (tag == MS_DT_SYMTAB) {
                symbols = (MsElfSymbol*)((uint8_t*)mapping + value);
                dynamic[cursor + 1] = (uint64_t)(uintptr_t)mapping + value;
            } else if (tag == MS_DT_HASH) {
                dynamic[cursor + 1] = (uint64_t)(uintptr_t)mapping + value;
            }
        }
    }
    if (strings == NULL || symbols == NULL) {
        ms_log("ELF libc image has no dynamic symbol table");
        return false;
    }
    for (size_t index = 1; index < 256; index++) {
        MsElfSymbol* symbol = &symbols[index];
        if (symbol->name == 0) {
            break;
        }
        const char* name = strings + symbol->name;
        void* target = resolve_substrate_target(name);
        ms_log("patching ELF symbol index=%zu name=%s value=0x%llx", index, name, (unsigned long long)symbol->value);
        if (symbol->value + 2 + sizeof(target) > g_elf_mapping_size) {
            ms_log("ELF symbol target is outside mapping index=%zu", index);
            break;
        }
        memcpy((uint8_t*)mapping + symbol->value + 2, &target, sizeof(target));
        if (index < 8) {
            ms_log("patched ELF symbol %s -> 0x%llx", name, (unsigned long long)(uintptr_t)target);
        }
        if (index >= 60) {
            break;
        }
    }
    return true;
}

__attribute__((constructor)) static void metalsharp_eac_substrate_init(void) {
    ms_log("MetalSharp Linux ABI substrate initializing pid=%d", (int)getpid());
    if (!map_linux_libc_image()) {
        ms_log("Linux ABI substrate did not initialize");
        return;
    }
    ms_log("Linux ABI substrate initialized; virtual /proc maps is active");
    bool host_tsd_bridge = resolve_host_tsd_bridge();
    ms_log("Darwin host TSD bridge resolved=%d set_tsd=0x%llx current_teb=0x%llx", host_tsd_bridge ? 1 : 0,
           (unsigned long long)(uintptr_t)g_thread_set_tsd_base, (unsigned long long)(uintptr_t)g_nt_current_teb);
    __atomic_store_n(&g_host_tsd_interpose_ready, 1, __ATOMIC_RELEASE);
    /* The exact Wine 11.5 PE files are patched on disk by the installer.
     * Do not scan or rewrite Rosetta's native address space here: guest PE
     * addresses are not host Mach VM addresses, and doing so corrupts Wine's
     * own ntdll/kernel32 state. */
}

/* dyld's documented interpose section is used only for the procfs view. */
__attribute__((used)) static struct {
    const void* replacement;
    const void* replacee;
} metalsharp_eac_open_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void*)&metalsharp_eac_open,
    (const void*)&open,
};

__attribute__((used)) static struct {
    const void* replacement;
    const void* replacee;
} metalsharp_eac_openat_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void*)&metalsharp_eac_openat,
    (const void*)&openat,
};

__attribute__((used)) static struct {
    const void* replacement;
    const void* replacee;
} metalsharp_eac_unix_name_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void*)&metalsharp_eac_ntdll_get_unix_file_name,
    (const void*)&ntdll_get_unix_file_name,
};

__attribute__((used)) static struct {
    const void* replacement;
    const void* replacee;
} metalsharp_eac_sigaction_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void*)&metalsharp_sigaction,
    (const void*)&sigaction,
};

/* ntdll.so calls the private Darwin entry through its lazy symbol stub.  A
 * normal exported function in an inserted dylib is not enough to redirect
 * that pointer under dyld's chained-fixup binding, so register the exact
 * replacee in the documented interpose section. */
extern void metalsharp_original_thread_set_tsd_base(void* base) __asm("__thread_set_tsd_base");
__attribute__((used)) static struct {
    const void* replacement;
    const void* replacee;
} metalsharp_eac_tsd_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void*)&metalsharp_thread_set_tsd_base,
    (const void*)&metalsharp_original_thread_set_tsd_base,
};
