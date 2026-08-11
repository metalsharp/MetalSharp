/// @file mscoree_unix.c
/// @brief Unix-side mscoree backend for Mono runtime loading.
///
/// Loads the system Mono runtime via dlopen and creates an application domain for executing managed .NET assemblies.
/// Handles assembly resolution, P/Invoke marshaling setup, and entry point invocation for managed game executables.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mscoree_unix.h"

typedef long NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0)

static NTSTATUS mscoree_unix_dispatch(unsigned int code, void* args);

typedef NTSTATUS (*unixlib_call_t)(unsigned int, void*);

__attribute__((visibility("default")))
__attribute__((used)) unixlib_call_t __wine_unix_call_funcs[2] = {mscoree_unix_dispatch, mscoree_unix_dispatch};

__attribute__((visibility("default")))
__attribute__((used)) unixlib_call_t __wine_unix_call_wow64_funcs[2] = {mscoree_unix_dispatch, mscoree_unix_dispatch};

#define DEFAULT_WINE_MONO_LIB        "@loader_path/libmonosgen-2.0.dylib"
#define METALSHARP_MONO_LIB_ENV      "METALSHARP_MONO_LIB"
#define METALSHARP_MONO_ROOT_ENV     "METALSHARP_MONO_ROOT"
#define METALSHARP_MONO_ASSEMBLY_ENV "METALSHARP_MONO_ASSEMBLY_DIR"
#define METALSHARP_MONO_CONFIG_ENV   "METALSHARP_MONO_CONFIG_DIR"
#define METALSHARP_HOME_ENV          "METALSHARP_HOME"

static void* g_mono_handle = NULL;
static int g_mono_initialized = 0;
static char g_mono_assembly_dir[1024];
static char g_mono_config_dir[1024];

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoAssembly MonoAssembly;
typedef struct _MonoImage MonoImage;

typedef enum {
    MONO_IMAGE_OK,
    MONO_IMAGE_ERROR_ERRNO,
    MONO_IMAGE_MISSING_ASSEMBLYREF,
    MONO_IMAGE_IMAGE_INVALID
} MonoImageOpenStatus;

static MonoDomain* (*p_mono_jit_init_version)(const char* domain_name, const char* runtime_version);
static int (*p_mono_jit_exec)(MonoDomain* domain, MonoAssembly* assembly, int argc, char* argv[]);
static MonoAssembly* (*p_mono_assembly_open)(const char* filename, MonoImageOpenStatus* status);
static MonoImage* (*p_mono_assembly_get_image)(MonoAssembly* assembly);
static MonoAssembly* (*p_mono_assembly_load_from)(MonoImage* image, const char* fname, MonoImageOpenStatus* status);
static MonoImage* (*p_mono_image_open)(const char* fname, MonoImageOpenStatus* status);
static void (*p_mono_set_dirs)(const char* assembly_dir, const char* config_dir);
static void (*p_mono_config_parse)(const char* filename);
static MonoDomain* (*p_mono_domain_get)(void);
static void (*p_mono_thread_attach)(MonoDomain* domain);
static void (*p_mono_runtime_quit)(void);
static void (*p_mono_thread_manage)(void);

#define LOAD_FUNC(name)                                                                                                \
    p_##name = dlsym(g_mono_handle, #name);                                                                            \
    if (!p_##name) {                                                                                                   \
        fprintf(stderr, "[mscoree-unix] FAILED to load " #name ": %s\n", dlerror());                                   \
        return -1;                                                                                                     \
    }

static const char* non_empty_env(const char* key) {
    const char* value = getenv(key);
    return value && value[0] ? value : NULL;
}

static int join_path(char* out, size_t out_size, const char* a, const char* b, const char* c) {
    if (!out || !out_size || !a || !a[0])
        return 0;

    int written = snprintf(out, out_size, "%s/%s%s%s", a, b ? b : "", c && c[0] ? "/" : "", c ? c : "");
    return written > 0 && (size_t)written < out_size;
}

static int join_home_path(char* out, size_t out_size, const char* suffix) {
    const char* ms_home = non_empty_env(METALSHARP_HOME_ENV);
    if (ms_home)
        return join_path(out, out_size, ms_home, suffix, NULL);

    const char* home = non_empty_env("HOME");
    if (!home)
        return 0;

    char root[1024];
    if (!join_path(root, sizeof(root), home, ".metalsharp", NULL))
        return 0;

    return join_path(out, out_size, root, suffix, NULL);
}

static const char* mono_lib_path(void) {
    const char* configured = non_empty_env(METALSHARP_MONO_LIB_ENV);
    return configured ? configured : DEFAULT_WINE_MONO_LIB;
}

static const char* mono_assembly_dir(void) {
    const char* configured = non_empty_env(METALSHARP_MONO_ASSEMBLY_ENV);
    if (configured)
        return configured;

    const char* mono_root = non_empty_env(METALSHARP_MONO_ROOT_ENV);
    if (mono_root && join_path(g_mono_assembly_dir, sizeof(g_mono_assembly_dir), mono_root, "lib", "mono"))
        return g_mono_assembly_dir;

    if (join_home_path(g_mono_assembly_dir, sizeof(g_mono_assembly_dir), "wine-dlls/mono-x86_64-lib/mono"))
        return g_mono_assembly_dir;

    return NULL;
}

static const char* mono_config_dir(void) {
    const char* configured = non_empty_env(METALSHARP_MONO_CONFIG_ENV);
    if (configured)
        return configured;

    const char* mono_root = non_empty_env(METALSHARP_MONO_ROOT_ENV);
    if (mono_root && join_path(g_mono_config_dir, sizeof(g_mono_config_dir), mono_root, "etc", "mono"))
        return g_mono_config_dir;

    if (join_home_path(g_mono_config_dir, sizeof(g_mono_config_dir), "wine-dlls/mono-x86_64-etc/mono"))
        return g_mono_config_dir;

    return NULL;
}

static int load_mono(void) {
    if (g_mono_handle)
        return 0;

    const char* lib_path = mono_lib_path();
    fprintf(stderr, "[mscoree-unix] Loading mono from %s\n", lib_path);
    g_mono_handle = dlopen(lib_path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_mono_handle) {
        fprintf(stderr, "[mscoree-unix] FAILED to load mono: %s\n", dlerror());
        return -1;
    }

    LOAD_FUNC(mono_jit_init_version);
    LOAD_FUNC(mono_jit_exec);
    LOAD_FUNC(mono_assembly_open);
    LOAD_FUNC(mono_assembly_get_image);
    LOAD_FUNC(mono_assembly_load_from);
    LOAD_FUNC(mono_image_open);
    LOAD_FUNC(mono_set_dirs);
    LOAD_FUNC(mono_config_parse);
    LOAD_FUNC(mono_domain_get);
    LOAD_FUNC(mono_thread_attach);
    LOAD_FUNC(mono_runtime_quit);
    LOAD_FUNC(mono_thread_manage);

    fprintf(stderr, "[mscoree-unix] All mono functions loaded\n");
    return 0;
}

#undef LOAD_FUNC

static int do_init(void* args) {
    (void)args;
    fprintf(stderr, "[mscoree-unix] INIT\n");
    if (load_mono() < 0)
        return -1;

    const char* assembly_dir = mono_assembly_dir();
    const char* config_dir = mono_config_dir();
    if (assembly_dir && config_dir) {
        p_mono_set_dirs(assembly_dir, config_dir);
        fprintf(stderr, "[mscoree-unix] mono dirs: assembly=%s config=%s\n", assembly_dir, config_dir);
    } else {
        fprintf(stderr, "[mscoree-unix] mono dirs not configured; using Mono defaults\n");
    }
    p_mono_config_parse(NULL);

    fprintf(stderr, "[mscoree-unix] mono dirs configured\n");
    return 0;
}

static int do_cor_exe_main(void* args) {
    fprintf(stderr, "[mscoree-unix] do_cor_exe_main ENTRY\n");
    fflush(stderr);

    struct mscoree_cor_exe_main_params* params = (struct mscoree_cor_exe_main_params*)args;
    if (!params) {
        fprintf(stderr, "[mscoree-unix] _CorExeMain requires launch parameters\n");
        return -1;
    }

    // The PE side owns this structure and consumes the result after the call.
    // Start in the failure state so every early return is reported as an error
    // instead of silently becoming a successful process exit.
    params->exit_code = 1;

    if (load_mono() < 0) {
        fprintf(stderr, "[mscoree-unix] mono not loaded\n");
        return -1;
    }

    if (!params->exe_path[0]) {
        fprintf(stderr, "[mscoree-unix] _CorExeMain received an empty exe path\n");
        return -1;
    }

    fprintf(stderr, "[mscoree-unix] using PE launch parameters: exe=%s dir=%s\n", params->exe_path, params->exe_dir);

    fprintf(stderr, "[mscoree-unix] about to call mono_jit_init_version...\n");
    fflush(stderr);

    MonoDomain* domain = NULL;

    typedef MonoDomain* (*jit_init_t)(const char*);
    jit_init_t p_init = (jit_init_t)dlsym(g_mono_handle, "mono_jit_init");
    fprintf(stderr, "[mscoree-unix] mono_jit_init at %p\n", (void*)p_init);
    fflush(stderr);

    if (p_init) {
        fprintf(stderr, "[mscoree-unix] calling mono_jit_init...\n");
        fflush(stderr);
        domain = p_init("metalsharp");
        fprintf(stderr, "[mscoree-unix] mono_jit_init returned %p\n", (void*)domain);
        fflush(stderr);
    }

    if (!domain) {
        fprintf(stderr, "[mscoree-unix] mono_jit_init failed, trying mono_jit_init_version...\n");
        fflush(stderr);
        domain = p_mono_jit_init_version("metalsharp", "v4.0.30319");
        fprintf(stderr, "[mscoree-unix] mono_jit_init_version returned %p\n", (void*)domain);
        fflush(stderr);
    }
    if (!domain) {
        fprintf(stderr, "[mscoree-unix] mono_jit_init_version failed\n");
        return -1;
    }
    fprintf(stderr, "[mscoree-unix] mono runtime initialized\n");

    MonoImageOpenStatus status;

    char unix_path[1024];
    const char* src = params->exe_path;
    char* dst = unix_path;
    while (*src && dst < unix_path + sizeof(unix_path) - 1) {
        *dst++ = (*src == '\\') ? '/' : *src;
        src++;
    }
    *dst = 0;

    fprintf(stderr, "[mscoree-unix] opening image: %s\n", unix_path);
    MonoImage* image = p_mono_image_open(unix_path, &status);
    if (!image) {
        fprintf(stderr, "[mscoree-unix] mono_image_open failed: %d\n", status);
        return -1;
    }

    MonoAssembly* assembly = p_mono_assembly_load_from(image, unix_path, &status);
    if (!assembly) {
        fprintf(stderr, "[mscoree-unix] mono_assembly_load_from failed: %d\n", status);
        return -1;
    }
    fprintf(stderr, "[mscoree-unix] assembly loaded, calling mono_jit_exec\n");

    g_mono_initialized = 1;
    char* argv[] = {unix_path, NULL};
    int exit_code = p_mono_jit_exec(domain, assembly, 1, argv);
    params->exit_code = exit_code;
    fprintf(stderr, "[mscoree-unix] mono_jit_exec returned %d\n", exit_code);

    p_mono_thread_manage();
    p_mono_runtime_quit();

    return exit_code;
}

static int do_get_cor_version(void* args) {
    struct mscoree_cor_version_params* p = (struct mscoree_cor_version_params*)args;
    const char* ver = "v4.0.30319";
    strncpy(p->version, ver, sizeof(p->version) - 1);
    p->version_len = (int32_t)strlen(ver);
    return 0;
}

static int g_state = 0;

static NTSTATUS mscoree_unix_dispatch(unsigned int code, void* args) {
    fprintf(stderr, "[mscoree-unix] dispatch: code=%u args=%p state=%d\n", code, args, g_state);
    fflush(stderr);

    if (g_state == 0) {
        g_state = 1;
        return do_init(args);
    } else if (g_state == 1) {
        g_state = 2;
        return do_cor_exe_main(args);
    } else {
        return do_cor_exe_main(args);
    }
}
