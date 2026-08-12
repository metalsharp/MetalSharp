#include <stddef.h>
#include <string.h>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoAssembly MonoAssembly;
typedef struct _MonoImage MonoImage;
typedef int MonoImageOpenStatus;

struct _MonoDomain {
    int unused;
};

struct _MonoAssembly {
    int unused;
};

struct _MonoImage {
    int unused;
};

static MonoDomain domain;
static MonoAssembly assembly;
static MonoImage image;

static const char* expected_exe_path = "test-fixtures/managed-exit-37.exe";

MonoDomain* mono_jit_init(const char* domain_name) {
    return domain_name && domain_name[0] ? &domain : NULL;
}

MonoDomain* mono_jit_init_version(const char* domain_name, const char* runtime_version) {
    (void)runtime_version;
    return mono_jit_init(domain_name);
}

int mono_jit_exec(MonoDomain* jit_domain, MonoAssembly* jit_assembly, int argc, char* argv[]) {
    if (jit_domain != &domain || jit_assembly != &assembly || argc != 1 || !argv || !argv[0] ||
        strcmp(argv[0], expected_exe_path) != 0)
        return 91;
    return 37;
}

MonoAssembly* mono_assembly_open(const char* filename, MonoImageOpenStatus* status) {
    (void)filename;
    if (status)
        *status = 0;
    return &assembly;
}

MonoImage* mono_assembly_get_image(MonoAssembly* loaded_assembly) {
    return loaded_assembly == &assembly ? &image : NULL;
}

MonoAssembly* mono_assembly_load_from(MonoImage* loaded_image, const char* filename, MonoImageOpenStatus* status) {
    if (status)
        *status = loaded_image == &image && filename && strcmp(filename, expected_exe_path) == 0 ? 0 : 1;
    return loaded_image == &image && filename && strcmp(filename, expected_exe_path) == 0 ? &assembly : NULL;
}

MonoImage* mono_image_open(const char* filename, MonoImageOpenStatus* status) {
    if (status)
        *status = filename && strcmp(filename, expected_exe_path) == 0 ? 0 : 1;
    return filename && strcmp(filename, expected_exe_path) == 0 ? &image : NULL;
}

void mono_set_dirs(const char* assembly_dir, const char* config_dir) {
    (void)assembly_dir;
    (void)config_dir;
}

void mono_config_parse(const char* filename) {
    (void)filename;
}

MonoDomain* mono_domain_get(void) {
    return &domain;
}

void mono_thread_attach(MonoDomain* attached_domain) {
    (void)attached_domain;
}

void mono_thread_manage(void) {}

void mono_runtime_quit(void) {}
